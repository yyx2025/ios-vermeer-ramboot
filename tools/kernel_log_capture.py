"""Capture the exact K70 CDC device, preserving bytes separately from events."""
from datetime import datetime, timezone
from pathlib import Path
import codecs,json,re,threading,time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = list_ports = None

PROFILE_LINE = 'VERMEER LOG: 8 MiB volatile console; preload logging before networking'
MAX_LINE = 16384

def require_serial():
    if serial is None or list_ports is None:
        raise RuntimeError('USB capture requires pyserial; offline log replay does not')

def now():return datetime.now(timezone.utc).isoformat()

class RawLog:
    def __init__(self,folder,limit=32*1024*1024):
        if not isinstance(limit,int) or isinstance(limit,bool) or limit<=0:
            raise ValueError('Log rotation limit must be a positive integer')
        self.folder=Path(folder);self.limit=limit;self.part=0;self.size=0;self.total=0;self.stream=None
        self.closed=False
    def write(self,data):
        if self.closed:raise ValueError('Raw log is closed')
        while data:
            if self.stream is None:
                self.stream=(self.folder/f'kernel-{self.part:03d}.log').open('xb')
            n=min(len(data),self.limit-self.size)
            self.stream.write(data[:n]);self.stream.flush();data=data[n:];self.size+=n;self.total+=n
            if self.size==self.limit:
                self.stream.close();self.stream=None;self.part+=1;self.size=0
    def close(self):
        self.closed=True
        if self.stream:
            stream,self.stream=self.stream,None
            stream.close()

class LogAnalysis:
    def __init__(self,folder):
        self.folder=Path(folder);self.decoder=codecs.getincrementaldecoder('utf-8')('replace');self.pending=''
        self.observations={};self.warning_count=0;self.last_lines=[];self.max_dropped=0;self.max_retained_from=0
        self.logging_profile_seen=False
        self.line_truncated=False;self.truncated_lines=0
    def feed(self,data):
        # Bound decoding and pending text even if a huge line ends in this read.
        # RawLog retains all bytes; incomplete lines cannot establish evidence.
        for offset in range(0,len(data),MAX_LINE):
            self._text(self.decoder.decode(data[offset:offset+MAX_LINE]))
    def _text(self,text):
        while text:
            before,separator,text=text.partition('\n')
            room=MAX_LINE-len(self.pending)
            self.pending+=before[:room]
            self.line_truncated |= len(before)>room
            if separator:self._end_line()
    def _end_line(self):
        line,truncated=self.pending.rstrip('\r'),self.line_truncated
        self.pending='';self.line_truncated=False
        self.line(line,truncated=truncated)
    def line(self,line,truncated=False):
        if truncated:
            self.truncated_lines+=1
            self.last_lines=(self.last_lines+[line+' [analysis truncated; see raw log]'])[-80:]
            return
        self.last_lines=(self.last_lines+[line])[-80:]
        if line==PROFILE_LINE:self.logging_profile_seen=True
        markers={'xnu_preload_hash_pass':'complete packed SHA256/index/HFS header verified',
                 'md0_registered':'compressed md0 registered',
                 'network_init_completed':'network initialization complete; finishing checked root receive',
                 'display_first_present':'VERMEER FB: first present',
                 'physical_touch_ready':'VERMEER TOUCH: physical GT9916 input ready',
                 'log_worker_started':'preloaded-root log worker owns DMA before networking'}
        for key,marker in markers.items():
            if marker in line:self.observations.setdefault(key,{'time':now(),'line':line})
        for key,attr in [('dropped','max_dropped'),('retained-from','max_retained_from')]:
            m=re.search(r'\b'+key+r'=([0-9]{1,20})(?![0-9])',line)
            if m and int(m[1])<=0xffffffffffffffff:
                setattr(self,attr,max(getattr(self,attr),int(m[1])))
        if re.search(r'panic|STOPPED|CRASH|error|fail|refus|unavailable|VERMEER NET:',line,re.I):
            self.warning_count+=1
            with (self.folder/'error-lines.txt').open('a',encoding='utf-8') as f:f.write(line+'\n')
    def finish(self):
        self._text(self.decoder.decode(b'',final=True))
        if self.pending or self.line_truncated:self._end_line()
    def report(self):
        return {'observations':self.observations,'warning_lines':self.warning_count,
                'max_reported_dropped_bytes':self.max_dropped,'max_reported_retained_from':self.max_retained_from,
                'last_lines':self.last_lines,'desktop_verified':False,'gpu_verified':False,
                'analysis_truncated_lines':self.truncated_lines,
                'completeness':'Not guaranteed: pre-USB failures, ring overwrite and panic-time USB may be missing.'}

def matching_ports():
    require_serial()
    return [p.device for p in list_ports.comports() if p.vid==0x1209 and p.pid==1 and p.serial_number=='K70DBG004']

class Capture:
    def __init__(self,folder):
        self.folder=Path(folder);self.stop=threading.Event();self.connected=threading.Event()
        self.raw=RawLog(folder);self.analysis=LogAnalysis(folder);self.error=None;self.disconnect=False
        self.thread=threading.Thread(target=self.run,name='K70 log capture',daemon=True)
    def event(self,kind,**fields):
        with (self.folder/'capture-events.jsonl').open('a',encoding='utf-8') as f:
            f.write(json.dumps({'time':now(),'event':kind,**fields},ensure_ascii=False)+'\n')
    def run(self):
        port=None
        try:
            self.event('waiting',expected='1209:0001 / K70DBG004')
            while not self.stop.is_set():
                candidates=matching_ports()
                if len(candidates)>1:raise RuntimeError('Multiple K70 log ports; refusing ambiguous capture')
                if not candidates:self.stop.wait(.5);continue
                try:
                    port=serial.Serial(port=None,baudrate=115200,timeout=.25,write_timeout=1)
                    port.port=candidates[0];port.dtr=True;port.rts=True;port.open()
                except (OSError,serial.SerialException) as e:
                    if port:port.close();port=None
                    self.event('open_failed',detail=str(e));self.stop.wait(2);continue
                self.connected.set();self.event('connected',port=candidates[0])
                print('K70 USB log connected. Saving raw kernel logs to '+str(self.folder),flush=True)
                next_status=time.monotonic()
                while not self.stop.is_set():
                    data=port.read(min(max(port.in_waiting,1),16384))
                    if data:self.raw.write(data);self.analysis.feed(data)
                    # Only this new log-only profile can receive these commands.
                    # Never send a text query into the earlier binary root receiver.
                    if self.analysis.logging_profile_seen and time.monotonic()>=next_status:
                        query=b'status\nframe\ninput\n'
                        if port.write(query)!=len(query):
                            raise RuntimeError('Partial USB status query; capture stopped')
                        next_status=time.monotonic()+10
                break
        except Exception as e:
            # Backend, decoder and cleanup failures must not report success just
            # because the now-dead worker saved some bytes before failing.
            self.error=str(e);self.disconnect=self.connected.is_set()
            if not self.error:self.error=type(e).__name__
        finally:
            cleanup=[self.raw.close,self.analysis.finish]
            if port:cleanup.insert(0,port.close)
            for action in cleanup:
                try:action()
                except Exception as e:self.error=self.error or str(e) or type(e).__name__
            try:
                if self.error:self.event('capture_stopped',error=self.error,connected_before=self.connected.is_set())
                self.event('finished',bytes=self.raw.total)
            except Exception as e:self.error=self.error or str(e) or type(e).__name__
    def start(self):
        require_serial()
        self.thread.start()
    def finish(self):
        self.stop.set();self.thread.join(5)
        if self.thread.is_alive():raise RuntimeError('Log reader did not stop; keep this process open')
        result={'connected':self.connected.is_set(),'bytes':self.raw.total,'error':self.error,
                'disconnected':self.disconnect,**self.analysis.report()}
        (self.folder/'kernel-summary.json').write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
        return result
    def wait_after_handoff(self,seconds):
        print(f'Waiting/capturing K70 kernel logs for up to {seconds}s. Ctrl+C stops capture; saved files remain.',flush=True)
        until=time.monotonic()+seconds;next_notice=0
        while time.monotonic()<until and self.thread.is_alive():
            if time.monotonic()>=next_notice:
                print('Kernel bytes saved: '+str(self.raw.total),flush=True);next_notice=time.monotonic()+30
            self.stop.wait(.5)

def replay(paths,folder):
    """Analyze existing logs in caller-supplied order without opening USB."""
    paths=[Path(p) for p in paths]
    if not paths or any(not p.is_file() for p in paths):
        raise ValueError('Replay requires existing raw log files in capture order')
    folder=Path(folder);folder.mkdir(parents=True,exist_ok=False)
    analysis=LogAnalysis(folder);total=0
    for path in paths:
        with path.open('rb') as stream:
            while data:=stream.read(MAX_LINE):
                total+=len(data);analysis.feed(data)
    analysis.finish()
    result={'mode':'offline_replay','sources':[str(p.resolve()) for p in paths],
            'bytes':total,'handset_commands_issued':False,**analysis.report()}
    (folder/'kernel-summary.json').write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8')
    return result

def main(argv=None):
    import argparse
    p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--seconds',type=int,default=1200)
    p.add_argument('--replay',type=Path,nargs='+',help='Analyze raw logs in supplied order without USB or pyserial')
    a=p.parse_args(argv)
    if not 30<=a.seconds<=86400:p.error('seconds must be 30..86400')
    if a.replay:
        result=replay(a.replay,a.logs);print(json.dumps(result,ensure_ascii=True))
        return 0 if result['bytes'] else 1
    require_serial()
    a.logs.mkdir(parents=True,exist_ok=False);cap=Capture(a.logs);cap.start()
    try:cap.wait_after_handoff(a.seconds)
    except KeyboardInterrupt:pass
    finally:result=cap.finish();print(json.dumps(result,ensure_ascii=True))
    return 0 if result['bytes'] and not result['error'] else 1

if __name__=='__main__':
    raise SystemExit(main())
