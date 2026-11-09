"""Run actual RAM readback and mapped witness gates, including corruption and wrong PAs."""
from pathlib import Path
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

project=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(project/'tools'))
import root_memory_probes as probes
parser=argparse.ArgumentParser();parser.add_argument('--tree',type=Path,required=True)
tree=parser.parse_args().tree
crypto=tree/'libkern/crypto';provider=crypto/'sweet_bearssl'
stream=(project/'xnu/vermeer/vermeer_ram_stream.c').read_text(encoding='utf-8')
def function(name):
    import re
    found=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\s*\{',stream,re.M)
    assert found,name
    at=stream.index('{',found.start())+1;depth=1;end=at
    while depth:
        depth+=(stream[end]=='{')-(stream[end]=='}');end+=1
    return stream[found.start():end]+'\n'
wrappers=(crypto/'corecrypto_sha2.c').read_text(encoding='utf-8')
wrappers=wrappers[wrappers.index('_Static_assert(sizeof(br_sha256_context)'):wrappers.index('#else /* !ARM64_BOARD_CONFIG_SWEET */')]
prefix=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "bearssl_hash.h"
typedef struct { _Alignas(8) uint8_t ctx[112]; } SHA256_CTX;
typedef int DTEntry;
typedef int pmap_t;
typedef uint32_t ppnum_t;
typedef uintptr_t vm_map_offset_t;
#define STREAM_BASE UINT64_C(0x940000000)
#define PAGE_SHIFT 14
#define kSuccess 0
static pmap_t kernel_pmap=7;
static uintptr_t area;
static struct {uint64_t packed_bytes;uint8_t sha256[32];} config;
static const char *root_failure_stage;
static uint8_t probe_blob[8192];static unsigned probe_size;
static uint64_t wrong_pa=UINT64_MAX;
static int SecureDTGetProperty(DTEntry entry,const char *name,const void **blob,unsigned *bytes) {
 assert(entry==1 && !strcmp(name,"vermeer-ram-probes"));*blob=probe_blob;*bytes=probe_size;return 0;
}
static ppnum_t pmap_find_phys(pmap_t map,vm_map_offset_t va) {
 assert(map==kernel_pmap && va>=area);uint64_t off=va-area;
 return (ppnum_t)((STREAM_BASE+off+(off==wrong_pa?16384:0))>>PAGE_SHIFT);
}
#define VERMEER_ROOT_VERIFY_HOST_TEST
#define RFB_VERIFY_ROOT_BASE area
void con_clear(uint32_t x){(void)x;}
void con_str(const char *a,const char *b){(void)a;(void)b;}
void con_hex(const char *a,uint64_t b){(void)a;(void)b;}
'''
structs=stream[stream.index('struct root_probe_header {'):stream.index('static int verify_root_probes(')]
checks=r'''
int main(void) {
 FILE *file=fopen(ROOT_INPUT,"rb");assert(file);
 uint8_t *memory=malloc(RFB_ROOT_BYTES+16);assert(memory);
 assert(fread(memory,1,RFB_ROOT_BYTES,file)==RFB_ROOT_BYTES);fclose(file);
 memset(memory+RFB_ROOT_BYTES,0xa5,16);area=(uintptr_t)memory;
 config.packed_bytes=RFB_ROOT_BYTES;memcpy(config.sha256,rfb_root_sha,32);
 file=fopen(PROBE_INPUT,"rb");assert(file);probe_size=(unsigned)fread(probe_blob,1,sizeof(probe_blob),file);fclose(file);
 assert(verify_sha_provider()==0);
 assert(rfb_verify_stored_root()==0 && rfb_verify_handoff_probes()==0 && verify_root_probes(1)==0);
 for(unsigned i=0;i<RFB_ROOT_PROBE_COUNT;i++) {
  uint64_t off=rfb_root_probes[i].offset;memory[off]^=0x80;
  assert(rfb_verify_stored_root()!=0 && rfb_verify_handoff_probes()!=0);
  assert(verify_root_probes(1)==EIO && !strcmp(root_failure_stage,"mapped-root-probe-mismatch"));memory[off]^=0x80;
 }
 /* A change outside the witnesses is still rejected by the full stored-root SHA. */
 memory[32768]^=1;assert(rfb_verify_handoff_probes()==0 && rfb_verify_stored_root()!=0);memory[32768]^=1;
 wrong_pa=0;assert(verify_root_probes(1)==EIO && !strcmp(root_failure_stage,"root-map-address"));wrong_pa=UINT64_MAX;
 struct root_probe_header *header=(void *)probe_blob;
 header->sha256[0]^=1;assert(verify_root_probes(1)==EINVAL);header->sha256[0]^=1;
 header->count=129;assert(verify_root_probes(1)==EINVAL);header->count=RFB_ROOT_PROBE_COUNT;
 struct root_probe_entry *entry=(void *)(probe_blob+sizeof(*header));
 entry->offset=RFB_ROOT_BYTES;assert(verify_root_probes(1)==EINVAL);entry->offset=0;
 entry->bytes=1;assert(verify_root_probes(1)==EINVAL);entry->bytes=16384;
 entry->reserved=1;assert(verify_root_probes(1)==EINVAL);entry->reserved=0;
 assert(verify_root_probes(1)==0 && rfb_verify_stored_root()==0);
 for(unsigned i=0;i<16;i++)assert(memory[RFB_ROOT_BYTES+i]==0xa5);
 free(memory);puts("Actual stored RAM SHA, handoff probes, kernel SHA selftest, mapped probes, wrong PA and configuration rejection PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    temp=Path(tmp);root=temp/'root.bin';packed=bytes((i*31+7)&255 for i in range(147456));root.write_bytes(packed)
    digest=hashlib.sha256(packed).digest()
    original_interval=probes.INTERVAL;probes.INTERVAL=65536
    blob,rows=probes.make_probes(root,len(packed),digest.hex());probes.INTERVAL=original_interval
    assert [row[0] for row in rows]==[0,65536,131072]
    assert probes.ENTRY.unpack(probes.ENTRY.pack(0x100000000,16384,0,digest))[0]==0x100000000
    witness=temp/'probes.bin';witness.write_bytes(blob)
    (temp/'ram_fastboot_config.h').write_text('#define RFB_ROOT_BYTES UINT64_C(%d)\nstatic const uint8_t rfb_root_sha[32]={%s};\n' % (len(packed),','.join(map(str,digest))),encoding='utf-8')
    (temp/'ram_fastboot_probes.h').write_text(probes.c_header(rows),encoding='utf-8')
    include='#include "'+(project/'boot/ram_root_verify.c').as_posix()+'"\n'
    paths='#define ROOT_INPUT "'+root.as_posix()+'"\n#define PROBE_INPUT "'+witness.as_posix()+'"\n'
    c=temp/'ram_witness.c';c.write_text(prefix+wrappers+function('hash_hex')+function('hash_mismatch')+
        function('verify_sha_provider')+structs+function('verify_root_probes')+include+paths+checks,encoding='utf-8')
    executable=temp/('ram_witness.exe' if os.name=='nt' else 'ram_witness')
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    command=[os.environ.get('CC','clang'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-format',*flags,
        '-I',str(temp),'-I',str(project/'boot'),'-I',str(provider/'inc'),'-I',str(provider/'src'),
        str(c),str(project/'boot/ram_fastboot.c'),
        *[str(provider/'src'/name) for name in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c')],'-o',str(executable)]
    subprocess.run(command,check=True);subprocess.run([str(executable)],check=True)
