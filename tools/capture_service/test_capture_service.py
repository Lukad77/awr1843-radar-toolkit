"""Real HTTP/native UDP process tests; only hardware control and belt are simulated."""
import csv
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.request import Request,urlopen
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer
import server
from hkh11c import Hkh11cDevice
from test_hkh11c import FakeSerial

def free_port():
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1',0));return s.getsockname()[1]

def wait_for(predicate,timeout=10):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.01)
    raise AssertionError('condition timed out')

class Stream:
    frame=bytes(range(256))*1024
    def __init__(self,port,gap_frame=None):
        self.port=port;self.gap_frame=gap_frame;self.stop=threading.Event();self.sent=0
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def run(self):
        seq=1;offset=0
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
            while not self.stop.is_set():
                for pos in range(0,len(self.frame),1400):
                    chunk=self.frame[pos:pos+1400]
                    if not(self.sent==self.gap_frame and pos==2800):
                        sock.sendto(struct.pack('<I',seq)+offset.to_bytes(6,'little')+chunk,('127.0.0.1',self.port))
                    offset+=len(chunk);seq+=1
                    time.sleep(.00005)
                self.sent+=1
                self.stop.wait(.01)
    def close(self):self.stop.set();self.thread.join(2)

class Acceptance(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=server.ROOT/'build');self.root=Path(self.tmp.name)
        self.session=server.CaptureSession(root=self.root);self.stream=None
        real_popen=subprocess.Popen
        def passive(args,*a,**kw):
            if '--json' in args:args=[*args,'--no-control']
            return real_popen(args,*a,**kw)
        self.patch=patch.object(server.subprocess,'Popen',side_effect=passive);self.patch.start()
        self.http=ThreadingHTTPServer(('127.0.0.1',0),server.Handler);self.http.session=self.session;self.http.token='test-token'
        self.thread=threading.Thread(target=self.http.serve_forever,daemon=True);self.thread.start()
        self.url=f'http://127.0.0.1:{self.http.server_port}';self.port=free_port()
        self.request=dict(cfgPath=str(server.ROOT/'awr1843.windows.cfg'),serial='COM8',bindIp='127.0.0.1',
            dcaIp='127.0.0.1',dataPort=self.port,configPort=free_port(),durationSeconds=0)
    def tearDown(self):
        self.session.stop()
        if self.session.worker:self.session.worker.join(50)
        if self.stream:self.stream.close()
        self.http.shutdown();self.http.server_close();self.thread.join();self.patch.stop();self.tmp.cleanup()
    def post(self,path,body,token='test-token'):
        req=Request(self.url+path,data=json.dumps(body).encode(),headers={'Content-Type':'application/json','X-Capture-Token':token})
        with urlopen(req,timeout=15) as r:return json.load(r)
    def receiver_ready(self):
        wait_for(lambda:any(line.startswith('READY ') for line in self.session.snapshot()['logs']))
    def ready(self,gap_frame=None):
        self.receiver_ready();self.stream=Stream(self.port,gap_frame)
        wait_for(lambda:bool(self.session.snapshot()['formalWindow']))
    def done(self):
        wait_for(lambda:self.session.worker and not self.session.worker.is_alive())
        if self.stream:self.stream.close();self.stream=None
        return self.session.snapshot()
    def rows(self,folder,name):
        with (folder/name).open() as f:return list(csv.DictReader(f))

    def test_http_stop_bin_integrity_and_repeated_sessions(self):
        cfg=self.root/'中文 配置.cfg';cfg.write_bytes((server.ROOT/'awr1843.windows.cfg').read_bytes())
        self.post('/api/start',{**self.request,'cfgPath':str(cfg)});self.ready()
        with self.assertRaises(HTTPError) as busy:self.post('/api/start',self.request)
        self.assertEqual(busy.exception.code,409)
        wait_for(lambda:self.session.snapshot()['frames']>=3)
        self.post('/api/stop',{});result=self.done();self.assertEqual(result['state'],'completed',result)
        folder=Path(result['directory']);r=self.rows(folder,'radar.bin.frames.csv');formal=self.rows(folder,'radar_formal.frames.csv')
        self.assertEqual((folder/'radar.bin').read_bytes(),Stream.frame*len(r))
        w=result['formalWindow'];self.assertGreater(w['radar_first_file_frame'],0)
        self.assertEqual(int(formal[0]['host_rx_monotonic_ns']),w['start_ns'])
        self.assertTrue(all(w['start_ns']<=int(x['host_rx_monotonic_ns'])<w['end_ns'] for x in formal))
        self.assertEqual(len(formal),result['frames']);self.assertGreater(len(r),len(formal))
        first_size=(folder/'radar.bin').stat().st_size
        self.post('/api/start',{**self.request,'maxFrames':2});self.ready();second=self.done()
        self.assertEqual(second['state'],'completed',second);self.assertEqual(second['frames'],2)
        self.assertGreaterEqual(second['result']['preRollFrames'],3)
        self.assertNotEqual(second['directory'],str(folder));self.assertEqual((folder/'radar.bin').stat().st_size,first_size)

    def test_duration_uses_common_t0_not_launch_time(self):
        self.post('/api/start',{**self.request,'durationSeconds':.25});self.receiver_ready()
        time.sleep(.35) # Longer than requested experiment duration, but no data => no T0.
        self.assertEqual(self.session.snapshot()['state'],'starting')
        self.assertIsNone(self.session.snapshot()['formalWindow'])
        self.stream=Stream(self.port);result=self.done()
        self.assertEqual(result['state'],'completed',result)
        self.assertEqual(result['formalWindow']['end_ns']-result['formalWindow']['start_ns'],250000000)
        m=json.loads((Path(result['directory'])/'session.json').read_text(encoding='utf-8'))
        e={x['name']:x['monotonic_ns'] for x in m['events']}
        self.assertLess(e['radar_ready'],e['formal_start']);self.assertGreater(e['formal_start']-e['session_start_requested'],350000000)
        self.assertEqual(e['formal_end'],result['formalWindow']['end_ns'])

    def test_gap_reports_failure_and_preserves_data(self):
        self.post('/api/start',self.request);self.ready(gap_frame=12)
        wait_for(lambda:self.stream.sent>=15);self.post('/api/stop',{});result=self.done()
        self.assertEqual(result['state'],'failed');self.assertEqual(result['result']['missingPackets'],1)
        self.assertGreater(result['result']['discardedFrames'],0);self.assertGreater(result['frames'],0)

    def test_ready_without_frames_does_not_start_formal_capture(self):
        self.post('/api/start',self.request);self.receiver_ready();time.sleep(.1)
        self.assertEqual(self.session.snapshot()['state'],'starting')
        self.post('/api/stop',{});result=self.done()
        self.assertEqual(result['state'],'failed');self.assertIsNone(result['formalWindow']);self.assertEqual(result['frames'],0)

    def test_stale_belt_does_not_release_formal_gate(self):
        class Belt:
            def prepare(self,context):pass
            def start(self):pass
            def status(self):return {'samples':100,'last_rx_monotonic_ns':time.perf_counter_ns()-1000000000}
            def stop(self):return {}
        self.session.adapters={'test':Belt}
        self.post('/api/start',{**self.request,'belt':'test'});self.receiver_ready();self.stream=Stream(self.port)
        wait_for(lambda:self.stream.sent>=5)
        self.assertEqual(self.session.snapshot()['state'],'starting');self.assertIsNone(self.session.snapshot()['formalWindow'])
        self.post('/api/stop',{});self.assertEqual(self.done()['frames'],0)

    def test_invalid_cfg_missing_belt_finite_cfg_and_cross_site(self):
        with self.assertRaises(HTTPError) as denied:self.post('/api/start',self.request,token='wrong')
        self.assertEqual(denied.exception.code,403)
        for extra in ({'belt':'not-installed'},{'durationSeconds':-1},{'durationSeconds':True}):
            with self.assertRaises(HTTPError):self.post('/api/start',{**self.request,**extra})
        cfg=self.root/'finite.cfg';cfg.write_text((server.ROOT/'awr1843.windows.cfg').read_text(encoding='utf-8').replace('frameCfg 0 0 64 0 ','frameCfg 0 0 64 10 '),encoding='utf-8')
        with self.assertRaises(HTTPError):self.post('/api/start',{**self.request,'cfgPath':str(cfg)})
        self.assertIsNone(self.session.worker)
        with self.assertRaises(HTTPError) as denied:urlopen(Request(self.url+'/api/options',headers={'Origin':'https://foreign.example'}))
        self.assertEqual(denied.exception.code,403)

    def test_belt_lifecycle_and_radar_only_independence(self):
        calls=[]
        class Belt:
            def prepare(self,context):calls.append('prepare')
            def start(self):calls.append('start')
            def status(self):return {'samples':1,'last_rx_monotonic_ns':time.perf_counter_ns()}
            def stop(self):calls.append('stop');return {'samples':1}
        self.session.adapters={'test':Belt}
        self.post('/api/start',{**self.request,'belt':'test','maxFrames':2});self.ready()
        self.assertEqual(self.done()['state'],'completed');self.assertEqual(calls,['prepare','start','stop'])
        calls.clear();self.post('/api/start',{**self.request,'maxFrames':2});self.ready()
        self.assertEqual(self.done()['state'],'completed');self.assertEqual(calls,[])

    def test_belt_failure_stops_radar_and_finalizes(self):
        calls=[]
        class Belt:
            def prepare(self,context):pass
            def start(self):pass
            def status(self):return {'error':'disconnected'}
            def stop(self):calls.append('stop');return {}
        self.session.adapters={'test':Belt}
        self.post('/api/start',{**self.request,'belt':'test'})
        result=self.done();self.assertEqual(result['state'],'failed');self.assertIn('disconnected',result['error']);self.assertEqual(calls,['stop'])

    def test_hkh11c_settles_while_radar_prerolls_then_shares_window(self):
        serial=FakeSerial();device=Hkh11cDevice(lambda:serial);device.SETTLE_SECONDS=.35
        self.session.adapters={'HKH-11C':lambda:device}
        self.post('/api/start',{**self.request,'belt':'HKH-11C','beltPort':'COM9','beltGain':3,'maxFrames':4})
        self.ready();result=self.done();self.assertEqual(result['state'],'completed',result)
        self.assertEqual(result['frames'],4);self.assertTrue(result['belt']['stop_acknowledged'])
        w=result['formalWindow'];self.assertGreaterEqual(result['result']['preRollFrames'],3);self.assertTrue(w['belt_covers_window'])
        folder=Path(result['directory']);belt=self.rows(folder,'belt_formal.samples.csv')
        self.assertEqual(len(belt),w['belt_samples']);self.assertGreater(len(belt),0)
        for row in belt:
            stamp=int(row['host_rx_monotonic_ns']);self.assertTrue(w['start_ns']<=stamp<w['end_ns'])
            self.assertAlmostEqual(float(row['relative_time_s']),(stamp-w['start_ns'])/1e9,places=8)
        m=json.loads((folder/'session.json').read_text(encoding='utf-8'));e={x['name']:x['monotonic_ns'] for x in m['events']}
        self.assertLess(e['radar_first_frame'],e['belt_stable']);self.assertLess(e['belt_stable'],e['formal_start'])
        self.assertLessEqual(e['formal_requested'],e['formal_start'])

    def test_hkh11c_configuration_and_warmup_cancel(self):
        body={**self.request,'belt':'HKH-11C','beltPort':'COM8','beltGain':3}
        for extra in ({},{'beltPort':''},{'beltPort':'COM9','beltGain':2}):
            with self.assertRaises(HTTPError):self.post('/api/start',{**body,**extra})
        serial=FakeSerial();device=Hkh11cDevice(lambda:serial)
        self.session.adapters={'HKH-11C':lambda:device}
        self.post('/api/start',{**body,'beltPort':'COM9'});self.receiver_ready();self.stream=Stream(self.port)
        wait_for(lambda:device.gain==3);self.post('/api/stop',{});result=self.done()
        self.assertEqual(result['state'],'failed');self.assertIn('取消',result['error'])
        self.assertTrue(result['belt']['stop_acknowledged']);self.assertFalse(serial.is_open)
        self.assertIsNone(result['formalWindow']);self.assertEqual(result['frames'],0)

if __name__=='__main__':unittest.main()
