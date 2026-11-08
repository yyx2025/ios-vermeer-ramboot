"""Verify the security-relevant boundary: GUI reuse cannot compile UFS sources."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    out = Path(tmp)/'profile'
    subprocess.run([sys.executable,str(root/'tools/prepare_ram_gui.py'),str(out)],check=True)
    patches = ''.join(p.read_text(errors='surrogateescape') for p in out.glob('*.patch'))
    for forbidden in ('+++ b/bsd/dev/sweet_ufs', '+++ b/bsd/dev/vermeer_ufs',
                      '#include "vermeer_ufs', 'SWEETLOG', 'vermeer_ufs_ios_rootdev'):
        assert forbidden not in patches, forbidden
    for wanted in ('+++ b/bsd/dev/vermeer_ram_root.c', 'strcmp(rd, "md0")',
                   'vermeer_ram_only', 'mdevgetrange(0', '+++ b/bsd/dev/vermeer_usb_debug.c',
                   '+++ b/bsd/dev/vermeer_touch.c', 'retained pending frame ownership',
                   '1125x2436', '1440x3200'):
        assert wanted in patches, wanted
    assert 'mounting dirty non-journaled root read-write' not in patches
    assert (out/'0075-vermeer-early-usb-network.patch').is_file()
    assert (out/'0076-vermeer-scalar-sha256.patch').is_file()
    assert 'sweet_sha2small.o_CFLAGS_ADD += -mgeneral-regs-only' in patches
    assert 'br_sha256_update(BR_SHA256_CTX(ctx), data, len);' in patches
    for wanted in ('starting USB before network initialization',
                   'VERMEER NET: interface=%s pending=%u',
                   'VERMEER NET: DLIL pending=%u'):
        assert wanted in patches, wanted
print('RAM GUI profile isolates physical storage and retains GUI/USB/touch patches: PASS')
