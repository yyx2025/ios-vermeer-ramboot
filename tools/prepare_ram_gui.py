"""Reuse the upstream GUI patch series with physical storage entirely excluded.

The current shim's 2 GiB guard is deliberately retained. This profile prepares
source patches; it does not claim a 6-8 GiB receiver or a verified kernel build.
"""
from pathlib import Path
import argparse
import difflib
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def new_file(path, content):
    return ''.join(difflib.unified_diff([], content.splitlines(True),
                  fromfile='/dev/null', tofile='b/'+path))


def prepare(destination):
    if destination.exists() and any(destination.iterdir()):
        raise ValueError('Destination must be empty')
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        generated = Path(tmp)/'profile'
        subprocess.run([sys.executable, str(ROOT/'tools/prepare_vermeer.py'),
                        str(ROOT/'xnu/patches'), str(generated), '--ios-gui'], check=True)
        for path in generated.iterdir():
            if path.name.startswith(('0021-vermeer-ios-root-hook', '0022-vermeer-ufs-read-probe-source')):
                continue
            data = path.read_text(errors='surrogateescape')
            # Keep contexts consistent for the subsequent BSD conf patches.
            data = data.replace('bsd/dev/vermeer_ufs_ios.c', 'bsd/dev/vermeer_ram_root.c')
            (destination/path.name).write_text(data, errors='surrogateescape')

    hook = '''--- a/bsd/conf/files
+++ b/bsd/conf/files
@@ -103,6 +103,7 @@
 
 bsd/dev/random/randomdev.c\t\tstandard
 
+bsd/dev/vermeer_ram_root.c\t\t\tstandard
 bsd/dev/memdev.c\t\t\tstandard
 bsd/dev/mem.c\t\t\tstandard
 bsd/dev/munge.c \t\t\toptional config_requires_u32_munging
'''
    (destination/'0021-vermeer-ram-root-hook.patch').write_text(hook)
    files = ['vermeer_ram_root.c', 'vermeer_ios_bootstrap.c', 'vermeer_migration_transaction.h',
             'vermeer_log_ring.h', 'vermeer_debug.c', 'vermeer_usb_protocol.h', 'vermeer_usb_debug.c']
    sources = []
    for name in files:
        data = (ROOT/'xnu/vermeer'/name).read_text()
        if name == 'vermeer_ios_bootstrap.c':
            data = data.replace('vermeer_ufs_ios_root_ready', 'vermeer_ram_root_ready')
            needle = 'strcmp(rd, "vermeerufs0")'
            if data.count(needle) != 1:
                raise ValueError('Unexpected upstream bootstrap root selector')
            data = data.replace(needle, 'strcmp(rd, "md0")')
        sources.append(new_file('bsd/dev/'+name, data))
    (destination/'0022-vermeer-ram-source.patch').write_text(''.join(sources))
    added = '\n'.join(p.read_text(errors='surrogateescape') for p in destination.glob('*.patch'))
    for forbidden in ('+++ b/bsd/dev/sweet_ufs.c', '+++ b/bsd/dev/vermeer_ufs',
                      'SWEETLOG', 'vermeer_ufs_ios_rootdev', 'SCSI_WRITE', 'ufshcd_queuecommand'):
        if forbidden in added:
            raise ValueError('Physical storage leaked into RAM profile: '+forbidden)
    report = {'profile':'vermeer-ios-ram-gui','root':'md0','root_writes':'RAM',
              'physical_storage_driver_included':False,'logging':'RAM ring + opt-in USB CDC',
              'upstream_commit':json.loads((ROOT/'UPSTREAM.json').read_text())['commit'],
              'patches':[p.name for p in sorted(destination.glob('*.patch'))],
              'built':False,'hardware_verified':False,
              'remaining':'full XNU compile; complete root construction; multi-GiB receiver and runtime RAM validation'}
    (destination/'RAM_PROFILE.json').write_text(json.dumps(report,indent=2))
    print('RAM GUI source profile:',len(report['patches']),'patches; no physical storage sources')


if __name__ == '__main__':
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('destination', type=Path)
    prepare(p.parse_args().destination)
