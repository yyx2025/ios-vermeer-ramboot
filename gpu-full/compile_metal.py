"""Compile the iOS16 frontend with the real Apple SDK; do not claim runtime linkage."""
from pathlib import Path
import hashlib,json,subprocess
W=Path(__file__).resolve().parent
O=W/'metal-objects';O.mkdir(exist_ok=True)
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
rows=[]
for name in ('NativeAdrenoMetalPlugin.mm','NativeBridgeMetalFrontend.mm'):
    p=W/'AdrenoMetal'/name
    command=['xcrun','--sdk','iphoneos','clang++','-target','arm64-apple-ios16.0','-isysroot',sdk,
             '-std=c++17','-fobjc-arc','-fblocks','-Wno-deprecated-declarations','-c',str(p),'-o',str(O/(p.stem+'.o'))]
    r=subprocess.run(command,capture_output=True,text=True)
    (O/(p.stem+'.log')).write_text(r.stdout+r.stderr)
    rows.append(dict(source=str(p.relative_to(W)),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),exit_code=r.returncode,
                     errors=[line for line in r.stderr.splitlines() if 'error:' in line]))
(W/'metal-compile.json').write_text(json.dumps(dict(checks=rows,linked=False,runtime_verified=False),indent=2)+'\n')
for row in rows:
    print(row['source'],row['exit_code'],*row['errors'][:5],sep='\n')
if any(row['exit_code'] for row in rows):raise SystemExit(1)
