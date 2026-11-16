"""Build and sign the functional iOS16 plugin, without diagnostic daemons."""
from pathlib import Path
import concurrent.futures, hashlib, json, os, plistlib, re, shutil, subprocess
R=Path(__file__).resolve().parent
OUT=R/'build';OUT.mkdir(exist_ok=True)
CACHE=R/'.cache';CACHE.mkdir(exist_ok=True)

def run(args, **kw):
    return subprocess.run([str(x) for x in args], check=True, **kw)
def clone(url, commit, destination, sparse=None):
    destination.mkdir(parents=True)
    run(['git','init','-q',destination])
    run(['git','-C',destination,'remote','add','origin',url])
    if sparse:
        run(['git','-C',destination,'sparse-checkout','init','--cone'])
        run(['git','-C',destination,'sparse-checkout','set',*sparse])
    run(['git','-C',destination,'fetch','--depth=1','--filter=blob:none','origin',commit])
    run(['git','-C',destination,'checkout','--detach','FETCH_HEAD'])
    assert subprocess.check_output(['git','-C',str(destination),'rev-parse','HEAD'],text=True).strip()==commit

def bridge():
    pin=json.loads((R/'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']
    clone(pin['repository'],pin['commit'],CACHE/'gpu-reference/metal2vulkan')
    run(['rustup','toolchain','install','1.98.0','--profile','minimal','--target','aarch64-apple-ios'])
    run(['python3',R/'scripts/build-native-metal-bridge.py','--target','ios'])

def turnip():
    pins=json.loads((R/'scripts/data/adreno-compiler-inputs.json').read_text());pin=pins['mesa']
    dst=CACHE/'mesa-work'
    clone(pin['mirror'],pin['commit'],dst,pin['sparse_directories'])
    for patch in pins['patches']:
        run(['git','-C',dst,'apply',R/patch])
    run(['python3','-m','venv',CACHE/'mesa-venv'])
    run([CACHE/'mesa-venv/bin/pip','install','meson','mako','pyyaml','packaging'])
    run(['python3',R/'scripts/build-turnip-darwin.py','--target','ios'])

os.environ['PATH']=subprocess.check_output(['brew','--prefix','llvm'],text=True).strip()+'/bin:'+subprocess.check_output(['brew','--prefix','bison'],text=True).strip()+'/bin:'+os.environ['PATH']
with concurrent.futures.ThreadPoolExecutor(2) as pool:
    results=[pool.submit(fn) for fn in (bridge,turnip)]
    failures=[]
    for f in results:
        try:f.result()
        except Exception as e:failures.append(str(e))
if failures:raise SystemExit('\n'.join(failures))

bundle=OUT/'NativeAdrenoMetal.bundle';bundle.mkdir()
sdk=subprocess.check_output(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
base=['xcrun','--sdk','iphoneos','clang++','-target','arm64-apple-ios16.0','-isysroot',sdk,'-O2','-std=c++17','-fobjc-arc','-fstack-protector-strong','-Wno-deprecated-declarations']
native=R/'drivers/NativeMetal'
run([*base,'-bundle','-I',R/'drivers/NativeAdreno',native/'NativeAdrenoMetalPlugin.mm',native/'NativeBridgeMetalFrontend.mm',OUT/'native-metal-bridge/ios/libnative_metal_bridge.a','-Wl,-dead_strip','-lobjc','-framework','Foundation','-framework','Metal','-framework','IOSurface','-o',bundle/'NativeAdrenoMetal'])
run(['xcrun','--sdk','iphoneos','metal','-target','air64-apple-ios16.0','-c',native/'K70FrameScale.metal','-o',OUT/'K70FrameScale.air'])
run(['xcrun','--sdk','iphoneos','metallib',OUT/'K70FrameScale.air','-o',bundle/'K70FrameScale.metallib'])
shutil.copy2(OUT/'mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib',bundle/'libvulkan_freedreno.dylib')
for name in ('NativeAdrenoMetal','libvulkan_freedreno.dylib'):
    run(['strip','-x',bundle/name])
(bundle/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier='org.blu-sharky.NativeAdrenoMetal',CFBundleExecutable='NativeAdrenoMetal',CFBundlePackageType='BNDL',CFBundleVersion='11',CFBundleShortVersionString='11',NSPrincipalClass='NativeAdrenoMetalDevice',MinimumOSVersion='16.0')))
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.turnip',bundle/'libvulkan_freedreno.dylib'])
run(['codesign','--force','--sign','-','--pagesize','4096','--identifier','org.blu-sharky.NativeAdrenoMetal',bundle])
run(['codesign','--verify','--strict',bundle])
report=dict(minimum_os='16.0',boot_integrated=False,hardware_verified=False,diagnostic_daemons=False,files={})
for name in ('NativeAdrenoMetal','libvulkan_freedreno.dylib'):
    p=bundle/name
    detail=subprocess.check_output(['codesign','-d','--verbose=4',p],stderr=subprocess.STDOUT,text=True)
    assert 'Page size=4096' in detail
    load=subprocess.check_output(['otool','-l',p],text=True)
    minimum=re.findall(r'\bminos\s+(\S+)',load)
    assert minimum and all(tuple(map(int,v.split('.'))) <= (16,0) for v in minimum),minimum
    report['files'][name]=dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),cdhash=re.search(r'^CDHash=([0-9a-f]{40})$',detail,re.M)[1],bytes=p.stat().st_size,minos=minimum)
(OUT/'runtime-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('iOS16 plugin and Turnip built and signed; device validation remains required')
