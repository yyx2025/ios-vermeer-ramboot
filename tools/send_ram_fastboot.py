"""Local preflight by default; --boot temporarily loads the RAM receiver and root.

Only devices/getvar/boot/stage/oem ram-boot are implemented. The custom receiver
accepts sequential RAM downloads; it implements no storage operations.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time
import adt
from ram_stream_config import load_record, file_sha256

SERIAL = 'VERMEERRAM001'
VERSION = 'VERMEER-RAM-FASTBOOT-1'
CHUNK = 64 * 1024 * 1024
BOOT_LIMIT = 805306368
QUERIES = {'product','version-bootloader','serialno','max-download-size',
           'ram-total','ram-received','ram-sha256-lo','ram-sha256-hi'}

class Fastboot:
    def __init__(self, executable, log):
        self.executable, self.log = str(executable), Path(log)

    def run(self, args, timeout=30):
        allowed = args == ['devices']
        if len(args)>=4 and args[0]=='-s' and args[1] and not args[1].startswith('-'):
            action=args[2:]
            allowed = ((len(action)==2 and action[0]=='getvar' and action[1] in QUERIES)
                or (len(action)==2 and action[0]=='boot' and args[1]!=SERIAL)
                or (len(action)==2 and action[0]=='stage' and args[1]==SERIAL)
                or (action==['oem','ram-boot'] and args[1]==SERIAL))
        if not allowed:
            raise ValueError('Fastboot operation is outside the RAM-only command list')
        record={'time':datetime.now(timezone.utc).isoformat(),'args':args}
        try:
            p=subprocess.run([self.executable,*args],capture_output=True,text=True,
                encoding='utf-8',errors='replace',timeout=timeout,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            record.update(returncode=p.returncode,output=p.stdout+p.stderr)
        except subprocess.TimeoutExpired:
            record['timeout']=timeout
            raise RuntimeError('Fastboot timed out; session stopped, keep the final phone screen') from None
        finally:
            with self.log.open('a',encoding='utf-8') as f:
                f.write(json.dumps(record,ensure_ascii=False)+'\n')
        if p.returncode:
            raise RuntimeError('Fastboot failed: '+record['output'].strip())
        return record['output']

    def devices(self):
        rows=[]
        for line in self.run(['devices']).splitlines():
            fields=line.split()
            if len(fields)==2 and fields[1]=='fastboot': rows.append(fields[0])
        return rows

    def getvar(self, serial, key):
        output=self.run(['-s',serial,'getvar',key])
        found=re.findall(r'^(?:\(bootloader\)\s*)?'+re.escape(key)+r':\s*(\S+)\s*$',output,re.M)
        if len(found)!=1:
            raise RuntimeError('Missing or ambiguous Fastboot value: '+key)
        return found[0]

def preflight(image, root_json):
    image=Path(image);record=json.loads(image.with_suffix('.json').read_text(encoding='utf-8'))
    root,packed,config=load_record(root_json)
    data=image.read_bytes()
    if (len(data)!=record['bytes'] or len(data)>BOOT_LIMIT or hashlib.sha256(data).hexdigest()!=record['sha256']):
        raise ValueError('Temporary image length/SHA256 mismatch')
    if len(data)<4096 or data[:8]!=b'ANDROID!' or struct.unpack_from('<I',data,40)[0]!=4 or struct.unpack_from('<I',data,12)[0]:
        raise ValueError('Expected RAM-only Android boot v4 image without boot ramdisk')
    kernel_bytes=struct.unpack_from('<I',data,8)[0]
    if len(data)!=4096+(kernel_bytes+4095)//4096*4096 or data[4152:4156]!=b'ARM\x64':
        raise ValueError('Invalid embedded ARM64 image bounds')
    signature=struct.pack('<8sII',b'SWEETXNU',1,10)
    if data.count(signature)!=1: raise ValueError('Ambiguous kernel payload')
    off=data.index(signature)
    args=data[off+0x48:off+0x48+608].split(b'\0',1)[0].decode('ascii')
    if args!=record['command_line']: raise ValueError('Embedded command line mismatch')
    options=dict(item.split('=',1) for item in args.split())
    required={'rd':'md0','vermeer_ram_only':'1','vermeer_ram_stream':'1',
              'vermeer_ram_preloaded':'1','vermeer_usb':'0','vermeer_touch':'1'}
    if any(options.get(k)!=v for k,v in required.items()) or 'init' in options:
        raise ValueError('Image does not use the pre-XNU complete-root path')
    _,_,adt_off,adt_bytes,_,physical,memory=struct.unpack_from('<7Q',data,off+16)
    if physical!=0x8c0000000 or memory!=0x80000000 or adt_off>kernel_bytes or adt_bytes>kernel_bytes-adt_off or off+adt_off+adt_bytes>4096+kernel_bytes:
        raise ValueError('Payload RAM/ADT bounds mismatch')
    tree,used=adt.parse(data[off+adt_off:off+adt_off+adt_bytes])
    chosen=next(c for c in tree.children if c.name=='chosen')
    if used!=adt_bytes or chosen.prop('vermeer-ram-config')!=config or chosen.prop('vermeer-ram-preloaded')!=struct.pack('<I',1):
        raise ValueError('Embedded root configuration/seal mismatch')
    def no_ramdisk(node):
        if any(n=='RAMDisk' for n,_,_ in node.props): raise ValueError('Unexpected embedded RAMDisk')
        for child in node.children: no_ramdisk(child)
    no_ramdisk(tree)
    if (record.get('root_receive_before_xnu') is not True or record.get('xnu_usb_required') is not False
        or record.get('windows_com_port_required') is not False or record.get('storage_commands') is not False
        or record.get('fastboot_ram_serial')!=SERIAL or record.get('ram_chunk_bytes')!=CHUNK
        or record.get('packed_bytes')!=root['packed_bytes'] or record.get('packed_sha256')!=root['packed_sha256']):
        raise ValueError('Image and complete-root records do not match')
    print('Checking the entire packed-root SHA256 before any handset command...',flush=True)
    with packed.open('rb') as f:
        if file_sha256(f)!=root['packed_sha256']: raise ValueError('Complete packed-root SHA256 mismatch')
    return record,root,packed

def validate_receiver(fb,total,sha):
    expected={'product':'vermeer','version-bootloader':VERSION,'serialno':SERIAL,
              'ram-total':f'{total:016x}','ram-received':'0000000000000000',
              'ram-sha256-lo':sha[:32],'ram-sha256-hi':sha[32:]}
    for key,want in expected.items():
        if fb.getvar(SERIAL,key)!=want:
            raise RuntimeError('RAM receiver identity/root mismatch: '+key)
    if int(fb.getvar(SERIAL,'max-download-size'),0)!=CHUNK:
        raise RuntimeError('Unexpected RAM receiver download bound')

def upload(fb,packed,total,sha,logs,state,chunk=CHUNK):
    if not 0<chunk<=CHUNK: raise ValueError('Invalid RAM chunk size')
    validate_receiver(fb,total,sha)
    if shutil.disk_usage(logs).free<2*chunk: raise RuntimeError('Insufficient PC space for bounded chunk staging')
    sent=0;digest=hashlib.sha256()
    with Path(packed).open('rb') as source, tempfile.TemporaryDirectory(prefix='ram-chunk-',dir=logs) as tmp:
        part=Path(tmp)/'root-chunk.bin'
        while sent<total:
            data=source.read(min(chunk,total-sent))
            if not data: raise RuntimeError('Root file truncated during transfer')
            digest.update(data);part.write_bytes(data)
            fb.run(['-s',SERIAL,'stage',str(part)],timeout=300)
            sent+=len(data)
            if fb.getvar(SERIAL,'ram-received')!=f'{sent:016x}':
                raise RuntimeError('Phone RAM byte count differs from the completed chunk; stopping')
            state['confirmed_ram_bytes']=sent
            print(f'Root in phone RAM: {sent}/{total} bytes ({sent/total:.1%})',flush=True)
        if source.read(1) or digest.hexdigest()!=sha:
            raise RuntimeError('Transmitted full-root SHA256/length mismatch; XNU was not requested')
    output=fb.run(['-s',SERIAL,'oem','ram-boot'],timeout=120)
    if 'root SHA256 verified' not in output or not re.search(r'\bOKAY\b',output):
        raise RuntimeError('No explicit complete-root verification confirmation from the loader')
    state.update(loader_sha256_confirmed=True,xnu_handoff_requested=True)
    print('Loader confirmed complete-root SHA256. XNU handoff requested.',flush=True)
    print('USB disappearance is expected now. Root mount, desktop and touch must be checked on the phone.',flush=True)

def boot_and_upload(fb,image,packed,root,logs,state,wait_seconds=300):
    serials=fb.devices()
    if len(serials)!=1 or serials[0]==SERIAL:
        raise RuntimeError('Expected exactly one K70 in its original Fastboot screen')
    stock=serials[0]
    if fb.getvar(stock,'product')!='vermeer': raise RuntimeError('Connected phone is not vermeer')
    maximum=fb.getvar(stock,'max-download-size')
    limit=int(maximum,16) if maximum.lower().startswith('0x') else int(maximum,10)
    state['stock_max_download_size']=limit
    if Path(image).stat().st_size>min(limit,BOOT_LIMIT): raise RuntimeError('Temporary IMG exceeds this phone download limit')
    state['temporary_boot_requested']=True
    print('Temporarily booting the RAM receiver...',flush=True)
    print(fb.run(['-s',stock,'boot',str(image)],timeout=120),flush=True)
    deadline=time.monotonic()+wait_seconds
    print('Waiting for VERMEERRAM001 Fastboot (no COM port is used)...',flush=True)
    while time.monotonic()<deadline:
        serials=fb.devices()
        if SERIAL in serials:
            if serials!=[SERIAL]: raise RuntimeError('More than one Fastboot device is present')
            break
        time.sleep(1)
    else: raise RuntimeError('RAM Fastboot did not appear; keep the final phone screen for the first hardware error')
    upload(fb,packed,root['packed_bytes'],root['packed_sha256'],logs,state)

def save_windows_usb(logs):
    if os.name!='nt': return
    query="Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -like 'USB*' -or $_.PNPClass -eq 'Ports' } | Select-Object Name,PNPDeviceID,Status,ConfigManagerErrorCode | ConvertTo-Json -Depth 3"
    try:
        p=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',query],capture_output=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
        (logs/'windows-usb.txt').write_bytes(p.stdout+b'\n'+p.stderr)
    except (OSError,subprocess.TimeoutExpired) as e:
        (logs/'windows-usb-error.txt').write_text(str(e),encoding='utf-8')

def main():
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--root-record',type=Path,required=True)
    p.add_argument('--fastboot',type=Path,required=True)
    p.add_argument('--logs',type=Path,required=True)
    p.add_argument('--boot',action='store_true')
    a=p.parse_args()
    logs=a.logs/datetime.now().strftime('%Y%m%d-%H%M%S-%f');logs.mkdir(parents=True,exist_ok=False)
    state={'temporary_boot_requested':False,'confirmed_ram_bytes':0,'loader_sha256_confirmed':False,
        'xnu_handoff_requested':False,'root_mount_verified':False,'desktop_verified':False,'touch_verified':False,
        'android_storage_commands':False,'boot_requested':a.boot}
    result=1
    try:
        if not a.fastboot.is_file(): raise ValueError('Local fastboot executable is missing')
        image,root,packed=preflight(a.image,a.root_record)
        state.update(image_sha256=image['sha256'],root_sha256=root['packed_sha256'],local_preflight_passed=True)
        if a.boot: boot_and_upload(Fastboot(a.fastboot,logs/'fastboot.jsonl'),a.image,packed,root,logs,state)
        else: print('Local preflight PASS. No handset command issued. Use --boot only for the combined physical test.',flush=True)
        result=0
    except (OSError,ValueError,RuntimeError,KeyError,StopIteration,KeyboardInterrupt) as e:
        state['error']=str(e) or type(e).__name__
        print('Stopped: '+state['error'],flush=True)
        if a.boot: save_windows_usb(logs)
    finally:
        state['exit_code']=result
        (logs/'result.json').write_text(json.dumps(state,indent=2),encoding='utf-8')
        print('Session result: '+str(logs/'result.json'),flush=True)
    return result

if __name__=='__main__':
    raise SystemExit(main())
