"""Send a verified complete root to the pre-mount RAM receiver and retain logs.

This program issues serial frames only. It never boots, reboots or flashes a
phone. Start it before manually running fastboot boot with the matching image.
"""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import time
import zlib
from ram_stream_config import load_record, file_sha256

FRAME = struct.Struct('<8sIIQIIQ32s')
CHUNK = 65536
ACK = re.compile(r'^VMRAM ACK (\d+) (\d+)$')


def frame(op, sequence, offset, total, payload=b'', digest=bytes(32)):
    if not 0 <= sequence <= 0xffffffff or not 0 <= offset <= total < 2**64:
        raise ValueError('Frame sequence or 64-bit length is invalid')
    if op not in (1, 2, 3) or len(payload) > CHUNK or len(digest) != 32:
        raise ValueError('Invalid frame')
    return FRAME.pack(b'VMRX0001', op, sequence, offset, len(payload),
                      zlib.crc32(payload), total, digest) + payload


def write_all(port, data):
    offset = 0
    while offset < len(data):
        n = port.write(data[offset:])
        if not n or n > len(data) - offset:
            raise OSError('Serial write made no valid progress')
        offset += n


class Reader:
    def __init__(self, port, log):
        self.port, self.log = port, log
        self.pending = bytearray()
        self.lines = []

    def line(self, deadline):
        while time.monotonic() < deadline:
            if self.lines:
                return self.lines.pop(0)
            data = self.port.read(max(1, min(self.port.in_waiting, 65536)))
            if not data:
                continue
            self.log.write(data.decode('utf-8', errors='replace'))
            self.log.flush()
            self.pending.extend(data)
            while b'\n' in self.pending:
                value, _, rest = self.pending.partition(b'\n')
                self.pending = bytearray(rest)
                self.lines.append(value.decode('utf-8', errors='replace').strip())
            if len(self.pending) > 65536:
                raise OSError('Unterminated serial log exceeds the limit')
        raise TimeoutError('Timed out waiting for the RAM receiver; see saved log')

    def ready(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            line = self.line(deadline)
            self.check_failure(line)
            print(line, flush=True)
            if 'VERMEER RAM: READY VMRX0001' in line:
                return

    @staticmethod
    def check_failure(line):
        if line.startswith('VMRAM ERROR') or 'VERMEER USB: stopped:' in line or 'pre-mount receive failed:' in line:
            raise OSError(line)

    def ack(self, sequence, received, timeout):
        deadline = time.monotonic() + timeout
        while True:
            line = self.line(deadline)
            self.check_failure(line)
            match = ACK.fullmatch(line)
            if not match:
                continue
            got_seq, got_count = map(int, match.groups())
            if got_seq < sequence:
                continue
            if (got_seq, got_count) != (sequence, received):
                raise OSError(f'Unexpected RAM ACK: {line}')
            return


def send_volume(port, reader, source, total, expected, timeout=30, progress=None):
    write_all(port, frame(1, 0, 0, total, digest=bytes.fromhex(expected)))
    reader.ack(0, 0, timeout)
    sequence, received = 1, 0
    digest = hashlib.sha256()
    while received < total:
        data = source.read(min(CHUNK, total - received))
        if not data:
            raise OSError('Packed root was truncated; transfer stopped before COMMIT')
        write_all(port, frame(2, sequence, received, total, payload=data))
        digest.update(data)
        received += len(data)
        reader.ack(sequence, received, timeout)
        sequence += 1
        if progress:
            progress(received)
    if source.read(1) or digest.hexdigest() != expected:
        raise OSError('Packed root changed during transfer; COMMIT refused')
    write_all(port, frame(3, sequence, received, total))
    reader.ack(sequence, received, timeout)
    return {'acknowledged_bytes': received, 'commit_acknowledged': True,
            'transmitted_sha256': digest.hexdigest()}


def is_vermeer(port):
    return port.serial_number == 'K70DBG004' and port.vid == 0x1209 and port.pid == 1


def find_port(requested, timeout):
    from serial.tools import list_ports
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        candidates = [p for p in list_ports.comports() if is_vermeer(p)]
        if requested:
            candidates = [p for p in candidates if p.device.casefold() == requested.casefold()]
        if len(candidates) > 1:
            raise OSError('Multiple K70DBG004 ports found; select one with --port COMx')
        if candidates:
            return candidates[0].device
        time.sleep(0.5)
    raise TimeoutError('K70DBG004 serial port did not appear; keep the final screen for the USB/SMMU values')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('packed_report', type=Path, nargs='?')
    parser.add_argument('--port')
    parser.add_argument('--list-ports', action='store_true')
    parser.add_argument('--wait-port', type=float, default=300)
    parser.add_argument('--ready-timeout', type=float, default=240)
    parser.add_argument('--ack-timeout', type=float, default=30)
    parser.add_argument('--monitor-seconds', type=float, default=600)
    parser.add_argument('--no-samples', action='store_true', help='disable the two scheduled read-only task/frame/input queries after COMMIT')
    parser.add_argument('--prepared-signal', type=Path, help='write a local readiness marker after the full computer-side SHA256 check')
    parser.add_argument('--log-dir', type=Path, default=Path(__file__).resolve().parents[2] / 'ram-test-logs')
    args = parser.parse_args()
    import serial
    if args.list_ports:
        from serial.tools import list_ports
        for p in list_ports.comports():
            print(p.device, p.description, p.serial_number, 'VERMEER' if is_vermeer(p) else '')
        return
    if not args.packed_report or min(args.wait_port, args.ready_timeout, args.ack_timeout) <= 0 or args.monitor_seconds < 0:
        parser.error('Supply packed-root JSON and positive timeouts')
    record, packed, _ = load_record(args.packed_report)
    args.log_dir.mkdir(parents=True, exist_ok=True)
    stem = 'ram-root-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    log_path = args.log_dir / (stem + '.log')
    report_path = args.log_dir / (stem + '.json')
    result = {'packed_root': str(packed), 'expected_sha256': record['packed_sha256'],
              'commit_acknowledged': False, 'full_ios_desktop_verified': False,
              'storage_commands': False, 'log': str(log_path)}
    try:
        with packed.open('rb') as source:
            print('Checking the complete packed-root SHA256 on the computer...', flush=True)
            if file_sha256(source) != record['packed_sha256']:
                raise ValueError('Packed root SHA256 differs from the verified record')
            source.seek(0)
            if args.prepared_signal:
                with args.prepared_signal.open('x', encoding='utf-8') as marker:
                    json.dump({'packed_sha256':record['packed_sha256'],'ready':True}, marker)
            print('Ready. Waiting for matching K70DBG004 USB after your temporary boot...', flush=True)
            device = find_port(args.port, args.wait_port)
            result['port'] = device
            with serial.Serial(device, 115200, timeout=0.2, write_timeout=args.ack_timeout,
                               rtscts=False, dsrdtr=False) as port, log_path.open('x', encoding='utf-8') as log:
                port.dtr = True
                reader = Reader(port, log)
                reader.ready(args.ready_timeout)
                started, last = time.monotonic(), [0.0]
                def progress(n):
                    now = time.monotonic()
                    if now - last[0] >= 10 or n == record['packed_bytes']:
                        print(f'RAM received {n / 2**30:.2f}/{record["packed_bytes"] / 2**30:.2f} GiB; '
                              f'{n / max(now-started, 0.001) / 2**20:.1f} MiB/s', flush=True)
                        last[0] = now
                result.update(send_volume(port, reader, source, record['packed_bytes'],
                                          record['packed_sha256'], args.ack_timeout, progress))
                print('Complete-root COMMIT acknowledged. Keeping actual boot logs...', flush=True)
                deadline = time.monotonic() + args.monitor_seconds
                samples = [] if args.no_samples else [time.monotonic() + 120, time.monotonic() + 300]
                while time.monotonic() < deadline:
                    if samples and time.monotonic() >= samples[0]:
                        write_all(port, b'sample\nframe\ninput\n')
                        samples.pop(0)
                    try:
                        print(reader.line(min(deadline, time.monotonic() + 1)), flush=True)
                    except TimeoutError:
                        continue
    except (OSError, ValueError, KeyError, KeyboardInterrupt) as error:
        result['error'] = str(error) or 'Interrupted by user'
        print('Stopped:', result['error'], flush=True)
    finally:
        report_path.write_text(json.dumps(result, indent=2), encoding='utf-8')
        print('Saved result:', report_path, flush=True)
    return 0 if result['commit_acknowledged'] and 'error' not in result else 1


if __name__ == '__main__':
    raise SystemExit(main())
