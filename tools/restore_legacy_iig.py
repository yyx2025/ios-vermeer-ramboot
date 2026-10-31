"""Reuse pinned open-source IIG output from the successful XNU-8796 build.

New Xcode IIG emits a newer IORPC ABI. No DriverKit definitions change in the
RAM stream profile. Keep the exact generated source and verify each definition
before copying its header/implementation; no tool binary is redistributed.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ARTIFACT = 13036539107
SHA256 = '87b206ef93cacd562edd96de12a77464842dfb441fcbae7e554d3f1b57082945'


class Redirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, url):
        result = super().redirect_request(req, fp, code, msg, headers, url)
        if result:
            result.remove_header('Authorization')
        return result


def prepare(dest, archive=None):
    dest = Path(dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    packaged = Path(__file__).resolve().parent / 'legacy_iig_cache'
    if not archive and (packaged / 'manifest.json').exists():
        manifest = json.loads((packaged / 'manifest.json').read_text())
        if len(manifest) != 17:
            raise ValueError('Expected the 17 pinned legacy IIG source pairs')
        for stem, entry in manifest.items():
            if not stem.isidentifier():
                raise ValueError('Invalid legacy IIG source name')
            for suffix, key in (('.h', 'header_sha256'), ('.iig.cpp', 'implementation_sha256')):
                file = packaged / (stem + suffix)
                if hashlib.sha256(file.read_bytes()).hexdigest() != entry[key]:
                    raise ValueError('Packaged legacy IIG source checksum mismatch')
                shutil.copyfile(file, dest / file.name)
        shutil.copyfile(packaged / 'manifest.json', dest / 'manifest.json')
        print('Verified permanent legacy IIG source pairs:', len(manifest), flush=True)
        return install_wrapper(dest)
    if archive:
        raw = Path(archive).read_bytes()
    else:
        request = urllib.request.Request(
            f'https://api.github.com/repos/yyx2025/ios-vermeer-ramboot/actions/artifacts/{ARTIFACT}/zip',
            headers={'Authorization': 'Bearer ' + os.environ['GITHUB_TOKEN'],
                     'User-Agent': 'Vermeer-RAM-IIG-compatibility'})
        with urllib.request.build_opener(Redirect()).open(request, timeout=60) as reply:
            raw = reply.read()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise ValueError('Pinned successful build-tree archive SHA256 mismatch')
    files = {}
    with zipfile.ZipFile(io.BytesIO(raw)) as z:
        with z.open('buildtree.tgz') as stream, tarfile.open(fileobj=stream, mode='r|gz') as tar:
            for member in tar:
                if member.isfile() and ('DriverKit/' in member.name or member.name.endswith('.iig.cpp')):
                    if member.size > 4 * 1024 * 1024:
                        raise ValueError('Unexpected generated source length')
                    files[member.name] = tar.extractfile(member).read()
    manifest = {}
    for name, data in files.items():
        if not name.endswith('.iig.cpp'):
            continue
        stem = Path(name).name.removesuffix('.iig.cpp')
        definition = files.get('xnu/iokit/DriverKit/' + stem + '.iig')
        header = files.get(str(Path(name).with_name(stem + '.h')).replace('\\', '/'))
        if header is None:
            candidates = [b for p, b in files.items() if p.endswith('/DriverKit/' + stem + '.h')]
            if candidates and all(b == candidates[0] for b in candidates):
                header = candidates[0]
        if definition is None or header is None:
            raise ValueError('Generated header or original definition is missing: ' + stem)
        entry = {'definition_sha256': hashlib.sha256(definition).hexdigest(),
                 'header_sha256': hashlib.sha256(header).hexdigest(),
                 'implementation_sha256': hashlib.sha256(data).hexdigest()}
        if stem in manifest and manifest[stem] != entry:
            raise ValueError('Conflicting IIG source configurations: ' + stem)
        manifest[stem] = entry
        (dest / (stem + '.h')).write_bytes(header)
        (dest / (stem + '.iig.cpp')).write_bytes(data)
    if not manifest:
        raise ValueError('Successful build archive contains no legacy generated IIG output')
    (dest / 'manifest.json').write_text(json.dumps(manifest, indent=2))
    print('Verified legacy IIG source pairs:', len(manifest), flush=True)
    return install_wrapper(dest)


def install_wrapper(dest):
    wrapper = dest / 'iig'
    wrapper.write_text('''#!/usr/bin/env python3
import hashlib,json
from pathlib import Path
import sys
root=Path(__file__).resolve().parent
args=sys.argv[1:]
def option(name):return args[args.index(name)+1]
definition=Path(option('--def'))
stem=definition.stem
entry=json.loads((root/'manifest.json').read_text())[stem]
if hashlib.sha256(definition.read_bytes()).hexdigest()!=entry['definition_sha256']:
    raise SystemExit('Legacy IIG definition changed; cached output refused: '+stem)
for option_name,suffix,key in (('--header','.h','header_sha256'),('--impl','.iig.cpp','implementation_sha256')):
    data=(root/(stem+suffix)).read_bytes()
    if hashlib.sha256(data).hexdigest()!=entry[key]:raise SystemExit('Corrupt cached IIG source')
    Path(option(option_name)).write_bytes(data)
print('Pinned Xcode15 IIG source: '+stem)
''')
    wrapper.chmod(0o755)
    return wrapper


if __name__ == '__main__':
    prepare(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
