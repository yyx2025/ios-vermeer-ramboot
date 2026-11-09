"""Execute actual kernel window hashing, rejecting hidden corruption and gaps."""
from pathlib import Path
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

project=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(project/'tools'))
import root_memory_windows as windows
parser=argparse.ArgumentParser();parser.add_argument('--tree',type=Path,required=True)
tree=parser.parse_args().tree;crypto=tree/'libkern/crypto';provider=crypto/'sweet_bearssl'
source=(project/'xnu/vermeer/vermeer_ram_stream.c').read_text(encoding='utf-8')
def function(name):
    match=re.search(r'^static (?:int|void) '+name+r'\([^;]+?\)\s*\{',source,re.M)
    assert match,name
    end=source.index('{',match.start())+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]+'\n'
wrappers=(crypto/'corecrypto_sha2.c').read_text(encoding='utf-8')
wrappers=wrappers[wrappers.index('_Static_assert(sizeof(br_sha256_context)'):wrappers.index('#else /* !ARM64_BOARD_CONFIG_SWEET */')]
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "bearssl_hash.h"
typedef struct {_Alignas(8) uint8_t ctx[112];} SHA256_CTX;
typedef int DTEntry;
static uintptr_t area,cache;
static struct {uint64_t packed_bytes;uint8_t sha256[32];} config;
static SHA256_CTX transfer_hash;
static const char *root_failure_stage;
static uint8_t blob_data[8192];static unsigned blob_size;static int missing;
#define kSuccess 0
#define VRS_BLOCK 16384u
#define VMRW_WINDOW_BYTES UINT64_C(65536)
static int SecureDTGetProperty(DTEntry d,const char *name,const void **blob,unsigned *size){
 assert(d==1 && !strcmp(name,"vermeer-ram-windows"));*blob=blob_data;*size=blob_size;return missing?-1:0;
}
'''
structs=source[source.index('struct root_probe_header {'):source.index('static int verify_root_probes(')]
checks=r'''
static int check(void){SHA256_Init(&transfer_hash);return verify_root_windows(1);}
int main(void){
 setbuf(stdout,NULL);
 FILE *f=fopen(ROOT_FILE,"rb");assert(f);
 uint8_t *data=malloc(ROOT_BYTES);assert(data && fread(data,1,ROOT_BYTES,f)==ROOT_BYTES);fclose(f);area=(uintptr_t)data;
 uint8_t scratch[VRS_BLOCK+32];memset(scratch,0xa5,sizeof(scratch));cache=(uintptr_t)(scratch+16);
 config.packed_bytes=ROOT_BYTES;memcpy(config.sha256,EXPECTED,32);
 f=fopen(WINDOW_FILE,"rb");assert(f);blob_size=(unsigned)fread(blob_data,1,sizeof(blob_data),f);fclose(f);
 int first=check();
 if(first){
  struct root_probe_header initial;memcpy(&initial,blob_data,sizeof(initial));
  fprintf(stderr,"Initial window verification returned %d stage=%s; blob=%u header=%lu entry=%lu magic=%.8s count=%u entry-bytes=%u packed=%llu config=%llu window=%llu sha-match=%d cache-set=%d\n",
   first,root_failure_stage,blob_size,(unsigned long)sizeof(initial),(unsigned long)sizeof(struct root_probe_entry),initial.magic,initial.count,initial.entry_bytes,
   (unsigned long long)initial.packed_bytes,(unsigned long long)config.packed_bytes,(unsigned long long)VMRW_WINDOW_BYTES,!memcmp(initial.sha256,config.sha256,32),cache!=0);
 }
 assert(!first);uint8_t actual[32];SHA256_Final(actual,&transfer_hash);assert(!memcmp(actual,EXPECTED,32));
 /* Changes between the old sparse witnesses are covered, including the tail. */
 const size_t offsets[]={32768,70000,ROOT_BYTES-1};
 for(unsigned i=0;i<3;i++){data[offsets[i]]^=1;assert(check()==EIO && !strcmp(root_failure_stage,"mapped-root-window-mismatch"));data[offsets[i]]^=1;}
 struct root_probe_header *header=(void *)blob_data;
 struct root_probe_entry *entry=(void *)(blob_data+sizeof(*header));
 entry[1].offset++;assert(check()==EINVAL);entry[1].offset--;
 entry[1].bytes--;assert(check()==EINVAL);entry[1].bytes++;
 entry[2].offset=0;assert(check()==EINVAL);entry[2].offset=131072;
 entry[0].reserved=1;assert(check()==EINVAL);entry[0].reserved=0;
 header->count--;assert(check()==EINVAL);header->count++;
 header->sha256[0]^=1;assert(check()==EINVAL);header->sha256[0]^=1;
 header->magic[0]^=1;assert(check()==EINVAL);header->magic[0]^=1;
 blob_size--;assert(check()==EINVAL);blob_size++;
 missing=1;assert(check()==EINVAL);missing=0;
 uintptr_t saved=cache;cache=0;assert(check()==EINVAL);cache=saved;
 assert(!check());SHA256_Final(actual,&transfer_hash);assert(!memcmp(actual,EXPECTED,32));
 for(unsigned i=0;i<16;i++)assert(scratch[i]==0xa5 && scratch[VRS_BLOCK+16+i]==0xa5);
 /* Passing the windows never substitutes for the independent full-file result. */
 ((uint8_t *)&transfer_hash)[80]^=1;SHA256_Final(actual,&transfer_hash);assert(memcmp(actual,EXPECTED,32));
 free(data);puts("Actual full-coverage windows, managed snapshot bounds, gaps, corruption and independent full SHA PASS");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    temp=Path(temporary);data=bytes((i*31+7)&255 for i in range(147456));root=temp/'root';root.write_bytes(data)
    expected=hashlib.sha256(data).digest();windows.WINDOW_BYTES=65536
    blob,rows=windows.make_windows(root,len(data),expected.hex())
    assert [(r[0],r[1]) for r in rows]==[(0,65536),(65536,65536),(131072,16384)]
    windows_file=temp/'windows';windows_file.write_bytes(blob)
    for bad in (data[:-1],bytes([data[0]^1])+data[1:],data+b'X'):
        root.write_bytes(bad)
        try:windows.make_windows(root,len(data),expected.hex())
        except ValueError:pass
        else:raise AssertionError('Unverified input accepted')
    root.write_bytes(data)
    paths='#define ROOT_FILE "'+root.as_posix()+'"\n#define WINDOW_FILE "'+windows_file.as_posix()+'"\n#define ROOT_BYTES 147456u\n'
    paths+='static const uint8_t EXPECTED[32]={'+','.join(map(str,expected))+'};\n'
    c=temp/'windows.c';c.write_text(prefix+wrappers+function('hash_hex')+function('hash_mismatch')+structs+function('verify_root_windows')+paths+checks,encoding='utf-8')
    exe=temp/('windows.exe' if os.name=='nt' else 'windows')
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    command=[os.environ.get('CC','clang'),'-O2','-std=c11','-Wall','-Wextra','-Werror','-Wno-format',*flags,
             '-I',str(provider/'inc'),'-I',str(provider/'src'),str(c),
             *[str(provider/'src'/name) for name in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c')],'-o',str(exe)]
    subprocess.run(command,check=True);subprocess.run([str(exe)],check=True)
