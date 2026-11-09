"""Exercise actual root finalization, hash, index, decoder and HFS failure gates.

Only allocator addresses and wakeup are adapted for the host. Synthetic fixtures
contain no firmware or root-volume content. An optional local full-root run does
not upload its input or establish that handset RAM contains the same bytes.
"""
from pathlib import Path
import argparse
import base64
import ctypes
import _ctypes
import hashlib
import json
import mmap
import os
import struct
import subprocess
import sys
import tempfile
import zlib

parser=argparse.ArgumentParser()
parser.add_argument('--tree',type=Path,required=True)
parser.add_argument('--root',type=Path)
parser.add_argument('--expected')
args=parser.parse_args()
project=Path(__file__).resolve().parents[1]
vermeer=args.tree/'bsd/dev' if (args.tree/'bsd/dev/vermeer_ram_stream.c').exists() else project/'xnu/vermeer'
crypto=args.tree/'libkern/crypto';provider=crypto/'sweet_bearssl'
source=(vermeer/'vermeer_ram_stream.c').read_text()
callbacks=source[source.index('static void hash_hex('):source.index('static int receive_begin(')]
finish=source[source.index('static int receive_finish('):source.index('static void receive_reply(')]
assert finish.count('(void *)(area+overlay_off)')==1
finish=finish.replace('(void *)(area+overlay_off)','test_overlay')
text=(crypto/'corecrypto_sha2.c').read_text()
sha=text[text.index('_Static_assert(sizeof(br_sha256_context)'):text.index('#else /* !ARM64_BOARD_CONFIG_SWEET */')]
prefix=r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "vermeer_packed_store.h"
#include "lzfse/lzfse_internal.h"
#include "bearssl_hash.h"
typedef struct { _Alignas(8) uint8_t ctx[112]; } SHA256_CTX;
typedef uintptr_t vm_offset_t;
#define PAGE_MASK UINT64_C(16383)
static struct { uint64_t packed_bytes,overlay_bytes;uint8_t sha256[32]; } config;
static struct vrs_store store;
static SHA256_CTX transfer_hash;
static lzfse_decoder_state decoder;
static vm_offset_t area,cache,page_index;
static void *test_overlay;
static int ready,sleep_channel,wakes,decode_status,decode_complete,decode_failed;
static size_t decode_written,decode_consumed;
static const char *root_failure_stage;
static void wakeup(void *p) { (void)p;wakes++; }
#if defined(_WIN32)
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif
'''
suffix=r'''
EXPORTED int run_finish(void *packed,size_t length,void *scratch,void *pages,
 void *overlay,const uint8_t *expected,int alias_cache) {
 config.packed_bytes=length;config.overlay_bytes=VRS_PAGE;
 memcpy(config.sha256,expected,32);area=(uintptr_t)packed;
 cache=(uintptr_t)(alias_cache?packed:scratch);page_index=(uintptr_t)pages;test_overlay=overlay;
 ready=wakes=0;root_failure_stage="test-begin";
 SHA256_Init(&transfer_hash);
 for(size_t off=0;off<length;) {
  size_t n=length-off;if(n>VRS_BLOCK)n=VRS_BLOCK;
  SHA256_Update(&transfer_hash,(const uint8_t *)packed+off,n);off+=n;
  if(length>UINT64_C(0x100000000) && !(off&UINT64_C(0x3fffffff))) {
   printf("Full root finalization hashed bytes: %zu\n",off);fflush(stdout);
  }
 }
 int result=receive_finish(NULL);fflush(stdout);return result;
}
EXPORTED const char *stage(void) { return root_failure_stage; }
EXPORTED int root_ready(void) { return ready; }
EXPORTED int wake_count(void) { return wakes; }
'''

def fixture(raw=None,compressed=None,index_bad=False,crc_bad=False,version=1,zero=False):
    raw=bytes(raw if raw is not None else synthetic)
    data=b'' if zero else compressed if compressed is not None else raw
    codec=0 if zero else 2 if compressed is not None else 1
    offset=0 if zero else 8192
    crc=zlib.crc32(raw)^int(crc_bad)
    entry=struct.pack('<QIIIIQ',offset,len(data),1048576,codec,crc,0)
    table_sha=bytearray(hashlib.sha256(entry).digest())
    if index_bad:table_sha[0]^=1
    length=8192 if zero else 8192+((len(data)+4095)&~4095)
    header=struct.pack('<8sIIQIIQQQ32s32s',b'VMRAM001',version,4096,1048576,1048576,1,
        4096,8192,length,hashlib.sha256(raw).digest(),table_sha)
    return header+bytes(4096-len(header))+entry+bytes(4096-len(entry))+data+bytes(length-8192-len(data))

synthetic=bytearray(1048576);synthetic[1024:1026]=b'HX';synthetic[1030]=0x20
compressed=base64.b64decode((project/'tests/root_finish_lzfse.b64').read_text())
with tempfile.TemporaryDirectory() as temporary:
    temp=Path(temporary);c=temp/'finish.c';c.write_text(prefix+sha+callbacks+finish+suffix)
    shared=temp/('finish.dll' if os.name=='nt' else 'finish.dylib' if sys.platform=='darwin' else 'finish.so')
    link=['-shared'] if os.name=='nt' else ['-dynamiclib'] if sys.platform=='darwin' else ['-shared','-fPIC']
    command=[os.environ.get('CC','clang'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-sign-compare',*link,
        '-I',str(vermeer),'-I',str(provider/'inc'),'-I',str(provider/'src'),str(c),
        str(vermeer/'vermeer_packed_store.c'),
        *[str(vermeer/'lzfse'/n) for n in ('lzfse_decode_base.c','lzfse_fse.c','lzvn_decode_base.c')],
        *[str(provider/'src'/n) for n in ('hash/sha2small.c','codec/dec32be.c','codec/enc32be.c')],'-o',str(shared)]
    subprocess.run(command,check=True)
    library=ctypes.CDLL(str(shared));run=library.run_finish
    run.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_int]
    run.restype=ctypes.c_int;library.stage.restype=ctypes.c_char_p
    cache=ctypes.create_string_buffer(1048576);pages=(ctypes.c_uint32*1048576)();overlay=ctypes.create_string_buffer(16384)
    def check(data,result,stage=None,bad_expected=False,alias=False):
        expected=bytearray(hashlib.sha256(data).digest())
        if bad_expected:expected[0]^=1
        packed=ctypes.create_string_buffer(data);digest=ctypes.create_string_buffer(bytes(expected))
        actual=run(packed,len(data),cache,pages,overlay,digest,int(alias))
        assert actual==result,(actual,result,library.stage())
        if stage:assert library.stage().decode()==stage
        assert bool(library.root_ready())==(result==0)
        assert library.wake_count()==int(result==0)
    check(fixture(),0)
    check(fixture(compressed=compressed),0)
    check(fixture(),5,'full-sha256-mismatch',bad_expected=True)
    check(fixture(index_bad=True),5,'index-sha256-mismatch')
    check(fixture(version=2),5,'packed-open')
    check(fixture(),5,'packed-open',alias=True)
    check(fixture(crc_bad=True),5,'hfs-block-crc')
    check(fixture(compressed=b'BAD!'),5,'block-decode')
    bad_signature=bytearray(synthetic);bad_signature[1024]=0
    check(fixture(raw=bad_signature),22,'hfs-header-policy')
    no_journal=bytearray(synthetic);no_journal[1030]=0
    check(fixture(raw=no_journal),22,'hfs-header-policy')
    check(fixture(raw=bytes(1048576),zero=True),22,'hfs-header-policy')
    report={'actual_finalize_cases':11,'actual_sha_and_index_checked':True,
        'actual_lzfse_decode_and_crc_checked':True,'failure_stages_distinct':True,
        'no_readiness_or_wakeup_on_failure':True,'allocator_adapted_for_host':True,'hardware_verified':False}
    if args.root:
        assert args.expected
        digest=ctypes.create_string_buffer(bytes.fromhex(args.expected))
        with args.root.open('rb') as handle,mmap.mmap(handle.fileno(),0,access=mmap.ACCESS_COPY) as mapping:
            packed=(ctypes.c_char*len(mapping)).from_buffer(mapping)
            result=run(packed,len(mapping),cache,pages,overlay,digest,0)
            assert result==0,(result,library.stage())
            assert library.root_ready() and library.wake_count()==1
            report.update(full_root_bytes=len(mapping),full_root_sha256=args.expected,
                actual_full_root_finalize_passed=True)
            del packed
    print(json.dumps(report),flush=True)
    if os.name=='nt':_ctypes.FreeLibrary(library._handle)
