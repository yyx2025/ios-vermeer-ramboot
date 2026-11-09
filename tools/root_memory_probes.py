"""Bind small, aligned RAM witnesses to the fully verified packed-root record."""
import hashlib
import struct

MAGIC = b'VMRP0001'
HEADER = struct.Struct('<8sIIQ32s')
ENTRY = struct.Struct('<QII32s')
SAMPLE_BYTES = 16384
INTERVAL = 64 * 1024 * 1024
MAX_ENTRIES = 128

def make_probes(path, total, root_sha):
    if total < SAMPLE_BYTES or path.stat().st_size != total:
        raise ValueError('Root probe length mismatch')
    offsets = sorted(set([*range(0, total - SAMPLE_BYTES + 1, INTERVAL),
                          (total - SAMPLE_BYTES) // SAMPLE_BYTES * SAMPLE_BYTES]))
    if not 0 < len(offsets) <= MAX_ENTRIES:
        raise ValueError('Unsupported root probe count')
    rows = []
    with path.open('rb') as handle:
        for offset in offsets:
            handle.seek(offset)
            data = handle.read(SAMPLE_BYTES)
            if len(data) != SAMPLE_BYTES:
                raise ValueError('Root probe truncated')
            rows.append((offset, SAMPLE_BYTES, hashlib.sha256(data).digest()))
    blob = HEADER.pack(MAGIC, len(rows), ENTRY.size, total, bytes.fromhex(root_sha))
    blob += b''.join(ENTRY.pack(offset, length, 0, digest) for offset, length, digest in rows)
    return blob, rows

def c_header(rows):
    result = '#include <stdint.h>\nstruct rfb_root_probe { uint64_t offset; uint32_t bytes; uint8_t sha256[32]; };\n'
    result += 'static const struct rfb_root_probe rfb_root_probes[]={\n'
    result += ''.join('{UINT64_C(%d),%d,{%s}},\n' % (offset, length, ','.join(map(str, digest)))
                      for offset, length, digest in rows)
    result += '};\n#define RFB_ROOT_PROBE_COUNT (sizeof(rfb_root_probes)/sizeof(rfb_root_probes[0]))\n'
    return result
