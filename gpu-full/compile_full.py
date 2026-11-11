"""Compile all GPU units and the Wi-Fi network interface with actual XNU8796 headers."""
from pathlib import Path
import argparse,concurrent.futures,hashlib,json,os,shlex,shutil,subprocess

P=argparse.ArgumentParser();P.add_argument('--xnu-root',type=Path,required=True)
A=P.parse_args();W=Path(__file__).resolve().parent;X=A.xnu_root.resolve()
OUT=W/'full-objects';OUT.mkdir(exist_ok=True)
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
includes=[W/p for p in ('Adreno740','ArmSmmu4K','AdrenoFirmware','AdrenoPM4','Sm8550Platform','Ath12kWifi/include','K70')]+[X/'xnu/osfmk']
page=OUT/'K70PageCheck.cpp';page.write_text('#include <mach/vm_param.h>\nstatic_assert(PAGE_SIZE==16384,"K70 CPU geometry");\n')
units=list(sorted((W/'Adreno740').glob('*.cpp')))+[W/'K70/WifiNet.cpp',W/'K70/K70WifiDma.c',page]
units+=list(sorted((W/'ArmSmmu4K').glob('ArmSmmu4K*.c')))
units+=[W/'AdrenoFirmware'/name for name in ('AdrenoHfi.c','AdrenoHfiStartup.c','AdrenoHfiVotes.c','AdrenoFirmware.c')]
units+=[W/'AdrenoPM4/AdrenoPM4.c']
assert len({p.stem for p in units})==len(units)
def compile_one(source):
    compiler=shutil.which('clang++' if source.suffix=='.cpp' else 'clang')
    command=[compiler,'--target=arm64-apple-macos14',*(cpp if source.suffix=='.cpp' else c),
        *[a for path in includes for a in ('-I',str(path))],'-Wno-error','-c',str(source),'-o',str(OUT/(source.stem+'.o'))]
    r=subprocess.run(command,cwd=X/'build/xnu.obj/DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT',capture_output=True,text=True)
    (OUT/(source.stem+'.log')).write_text(r.stdout+r.stderr)
    return {'source':source.relative_to(W).as_posix(),'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
        'exit_code':r.returncode,'errors':[s for s in r.stderr.splitlines() if 'error:' in s]}
with concurrent.futures.ThreadPoolExecutor(4) as pool:rows=list(pool.map(compile_one,units))
report={'profile':'iOS16/XNU8796 DEVELOPMENT_ARM64_SWEET, 16KiB CPU pages','checks':rows,
    'passing_units':sum(r['exit_code']==0 for r in rows),'total_units':len(rows),
    'linked_to_boot_kernel':False,'hardware_verified':False}
(W/'full-compile.json').write_text(json.dumps(report,indent=2)+'\n')
for row in rows:
    if row['exit_code']:print(row['source'],*row['errors'][:4],sep='\n',flush=True)
print('Actual Apple kernel compilation:',report['passing_units'],'/',len(rows),flush=True)
if any(row['exit_code'] for row in rows):raise SystemExit(1)
archive=OUT/'libAdreno740-iOS16.a'
gpu=[OUT/(s.stem+'.o') for s in units if s not in (W/'K70/WifiNet.cpp',W/'K70/K70WifiDma.c',page)]
subprocess.run(['xcrun','ar','rcs',str(archive),*[str(p) for p in gpu]],check=True)
report['gpu_archive_sha256']=hashlib.sha256(archive.read_bytes()).hexdigest()
report['gpu_archive_bytes']=archive.stat().st_size
(W/'full-compile.json').write_text(json.dumps(report,indent=2)+'\n')
