"""Hardware-free acceptance of the native receiver and HTTP session lifecycle.
Run: python -m unittest discover -s tools/capture_service -p 'test_*.py' -v
"""
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
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from http.server import ThreadingHTTPServer
import server
from hkh11c import Hkh11cDevice
from test_hkh11c import FakeSerial

def free_port(kind=socket.SOCK_DGRAM):
    with socket.socket(socket.AF_INET,kind) as s:
        s.bind(('127.0.0.1',0)); return s.getsockname()[1]

def wait_for(predicate, timeout=10):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if predicate(): return
        time.sleep(.03)
    raise AssertionError('condition timed out')

def pump(port, frames=2, gap=False):
    payload=bytes(range(256))*1024*frames
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
        for seq,pos in enumerate(range(0,len(payload),1400),1):
            if gap and seq==3: continue
            sock.sendto(struct.pack('<I',seq)+pos.to_bytes(6,'little')+payload[pos:pos+1400],('127.0.0.1',port))
            time.sleep(.0001)
    return payload

class Acceptance(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=server.ROOT/'build')
        self.root=Path(self.tmp.name)
        self.session=server.CaptureSession(root=self.root)
        real_popen=subprocess.Popen
        def replay_only(args,*a,**kw):
            # Test seam only: production API cannot request passive/synthetic mode.
            if '--json' in args: args=[*args,'--no-control']
            return real_popen(args,*a,**kw)
        self.patch=patch.object(server.subprocess,'Popen',side_effect=replay_only);self.patch.start()
        self.http=ThreadingHTTPServer(('127.0.0.1',0),server.Handler)
        self.http.session=self.session;self.http.token='test-token'
        self.thread=threading.Thread(target=self.http.serve_forever,daemon=True);self.thread.start()
        self.url=f'http://127.0.0.1:{self.http.server_port}'
        self.port=free_port()
        self.request=dict(cfgPath=str(server.ROOT/'awr1843.cfg'),serial='COM8',bindIp='127.0.0.1',dcaIp='127.0.0.1',dataPort=self.port,configPort=free_port())

    def tearDown(self):
        self.session.stop()
        if self.session.worker:self.session.worker.join(50)
        self.http.shutdown();self.http.server_close();self.thread.join()
        self.patch.stop();self.tmp.cleanup()

    def post(self,path,body,token='test-token'):
        req=Request(self.url+path,data=json.dumps(body).encode(),headers={'Content-Type':'application/json','X-Capture-Token':token})
        with urlopen(req,timeout=15) as r:return json.load(r)

    def ready(self):
        wait_for(lambda:self.session.snapshot()['state']=='recording')

    def done(self):
        wait_for(lambda:self.session.worker and not self.session.worker.is_alive())
        return self.session.snapshot()

    def test_http_start_stop_bin_timestamps_and_restart(self):
        cfg_copy=self.root/'中文 配置.cfg';cfg_copy.write_bytes((server.ROOT/'awr1843.cfg').read_bytes())
        self.request['cfgPath']=str(cfg_copy)
        self.post('/api/start',self.request);self.ready()
        with self.assertRaises(HTTPError) as busy:self.post('/api/start',self.request)
        self.assertEqual(busy.exception.code,409)
        begin=time.perf_counter_ns();expected=pump(self.port)
        wait_for(lambda:self.session.snapshot()['frames']>=2)
        self.post('/api/stop',{});result=self.done();end=time.perf_counter_ns()
        self.assertEqual(result['state'],'completed',result)
        directory=Path(result['directory'])
        self.assertEqual((directory/'radar.bin').read_bytes(),expected)
        with (directory/'radar.bin.frames.csv').open() as f: rows=list(csv.DictReader(f))
        stamps=[int(row['host_rx_monotonic_ns']) for row in rows]
        self.assertEqual(len(stamps),2);self.assertEqual(stamps,sorted(stamps))
        self.assertTrue(all(begin<=stamp<=end for stamp in stamps))
        self.assertEqual(result['result']['missingPackets'],0)
        manifest=json.loads((directory/'session.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['mode'],'radar');self.assertIsNone(manifest['beltAdapter'])
        # Repeated capture never truncates the preceding BIN.
        self.post('/api/start',{**self.request,'maxFrames':2});self.ready();pump(self.port)
        second=self.done();self.assertEqual(second['state'],'completed',second)
        self.assertNotEqual(second['directory'],str(directory))
        self.assertEqual((directory/'radar.bin').read_bytes(),expected)

    def test_gap_is_reported_as_failure_with_partial_data_preserved(self):
        self.post('/api/start',self.request);self.ready();pump(self.port,gap=True)
        wait_for(lambda:self.session.snapshot()['frames']>=1)
        self.post('/api/stop',{});result=self.done()
        self.assertEqual(result['state'],'failed')
        self.assertEqual(result['result']['missingPackets'],1)
        self.assertEqual(result['result']['discardedFrames'],1)
        self.assertEqual((Path(result['directory'])/'radar.bin').stat().st_size,262144)

    def test_invalid_cfg_missing_belt_and_cross_site_rejected(self):
        with self.assertRaises(HTTPError) as denied:self.post('/api/start',self.request,token='wrong')
        self.assertEqual(denied.exception.code,403)
        with self.assertRaises(HTTPError):self.post('/api/start',{**self.request,'belt':'not-installed'})
        bad=self.root/'invalid.cfg';bad.write_text('sensorStart\n')
        with self.assertRaises(HTTPError):self.post('/api/start',{**self.request,'cfgPath':str(bad)})
        self.assertIsNone(self.session.worker)
        request=Request(self.url+'/api/options',headers={'Origin':'https://foreign.example'})
        with self.assertRaises(HTTPError) as denied:urlopen(request)
        self.assertEqual(denied.exception.code,403)

    def test_belt_lifecycle_and_radar_only_independence(self):
        calls=[]
        class Belt:
            def prepare(self,context):calls.append('prepare');self.context=context
            def start(self):calls.append('start')
            def status(self):return {'samples':1}
            def stop(self):calls.append('stop');return {'samples':1}
        self.session.adapters={'test':Belt}
        self.post('/api/start',{**self.request,'belt':'test','maxFrames':2});self.ready();pump(self.port)
        result=self.done();self.assertEqual(result['state'],'completed',result)
        self.assertEqual(calls,['prepare','start','stop'])
        calls.clear()
        self.post('/api/start',{**self.request,'maxFrames':2});self.ready();pump(self.port)
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
        result=self.done();self.assertEqual(result['state'],'failed')
        self.assertIn('disconnected',result['error']);self.assertEqual(calls,['stop'])

    def test_hkh11c_adapter_with_native_radar_and_shared_timeline(self):
        serial = FakeSerial()
        def factory():
            device = Hkh11cDevice(lambda:serial)
            device.SETTLE_SECONDS = .03
            return device
        self.session.adapters = {'HKH-11C':factory}
        body = {**self.request, 'belt':'HKH-11C', 'beltPort':'COM9', 'beltGain':3, 'maxFrames':2}
        self.post('/api/start',body);self.ready();pump(self.port)
        result = self.done()
        self.assertEqual(result['state'],'completed',result)
        self.assertTrue(result['belt']['stop_acknowledged'])
        directory = Path(result['directory'])
        with (directory/'radar.bin.frames.csv').open() as f:radar=list(csv.DictReader(f))
        with (directory/'belt_samples.csv').open() as f:belt=list(csv.DictReader(f))
        self.assertLessEqual(int(belt[0]['host_rx_monotonic_ns']),int(radar[0]['host_rx_monotonic_ns']))
        self.assertGreaterEqual(int(belt[-1]['host_rx_monotonic_ns']),int(radar[-1]['host_rx_monotonic_ns']))
        manifest=json.loads((directory/'session.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['beltConfig']['port'],'COM9')
        self.assertEqual(manifest['beltResult']['device_id_hex'],'FE 01 10 CC')
        self.assertGreater(manifest['beltResult']['phase_stats']['formal']['samples'],0)

    def test_hkh11c_configuration_and_warmup_cancel(self):
        body={**self.request,'belt':'HKH-11C','beltPort':'COM8','beltGain':3}
        for override in ({}, {'beltPort':''}, {'beltPort':'COM9','beltGain':2}):
            with self.assertRaises(HTTPError) as invalid:self.post('/api/start',{**body,**override})
            self.assertEqual(invalid.exception.code,400)
        serial=FakeSerial()
        device=Hkh11cDevice(lambda:serial)
        self.session.adapters={'HKH-11C':lambda:device}
        self.post('/api/start',{**body,'beltPort':'COM9'})
        wait_for(lambda:device.gain==3)
        self.post('/api/stop',{})
        result=self.done()
        self.assertEqual(result['state'],'failed')
        self.assertIn('取消',result['error'])
        self.assertTrue(result['belt']['stop_acknowledged'])
        self.assertFalse(serial.is_open)
        self.assertFalse((Path(result['directory'])/'radar.bin').exists())

if __name__=='__main__':unittest.main()
