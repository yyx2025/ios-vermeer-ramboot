"""Exercise the real host sender with a fake Fastboot transport; no device access."""
from pathlib import Path
import contextlib, hashlib, io, json, struct, sys, tempfile, unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import send_ram_fastboot as host

class Fake:
    def __init__(self,data):
        self.data=data;self.sha=hashlib.sha256(data).hexdigest();self.sent=b'';self.calls=[];self.override={};self.fail_stage=False
    def getvar(self,serial,key):
        assert serial==host.SERIAL;self.calls.append(('getvar',key))
        values={'product':'vermeer','version-bootloader':host.VERSION,'serialno':host.SERIAL,
            'ram-total':f'{len(self.data):016x}','ram-received':f'{len(self.sent):016x}',
            'ram-sha256-lo':self.sha[:32],'ram-sha256-hi':self.sha[32:],'max-download-size':'0x04000000'}
        return self.override.get(key,values[key])
    def run(self,args,timeout=30):
        assert args[:2]==['-s',host.SERIAL];self.calls.append(tuple(args[2:]))
        if args[2]=='stage':
            if self.fail_stage: raise RuntimeError('simulated transfer failure')
            self.sent+=Path(args[3]).read_bytes();return 'Sending OKAY'
        assert args[2:]==['oem','ram-boot']
        assert self.sent==self.data
        return '(bootloader) root SHA256 verified\nOKAY [ 0.001s ]'

class SenderTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.logs=Path(self.tmp.name);self.packed=self.logs/'root.vram'
        self.data=bytes(range(251))*41;self.packed.write_bytes(self.data);self.fb=Fake(self.data);self.state={}
    def upload(self):
        with contextlib.redirect_stdout(io.StringIO()):
            host.upload(self.fb,self.packed,len(self.data),self.fb.sha,self.logs,self.state,chunk=1024)
    def test_complete_and_order(self):
        self.upload();self.assertEqual(self.fb.sent,self.data);self.assertTrue(self.state['loader_sha256_confirmed'])
        self.assertEqual(self.fb.calls[-1],('oem','ram-boot'));self.assertFalse(list(self.logs.glob('ram-chunk-*')))
        for i,c in enumerate(self.fb.calls):
            if c[0]=='stage': self.assertEqual(self.fb.calls[i+1],('getvar','ram-received'))
    def test_wrong_identity_or_root_sends_nothing(self):
        for key in ('product','version-bootloader','serialno','ram-total','ram-received','ram-sha256-lo','ram-sha256-hi','max-download-size'):
            self.fb=Fake(self.data);self.fb.override[key]='0'
            with self.assertRaises(RuntimeError):self.upload()
            self.assertFalse(self.fb.sent);self.assertFalse(any(c[0]=='stage' for c in self.fb.calls))
    def test_transfer_failure_does_not_boot(self):
        self.fb.fail_stage=True
        with self.assertRaises(RuntimeError):self.upload()
        self.assertNotIn(('oem','ram-boot'),self.fb.calls)
    def test_corruption_and_truncation_do_not_boot(self):
        for changed in (self.data[:-1],bytes([255])+self.data[1:],self.data+b'X'):
            self.fb=Fake(self.data);self.packed.write_bytes(changed)
            with self.assertRaises(RuntimeError):self.upload()
            self.assertNotIn(('oem','ram-boot'),self.fb.calls)
    def test_progress_mismatch_does_not_boot(self):
        original=self.fb.getvar
        def wrong(s,k):
            if k=='ram-received' and self.fb.sent:return '0000000000000000'
            return original(s,k)
        self.fb.getvar=wrong
        with self.assertRaises(RuntimeError):self.upload()
        self.assertNotIn(('oem','ram-boot'),self.fb.calls)
    def test_command_filter(self):
        fb=host.Fastboot('unused',self.logs/'commands.jsonl')
        with patch.object(host.subprocess,'run') as run:
            for cmd in (['flash','boot','x'],['-s','stock','erase','userdata'],['-s','stock','stage','x'],['-s',host.SERIAL,'boot','x'],['-s',host.SERIAL,'oem','unlock']):
                with self.assertRaises(ValueError):fb.run(cmd)
            run.assert_not_called()
    def test_getvar_parser(self):
        fb=host.Fastboot('unused',self.logs/'commands.jsonl')
        for output in ('product: vermeer\nFinished. Total time: 0.0s\n','(bootloader) product: vermeer\n'):
            with patch.object(fb,'run',return_value=output):self.assertEqual(fb.getvar('stock','product'),'vermeer')
        with patch.object(fb,'run',return_value='product: one\nproduct: two\n'):
            with self.assertRaises(RuntimeError):fb.getvar('stock','product')
    def test_default_main_never_calls_fastboot(self):
        args=['sender','--image',str(self.packed),'--root-record','unused','--fastboot',str(self.packed),'--logs',str(self.logs)]
        with patch.object(sys,'argv',args),patch.object(host,'preflight',return_value=({'sha256':'image'},{'packed_sha256':self.fb.sha},self.packed)),patch.object(host.Fastboot,'run') as run,contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(host.main(),0);run.assert_not_called()
    def test_failed_preflight_never_calls_fastboot_even_with_boot(self):
        args=['sender','--image',str(self.packed),'--root-record','unused','--fastboot',str(self.packed),'--logs',str(self.logs),'--boot']
        with patch.object(sys,'argv',args),patch.object(host,'preflight',side_effect=ValueError('bad image')),patch.object(host,'save_windows_usb'),patch.object(host.Fastboot,'run') as run,contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(host.main(),1);run.assert_not_called()

if __name__=='__main__':unittest.main()
