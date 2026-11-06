from pathlib import Path
import contextlib,io,json,sys,tempfile,time,unittest
from unittest.mock import patch
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import kernel_log_capture as log
import send_ram_fastboot as sender

class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.path=Path(self.temp.name)
        # These are transport failure-injection tests, independent of a USB
        # library installation. Live capture still requires real pyserial.
        if log.serial is None:
            backend=SimpleNamespace(Serial=None,SerialException=OSError)
            p=patch.object(log,'serial',backend);p.start();self.addCleanup(p.stop)
            p=patch.object(log,'list_ports',SimpleNamespace(comports=lambda: []));p.start();self.addCleanup(p.stop)
    def test_binary_rotation_preserves_every_byte(self):
        raw=log.RawLog(self.path,17);data=bytes(range(256))*3
        for n in range(0,len(data),13):raw.write(data[n:n+13])
        raw.close();self.assertEqual(b''.join(p.read_bytes() for p in sorted(self.path.glob('kernel-*.log'))),data)
        self.assertEqual(raw.total,len(data))
    def test_chunked_unicode_and_evidence(self):
        analyzer=log.LogAnalysis(self.path)
        data=('中文日志\nVERMEER RAM: compressed md0 registered\nCRASH demo reason 3\nVERMEER DEBUG: dropped=7\nVERMEER LOG: capacity=8388608 retained-from=19\n').encode()
        for b in data:analyzer.feed(bytes([b]))
        analyzer.finish();report=analyzer.report()
        self.assertEqual(report['last_lines'][0],'中文日志');self.assertIn('md0_registered',report['observations'])
        self.assertFalse(report['desktop_verified']);self.assertEqual(report['max_reported_dropped_bytes'],7)
        self.assertEqual(report['max_reported_retained_from'],19)
    def test_port_identity(self):
        ports=[SimpleNamespace(device='COM1',vid=0x1209,pid=1,serial_number='unrelated'),
               SimpleNamespace(device='COM2',vid=0x1209,pid=1,serial_number='K70DBG004')]
        with patch.object(log.list_ports,'comports',return_value=ports):self.assertEqual(log.matching_ports(),['COM2'])
    def test_no_device_is_not_success(self):
        cap=log.Capture(self.path)
        with patch.object(log,'matching_ports',return_value=[]):
            cap.start();time.sleep(.02);result=cap.finish()
        self.assertFalse(result['connected']);self.assertEqual(result['bytes'],0)
    def test_disconnect_keeps_raw_data(self):
        cap=log.Capture(self.path);payload=b'VERMEER LOG: 8 MiB volatile console\nCRASH before disconnect\n'
        class Port:
            in_waiting=100
            def __init__(self,**kw):self.n=0;self.writes=[]
            def open(self):pass
            def close(self):pass
            def read(self,n):
                self.n+=1
                if self.n==1:return payload
                raise log.serial.SerialException('test cable disconnected')
            def write(self,b):self.writes.append(b);return len(b)
        with patch.object(log,'matching_ports',return_value=['COM2']),patch.object(log.serial,'Serial',Port),contextlib.redirect_stdout(io.StringIO()):
            cap.start();cap.thread.join(2);result=cap.finish()
        self.assertEqual((self.path/'kernel-000.log').read_bytes(),payload)
        self.assertTrue(result['disconnected']);self.assertIn('test cable',result['error'])
        self.assertFalse(result['desktop_verified'])
    def test_loader_hex_chunks_and_limits(self):
        data=bytes(range(67))
        class Fastboot:
            def getvar(self,serial,key):
                if key=='boot-log-size':return f'{len(data):08x}'
                offset=int(key[len('boot-log-'):],16);return data[offset:offset+24].hex()
        self.assertEqual(sender.collect_loader_log(Fastboot(),self.path,'test'),len(data))
        self.assertEqual((self.path/'loader-test.log').read_bytes(),data)
        with patch.object(Fastboot,'getvar',return_value='00008001'):
            with self.assertRaises(RuntimeError):sender.collect_loader_log(Fastboot(),self.path,'bad')
    def test_timeout_saves_partial_stderr(self):
        fb=sender.Fastboot('unused',self.path/'fastboot.jsonl')
        error=sender.subprocess.TimeoutExpired('unused',30,output=b'partial out',stderr=b'partial error')
        with patch.object(sender.subprocess,'run',side_effect=error):
            with self.assertRaises(RuntimeError):fb.run(['devices'])
        record=json.loads((self.path/'fastboot.jsonl').read_text())
        self.assertIn('partial error',record['output'])

    def test_rotation_rejects_nonpositive_limit_and_write_after_close(self):
        for limit in (0,-1,True,1.5):
            with self.subTest(limit=limit),self.assertRaises(ValueError):log.RawLog(self.path,limit)
        raw=log.RawLog(self.path);raw.close();raw.close()
        with self.assertRaises(ValueError):raw.write(b'no')

    def test_huge_line_with_newline_is_bounded_and_not_evidence(self):
        analyzer=log.LogAnalysis(self.path)
        analyzer.feed(log.PROFILE_LINE.encode()+b'x'*(3*log.MAX_LINE)+b'\nnormal\n')
        report=analyzer.report()
        self.assertEqual(report['analysis_truncated_lines'],1)
        self.assertLess(len(report['last_lines'][0]),log.MAX_LINE+100)
        self.assertFalse(analyzer.logging_profile_seen)
        self.assertEqual(report['last_lines'][-1],'normal')

    def test_unterminated_line_stays_bounded_across_reads(self):
        analyzer=log.LogAnalysis(self.path)
        for _ in range(100):
            analyzer.feed(b'x'*1000)
            self.assertLessEqual(len(analyzer.pending),log.MAX_LINE)
        analyzer.finish()
        self.assertEqual(analyzer.report()['analysis_truncated_lines'],1)

    def test_bad_counters_cannot_kill_capture(self):
        analyzer=log.LogAnalysis(self.path)
        analyzer.feed(('dropped='+'9'*5000+'\nretained-from=18446744073709551616\n').encode())
        self.assertEqual(analyzer.max_dropped,0);self.assertEqual(analyzer.max_retained_from,0)
        analyzer.feed(b'dropped=18446744073709551615\nretained-from=13\n')
        self.assertEqual(analyzer.max_dropped,2**64-1);self.assertEqual(analyzer.max_retained_from,13)

    def test_queries_require_complete_profile_line(self):
        for line in ('prefix '+log.PROFILE_LINE,log.PROFILE_LINE+' stale','VERMEER LOG: 8 MiB volatile console'):
            analyzer=log.LogAnalysis(self.path);analyzer.feed((line+'\n').encode())
            self.assertFalse(analyzer.logging_profile_seen)
        analyzer.feed((log.PROFILE_LINE+'\r\n').encode());self.assertTrue(analyzer.logging_profile_seen)

    def run_port(self,port):
        cap=log.Capture(self.path)
        with patch.object(log,'matching_ports',return_value=['/dev/cu.test']),patch.object(log.serial,'Serial',return_value=port),contextlib.redirect_stdout(io.StringIO()):
            cap.start();cap.thread.join(2);result=cap.finish()
        return cap,result

    def test_unexpected_backend_error_is_not_success(self):
        payload=b'normal kernel line\n'
        port=SimpleNamespace(in_waiting=10,open=lambda:None,close=lambda:None)
        reads=iter([payload])
        def read(n):
            try:return next(reads)
            except StopIteration:raise ValueError('backend state invalid')
        port.read=read
        cap,result=self.run_port(port)
        self.assertIn('backend state invalid',result['error'])
        self.assertEqual((self.path/'kernel-000.log').read_bytes(),payload)
        self.assertTrue(cap.raw.closed)

    def test_partial_query_stops_without_retry(self):
        writes=[]
        def write(data):writes.append(data);return 2
        port=SimpleNamespace(in_waiting=100,open=lambda:None,close=lambda:None,
                             read=lambda n:(log.PROFILE_LINE+'\n').encode(),write=write)
        _,result=self.run_port(port)
        self.assertIn('Partial USB status query',result['error']);self.assertEqual(len(writes),1)

    def test_close_failure_keeps_error_and_flushes_tail(self):
        payload='最后一行'.encode()
        port=SimpleNamespace(in_waiting=10,open=lambda:None)
        reads=iter([payload])
        def read(n):
            try:return next(reads)
            except StopIteration:raise log.serial.SerialException('disconnected')
        def close():raise OSError('close failure')
        port.read=read;port.close=close
        cap,result=self.run_port(port)
        self.assertIn('disconnected',result['error']);self.assertTrue(cap.raw.closed)
        self.assertEqual(result['last_lines'],['最后一行'])

    def test_event_write_failure_is_reported(self):
        cap=log.Capture(self.path)
        with patch.object(cap,'event',side_effect=OSError('disk full')):
            cap.start();cap.thread.join(2);result=cap.finish()
        self.assertEqual(result['error'],'disk full');self.assertTrue(cap.raw.closed)

    def test_replay_preserves_unicode_across_files_without_usb(self):
        first=self.path/'kernel-000.log';second=self.path/'kernel-001.log'
        data='中文日志\nCRASH final'.encode();first.write_bytes(data[:2]);second.write_bytes(data[2:])
        with patch.object(log,'serial',None),patch.object(log,'list_ports',None),patch.object(log,'matching_ports',side_effect=AssertionError('USB accessed')):
            result=log.replay([first,second],self.path/'replay')
        self.assertEqual(result['last_lines'],['中文日志','CRASH final'])
        self.assertEqual(result['bytes'],len(data));self.assertFalse(result['handset_commands_issued'])
        self.assertEqual(first.read_bytes()+second.read_bytes(),data)
        with self.assertRaises(FileExistsError):log.replay([first,second],self.path/'replay')

    def test_missing_serial_fails_before_starting_worker(self):
        cap=log.Capture(self.path)
        with patch.object(log,'serial',None),self.assertRaises(RuntimeError):cap.start()
        self.assertFalse(cap.thread.is_alive())

if __name__=='__main__':unittest.main()
