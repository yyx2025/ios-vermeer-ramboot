"""Portable K70 package check and log capture. Default mode never opens USB."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import sys

import send_ram_fastboot as sender
from ram_stream_config import load_record, file_sha256


def fastboot_path(package, explicit=None):
    if explicit:
        candidates = [Path(explicit)]
    else:
        name = 'fastboot.exe' if os.name == 'nt' else 'fastboot'
        candidates = [Path(package) / 'platform-tools' / name]
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for path in candidates:
        if not path.is_file():
            continue
        # The transferred package currently includes Windows platform-tools.
        # A copied executable flag does not make a PE binary run on macOS.
        with path.open('rb') as stream:
            windows_binary = stream.read(2) == b'MZ'
        if os.name != 'nt' and (path.suffix.lower() == '.exe' or windows_binary):
            continue
        if os.name == 'nt' or os.access(path, os.X_OK):
            return path.resolve()
    raise ValueError('Native fastboot is missing; supply --fastboot with this computer\'s platform-tools executable')


def check_build(package):
    """Require the final image's provenance, not a loader-only compile artifact."""
    image = json.loads((package / 'vermeer-fullroot-logs.json').read_text(encoding='utf-8'))
    build = json.loads((package / 'kernel-build.json').read_text(encoding='utf-8'))
    if (not re.fullmatch(r'[0-9a-f]{64}', str(build.get('sha256', '')))
            or not re.fullmatch(r'[0-9a-f]{40}', str(build.get('commit', '')))
            or type(build.get('run')) is not int or build['run'] <= 0
            or image.get('kernel_sha256') != build['sha256']
            or image.get('kernel_source_commit') != build['commit']
            or image.get('kernel_build_run') != build['run']
            or image.get('kernel_rebuilt') is not True):
        raise ValueError('Final image and rebuilt-kernel provenance do not match')
    return build


def inspect_package(package, explicit_fastboot=None):
    package = Path(package).resolve()
    report = {'time': datetime.now(timezone.utc).isoformat(), 'package': str(package),
              'handset_commands_issued': False, 'hardware_verified': False,
              'root_sha256_verified': False, 'image_verified': False,
              'kernel_build_record_verified': False, 'errors': {}}
    root_record = package / 'complete-root.json'
    image = package / 'vermeer-fullroot-logs.img'
    try:
        if image.is_file() and image.with_suffix('.json').is_file():
            info, root, packed = sender.preflight(image, root_record)
            report.update(image_verified=True, image_sha256=info['sha256'])
        else:
            report['errors']['image'] = 'Final retained-log IMG/JSON is missing; loader-compile-only.img is not a replacement'
            root, packed, _ = load_record(root_record)
            print('Checking the complete packed-root SHA256...', flush=True)
            with packed.open('rb') as stream:
                if file_sha256(stream) != root['packed_sha256']:
                    raise ValueError('Complete packed-root SHA256 mismatch')
        report.update(root_sha256_verified=True, root_bytes=root['packed_bytes'],
                      root_sha256=root['packed_sha256'], root_path=str(packed))
    except (OSError, ValueError, KeyError, StopIteration) as error:
        report['errors']['payload'] = str(error)
    try:
        report['kernel_build'] = check_build(package)
        report['kernel_build_record_verified'] = True
    except (OSError, ValueError, KeyError) as error:
        report['errors']['kernel_build'] = str(error)
    try:
        report['fastboot'] = str(fastboot_path(package, explicit_fastboot))
    except (OSError, ValueError) as error:
        report['errors']['fastboot'] = str(error)
    report['local_preflight_passed'] = not report['errors']
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--logs', type=Path, required=True, help='Writable host log directory, separate from a read-only package')
    parser.add_argument('--fastboot', type=Path)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--boot', action='store_true', help='Explicitly run one temporary RAM boot after preflight')
    modes.add_argument('--capture-only', action='store_true', help='Capture an already-running K70DBG004 without Fastboot')
    parser.add_argument('--seconds', type=int, default=1200)
    args = parser.parse_args(argv)
    if not 30 <= args.seconds <= 86400:
        parser.error('seconds must be 30..86400')
    package = args.package.resolve()
    args.logs.mkdir(parents=True, exist_ok=True)
    try:
        if args.capture_only or args.boot:
            # Prefer this source tree's modules; use only the shipped pyserial
            # dependency from the package if no installed copy is available.
            sys.path.append(str(package / 'tools'))
            import kernel_log_capture as capture
            capture.require_serial()
        if args.capture_only:
            session = args.logs / ('capture-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S-%fZ'))
            return capture.main(['--logs', str(session), '--seconds', str(args.seconds)])
        if args.boot:
            check_build(package)
            executable = fastboot_path(package, args.fastboot)
            return sender.main(['--image', str(package / 'vermeer-fullroot-logs.img'),
                                '--root-record', str(package / 'complete-root.json'),
                                '--fastboot', str(executable), '--logs', str(args.logs),
                                '--capture-seconds', str(args.seconds), '--boot'])
        report = inspect_package(package, args.fastboot)
        output = args.logs / ('preflight-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S-%fZ') + '.json')
        with output.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, ensure_ascii=False)
            stream.write('\n')
        print(json.dumps(report, indent=2, ensure_ascii=False))
        print('Preflight report: ' + str(output))
        return 0 if report['local_preflight_passed'] else 1
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print('Stopped: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
