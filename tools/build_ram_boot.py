"""Cross-build the upstream storage-free v4 shim on Windows; package only RAM roots.

Checkpoint mode is a source PID1 test, not Apple userspace. GUI mode requires
the RAM-only kernel and a real HFS root. Stream mode keeps XNU at 2 GiB and
adds a live 8 GiB FDT guard for the external compressed-root/overlay region.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import adt
import mkpayload
import mkvermeeradt
import ram_stream_config


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('kernel',type=Path)
    p.add_argument('ramdisk',type=Path)
    p.add_argument('stock_boot',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('--checkpoint',action='store_true')
    p.add_argument('--ram-stream',action='store_true',help='ramdisk positional argument is the verified packed-root JSON; root is received over USB before mount')
    p.add_argument('--ram-map-only',action='store_true',help='read bounded live FDT and return; no XNU jump')
    p.add_argument('--ram-read-only',action='store_true',help='after a live-FDT guard, sample 9 GiB using aligned reads only')
    p.add_argument('--ram-write-restore',action='store_true',help='test and restore 65536 sampled words in lower 8 GiB; no XNU or partition writes')
    p.add_argument('--usb-checkpoint',action='store_true',help='boot verified RAM GUI kernel with source PID1 only to test USB CDC, not iOS')
    p.add_argument('--usb-map-only',action='store_true',help='read USB/SMMU registers only, no XNU jump or controller writes')
    p.add_argument('--product-adt',type=Path)
    p.add_argument('--llvm',type=Path,default=Path(r'D:\msys64\ucrt64\bin'))
    a = p.parse_args()
    if a.ram_stream and any((a.checkpoint,a.ram_map_only,a.ram_read_only,a.ram_write_restore,a.usb_checkpoint,a.usb_map_only)):
        raise ValueError('Complete-root RAM streaming is separate from checkpoint diagnostics')
    packed_record=packed_file=stream_config=None
    if a.ram_stream:
        packed_record,packed_file,stream_config=ram_stream_config.load_record(a.ramdisk)
    if a.ram_read_only and a.ram_write_restore:
        raise ValueError('Read-only and write/restore diagnostics are separate modes')
    if a.ram_read_only or a.ram_write_restore: a.ram_map_only = True
    if a.usb_map_only:
        if a.ram_read_only or a.ram_write_restore:
            raise ValueError('USB state and RAM tests are separate diagnostics')
        a.ram_map_only=True
    if a.usb_checkpoint:
        if not a.checkpoint or a.ram_map_only:
            raise ValueError('USB checkpoint requires --checkpoint and an actual XNU jump')
        subprocess.run([sys.executable,str(ROOT/'tools/check_ios_kernel.py'),str(a.kernel),
                        '--gui','--ram-only'],check=True)
    if a.ram_map_only and not a.checkpoint:
        raise ValueError('RAM map diagnostics require explicit checkpoint mode')
    if a.output.exists(): raise ValueError('Refusing to overwrite existing image')
    if not a.ram_stream and a.ramdisk.stat().st_size >= 0x80000000:
        raise ValueError('Root alone exceeds the retained 2 GiB window; multi-GiB streaming/mapping is not implemented yet')
    kernel = a.kernel.read_bytes()
    if not a.checkpoint:
        subprocess.run([sys.executable,str(ROOT/'tools/check_ios_kernel.py'),str(a.kernel),
                        '--gui','--ram-only',*(['--ram-stream'] if a.ram_stream else [])],check=True)
        if not a.product_adt: raise ValueError('GUI requires official D22AP product ADT')
        if not a.ram_stream:
            with a.ramdisk.open('rb') as f:
                f.seek(1024)
                header = f.read(512)
            if header[:2] not in (b'HX',b'H+') or not struct.unpack_from('>I',header,4)[0] & (1<<13):
                raise ValueError('GUI root must be a journaled HFS/HFSX image')
    elif any(x in kernel for x in (b'vermeer_ufs_rw',b'vermeerufs0',b'ufsroot0',b'SWEETLOG')):
        raise ValueError('Checkpoint kernel contains storage-root code')
    with a.stock_boot.open('rb') as f:
        stock = f.read(4096)
    if stock[:8] != b'ANDROID!' or struct.unpack_from('<I',stock,40)[0] != 4:
        raise ValueError('Expected the backed-up K70 stock boot v4 header')
    out = ROOT/'build'/'ram-boot'
    out.mkdir(parents=True,exist_ok=True)
    tree = mkvermeeradt.build(True,not a.checkpoint,not a.checkpoint,a.product_adt)
    if a.ram_stream:
        chosen=next(child for child in tree.children if child.name=='chosen')
        chosen.props.append(('vermeer-ram-config',0,stream_config))
    tree_path = out/'vermeer.adt'
    tree_path.write_bytes(adt.serialize(tree))
    args = 'debug=0xc serial=3 cpus=1 keepsyms=1 rd=md0'
    if a.usb_checkpoint:
        args += ' vermeer_usb=1 sweet_sample_names=-'
    if not a.checkpoint:
        args += ' rootrw=1 ignition_level=1 vermeer_ram_only=1 vermeer_prepare_root=1 vermeer_migration_baseline=1 vermeer_usb=1'
    if a.ram_stream: args += ' vermeer_ram_stream=1'
    blob,info = mkpayload.build(a.kernel,tree_path,args,None if a.ram_stream else a.ramdisk)
    mem = 0x80000000
    if int(info['kernel_end'],16)-int(info['virt_base'],16)+len(tree_path.read_bytes())+0x8000 > mem:
        raise ValueError('Root exceeds the validated 2 GiB kernel window; a large-root receiver and mapping are still required')
    if len(blob)>0x10000000:
        raise ValueError('Embedded payload exceeds upstream 256 MiB receiver limit; refusing a giant fastboot boot image')
    build = out/'shim'
    build.mkdir(exist_ok=True)
    for src in (ROOT/'boot').iterdir():
        if src.is_file(): shutil.copy2(src,build/src.name)
    (build/'out').mkdir(exist_ok=True)
    (build/'out/payload.bin').write_bytes(blob)
    clang = a.llvm/'clang.exe'
    ld = a.llvm/'lld.exe'
    objcopy = a.llvm/'llvm-objcopy.exe'
    readelf = a.llvm/'llvm-readelf.exe'
    env = dict(os.environ)
    env['PATH'] = str(a.llvm)+os.pathsep+env.get('PATH','')
    objects=[]
    for source in ('entry.S','vectors.S','payload.S','main.c','console.c','loader.c','fdt.c','ram_map.c','usb_map.c'):
        target = 'out/'+Path(source).stem+'.o'
        subprocess.run([str(clang),'--target=aarch64-none-elf','-O2','-ffreestanding','-nostdlib',
                        '-fno-pic','-fno-pie','-mgeneral-regs-only','-mstrict-align','-fno-stack-protector',
                        '-Wall','-Wextra','-Werror',*(['-DVERMEER_RAM_MAP_ONLY'] if a.ram_map_only else []),
                        *(['-DVERMEER_RAM_READ_ONLY'] if a.ram_read_only else []),
                        *(['-DVERMEER_RAM_WRITE_RESTORE'] if a.ram_write_restore else []),
                        *(['-DVERMEER_USB_MAP_ONLY'] if a.usb_map_only else []),
                        *(['-DVERMEER_RAM_STREAM'] if a.ram_stream else []),
                        '-c',source,'-o',target],cwd=build,env=env,check=True)
        objects.append(target)
    # entry.S intentionally stores _end as the ARM64 image-size field.
    # Match upstream: the executable C code must have no absolute pointers.
    relocs=subprocess.check_output([str(readelf),'-r','out/main.o','out/console.o',
                                  'out/loader.o','out/fdt.o','out/ram_map.o','out/usb_map.o'],cwd=build,env=env,text=True)
    if 'R_AARCH64_ABS64' in relocs:raise ValueError('Absolute relocation in freestanding shim')
    subprocess.run([str(ld),'-flavor','gnu','-m','aarch64elf','-T','link.ld',
                    '--build-id=none',*objects,'-o','out/shim.elf'],cwd=build,env=env,check=True)
    subprocess.run([str(objcopy),'-O','binary','out/shim.elf','out/Image'],cwd=build,env=env,check=True)
    image=(build/'out/Image').read_bytes()
    if image[56:60] != b'ARM\x64' or image.count(blob)!=1:
        raise ValueError('Embedded payload/image validation failed')
    header=bytearray(4096);header[:8]=b'ANDROID!'
    struct.pack_into('<IIII',header,8,len(image),0,struct.unpack_from('<I',stock,16)[0],1584)
    struct.pack_into('<I',header,40,4);header[44:56]=b'console=null'
    boot=header+image+bytes(-len(image)%4096)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_bytes(boot)
    report={'image':str(a.output.resolve()),'sha256':hashlib.sha256(boot).hexdigest(),'bytes':len(boot),
            'kernel_sha256':hashlib.sha256(kernel).hexdigest(),'ramdisk_sha256':None if a.ram_stream else hashlib.sha256(a.ramdisk.read_bytes()).hexdigest(),
            'profile':'live-FDT guarded 8 GiB sparse write/alias/restoration test' if a.ram_write_restore else ('live-FDT guarded 9 GiB sparse read-only test' if a.ram_read_only else ('live FDT RAM map only' if a.ram_map_only else ('source PID1 checkpoint' if a.checkpoint else 'RAM iOS GUI candidate'))),
            'candidate_ram_write_enabled':a.ram_write_restore or not a.ram_map_only,
            'xnu_jump_enabled':not a.ram_map_only,
            'command_line':args,'window_base':'0x8c0000000','window_bytes':mem,
            'live_fdt_guard':True,'boot_ramdisk_bytes':0,'storage_commands':False,'hardware_verified':False,
            'payload':info}
    if a.ram_write_restore:
        report.update({'test_window_bytes':0x200000000,'test_words':65536,
                       'mmu_and_data_cache_must_be_off':True,'original_words_restored_and_checked':True,
                       'whole_memory_test':False,'hardware_write_test_verified':False})
    if a.usb_checkpoint:
        report.update({'profile':'RAM XNU + source PID1 + opt-in USB CDC diagnostic',
                       'apple_userspace_present':False,'full_ios_root_present':False,
                       'usb_before_full_root_mount_implemented':False})
    if a.usb_map_only:
        report.update({'profile':'read-only USB and SMMU state diagnostic',
                       'controller_register_writes':False,'smmu_mappings_changed':False,
                       'candidate_ram_write_enabled':False,'xnu_jump_enabled':False})
    if a.ram_stream:
        report.update({'profile':'complete official root compressed RAM boot candidate',
            'packed_root':str(packed_file.resolve()),'packed_sha256':packed_record['packed_sha256'],
            'packed_bytes':packed_record['packed_bytes'],'logical_root_bytes':packed_record['logical_bytes'],
            'external_ram_base':hex(ram_stream_config.BASE),'external_ram_bytes':ram_stream_config.SIZE,
            'write_overlay_bytes':ram_stream_config.OVERLAY,'live_guard_bytes':0x200000000,
            'embedded_root_bytes':0,'usb_before_root_mount_implemented':True,
            'full_ios_desktop_verified':False})
    a.output.with_suffix('.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
