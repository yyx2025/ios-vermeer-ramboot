"""Seal contiguous SHA256 windows covering every byte of the verified root."""
import hashlib
import struct

MAGIC = b'VMRW0001'
HEADER = struct.Struct('<8sIIQ32s')
ENTRY = struct.Struct('<QII32s')
WINDOW_BYTES = 64 * 1024 * 1024
READ_BYTES = 1024 * 1024
MAX_ENTRIES = 128

def make_windows(path, total, root_sha):
    if total <= 0 or path.stat().st_size != total:
        raise ValueError('Root window length mismatch')
    count = (total + WINDOW_BYTES - 1) // WINDOW_BYTES
    if not 0 < count <= MAX_ENTRIES:
        raise ValueError('Unsupported root window count')
    rows = []
    full = hashlib.sha256()
    with path.open('rb') as handle:
        for offset in range(0, total, WINDOW_BYTES):
            size = min(WINDOW_BYTES, total - offset)
            digest = hashlib.sha256()
            remaining = size
            while remaining:
                data = handle.read(min(remaining, READ_BYTES))
                if not data:
                    raise ValueError('Root windows truncated')
                full.update(data)
                digest.update(data)
                remaining -= len(data)
            rows.append((offset, size, digest.digest()))
        if handle.read(1) or full.hexdigest() != root_sha:
            raise ValueError('Root windows complete-file SHA256 mismatch')
    blob = HEADER.pack(MAGIC, len(rows), ENTRY.size, total, bytes.fromhex(root_sha))
    blob += b''.join(ENTRY.pack(offset, size, 0, digest) for offset, size, digest in rows)
    return blob, rows
