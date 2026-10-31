"""Pack a verified full HFSX volume into independently checked LZFSE blocks.

This is a computer-side container tool, not an implemented XNU block driver.
All original volume bytes are retained. No complete-volume RAM copy is made.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import time
import zlib

MAGIC=b'VMRAM001'
HEADER=struct.Struct('<8sIIQIIQQQ32s32s')
ENTRY=struct.Struct('<QIIIIQ')
BLOCK=1024**2
ALIGN=4096
MAX_LOGICAL=16*1024**3

def aligned(n):return (n+ALIGN-1)&~(ALIGN-1)

class Lzfse:
    def __init__(self,path):
        self.library=ctypes.CDLL(str(Path(path).resolve()))
        self.encode=self.library.lzfse_encode_buffer
        self.decode=self.library.lzfse_decode_buffer
        for fn in (self.encode,self.decode):
            fn.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p]
            fn.restype=ctypes.c_size_t
    def compress(self,raw):
        buf=ctypes.create_string_buffer(len(raw)+1024)
        n=self.encode(buf,len(buf),raw,len(raw),None)
        return buf.raw[:n] if n and n<len(raw) else None
    def decompress(self,data,size):
        buf=ctypes.create_string_buffer(size)
        if self.decode(buf,size,data,len(data),None)!=size:raise ValueError('LZFSE decoded length mismatch')
        return buf.raw

def pack(source,destination,lzfse):
    source,destination=Path(source),Path(destination)
    logical=source.stat().st_size
    if not logical or logical>MAX_LOGICAL:raise ValueError('Logical size outside prototype format bounds')
    count=(logical+BLOCK-1)//BLOCK
    data_start=aligned(ALIGN+count*ENTRY.size)
    partial=destination.with_suffix(destination.suffix+'.partial')
    if destination.exists() or partial.exists():raise ValueError('Packed output already exists')
    index=[];digest=hashlib.sha256();last=time.monotonic()
    with source.open('rb') as src,partial.open('xb') as out:
        out.seek(data_start)
        for block in range(count):
            raw=src.read(min(BLOCK,logical-block*BLOCK))
            if not raw:raise ValueError('Truncated source volume')
            digest.update(raw);crc=zlib.crc32(raw)
            if not any(raw):
                index.append(ENTRY.pack(0,0,len(raw),0,crc,0))
            else:
                compressed=lzfse.compress(raw)
                data=compressed if compressed is not None else raw
                offset=out.tell()
                out.write(data);out.write(bytes(aligned(len(data))-len(data)))
                index.append(ENTRY.pack(offset,len(data),len(raw),2 if compressed is not None else 1,crc,0))
            if time.monotonic()-last>=15:
                print('Packed blocks',block+1,'/',count,'stored bytes',out.tell(),flush=True);last=time.monotonic()
        if src.read(1):raise ValueError('Source size changed during packing')
        size=out.tell();table=b''.join(index)
        out.seek(ALIGN);out.write(table)
        header=HEADER.pack(MAGIC,1,ALIGN,logical,BLOCK,count,ALIGN,data_start,size,
                           digest.digest(),hashlib.sha256(table).digest())
        out.seek(0);out.write(header);out.write(bytes(ALIGN-len(header)))
    partial.rename(destination)
    return {'logical_bytes':logical,'packed_bytes':size,'raw_sha256':digest.hexdigest(),
            'block_bytes':BLOCK,'blocks':count,'kernel_driver_implemented':False}

def verify(path,lzfse):
    path=Path(path);size=path.stat().st_size;digest=hashlib.sha256()
    with path.open('rb') as src:
        header=src.read(ALIGN)
        if len(header)!=ALIGN:raise ValueError('Truncated packed header')
        magic,version,hsize,logical,blocksize,count,table_offset,data_offset,packed,raw_sha,table_sha=HEADER.unpack_from(header)
        if magic!=MAGIC or version!=1 or hsize!=ALIGN or blocksize!=BLOCK or not 0<logical<=MAX_LOGICAL:
            raise ValueError('Unsupported packed root format')
        if count!=(logical+BLOCK-1)//BLOCK or table_offset!=ALIGN or data_offset!=aligned(ALIGN+count*ENTRY.size) or packed!=size:
            raise ValueError('Packed root bounds mismatch')
        src.seek(table_offset);table=src.read(count*ENTRY.size)
        if len(table)!=count*ENTRY.size or hashlib.sha256(table).digest()!=table_sha:
            raise ValueError('Root index checksum mismatch')
        cursor=data_offset;last=time.monotonic()
        for i in range(count):
            offset,n,decoded,codec,crc,reserved=ENTRY.unpack_from(table,i*ENTRY.size)
            expected=min(BLOCK,logical-i*BLOCK)
            if decoded!=expected or reserved:raise ValueError('Invalid block length/flags')
            if codec==0:
                if offset or n:raise ValueError('Malformed zero block')
                raw=bytes(decoded)
            elif codec in (1,2):
                if offset!=cursor or offset%ALIGN or not n or n>decoded or offset+n>size:
                    raise ValueError('Root block offset/length mismatch')
                if codec==1 and n!=decoded:raise ValueError('Raw root block length mismatch')
                src.seek(offset);data=src.read(n)
                if len(data)!=n:raise ValueError('Truncated packed root block')
                raw=data if codec==1 else lzfse.decompress(data,decoded)
                cursor=aligned(offset+n)
            else:raise ValueError('Unknown root block codec')
            if zlib.crc32(raw)!=crc:raise ValueError('Decoded root block checksum mismatch')
            digest.update(raw)
            if time.monotonic()-last>=15:
                print('Verified blocks',i+1,'/',count,flush=True);last=time.monotonic()
        if cursor!=size or digest.digest()!=raw_sha:raise ValueError('Full logical root SHA256 mismatch')
    return digest.hexdigest()

def main():
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('input',type=Path)
    p.add_argument('output',type=Path,nargs='?')
    p.add_argument('--lzfse',type=Path,required=True)
    p.add_argument('--build-record',type=Path)
    p.add_argument('--verify-only',action='store_true')
    a=p.parse_args();codec=Lzfse(a.lzfse)
    if a.verify_only:
        print('Verified full logical root SHA256:',verify(a.input,codec));return
    if not a.output or not a.build_record:raise ValueError('Packing requires output and verified build record')
    record=json.loads(a.build_record.read_text())
    if not record.get('complete') or not record.get('fsck_verified') or Path(record['output_file']).resolve()!=a.input.resolve():
        raise ValueError('A completed, fsck-verified HFS root is required')
    with a.input.open('rb') as f:
        f.seek(1024);h=f.read(512)
    if h[:2] not in (b'HX',b'H+') or not struct.unpack_from('>I',h,4)[0]&(1<<13):
        raise ValueError('Journaled HFS/HFSX root required')
    result=pack(a.input,a.output,codec)
    if record.get('sha256') and record['sha256']!=result['raw_sha256']:raise ValueError('Root changed since build verification')
    if verify(a.output,codec)!=result['raw_sha256']:raise ValueError('Root round-trip mismatch')
    packed_sha=hashlib.sha256()
    with a.output.open('rb') as f:
        while b:=f.read(4*1024**2):packed_sha.update(b)
    result.update({'packed_file':str(a.output.resolve()),'packed_sha256':packed_sha.hexdigest(),
                   'full_round_trip_verified':True,'hardware_verified':False,'is_fastboot_boot_image':False})
    a.output.with_suffix('.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
