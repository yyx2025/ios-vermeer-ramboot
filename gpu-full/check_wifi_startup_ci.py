"""Exercise the production Wi-Fi startup code with sanitizer-enabled hardware peers."""
from pathlib import Path
import hashlib, json, os, subprocess
W=Path(__file__).resolve().parent
out=W/'wifi-startup-tests';out.mkdir(exist_ok=True)
sources=['tests/k70_wifi_startup_test.cpp','K70/K70WifiStartup.cpp',
         'K70/K70WifiDma.c','Adreno740/NativeAdrenoSha256.cpp']
inputs={name:hashlib.sha256((W/name).read_bytes()).hexdigest() for name in sources+
        ['K70/K70WifiStartup.h','K70/K70WifiDma.h','Ath12kWifi/include/nw_abi.h','Adreno740/NativeAdrenoSha256.h']}
flags=['-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined']
subprocess.run(['clang','-std=c11',*flags,'-I'+str(W/'K70'),'-c',str(W/sources[2]),'-o',str(out/'dma.o')],check=True)
subprocess.run(['clang++','-std=c++17',*flags,'-Wno-unused-parameter',
    '-I'+str(W/'K70'),'-I'+str(W/'Adreno740'),'-I'+str(W/'Ath12kWifi/include'),
    str(W/sources[0]),str(W/sources[1]),str(W/sources[3]),str(out/'dma.o'),'-o',str(out/'startup')],check=True)
checks=[]
env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0'
for page in (4096,16384):
    for case in ('preflight','missing_backend','power_failure','wrong_endpoint','dma_failure','msi_failure',
                 'driver_failure','wake','identity_mismatch','identity_unavailable','regdb_only',
                 'concurrent_dispatch','corrupt_after_start','dispatch_error',
                 'dma_cache_failure','dma_free_failure','bad_dma_alignment'):
        r=subprocess.run([str(out/'startup'),case,str(page)],capture_output=True,text=True,env=env,timeout=30)
        checks.append(dict(scenario=case,cpu_page_bytes=page,exit_code=r.returncode,output=r.stdout+r.stderr))
report=dict(schema='k70-wifi-startup-sanitizer/1',checks=checks,input_files=inputs,
            passing_scenarios=sum(c['exit_code']==0 for c in checks),sanitizers=True,hardware_mocked=True,
            hardware_verified=False,boot_integrated=False)
(W/'wifi-startup-sanitizer.json').write_text(json.dumps(report,indent=2)+'\n')
assert all(hashlib.sha256((W/name).read_bytes()).hexdigest()==digest for name,digest in inputs.items()),'Inputs changed during checks'
for c in checks: print(c['scenario'],c['cpu_page_bytes'],c['exit_code'],c['output'].strip())
raise SystemExit(0 if all(c['exit_code']==0 for c in checks) else 1)
