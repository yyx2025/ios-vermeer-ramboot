"""Check the actual ARM64 kernel's SHA256 call graph uses general registers."""
from pathlib import Path
import argparse
import hashlib
import json
import struct

parser=argparse.ArgumentParser()
parser.add_argument('kernel',type=Path)
parser.add_argument('--root-verifier',action='store_true')
args=parser.parse_args()
data=args.kernel.read_bytes()
assert struct.unpack_from('<I',data)[0]==0xfeedfacf
sections={};symtab=None;cursor=32;index=0
for _ in range(struct.unpack_from('<I',data,16)[0]):
    command,size=struct.unpack_from('<II',data,cursor)
    if command==0x19:
        count=struct.unpack_from('<I',data,cursor+64)[0]
        for position in range(count):
            at=cursor+72+position*80;index+=1
            name=data[at:at+16].split(b'\0')[0].decode()
            segment=data[at+16:at+32].split(b'\0')[0].decode()
            address,length,offset=struct.unpack_from('<QQI',data,at+32)
            flags=struct.unpack_from('<I',data,at+64)[0]
            sections[index]={'name':name,'segment':segment,'address':address,'size':length,'offset':offset,'flags':flags}
    elif command==2:symtab=struct.unpack_from('<IIII',data,cursor+8)
    cursor+=size
assert symtab
symoff,count,stroff,strsize=symtab
symbols={};by_address={}
for position in range(count):
    string,kind,section,_,value=struct.unpack_from('<IBBHQ',data,symoff+position*16)
    if not string or kind&0xe!=0xe or section not in sections:continue
    info=sections[section]
    if not (info['flags']&0x80000400):continue
    end=data.index(b'\0',stroff+string,stroff+strsize)
    name=data[stroff+string:end].decode('utf-8',errors='replace')
    symbols[name]=(value,section);by_address.setdefault(value,[]).append((name,section))
starts=sorted(by_address)
roots=('_SHA256_Init','_SHA256_Update','_SHA256_Final','_br_sha256_init','_br_sha224_update','_br_sha256_out','_br_sha2small_round')
if args.root_verifier:roots+=('_receive_finish','_index_sha','_decode_block','_vrs_open','_vrs_read','_lzfse_decode','_verify_root_windows')
assert all(name in symbols for name in roots),'Actual scalar SHA256 symbols missing'
queue=[symbols[name] for name in roots]
visited={};instructions=0
skip={'___stack_chk_fail','___chk_fail','_panic','_panic_plain'}
if args.root_verifier:skip.update(('_printf','_wakeup','___assert_rtn','_SecureDTGetProperty'))
while queue:
    address,section=queue.pop()
    if address in visited:continue
    info=sections[section]
    end=min((start for start in starts if start>address),default=info['address']+info['size'])
    end=min(end,info['address']+info['size'])
    names=[name for name,_ in by_address[address]]
    visited[address]=names
    assert not any(name.startswith(('_ccdigest','_ccsha256')) for name in names),'SHA256 still enters corecrypto'
    for pc in range(address,end,4):
        word=struct.unpack_from('<I',data,info['offset']+pc-info['address'])[0]
        group=(word>>25)&15
        assert group not in (2,7,15),f'FP/SIMD/SVE arithmetic in {names} at {pc:x}'
        assert not (group in (4,6,12,14) and word&0x04000000),f'FP/SIMD memory instruction in {names} at {pc:x}'
        allowed_callback=args.root_verifier and any(name in ('_vrs_open','_vrs_read','_load_block') for name in names)
        assert allowed_callback or word&0xfffffc1f!=0xd63f0000,f'Unexpected indirect call in verifier graph {names} at {pc:x}'
        instructions+=1
        if word&0x7c000000==0x14000000:
            immediate=word&0x3ffffff
            if immediate&0x2000000:immediate-=0x4000000
            target=pc+immediate*4
            if target in by_address and not address<=target<end:
                target_names=by_address[target]
                if not all(name in skip for name,_ in target_names):queue.append((target,target_names[0][1]))
assert instructions>100
print(json.dumps({'actual_kernel_sha256':hashlib.sha256(data).hexdigest(),
    'scalar_sha256_functions_checked':sorted(name for names in visited.values() for name in names),
    'instruction_count':instructions,'no_fp_simd_or_sve':True,'no_corecrypto_dispatch':True}),flush=True)
