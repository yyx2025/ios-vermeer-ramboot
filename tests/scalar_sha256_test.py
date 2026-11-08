"""Test actual patched SHA256 wrappers and pinned scalar provider against hashlib."""
from pathlib import Path
import argparse
import ctypes
import _ctypes
import hashlib
import json
import mmap
import os
import subprocess
import sys
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--tree',type=Path,required=True)
parser.add_argument('--root',type=Path)
parser.add_argument('--expected')
args=parser.parse_args()
tree=args.tree.resolve()
crypto=tree/'libkern/crypto'
provider=crypto/'sweet_bearssl'
text=(crypto/'corecrypto_sha2.c').read_text()
begin=text.index('_Static_assert(sizeof(br_sha256_context)')
end=text.index('#else /* !ARM64_BOARD_CONFIG_SWEET */',begin)
body=text[begin:end]
prefix=r'''
#include <stddef.h>
#include <stdint.h>
#include "bearssl_hash.h"
/* XNU's legacy SHA256 context: 112 bytes, naturally aligned to 8 bytes.
 * Its actual header also undergoes the static_assert during kernel compilation. */
typedef struct { _Alignas(8) uint8_t ctx[112]; } SHA256_CTX;
#if defined(_WIN32)
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif
EXPORTED void SHA256_Init(SHA256_CTX *);
EXPORTED void SHA256_Update(SHA256_CTX *,const void *,size_t);
EXPORTED void SHA256_Final(void *,SHA256_CTX *);
'''

class GuardedContext(ctypes.Structure):
    _fields_=[('before',ctypes.c_uint64*2),('context',ctypes.c_uint64*14),('after',ctypes.c_uint64*2)]

with tempfile.TemporaryDirectory() as temporary:
    temp=Path(temporary)
    c=temp/'wrappers.c';c.write_text(prefix+body)
    shared=temp/('sha256.dll' if os.name=='nt' else 'sha256.dylib' if sys.platform=='darwin' else 'sha256.so')
    link=['-shared'] if os.name=='nt' else ['-dynamiclib'] if sys.platform=='darwin' else ['-shared','-fPIC']
    command=[os.environ.get('CC','clang'),'-std=c11','-O2','-Wall','-Wextra','-Werror',*link,
        '-I',str(provider/'inc'),'-I',str(provider/'src'),str(c),
        str(provider/'src/hash/sha2small.c'),str(provider/'src/codec/dec32be.c'),
        str(provider/'src/codec/enc32be.c'),'-o',str(shared)]
    subprocess.run(command,check=True)
    library=ctypes.CDLL(str(shared))
    library.SHA256_Init.argtypes=[ctypes.c_void_p]
    library.SHA256_Update.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t]
    library.SHA256_Final.argtypes=[ctypes.c_void_p,ctypes.c_void_p]
    for function in (library.SHA256_Init,library.SHA256_Update,library.SHA256_Final):function.restype=None
    def initialize():
        context=GuardedContext()
        for index in range(2):context.before[index]=context.after[index]=0x93B1C4586A720DEF
        pointer=ctypes.byref(context,GuardedContext.context.offset)
        library.SHA256_Init(pointer)
        return context,pointer
    def finish(context,pointer):
        digest=(ctypes.c_ubyte*34)();digest[0]=0x93;digest[33]=0xB1
        library.SHA256_Final(ctypes.byref(digest,1),pointer)
        assert digest[0]==0x93 and digest[33]==0xB1
        assert list(context.before)==list(context.after)==[0x93B1C4586A720DEF]*2
        return bytes(digest[1:33]).hex()
    cases=0
    for length in (0,1,7,55,56,63,64,65,127,128,129,511,1024,4096,65537,1048576):
        data=bytes((index*31+7)&255 for index in range(length))
        expected=hashlib.sha256(data).hexdigest()
        for step,alignment in ((max(length,1),0),(2053,1),(17,3)):
            context,pointer=initialize()
            buffer=ctypes.create_string_buffer(b'\0'*alignment+data)
            for offset in range(0,length,step):
                library.SHA256_Update(pointer,ctypes.byref(buffer,alignment+offset),min(step,length-offset))
            assert finish(context,pointer)==expected
            assert finish(context,pointer)==expected
            cases+=1
    report={'wrapper_vectors_passed':cases,'aligned_unaligned_and_fragmented':True,
        'padding_boundaries_checked':True,'context_and_digest_canaries_checked':True,
        'actual_wrapper_source_tested':True,'hardware_verified':False}
    if args.root:
        assert args.expected and args.root.stat().st_size>0x100000000
        context,pointer=initialize()
        size=args.root.stat().st_size
        with args.root.open('rb') as handle,mmap.mmap(handle.fileno(),0,access=mmap.ACCESS_COPY) as mapping:
            for offset in range(0,size,1048576):
                count=min(1048576,size-offset)
                view=(ctypes.c_char*count).from_buffer(mapping,offset)
                library.SHA256_Update(pointer,view,count)
                del view
                if (offset+count)%0x40000000==0:print('Scalar root SHA256 bytes:',offset+count,flush=True)
        digest=finish(context,pointer)
        assert digest==args.expected
        report.update(full_root_bytes=size,full_root_sha256=digest,full_root_above_4gib_verified=True)
    print(json.dumps(report),flush=True)
    # Windows holds the DLL open until it is explicitly unloaded.
    if os.name == 'nt':
        _ctypes.FreeLibrary(library._handle)
