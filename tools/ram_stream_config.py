"""Validate local packed-root metadata and describe the guarded RAM aperture."""
import hashlib
import json
from pathlib import Path
import struct

BASE = 0x940000000
SIZE = 0x180000000
OVERLAY = 0x20000000
PAGE = 16384
CONFIG = struct.Struct('<8sQQQQ32s')
HEADER = struct.Struct('<8sIIQIIQQQ32s32s')


def load_record(report_path):
    report_path = Path(report_path)
    record = json.loads(report_path.read_text(encoding='utf-8'))
    if record.get('full_round_trip_verified') is not True:
        raise ValueError('A complete packed-root round-trip verification is required')
    packed = Path(record['packed_file'])
    if not packed.is_absolute():
        packed = report_path.resolve().parent / packed
    count = packed.stat().st_size
    if count != record['packed_bytes'] or count < 4096 or (count + PAGE - 1) // PAGE * PAGE + OVERLAY > SIZE:
        raise ValueError('Packed root plus the bounded overlay does not fit guarded external RAM')
    digest = bytes.fromhex(record['packed_sha256'])
    raw_digest = bytes.fromhex(record['raw_sha256'])
    if len(digest) != 32 or len(raw_digest) != 32:
        raise ValueError('Expected SHA256 hashes')
    with packed.open('rb') as f:
        header = f.read(4096)
        magic, version, header_bytes, logical, block, blocks, index_off, data_off, total, raw, index_sha = HEADER.unpack_from(header)
        if (magic, version, header_bytes, block, index_off) != (b'VMRAM001', 1, 4096, 1048576, 4096):
            raise ValueError('Unsupported packed-root header')
        if not 0 < logical <= 0x400000000 or logical % 4096 or blocks != (logical + block - 1) // block:
            raise ValueError('Invalid logical root length')
        if total != count or raw != raw_digest or logical != record['logical_bytes']:
            raise ValueError('Packed-root header and verified record differ')
        if data_off != (4096 + blocks * 32 + 4095) // 4096 * 4096 or data_off > count or any(header[HEADER.size:]):
            raise ValueError('Invalid packed-root index bounds')
        index = f.read(blocks * 32)
        if len(index) != blocks * 32 or hashlib.sha256(index).digest() != index_sha:
            raise ValueError('Packed-root index SHA256 mismatch')
    return record, packed, CONFIG.pack(b'VMRC0001', count, BASE, SIZE, OVERLAY, digest)


def file_sha256(file, progress=None):
    digest = hashlib.sha256()
    n = 0
    while data := file.read(8 * 1024 * 1024):
        digest.update(data)
        n += len(data)
        if progress:
            progress(n)
    return digest.hexdigest()
