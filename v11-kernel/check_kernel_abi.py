"""Verify the actual rebuilt kernel's mapped IOService methods and prelink ABI."""
from pathlib import Path
import hashlib,json,struct,subprocess,sys
p=Path(sys.argv[1]);data=p.read_bytes()
assert struct.unpack_from('<I',data)[0]==0xfeedfacf
at=32;count=struct.unpack_from('<I',data,16)[0];segments=[];symtab=None
for _ in range(count):
    cmd,n=struct.unpack_from('<II',data,at);assert n>=8
    if cmd==0x19:
        name,va,extent,off,size=struct.unpack_from('<16s4Q',data,at+8)
        segments.append((name.rstrip(b'\0').decode(),va,extent,off,size))
    elif cmd==2:symtab=struct.unpack_from('<4I',data,at+8)
    at+=n
assert symtab
so,n,st,sz=symtab;symbols={};names={}
for i in range(n):
    ix,kind,section,desc,addr=struct.unpack_from('<IBBHQ',data,so+i*16)
    if kind&0xe0 or not kind&0xe:continue
    end=data.index(b'\0',st+ix,st+sz);name=data[st+ix:end].decode()
    symbols[name]=addr;names.setdefault(addr,[]).append(name)
def mapped(va,size):
    matches=[off+va-base for name,base,extent,off,filesz in segments if base<=va and va+size<=base+min(extent,filesz)]
    assert len(matches)==1,(hex(va),size)
    return data[matches[0]:matches[0]+size]
vptr=symbols['__ZTV9IOService']+16
words=struct.unpack('<512Q',mapped(vptr,4096));methods=next(i for i in range(16,512) if not words[i])
assert methods==168,('IOService ABI mismatch',methods)
assert '__ZN8OSObject4initEv' in names.get(words[13],[]),names.get(words[13])
assert '__ZN9IOService4freeEv' in names.get(words[14],[]),names.get(words[14])
for value in words[:methods]:assert not any('_RESERVED' in name for name in names.get(value,[]))
markers={}
for name in ('_kalloc_sparse_prelink_index_version','_native_prelink_security_init_version'):
    markers[name]=struct.unpack('<I',mapped(symbols[name],4))[0];assert markers[name]==1
nm=subprocess.check_output(['xcrun','nm','-u',str(p)],text=True)
assert not nm.strip(),'Unresolved final kernel symbols'
report=dict(schema='k70-ios16-rebuilt-kernel-abi/1',kernel_sha256=hashlib.sha256(data).hexdigest(),
    kernel_bytes=len(data),native_io_service_virtual_entries=methods,init_slot=13,free_slot=14,
    no_reserved_slots=True,prelink_abi_versions=markers,all_cpp_clean_rebuild_required=True,
    full_kernel_linked=True,hardware_verified=False)
(p.parent/'K70_IOS16_ABI.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
