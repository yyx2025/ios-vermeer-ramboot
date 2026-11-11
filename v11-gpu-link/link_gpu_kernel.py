"""Apple two-pass kernel link with local firmware insertion slots.

The resulting candidate deliberately contains zeros in the three firmware
images. It is not a boot image and is never marked runnable.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess

P=argparse.ArgumentParser()
P.add_argument('--xnu-build',type=Path,required=True)
P.add_argument('--drivers',type=Path,required=True)
P.add_argument('--output',type=Path,required=True)
A=P.parse_args()
W=Path(__file__).resolve().parent
X=A.xnu_build.resolve()
OUT=A.output.resolve()
OUT.mkdir(exist_ok=False)
C=json.loads((W/'link-config.json').read_text())
recipe=json.loads((W/'link-recipe.json').read_text())
report_path=next(A.drivers.resolve().rglob('full-compile.json'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(report_path)==C['compile_report_sha256']
compiled=json.loads(report_path.read_text())
assert compiled['passing_units']==compiled['total_units']==56
assert compiled['global_abi_flags']==['-DK70_IOS16_IOKIT_ABI=1','-DK70_NATIVE_RUNTIME_TRUST=1']
O=report_path.parent/'full-objects'
for name,row in compiled['object_files'].items():
    p=O/name
    assert p.stat().st_size==row['bytes'] and sha(p)==row['sha256'],name
gpu=O/'libAdreno740-iOS16.a'
assert sha(gpu)==compiled['gpu_archive_sha256']
B=X/'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
original_list=(B/'link.filelist').read_text().splitlines()
removed=[p for p in original_list if Path(p).name=='SweetFramebuffer.cpo']
assert len(removed)==1
filelist=[p for p in original_list if p not in removed]
assert filelist and all(Path(p).is_file() for p in filelist)
filelist.append(str(O/'SweetFramebuffer.o'))
fl=OUT/'gpu.link.filelist'
fl.write_text('\n'.join(filelist)+'\n')
firmware=['.section __TEXT,__const']
for role,row in C['firmware'].items():
    symbol='_'+row['symbol']
    firmware+=['.p2align 14','.globl '+symbol,symbol+':','.space '+str(row['bytes']),
               '.p2align 3','.globl '+symbol+'_size',symbol+'_size:', '.quad '+str(row['bytes'])]
fw=OUT/'FirmwareInsertionSlots.S'
fw.write_text('\n'.join(firmware)+'\n')
subprocess.run(['xcrun','clang','-target','arm64-apple-ios16','-c',str(fw),'-o',str(fw.with_suffix('.o'))],check=True)

def runtime_end(path):
    raw=path.read_bytes()
    magic,cpu,subtype,kind,n,sizeof=struct.unpack_from('<6I',raw)
    assert (magic,cpu,subtype,kind)==(0xfeedfacf,0x100000c,0,2)
    at=32;end=0
    for _ in range(n):
        command,length=struct.unpack_from('<2I',raw,at)
        assert length>=8 and at+length<=32+sizeof
        if command==25:
            va,size=struct.unpack_from('<2Q',raw,at+24)
            if size:end=max(end,va+size)
        at+=length
    assert at==32+sizeof
    return end

slide=C['initial_slide']
passes=[]
final=OUT/'kernel.gpu-firmware-slots'
for attempt in range(3):
    bindings=OUT/'OriginalParentBindings.S'
    bindings.write_text(''.join('.globl '+name+'\n.set '+name+', '+hex(value+slide)+'\n'
        for name,value in sorted(C['parent_symbols'].items())))
    subprocess.run(['xcrun','clang','-target','arm64-apple-ios16','-c',str(bindings),
        '-o',str(bindings.with_suffix('.o'))],check=True)
    unstripped=OUT/('kernel.pass'+str(attempt)+'.unstripped')
    args=[t.replace('@XNU_BUILD@',str(X)).replace('@LINK_OUTPUT@',str(unstripped)) for t in recipe]
    assert args.count('-filelist')==1
    args[args.index('-filelist')+1]=str(fl)
    args+=['-Wl,-force_load,'+str(gpu),str(fw.with_suffix('.o')),str(bindings.with_suffix('.o'))]
    r=subprocess.run(['xcrun','clang++',*args],cwd=B,capture_output=True,text=True)
    (OUT/('link-pass'+str(attempt)+'.log')).write_text(r.stdout+r.stderr)
    print('GPU kernel link pass',attempt,'status',r.returncode,flush=True)
    if r.returncode:
        print(r.stderr[-6000:])
        raise SystemExit(r.returncode)
    subprocess.run(['xcrun','strip','-S','-o',str(final),str(unstripped)],check=True)
    next_slide=((runtime_end(final)+0x100000+0x3fff)&~0x3fff)-C['original_prelink_base']
    passes.append(dict(pass_index=attempt,slide=slide,next_slide=next_slide,kernel_sha256=sha(final)))
    if next_slide==slide:break
    slide=next_slide
else:raise SystemExit('Parent mapping did not stabilize; refusing output qualification')
symbols=subprocess.check_output(['xcrun','nm','-g','-m',str(final)],text=True)
(OUT/'kernel-symbols.txt').write_text(symbols)
required=['_k70_gpu_boot','_k70_gpu_execution','_k70_gpu_start_worker',
          '_k70_frame_register','_k70_frame_offer',*C['parent_symbols']]
strong={line.rsplit(None,1)[-1] for line in symbols.splitlines()
        if 'external ' in line and '(undefined)' not in line and 'weak ' not in line}
assert set(required)<=strong,set(required)-strong
assert '_sweet_amfi_policy_register' not in strong,'Legacy AMFI stand-in remains in the native profile'
inputs=[report_path,gpu,O/'SweetFramebuffer.o',fw.with_suffix('.o'),bindings.with_suffix('.o'),
        *[W/name for name in ('link_gpu_kernel.py','link-recipe.json','link-config.json')]]
native_inputs=[Path(p) for p in filelist[:-1]] + [B/name for name in
    ('version.o','lastkerneldataconst.o','lastkernelconstructor.o','nonlto.o','all-alias.exp','symbolsets.plist')]
native_inputs+=sorted((X/'fakeroot/usr/local/lib/kernel').glob('lib*.a'))
index=OUT/'native-link-inputs.json'
index.write_text(json.dumps([dict(path=str(p),bytes=p.stat().st_size,sha256=sha(p))
                            for p in native_inputs],indent=2)+'\n')
inputs.append(index)
result=dict(schema='k70-v11-gpu-kernel-link/1',kernel_sha256=sha(final),kernel_bytes=final.stat().st_size,
    driver_run=C['driver_run'],driver_commit=C['driver_commit'],passing_driver_units=56,
    source_inputs=[dict(path=str(p),bytes=p.stat().st_size,sha256=sha(p)) for p in inputs],
    removed_framebuffer_object=removed[0],replaced_framebuffer=True,
    gpu_worker_strong_definition=True,required_strong_symbols=required,
    passes=passes,prelink_slide=slide,firmware=C['firmware'],
    firmware_uploaded=False,firmware_inserted=False,original_prelink_emitted=False,
    wifi_integrated=False,boot_qualified=False,hardware_verified=False)
(OUT/'gpu-kernel-link.json').write_text(json.dumps(result,indent=2)+'\n')
print('Linked actual GPU and display; firmware insertion, original prelink and ABI qualification remain')
