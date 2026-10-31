"""Read Android's memory device tree over ADB. Never modifies the handset.

Android's tree is corroborating evidence: it may differ from the live ABL tree.
An explicit --root uses an already-authorized KSU Shell grant for reads only.
This utility never reboots, installs root, writes a handset file or changes slots.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shlex
import hashlib
import struct
import subprocess

DT_BASE='/sys/firmware/devicetree/base'

def parse_fdt(data):
    if len(data)<40:raise ValueError('FDT header truncated')
    magic,total,so,ss,ro,version,last,_,sn,sz=struct.unpack_from('>10I',data)
    if magic!=0xd00dfeed or total>len(data) or total>4*1024**2 or version<17 or last>17:
        raise ValueError('Invalid bounded FDT header')
    if so<40 or ss<40 or ro<40 or so+sz>total or ss+sn>total or ro%8 or so%4:
        raise ValueError('Invalid FDT section bounds')
    strings=data[ss:ss+sn];end=so+sz;p=so;stack=[];properties={};reservations=[]
    limit=min([x for x in (so,ss,total) if x>ro])
    while ro+16<=limit:
        base,size=struct.unpack_from('>QQ',data,ro);ro+=16
        if not base and not size:break
        reservations.append((base,size))
    else:raise ValueError('Unterminated FDT reserve map')
    while p+4<=end:
        token=struct.unpack_from('>I',data,p)[0];p+=4
        if token==1:
            nul=data.find(b'\0',p,end)
            if nul<0 or len(stack)>=64:raise ValueError('Malformed FDT node')
            name=data[p:nul].decode('ascii');stack.append(name);p=(nul+4)&~3
        elif token==2:
            if not stack:raise ValueError('Unbalanced FDT end node')
            stack.pop()
        elif token==3:
            if not stack or p+8>end:raise ValueError('Malformed FDT property')
            n,off=struct.unpack_from('>II',data,p);p+=8
            nul=strings.find(b'\0',off) if off<len(strings) else -1
            if nul<0 or p+n>end:raise ValueError('FDT property out of bounds')
            name=strings[off:nul].decode('ascii')
            properties['/'.join(stack)+'/'+name]=data[p:p+n];p=(p+n+3)&~3
        elif token==4:continue
        elif token==9:
            if stack:raise ValueError('Unbalanced FDT tree')
            return properties,reservations
        else:raise ValueError('Unknown FDT token')
    raise ValueError('Unterminated FDT tree')

def decode_ranges(data,ac,sc):
    width=4*(ac+sc)
    if ac not in (1,2) or sc not in (1,2) or not data or len(data)%width:
        raise ValueError('Unknown/truncated device-tree cells')
    result=[]
    for offset in range(0,len(data),width):
        base=int.from_bytes(data[offset:offset+4*ac],'big')
        size=int.from_bytes(data[offset+4*ac:offset+width],'big')
        if base+size>2**64:raise ValueError('Memory range overflow')
        if size:result.append((base,size))
    return result

def merged(ranges):
    result=[]
    for base,size in sorted(ranges):
        end=base+size
        if result and base<=result[-1][1]:result[-1]=(result[-1][0],max(result[-1][1],end))
        else:result.append((base,end))
    return result

def summarize(properties,reservations):
    def cells(path,default):
        raw=properties.get(path)
        if raw is None:return default
        if len(raw)!=4:raise ValueError('Malformed cell width')
        return int.from_bytes(raw,'big')
    ac=cells('/#address-cells',2);sc=cells('/#size-cells',1)
    rac=cells('/reserved-memory/#address-cells',ac);rsc=cells('/reserved-memory/#size-cells',sc)
    banks=[];fixed=list(reservations);dynamic=[]
    for name,data in properties.items():
        if re.fullmatch(r'/memory(?:@[^/]+)?/reg',name):banks.extend(decode_ranges(data,ac,sc))
        elif re.fullmatch(r'/reserved-memory/[^/]+/reg',name):fixed.extend(decode_ranges(data,rac,rsc))
        elif re.fullmatch(r'/reserved-memory/[^/]+/size',name):
            parent=name.rsplit('/',1)[0]
            if parent+'/reg' not in properties:dynamic.append(parent)
    if not banks:raise ValueError('No memory banks found')
    if properties.get('/reserved-memory/ranges',b''):
        raise ValueError('Translated reserved-memory ranges require separate decoding')
    available=merged(banks)
    for base,size in fixed:
        end=base+size;next_ranges=[]
        if end>2**64:raise ValueError('Reservation overflow')
        for lo,hi in available:
            if end<=lo or base>=hi:next_ranges.append((lo,hi))
            else:
                if lo<base:next_ranges.append((lo,base))
                if end<hi:next_ranges.append((end,hi))
        available=next_ranges
    def range_json(lo,hi):return {'base':f'0x{lo:016x}','end':f'0x{hi:016x}','bytes':hi-lo,'GiB':round((hi-lo)/2**30,6)}
    candidates={}
    lo=0x8c0000000
    for gib in (2,4,6,8,10):
        hi=lo+gib*2**30
        candidates[str(gib)+'GiB']={
            'single_bank_covers':any(b<=lo and b+n>=hi for b,n in banks),
            'memory_union_covers':any(b<=lo and e>=hi for b,e in merged(banks)),
            'fixed_reservation_free':any(b<=lo and e>=hi for b,e in available)}
    return {'banks':[range_json(b,b+n) for b,n in banks],
            'fixed_reservations':[range_json(b,b+n) for b,n in fixed],
            'available_after_fixed_reservations':[range_json(b,e) for b,e in available],
            'dynamic_reservation_nodes':dynamic,'candidates_from_8c0000000':candidates,
            'larger_window_authorized':False,'hardware_access_tested':False}

def main():
    p=argparse.ArgumentParser(__doc__)
    p.add_argument('--adb',type=Path,default=Path(r'D:\download\platform-tools-latest-windows\platform-tools\adb.exe'))
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--root',action='store_true',help='use the user-authorized existing KSU Shell grant for reads')
    a=p.parse_args()
    def adb(*args):return subprocess.run([str(a.adb),*args],capture_output=True,timeout=30)
    devices=adb('devices').stdout.decode().splitlines()
    ready=[line.split()[0] for line in devices if re.fullmatch(r'\S+\s+device',line.strip())]
    if len(ready)!=1:raise SystemExit('未发现唯一已授权的 ADB 设备。请回到安卓、开启 USB 调试并确认电脑连接。')
    serial=ready[0]
    def read(*args):
        if a.root:
            return adb('-s',serial,'exec-out','su -c '+shlex.quote(shlex.join(args)))
        return adb('-s',serial,'exec-out',*args)
    if a.root:
        identity=read('id')
        if identity.returncode or not identity.stdout.startswith(b'uid=0('):
            raise SystemExit('KSU Shell 的已有 root 授权未生效，停止读取。')
    product=read('getprop','ro.product.device').stdout.decode().strip()
    if product!='vermeer':raise SystemExit('设备标识不是 vermeer，停止读取。')
    full=read('cat','/sys/firmware/fdt')
    if full.returncode==0 and full.stdout[:4]==b'\xd0\x0d\xfe\xed':
        properties,reservations=parse_fdt(full.stdout);source='/sys/firmware/fdt'
    else:
        listing=read('find',DT_BASE,'-type','f')
        if listing.returncode:raise SystemExit('当前安卓权限无法读取设备树：'+listing.stderr.decode(errors='replace'))
        properties={};reservations=[];source=DT_BASE
        for line in listing.stdout.decode().splitlines():
            if not line.startswith(DT_BASE+'/') or not re.fullmatch(r'[A-Za-z0-9_,.@/+\-#]+',line):continue
            rel=line[len(DT_BASE):]
            if not (rel in ('/#address-cells','/#size-cells') or
                    re.fullmatch(r'/memory(?:@[^/]+)?/reg',rel) or rel.startswith('/reserved-memory/')):continue
            raw=read('cat',line)
            if raw.returncode:raise SystemExit('设备树属性读取失败：'+rel)
            properties[rel]=raw.stdout
        if not any(re.fullmatch(r'/memory(?:@[^/]+)?/reg',name) for name in properties):
            raise SystemExit('这台手机限制普通 ADB 读取设备树，未取得内存段。可使用诊断屏幕照片；此脚本不会获取 root 或修改安卓。')
    report=summarize(properties,reservations)
    report.update({'read_at_utc':datetime.now(timezone.utc).isoformat(),'source':source,
                  'device':'vermeer','android_tree_may_differ_from_abl':True,
                  'existing_ksu_shell_grant_used':a.root,
                  'handset_commands':'devices, id, getprop, cat, find only; optional authorized su; no reboot/install/write'})
    a.output.mkdir(parents=True,exist_ok=True)
    path=a.output/('k70-memory-'+datetime.now().strftime('%Y%m%d-%H%M%S')+'.json')
    if source=='/sys/firmware/fdt':
        raw_path=path.with_suffix('.fdt')
        raw_path.write_bytes(full.stdout)
        report['raw_fdt_path']=str(raw_path.resolve())
        report['raw_fdt_sha256']=hashlib.sha256(full.stdout).hexdigest()
    path.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('已只读导出内存布局：',path)
    for index,b in enumerate(report['banks']):print(f"BANK {index}: BASE {b['base']} SIZE {b['bytes']:#x} ({b['GiB']} GiB)")
    print('大窗口检查：',json.dumps(report['candidates_from_8c0000000'],ensure_ascii=False))

if __name__=='__main__':main()
