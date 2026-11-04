"""Reject an integrated artifact missing the services required by boot tasks."""
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
if '--ram-only' in sys.argv:
    forbidden = (b'vermeer_ufs_rw', b'vermeerufs0', b'ufsroot0', b'SWEETLOG', b'vermeer_ufs_ios_rootdev')
    present = [x.decode() for x in forbidden if x in data]
    if present:
        raise SystemExit('RAM-only kernel contains physical-storage paths: '+', '.join(present))
    if b'vermeer_ram_only' not in data:
        raise SystemExit('RAM-only kernel is missing the md0 bootstrap gate')
required = (
    b'SWEET AKS: AppleKeyStore registered', b'AppleKeyStoreUserClient',
    b'SWEET WDT: IOWatchdog registered', b'IOWatchdogUserClient',
    b'CRASH %s reason', b'VERMEER ROOT: first-boot test systembag prepared',
    b'VERMEER USB: CDC ACM debug004', b'K70DBG004',
    b'VERMEER ROOT: experimental clean-root migration baseline:',
)
if '--gui' in sys.argv:
    required += (b'IOMobileFramebufferUserClient', b'IOCoreSurfaceRoot',
                 b'SWEET HID: touch registered', b'SWEET HID: buttons registered',
                 b'SWEET Sandbox: permissive policy', b'VERMEER FB: geometry guard',
                 b'VERMEER FB: first present; muting panel text only',
                 b'VERMEER FB: retained pending frame ownership enabled',
                 b'VERMEER FRAME: submits=', b'VERMEER INPUT: touch-calls=',
                 b'VERMEER TOUCH: physical GT9916 input ready',
                 b'vermeer_touch', b'VERMEER TOUCH: IC-info guard refused',
                 b'VERMEER TOUCH: clock-snapshot', b'VERMEER TOUCH: spi-snapshot',
                 b'VERMEER HID: atomic physical frames and canvas-edge origin v2',
                 b'VERMEER FB: logical canvas 1125x2436 centered in 1440x3200',
                 b'VERMEER SAMPLE: read-only dyld maps and USB requests enabled',
                 b'IMG %s[%d] base', b'selected tasks only')
missing = [s.decode() for s in required if s not in data]
if '--ram-stream' in sys.argv:
    stream = (b'vermeer_ram_stream', b'VMRC0001', b'VMRX0001', b'VMRAM001',
              b'VERMEER RAM: READY VMRX0001', b'complete packed SHA256/index/HFS header verified',
              b'compressed md0 registered', b'external RAM mapped', b'VMRAM %s %u %llu',
              b'verified SMMU layout/control gate refused',
              b'starting USB before network initialization',
              b'network initialization complete; finishing checked root receive',
              b'VERMEER NET: interface=%s pending=%u', b'VERMEER NET: DLIL pending=%u')
    missing += [s.decode() for s in stream if s not in data]
if '--ram-preload' in sys.argv:
    preload = (b'vermeer_ram_preloaded', b'vermeer-ram-preloaded',
               b'loader supplied compressed root; no XNU USB receiver required',
               b'verifying preloaded full-root SHA256 before use',
               b'preloaded root ready; block decompression stays in RAM',
               b'preloaded root setup failed: %d; storage untouched')
    missing += [s.decode() for s in preload if s not in data]
if missing:
    raise SystemExit('Integrated kernel missing required services: ' + ', '.join(missing))
if '--ram-poll' in sys.argv:
    for marker in (b'bootstrap polling active; no scheduler dependency',
                   b'complete root received before networking; bootstrap poll v1',
                   b'polling CDC enumeration before root mapping/network'):
        if marker not in data:raise SystemExit('Kernel lacks polled RAM receive: '+marker.decode())
print('Final integrated kernel contains AKS, watchdog, crash reasons, root preparation and USB debug: PASS')
