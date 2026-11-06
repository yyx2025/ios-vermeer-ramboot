"""Exercise the portable entry point without device access or a real root image."""
from pathlib import Path
import contextlib
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import k70_session as session


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name);self.package=self.path/'readonly-package';self.package.mkdir()
        self.logs=self.path/'writable-logs'

    def test_default_reports_all_missing_artifacts_without_devices(self):
        with patch.object(session.shutil,'which',return_value=None),patch.object(session.sender.Fastboot,'run',side_effect=AssertionError('device accessed')),contextlib.redirect_stdout(io.StringIO()):
            result=session.main(['--package',str(self.package),'--logs',str(self.logs)])
        self.assertEqual(result,1);self.assertEqual(list(self.package.iterdir()),[])
        report=json.loads(next(self.logs.glob('preflight-*.json')).read_text())
        self.assertEqual(set(report['errors']),{'image','payload','kernel_build','fastboot'})
        self.assertFalse(report['local_preflight_passed']);self.assertFalse(report['handset_commands_issued'])

    @unittest.skipIf(os.name=='nt','Native POSIX executable checks')
    def test_windows_binary_and_nonexecutable_rejected(self):
        candidate=self.path/'fastboot';candidate.write_bytes(b'MZwindows');candidate.chmod(0o755)
        with self.assertRaises(ValueError):session.fastboot_path(self.package,candidate)
        candidate.write_bytes(b'#!/bin/sh\nexit 0\n');candidate.chmod(0o644)
        with self.assertRaises(ValueError):session.fastboot_path(self.package,candidate)
        candidate.chmod(0o755);self.assertEqual(session.fastboot_path(self.package,candidate),candidate.resolve())

    def test_wrong_explicit_fastboot_does_not_fall_back(self):
        with patch.object(session.shutil,'which') as search,self.assertRaises(ValueError):
            session.fastboot_path(self.package,self.path/'missing')
        search.assert_not_called()

    def test_root_hash_is_checked_even_when_image_is_missing(self):
        root=self.package/'complete-root.vram';root.write_bytes(b'packed-root-test')
        record={'packed_sha256':hashlib.sha256(root.read_bytes()).hexdigest(),'packed_bytes':root.stat().st_size}
        with patch.object(session,'load_record',return_value=(record,root,b'')),patch.object(session.shutil,'which',return_value=None),contextlib.redirect_stdout(io.StringIO()):
            report=session.inspect_package(self.package)
            self.assertTrue(report['root_sha256_verified']);self.assertFalse(report['local_preflight_passed'])
            root.write_bytes(b'corrupt');report=session.inspect_package(self.package)
            self.assertFalse(report['root_sha256_verified']);self.assertIn('SHA256 mismatch',report['errors']['payload'])

    def test_kernel_provenance_mismatch_rejected(self):
        build={'sha256':'a'*64,'commit':'b'*40,'run':123}
        image={'kernel_sha256':build['sha256'],'kernel_source_commit':build['commit'],'kernel_build_run':123,'kernel_rebuilt':True}
        (self.package/'kernel-build.json').write_text(json.dumps(build))
        for key in image:
            changed={**image,key:None};(self.package/'vermeer-fullroot-logs.json').write_text(json.dumps(changed))
            with self.subTest(key=key),self.assertRaises(ValueError):session.check_build(self.package)
        (self.package/'vermeer-fullroot-logs.json').write_text(json.dumps(image))
        self.assertEqual(session.check_build(self.package),build)

    def test_capture_does_not_require_root_image_or_fastboot(self):
        import kernel_log_capture as capture
        with patch.object(capture,'require_serial'),patch.object(capture,'main',return_value=0) as run,patch.object(session,'check_build',side_effect=AssertionError('unneeded build')),patch.object(session,'fastboot_path',side_effect=AssertionError('unneeded fastboot')):
            result=session.main(['--package',str(self.package),'--logs',str(self.logs),'--capture-only'])
        self.assertEqual(result,0);self.assertIn('--seconds',run.call_args.args[0])

    def test_boot_refuses_incomplete_package_before_sender(self):
        import kernel_log_capture as capture
        with patch.object(capture,'require_serial'),patch.object(session.sender,'main') as run,contextlib.redirect_stderr(io.StringIO()):
            result=session.main(['--package',str(self.package),'--logs',str(self.logs),'--boot'])
        self.assertEqual(result,1);run.assert_not_called()


if __name__=='__main__':unittest.main()
