"""Compile all GPU units and the Wi-Fi network interface with actual XNU8796 headers."""
from pathlib import Path
import argparse,concurrent.futures,hashlib,json,os,shlex,shutil,subprocess

P=argparse.ArgumentParser();P.add_argument('--xnu-root',type=Path,required=True)
P.add_argument('--ios16-iokit-abi',action='store_true',help='Require the clean iOS16 ABI tree and apply its global flags to every unit')
A=P.parse_args();W=Path(__file__).resolve().parent;X=A.xnu_root.resolve()
OUT=W/'full-objects';OUT.mkdir(exist_ok=True)
def input_identity():
    # Include headers and policy inputs, not only the translation units.
    return {p.relative_to(W).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(W.rglob('*')) if p.is_file()
            and not any(part in ('full-objects','metal-objects','__pycache__') for part in p.relative_to(W).parts)
            and (p.suffix in ('.h','.c','.cpp','.mm','.metal','.py')
                 or p == W/'compile-recipes.log')}
inputs=input_identity()
lines=(W/'compile-recipes.log').read_text().splitlines()
def recipe(suffix):
    line=next(r.split('Z ',1)[1] for r in lines if r.endswith(suffix))
    tokens=shlex.split(line)[1:];result=[];i=0
    remote='/Users/runner/work/ios-vermeer-ramboot/ios-vermeer-ramboot/dxb'
    platform='/Applications/Xcode_15.0.1.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX14.0.sdk//System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform'
    while i<len(tokens):
        t=tokens[i]
        if t in ('-o','-MF','-MJ'):i+=2;continue
        if t not in ('-MD','-MP','-c') and not t.endswith(suffix) and not t.startswith('-W'):
            result.append(t.replace(remote,str(X)).replace(platform,str(X/'sdk-kernel-platform')))
        i+=1
    return result
c=recipe('/bsd/dev/vermeer_usb_debug.c');cpp=recipe('/iokit/Kernel/arm/hid/SweetHID.cpp')
abi_flags=[]
if A.ios16_iokit_abi:
    header=(X/'xnu/libkern/libkern/c++/OSMetaClass.h').read_text()
    assert '(defined(K70_IOS16_IOKIT_ABI) && K70_IOS16_IOKIT_ABI)' in header, 'Unpatched IOKit ABI tree'
    assert 'OTHER_CFLAGS += -DK70_IOS16_IOKIT_ABI=1 -DK70_NATIVE_RUNTIME_TRUST=1' in (X/'xnu/makedefs/MakeInc.def').read_text(), 'The kernel was not globally rebuilt with the same ABI'
    abi_flags=['-DK70_IOS16_IOKIT_ABI=1','-DK70_NATIVE_RUNTIME_TRUST=1']
includes=[W/p for p in ('Adreno740','ArmSmmu4K','AdrenoFirmware','AdrenoPM4','Sm8550Platform','Ath12kWifi/include','K70','K70Display','generated/sheng-gmu','generated/sheng-gpu-smmu')]+[X/'xnu/osfmk']
required_headers=('ShengGmuSource.h','ShengGmuVotesSource.h','ShengGmuCommandDbSource.h','ShengGpuSmmuSource.h')
for name in required_headers:
    assert any((path/name).is_file() for path in includes), 'Missing actual startup policy: '+name
page=OUT/'K70PageCheck.cpp';page.write_text('#include <mach/vm_param.h>\n#include "AdrenoHfiVotes.h"\nstatic_assert(PAGE_SIZE==16384,"K70 CPU geometry");\n'+''.join('#include "'+name+'"\n' for name in required_headers))
wifi_units=[W/'K70/WifiNet.cpp',W/'K70/K70WifiDma.c',W/'K70/K70WifiStartup.cpp',
    W/'K70/K70WifiNativePower.cpp',W/'K70/K70WifiNativeDma.cpp',W/'K70/K70WifiNativeIts.cpp']
units=list(sorted((W/'Adreno740').glob('*.cpp')))+wifi_units+[page]
units+=list(sorted((W/'ArmSmmu4K').glob('ArmSmmu4K*.c')))
units+=[W/'AdrenoFirmware'/name for name in ('AdrenoHfi.c','AdrenoHfiStartup.c','AdrenoHfiVotes.c','AdrenoFirmware.c')]
units+=[W/'AdrenoPM4/AdrenoPM4.c']
units+=[W/'Sm8550Platform/ShengGmu.c',W/'Sm8550Platform/ShengGmuCommandDb.c',W/'K70/ShengGmuVotes.c',W/'K70/K70GpuStartup.cpp']
units+=[W/'K70/K70GpuCp.cpp',W/'K70/K70GpuZap.c']
units+=[W/'K70/K70GpuBoot.cpp']
if A.ios16_iokit_abi:units+=[W/'K70/K70GpuWorker.cpp']
units+=[W/'K70Display/K70FrameHandoff.cpp',W/'K70Display/SweetFramebuffer.cpp']
assert len({p.stem for p in units})==len(units)
def compile_one(source):
    compiler=shutil.which('clang++' if source.suffix=='.cpp' else 'clang')
    command=[compiler,'--target=arm64-apple-macos14',*(cpp if source.suffix=='.cpp' else c),
        *[a for path in includes for a in ('-I',str(path))],*abi_flags,'-Wno-error','-c',str(source),'-o',str(OUT/(source.stem+'.o'))]
    r=subprocess.run(command,cwd=X/'build/xnu.obj/DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT',capture_output=True,text=True)
    (OUT/(source.stem+'.log')).write_text(r.stdout+r.stderr)
    return {'source':source.relative_to(W).as_posix(),'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'exit_code':r.returncode,'errors':[s for s in r.stderr.splitlines() if 'error:' in s]}
with concurrent.futures.ThreadPoolExecutor(4) as pool:rows=list(pool.map(compile_one,units))
report={'profile':'iOS16/XNU8796 DEVELOPMENT_ARM64_SWEET, 16KiB CPU pages','checks':rows,
    'passing_units':sum(r['exit_code']==0 for r in rows),'total_units':len(rows),
    'actual_gmu_startup_compiled':True,'required_generated_policy':list(required_headers),
    'linked_to_boot_kernel':False,'hardware_verified':False,
    'ios16_iokit_abi_requested':A.ios16_iokit_abi,'global_abi_flags':abi_flags,
    'input_files':inputs,'input_identity_sha256':hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()}
(W/'full-compile.json').write_text(json.dumps(report,indent=2)+'\n')
for row in rows:
    if row['exit_code']:print(row['source'],*row['errors'][:4],sep='\n',flush=True)
print('Actual Apple kernel compilation:',report['passing_units'],'/',len(rows),flush=True)
if any(row['exit_code'] for row in rows):raise SystemExit(1)
assert input_identity()==inputs, 'Source or header changed during compilation'
archive=OUT/'libAdreno740-iOS16.a'
archive.unlink(missing_ok=True)
# Load the original parent before the derived accelerator's initializer.
llvm=Path(subprocess.check_output(['brew','--prefix','llvm'],text=True).strip())/'bin/llvm-objcopy'
late=OUT/'NativeAdrenoAccelerator.late.o'
subprocess.run([llvm,'--remove-section=__DATA,__mod_init_func',
    '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp',OUT/'NativeAdrenoAccelerator.o',late],check=True)
gpu=[OUT/(s.stem+'.o') for s in units if s not in (*wifi_units,W/'K70Display/SweetFramebuffer.cpp',W/'Adreno740/NativeAdrenoAccelerator.cpp',page)]+[late]
subprocess.run(['xcrun','ar','rcs',str(archive),*[str(p) for p in gpu]],check=True)
symbols=subprocess.check_output(['xcrun','nm','-g',str(archive)],text=True)
required_symbols=('ShengGmuColdStart','ShengGmuBuildStartup','ShengGmuCommandDbInputs',
    'NativeAdrenoGmuContextBringup','NativeAdrenoGpuContextBringup','k70_gpu_prepare_gmu','k70_gpu_start_execution','k70_gpu_execution','k70_gpu_boot')
for symbol in required_symbols:
    assert ' T _'+symbol in symbols,'Actual startup definition missing: '+symbol
report['required_startup_definitions']=list(required_symbols)
report['gpu_archive_sha256']=hashlib.sha256(archive.read_bytes()).hexdigest()
report['gpu_archive_bytes']=archive.stat().st_size
wifi_archive=OUT/'libK70WifiNative-iOS16.a'
wifi_archive.unlink(missing_ok=True)
subprocess.run(['xcrun','ar','rcs',str(wifi_archive),*[str(OUT/(s.stem+'.o')) for s in wifi_units]],check=True)
wifi_symbols=subprocess.check_output(['xcrun','nm','-g',str(wifi_archive)],text=True)
required_wifi_symbols=('k70_wifi_startup','k70_wifi_dispatch','sheng_wifi_wake',
    'k70_wifi_its_init','k70_wifi_its_handle_irq','k70_wifi_its_irq_hook_version')
for symbol in required_wifi_symbols:
    assert ' T _'+symbol in wifi_symbols,'Native Wi-Fi definition missing: '+symbol
report['required_wifi_definitions']=list(required_wifi_symbols)
report['wifi_archive_sha256']=hashlib.sha256(wifi_archive.read_bytes()).hexdigest()
report['wifi_archive_bytes']=wifi_archive.stat().st_size
report['object_files']={p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                       for p in sorted(OUT.glob('*.o'))}
(W/'full-compile.json').write_text(json.dumps(report,indent=2)+'\n')
