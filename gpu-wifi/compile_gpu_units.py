"""Compile ported units against retained, actual K70 XNU8796/16K headers."""
from pathlib import Path
import argparse,hashlib,json,os,shlex,shutil,subprocess
p=argparse.ArgumentParser();p.add_argument('--xnu-root',type=Path);p.add_argument('--reference',type=Path)
p.add_argument('--log',type=Path);p.add_argument('--compiler');p.add_argument('--syntax-only',action='store_true')
args=p.parse_args();w=Path(__file__).resolve().parent
project=Path(r'C:/Users/Administrator/Documents/Codex/2026-10-30/github-unixnu-vermeer-ipad')
xnu=args.xnu_root or project/'work/stream-local-xnu'
refs=args.reference or project/'work/sheng-gpu-reference'
log=args.log or project/'work/private-run-41186716430/build/6_Build.txt'
lines=log.read_text(encoding='utf-8').splitlines()
def recipe(suffix):
    row=next(row.split('Z ',1)[1] for row in lines if '/usr/bin/clang' in row and row.endswith(suffix))
    tokens=shlex.split(row)[1:];result=[];i=0
    remote='/Users/runner/work/ios-vermeer-ramboot/ios-vermeer-ramboot/dxb'
    platform='/Applications/Xcode_15.0.1.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX14.0.sdk//System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/platform'
    while i<len(tokens):
        tok=tokens[i]
        if tok in ('-o','-MF','-MJ'):i+=2;continue
        if tok not in ('-MD','-MP','-c') and not tok.endswith(suffix) and not tok.startswith('-W'):
            result.append(tok.replace(remote,str(xnu)).replace(platform,str(xnu/'sdk-kernel-platform')))
        i+=1
    return result
c=recipe('/bsd/dev/vermeer_usb_debug.c');cpp=recipe('/iokit/Kernel/arm/hid/SweetHID.cpp')
out=w/'kernel-objects';out.mkdir(exist_ok=True)
page=out/'K70PageCheck.cpp';page.write_text('#include <mach/vm_param.h>\nstatic_assert(PAGE_SIZE==16384,"K70 CPU pages");\nstatic_assert(atop_64(0x940000000ULL)==(0x940000000ULL>>14),"K70 ppnum");\n')
units=[w/'Adreno740'/name for name in ('NativeAdrenoMemory.cpp','NativeAdrenoCpu.cpp',
    'NativeAdrenoUserClient.cpp','NativeAdrenoSmmuMemory.cpp','NativeAdrenoVm.cpp',
    'NativeAdrenoGmuMemory.cpp','NativeAdrenoGpuContext.cpp')]+[page]
includes=[w/'Adreno740']+[refs/name for name in ('ArmSmmu4K','AdrenoFirmware','AdrenoPM4')]+[xnu/'xnu/osfmk']
env=dict(os.environ)
if os.name=='nt':env['PATH']='D:/msys64/ucrt64/bin;'+env.get('PATH','')
rows=[]
for source in units:
    compiler=args.compiler or ('D:/msys64/ucrt64/bin/clang++.exe' if os.name=='nt' else shutil.which('clang++'))
    cmd=[compiler,'--target=arm64-apple-macos14',*(cpp if source.suffix=='.cpp' else c),
        *[t for path in includes for t in ('-I',str(path))],
        *(['-fsyntax-only'] if args.syntax_only else ['-c']),str(source)]
    if not args.syntax_only:cmd+=['-o',str(out/(source.stem+'.o'))]
    r=subprocess.run(cmd,cwd=xnu/'build/xnu.obj/DEVELOPMENT_ARM64_SWEET/iokit/DEVELOPMENT',
        env=env,capture_output=True,text=True)
    (out/(source.stem+'.log')).write_text(r.stdout+r.stderr)
    errors=[line for line in r.stderr.splitlines() if 'error:' in line]
    row={'source':source.name,'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'exit_code':r.returncode,'errors':errors}
    rows.append(row);print(source.name,'PASS' if not r.returncode else 'FAIL',*errors[:2],sep='\n',flush=True)
report={'profile':'XNU8796 DEVELOPMENT_ARM64_SWEET, 16KiB CPU pages','syntax_only':args.syntax_only,
    'checks':rows,'all_passed':all(not r['exit_code'] for r in rows),'linked_to_kernel':False,'hardware_verified':False}
(w/'gpu-kernel-compile.json').write_text(json.dumps(report,indent=2)+'\n')
raise SystemExit(not report['all_passed'])
