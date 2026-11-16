"""v11.21-Present: relink the K70 kernel with the v11.21 present() fast path.

Same retained inputs and link recipe as v11.12 (runs 43076755379 /
43091498480). Adds the ios-sweet 0065+0078 SMP/PSCI units so the
single-CPU panic and KERN_NOT_SUPPORTED startCPU disappear:/n  iokit/Kernel/arm/SweetCPU.cpp        PSCI CPU_ON + secondary handshake
  osfmk/arm64/cpu.c                    PoC cache clean before release
  osfmk/arm64/machine_routines.c       bring-up observer
  osfmk/arm64/start.s                  sweet_secondary_entry trampoline
  pexpert/pexpert/arm64/SWEET.h        SWEET_SMP_MAX_CPUS
v11.13 (KCKindUnknown guard) and v11.17 (IOCoreSurfaceRoot) are fixed in
source here instead of by binary patch, so offsets may move freely.
v11.21 compiles SweetFramebuffer.cpp from this tree instead of linking the
cached retained object, so the panel expansion is the periodic 25:32 template
with per-row content skipping and present() timing."""
from pathlib import Path
import hashlib,json,shlex,struct,subprocess,sys

W=Path(__file__).resolve().parent
X=Path('dxb').resolve();B=X/'build/xnu.obj/DEVELOPMENT_ARM64_SWEET'
OUT=Path('v11-21-result').resolve();OUT.mkdir(exist_ok=False)
R=next(Path('gpu-driver-build').rglob('full-compile.json')).resolve()
old=json.loads(R.read_text());O=R.parent/'full-objects'
C=json.loads((W/'link-config.json').read_text())
recipe=json.loads((W/'link-recipe.json').read_text())
aes=json.loads((W/'ios16-aes-parent-audit.json').read_text())

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(R)==C['compile_report_sha256']
assert old['passing_units']==old['total_units']==59
for name,row in old['object_files'].items():
    assert (O/name).stat().st_size==row['bytes'] and sha(O/name)==row['sha256'],name
# Verify all cached native link inputs before replacing selected objects.
native=json.loads((W/'native-link-inputs.json').read_text())
for row in native:
    remote=row['path'].split('/dxb/',1)[1]
    p=X/remote
    assert p.stat().st_size==row['bytes'] and sha(p)==row['sha256'],remote

flags=json.loads((W/'compile-flags.json').read_text())
checks=[]
def compile(source,output,cpp=True,native_relative=None):
    selected=flags['native'][native_relative] if native_relative in flags.get('native',{}) else flags['cpp' if cpp else 'c']
    args=[s.replace('@XNU_BUILD@',str(X)).replace('@SDK_PLATFORM@',str(X/'sdk-kernel-platform')) for s in selected]
    # Source replacement headers take precedence over the archived tree.
    source_root=W/'replacements/xnu'
    inc=[W/'replacements/drivers/K70',source_root/'libkern',source_root/'iokit',source_root/'osfmk',source_root/'pexpert',
         *[X/'xnu'/p for p in ('libkern','iokit','osfmk','bsd','pexpert')],W/'SoftwareAES']
    # v11.31: QcomXnuUsbDebug (polled DWC3 CDC-ACM console). The module is
    # freestanding except for three XNU services: ml_io_map, PE_parse_boot_argn
    # and the processor/thread primitives used by the consumer worker.
    qud=W/'replacements/qud'
    inc += [qud/'include',qud/'soc',qud/'usb',qud/'kernel']
    inc += [O.parent.parent.parent/'source'/p for p in ()]
    # Only the required retained driver headers are fetched from the pinned
    # source commit; the source archive is prepared separately by the workflow.
    driver=Path('retained-driver-source').resolve()/'gpu-full'
    inc += [driver/p for p in ('Adreno740','AdrenoMetal','ArmSmmu4K','AdrenoFirmware','AdrenoPM4','Sm8550Platform','K70','K70Display','generated/sheng-gmu','generated/sheng-gpu-smmu')]
    # osfmk/arm64 units need the kernel-private define the real XNU makefile
    # passes: without it <kern/thread.h>, <kern/processor.h>, flush_dcache() and
    # the getCpuDatap() macro stay hidden and cpu.c/machine_routines.c fail.
    # With that define <osfmk/string.h> asks for a bare <types.h>, so a shim
    # directory on the include path forwards it to the exported sys/types.h.
    extra=[]
    if source.name in ('cpu.c', 'machine_routines.c', 'start.s',
                       'K70UsbConsole.c', 'K70MemReport.c',
                       'QcomXnuUsbDebug.c', 'QcomUsbScreenshot.c'):
        extra=['-DMACH_KERNEL_PRIVATE=1','-I',str(W/'kernel-private-shim')]
    if source.name in ('K70UsbConsole.c', 'K70MemReport.c') or 'replacements/qud' in str(source):
        # <kern/thread.h> (directly, or through <IOKit/IOLib.h> for the
        # allocator) pulls osfmk/arm/simple_lock.h, which includes the
        # generated <mach_ldebug.h>. That header only exists in the osfmk
        # build directory; the real XNU makefile gets it from the unit's cwd.
        # Point at it explicitly instead of moving cwd there, because the
        # osfmk cwd order pulls bsd/sys/types.h in ahead of the kernel types
        # and every typedef then conflicts.
        extra += ['-I', str(B/'osfmk'/'DEVELOPMENT')]
    if 'replacements/qud' in str(source) or source.name == 'K70UsbConsole.c':
        # The SoC profile is selected by macro, exactly as the module's own
        # build.py does with --soc; Sm8550.h holds the kalama GCC/DWC3/QSMMU
        # addresses, which the vermeer device tree confirms verbatim
        # (ssusb@a600000 reg 0xa600000/0x100000, apps-smmu@15000000,
        #  dwc3 iommus stream 0x40, iommu-dma-addr-pool 0x90000000).
        extra += ['-DQUD_SOC_HEADER="Sm8550.h"']
    args=[*extra,*[v for p in inc for v in ('-I',str(p))],*args,'-Wno-error','-c',str(source),'-o',str(output)]
    directory='iokit' if native_relative is None else native_relative.split('/')[0]
    cwd=B/directory/'DEVELOPMENT'
    if not cwd.is_dir():
        cwd=B
    result=subprocess.run(['xcrun','clang++' if cpp else 'clang',*args],cwd=cwd,capture_output=True,text=True)
    (OUT/(source.stem+'.log')).write_text(result.stdout+result.stderr)
    print('Compile',source.name,result.returncode,flush=True)
    if result.returncode:print(result.stderr[-8000:]);raise SystemExit(result.returncode)
    checks.append(dict(source=str(source.relative_to(W)),source_sha256=sha(source),object=str(output),object_sha256=sha(output),bytes=output.stat().st_size,exit_code=0))

# DO NOT add osfmk/arm64/sleh.c to this table.
#
# v11.25 did, to get a fault-tolerant MMIO probe (record FAR/ESR and resume at
# the next instruction instead of panicking).  That single-unit rebuild changed
# the preprocessor environment of the whole translation unit: sleh.c gates its
# interrupt entry on `#if USE_APPLEARMSMP`, and that macro only exists when a
# board config (ARM64_BOARD_CONFIG_Txxxx) is on the command line.  The XNU
# makefile passes one; this relink does not.  The result was a *different*
# sleh_irq:
#
#   shipped object : mrs x24, icc_iar1_el1 ... (Acknowledge, dispatch, EOI)
#   v11.25 object  : ldr x8, [cdp, #0xd0]; blr x8   <- cdp->interrupt_handler
#
# In an AppleSMP kernel nothing ever fills in cpu_data.interrupt_handler, so the
# first interrupt -- which arrives long before any of the GICR work -- branched
# to address 0 and the machine died with
#   Kernel instruction fetch abort at pc 0x0, esr 0x86000005 (_sleh_irq+0x40).
# Passing -DARM64_BOARD_CONFIG_* instead would drag MAX_CPUS/PMAP_CS/XNU_MONITOR
# and friends into one unit that disagrees with the other 500, which is worse.
# The GICR layout is therefore obtained from the device tree, not by probing.

units={
 'iokit/Kernel/arm/SweetSurface.cpp':('iokit/DEVELOPMENT/SweetSurface.cpo',True),
 'iokit/KernelConfigTables.cpp':('iokit/DEVELOPMENT/KernelConfigTables.cpo',True),
 'libkern/c++/OSKext.cpp':('libkern/DEVELOPMENT/OSKext.cpo',True),
 'osfmk/prng/prng_random.c':('osfmk/DEVELOPMENT/prng_random.o',False),
 'osfmk/arm/pmap/pmap.c':('osfmk/DEVELOPMENT/pmap.o',False),
 # v11.20 SMP / PSCI bring-up
 'iokit/Kernel/arm/SweetCPU.cpp':('iokit/DEVELOPMENT/SweetCPU.cpo',True),
 'osfmk/arm64/cpu.c':('osfmk/DEVELOPMENT/cpu.o',False),
 'osfmk/arm64/machine_routines.c':('osfmk/DEVELOPMENT/machine_routines.o',False),
 'osfmk/arm64/start.s':('osfmk/DEVELOPMENT/start.o',False),
 # v11.21 present() fast path replaces the cached retained driver object
 'iokit/Kernel/arm/SweetFramebuffer.cpp':('iokit/DEVELOPMENT/SweetFramebuffer.cpo',True),
 # v11.31 per-CPU GIC redistributor selection (no speculative GICR_TYPER walk)
 'pexpert/arm/pe_fiq.c':('pexpert/DEVELOPMENT/pe_fiq.o',False),
 # osfmk/arm64/sleh.c is deliberately NOT rebuilt.  See the note below.
}

def object_path(output):
    """Locate an object in the retained build tree; new units may live elsewhere."""
    if (B/output).is_file():
        return B/output
    hits=sorted(p for p in B.rglob(Path(output).name) if p.is_file())
    assert len(hits)==1,(output,[str(p) for p in hits])
    return hits[0]

for relative,(output,cpp) in units.items():
    target=object_path(output)
    compile(W/'replacements/xnu'/relative,target,cpp,relative)
for name in ('K70GpuWorker','K70GpuBoot','K70GpuStartup','K70StartupService',
             'K70MetaBind','K70FrameHandoff'):
    compile(W/f'replacements/drivers/K70/{name}.cpp',O/(name+'.o'))
# Rebuild the exact retained iOS16 publishing unit with balanced CPU-map
# teardown. Cached objects are verified above before this replacement.
compile(W/'replacements/drivers/Adreno740/NativeAdrenoPublish.cpp',O/'NativeAdrenoPublish.o')
# v11.31 USB CDC-ACM console. Ownership split is the module's: the freestanding
# DWC3 core knows nothing about Qualcomm or XNU, the SoC layer owns GCC/DWC3/QSMMU
# qualification plus the SMMUv2 private S1 lease, and the kernel layer is the only
# place that touches ml_io_map or the boot args. K70UsbConsole.c is the consumer
# worker (boot-CPU bound, 5 ms poll) and the panel fallback line.
qud_objects=[]
for name in ('usb/Dwc3.c','soc/QcomUsbQualify.c','soc/QcomUsbDmaDomain.c',
             'kernel/QcomXnuUsbDebug.c','kernel/QcomUsbScreenshot.c'):
    target=OUT/('qud-'+Path(name).stem+'.o')
    compile(W/'replacements/qud'/name,target,False)
    qud_objects.append(target)
compile(W/'replacements/drivers/K70/K70UsbConsole.c',OUT/'K70UsbConsole.o',False)
qud_objects.append(OUT/'K70UsbConsole.o')
# v11.31: page accounting to the panel and the serial port.
compile(W/'replacements/drivers/K70/K70MemReport.c',OUT/'K70MemReport.o',False)
qud_objects.append(OUT/'K70MemReport.o')
compile(W/'SoftwareAES/NativeAES.cpp',OUT/'NativeAES.o')

llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin'
subprocess.run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeAES.cpp',OUT/'NativeAES.o',OUT/'NativeAES.late.o'],check=True)
gpu=OUT/'libAdreno740-iOS16.a'
exclude={'NativeAdrenoAccelerator.o','SweetFramebuffer.o','K70PageCheck.o',
         'WifiNet.o','K70WifiDma.o','K70WifiStartup.o','K70WifiNativePower.o','K70WifiNativeDma.o','K70WifiNativeIts.o',
         'K70MetaBind.o'}
subprocess.run(['xcrun','ar','rcs',gpu,*[p for p in sorted(O.glob('*.o')) if p.name not in exclude]],check=True)

obj=(OUT/'NativeAES.o').read_bytes()
# Parse ordinary MH_OBJECT symbols and unsigned 64-bit vtable relocations.
magic,cpu,subtype,kind,n,size=struct.unpack_from('<6I',obj)
assert (magic,cpu,subtype,kind)==(0xfeedfacf,0x100000c,0,1)
at=32;sections=[]
for _ in range(n):
    cmd,length=struct.unpack_from('<2I',obj,at)
    if cmd==2:so,count,st,ss=struct.unpack_from('<4I',obj,at+8)
    if cmd==25:
        ns=struct.unpack_from('<I',obj,at+64)[0]
        for i in range(ns):
            q=at+72+i*80
            addr,extent,off,align,ro,rn=struct.unpack_from('<2Q4I',obj,q+32)
            sections.append((addr,extent,ro,rn))
    at+=length
symbols=[]
for i in range(count):
    ix,ty,sect,desc,va=struct.unpack_from('<IBBHQ',obj,so+i*16)
    name=obj[st+ix:obj.index(b'\0',st+ix)].decode()
    symbols.append((name,va,ty))
vtable=next(v for name,v,ty in symbols if name=='__ZTV9NativeAES')
relocations={}
for addr,extent,ro,rn in sections:
    if addr<=vtable<addr+extent:
        for i in range(rn):
            pos,word=struct.unpack_from('<2I',obj,ro+i*8)
            if word>>27&1 and word>>28==0 and word>>25&3==3:
                relocations[addr+pos]=symbols[word&0xffffff][0]
own={0,1,7,86,111,117,168,169,170,171,172}
bindings={}
for row in aes['methods']:
    i=row['slot'];name=relocations[vtable+16+i*8]
    if i in own:assert '9NativeAES' in name,(i,name)
    elif row['kernel_names']:assert name in row['kernel_names'],(i,name,row['kernel_names'])
    else:
        assert name.startswith('__ZN16IOAESAccelerator'),(i,name)
        value=int(row['address'],16)
        assert name not in bindings or bindings[name]==value
        bindings[name]=value
assert vtable+16+181*8 not in relocations
bindings.update({
 '__ZN16IOAESAcceleratorC2EPK11OSMetaClass':int(aes['constructors'][0],16),
 '__ZN16IOAESAcceleratorD2Ev':int(aes['destructor_d2'],16),
 '__ZN16IOAESAccelerator10gMetaClassE':int(aes['metaclass'],16)})
(OUT/'aes-bindings.json').write_text(json.dumps(dict(bindings={k:hex(v) for k,v in bindings.items()},compiled_vtable_slots=181,original_ios16_sha256=aes['original_sha256'],hardware_verified=False),indent=2)+'\n')

# The retained SweetFramebuffer object is replaced in place: the unit above
# compiles this tree's source to the exact path the filelist already names, so
# link order and every other retained input stay untouched.
filelist=(B/'link.filelist').read_text().splitlines()
removed=[p for p in filelist if Path(p).name=='SweetFramebuffer.cpo'];assert len(removed)==1
assert (B/'iokit/DEVELOPMENT/SweetFramebuffer.cpo').is_file()
fl=OUT/'link.filelist';fl.write_text('\n'.join(filelist)+'\n')
fw=OUT/'FirmwareInsertionSlots.S'
lines=['.section __TEXT,__const']
for row in C['firmware'].values():
    symbol='_'+row['symbol'];lines+=['.p2align 14','.globl '+symbol,symbol+':','.space '+str(row['bytes']),'.p2align 3','.globl '+symbol+'_size',symbol+'_size:','.quad '+str(row['bytes'])]
fw.write_text('\n'.join(lines)+'\n')
subprocess.run(['xcrun','clang','-target','arm64-apple-macos14','-c',fw,'-o',fw.with_suffix('.o')],check=True)
def end(path):
    raw=path.read_bytes();at=32;result=0
    for _ in range(struct.unpack_from('<I',raw,16)[0]):
        cmd,length=struct.unpack_from('<2I',raw,at)
        if cmd==25:
            va,n=struct.unpack_from('<2Q',raw,at+24)
            if n:result=max(result,va+n)
        at+=length
    return result
slide=C['initial_slide'];passes=[]
final=OUT/'kernel.firmware-slots'
for attempt in range(4):
    asm=OUT/'OriginalParentBindings.S'
    combined={**C['parent_symbols'],**bindings}
    asm.write_text(''.join('.globl '+name+'\n.set '+name+', '+hex(value+slide)+'\n' for name,value in sorted(combined.items())))
    subprocess.run(['xcrun','clang','-target','arm64-apple-macos14','-c',asm,'-o',asm.with_suffix('.o')],check=True)
    unstripped=OUT/('kernel.pass'+str(attempt)+'.unstripped')
    args=[s.replace('@XNU_BUILD@',str(X)).replace('@LINK_OUTPUT@',str(unstripped)) for s in recipe]
    args[args.index('-filelist')+1]=str(fl)
    # This candidate requests AES and GPU only. The newer Wi-Fi archive has
    # an iOS userspace platform object and requires an absent ITS kernel hook;
    # it was never linked into the successful retained GPU kernel. Do not
    # introduce that unfinished backend into a Wi-Fi-disabled RAM profile.
    args+=['-Wl,-force_load,'+str(gpu),str(fw.with_suffix('.o')),str(asm.with_suffix('.o')),str(OUT/'NativeAES.late.o')]
    args+=[str(p) for p in qud_objects]
    # v11.31: runtime OSMetaClass superClassLink binding (GPU + AES).
    args+=[str(O/'K70MetaBind.o')]
    result=subprocess.run(['xcrun','clang++',*args],cwd=B,capture_output=True,text=True)
    (OUT/('link-pass'+str(attempt)+'.log')).write_text(result.stdout+result.stderr)
    print('Link pass',attempt,result.returncode,flush=True)
    if result.returncode:print(result.stderr[-8000:]);raise SystemExit(result.returncode)
    subprocess.run(['xcrun','strip','-S','-o',final,unstripped],check=True)
    next_slide=((end(final)+0x100000+0x3fff)&~0x3fff)-C['original_prelink_base']
    passes.append(dict(slide=slide,next_slide=next_slide,kernel_sha256=sha(final)))
    if slide==next_slide:break
    slide=next_slide
else:raise SystemExit('Parent slide did not converge')
names=subprocess.check_output(['xcrun','nm','-g','-m',final],text=True)
(OUT/'kernel-symbols.txt').write_text(names)
assert '_sweet_amfi_policy_register' not in names
assert '_sweet_prng_startup' not in names
assert '_k70_aes_start' in names and '_k70_gpu_boot' in names
# v11.20: the SMP path must actually exist in the linked image.  The PSCI
# wrapper is `static` inside SweetCPU.cpp, so it only shows in the local table.
local=subprocess.check_output(['xcrun','nm','-m',final],text=True)
assert '_sweet_secondary_entry' in names, 'missing SMP trampoline'
assert '_sweet_cpu_signal_is_enabled' in names, 'missing SMP bring-up observer'
# The PSCI wrapper is a small `static` helper at -O2 and is inlined into
# startAll(), so it never materialises as a symbol.  Assert the SMP bring-up
# code and its log strings are really in the linked image instead.
blob = final.read_bytes()
assert '_sweet_secondary_entry' in local, 'missing SMP trampoline in the image'
for text in (b'PSCI CPU_ON rc=%d', b'came alive; SGI1 OK', b'CPUs initialized, SGI1 verified'):
    assert text in blob, 'missing SMP log string ' + text.decode()
assert b'only single-CPU bring-up' not in blob, 'single-CPU panic string survived'
# v11.21: the fast path must be the linked one, not the cached retained object.
assert b'present fast path' in blob, 'missing present fast path log'
assert b'row cache %s, stats %s' in blob, 'missing row cache log'
assert b'K70 FRAME: GPU completed %llu sequence %llu layers %u' in blob
assert b'K70 GPU: CPU view teardown token %u mappings %u status 0x0 total %u' in blob
assert b'K70 AES: selftest kat=%u interface=%u completed=%u' in blob
assert b'K70 AES: 8 init failed' in blob
assert b'VERMEER FB: logical canvas 1125x2436 scaled to full panel' in blob
# v11.31: the USB console must be a real link, not a declared unit. The module's
# own banner, our worker's entry point and the exported API all have to be there.
assert '_k70_usb_console_start' in names, 'missing USB console entry point'
assert '_qcom_usb_debug_init' in names and '_qcom_usb_debug_poll' in names, 'missing QUD backend'
assert '_dwc3_init' in names and '_QcomUsbDomainInstall' in names, 'missing DWC3/SMMU units'
assert '_vermeer_debug_copy' in names, 'missing retained console ring API'
assert b'K70 FRAME: GPU presenter prepared console_muted %u' in blob
assert b'K70 AES: selftest raw encrypt status 0x%x bytes_equal %u' in blob
for text in (b'QCOM XNU USB CDC ACM v1', b'K70 USB: controller takeover starting',
             b'K70 USB console attached'):
    assert text in blob, 'missing USB console string ' + text.decode()
# v11.31: the console has to be started from the entry SweetFramebuffer::start()
# actually calls, and the AES bring-up has to be traceable step by step.
for text in (b'K70 USB: console start returned %d',
             b'K70 USB: MMIO mapped before owner guard',
             b'K70 USB: attempt %u %s phase=%u error=%u qcom=%u domain=%u',
             b'K70 AES: 1 kext load status 0x%x',
             b'K70 AES: 11 started registered=%u kat=%u interface=%u',
             b'K70 MEM: free=%uMB target=%uMB wire=%uMB active=%uMB inactive=%uMB spec=%uMB'):
    assert text in blob, 'missing v11.31 string ' + text.decode()
assert '_k70_mem_report_start' in names, 'missing memory reporter entry point'
report=dict(schema='k70-v11-21-present-apple-source-relink/1',compiler=subprocess.check_output(['xcrun','clang','--version'],text=True).splitlines()[0],
    kernel_sha256=sha(final),kernel_bytes=final.stat().st_size,checks=checks,passes=passes,prelink_slide=slide,
    firmware=C['firmware'],cached_kernel_run=43076755379,retained_driver_run=43091498480,
    changed_native_units=list(units),original_ios16_aes_vtable_slots_verified=181,
    hardware_verified=False,firmware_inserted=False,original_prelink_emitted=False,
    wifi_boot_requested=False,new_wifi_backend_linked=False)
(OUT/'source-relink.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: present fast path + SMP units compiled and relinked; hardware pending')
