import csv,json,tempfile,unittest,threading
from pathlib import Path
from http.server import ThreadingHTTPServer
from urllib.request import Request,urlopen
from urllib.error import HTTPError
from replay import Replay
from server import ROOT,CaptureSession,Handler,command_environment

class ReplayTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build');self.root=Path(self.tmp.name)
  self.folder=self.root/'captures'/'test-session';self.folder.mkdir(parents=True)
  (self.folder/'radar.cfg').write_bytes((ROOT/'awr1843.windows.cfg').read_bytes())
  (self.folder/'radar.bin').write_bytes(bytes(range(256))*1024*5)
  (self.folder/'session.json').write_text(json.dumps({'state':'completed','mode':'radar','radarStats':{}}))
  self.index([0,1,2,3,4])
  self.replay=Replay(self.root,ROOT/'build/radar_replay_extract.exe',command_environment)
 def tearDown(self):self.tmp.cleanup()
 def index(self,wires):
  with (self.folder/'radar.bin.frames.csv').open('w',newline='') as f:
   w=csv.writer(f);w.writerow(['file_frame','wire_frame','host_rx_monotonic_ns']);w.writerows((i,v,10**15+v*20000000) for i,v in enumerate(wires))
 def load(self,**kw):return self.replay.load({'session':'test-session',**kw})
 def test_radar_only_cache_and_source_integrity(self):
  before={p.name:p.read_bytes() for p in self.folder.iterdir() if p.is_file()}
  result=self.load();self.assertEqual(result['summary']['radarFrames'],5);self.assertEqual(result['belt'],[[],[]])
  self.assertEqual(result['timeBasis'],'first_radar_frame')
  self.assertIsNone(result['summary']['formal'])
  self.assertEqual(result['summary']['total'],{'radarFrames':5,'beltSamples':0,'duration':.08})
  self.assertTrue(all(v is None for v in result['comparison'][1]))
  cached=self.load();self.assertEqual(result,cached)
  self.assertEqual(before,{p.name:p.read_bytes() for p in self.folder.iterdir() if p.is_file()})
  with (self.folder/'radar.cfg').open('a') as f:f.write('\n% new cfg fingerprint\n')
  self.assertNotEqual(result['cacheKey'],self.load()['cacheKey'])
 def test_gap_and_fixed_dc_parameters(self):
  self.index([0,1,3,4,5]);result=self.load(fixedBin=4,dc=False)
  self.assertEqual(result['summary']['radarGaps'],1);self.assertIn(None,result['radar'][1])
  self.assertEqual(result['summary']['switches'],0)
  self.assertEqual(set(result['track'][1]),{4})
  phase=Replay.rows(Path(result['cacheDirectory'])/'radar_phase.csv',30)
  self.assertEqual([int(r['segment']) for r in phase],[0,0,1,1,1])
 def test_common_window_origin_and_formal_quality_override_live_phase(self):
  t0=10**15+20000000;t1=t0+40000000
  (self.folder/'session.json').write_text(json.dumps({'state':'completed','formalWindow':{'start_ns':t0,'end_ns':t1}}))
  with (self.folder/'belt_samples.csv').open('w',newline='') as f:
   w=csv.writer(f);w.writerow(['host_rx_monotonic_ns','value_raw','phase'])
   w.writerows([(t0-10000000,0,'formal'),(t0,400,'ready'),(t1-1,500,'ready'),(t1,1023,'formal')])
  result=self.load()
  self.assertAlmostEqual(result['radar'][0][0],-.02)
  self.assertEqual(result['markers']['formalWindow'],[0.,.04])
  self.assertEqual(result['summary']['formalSamples'],2)
  self.assertEqual(result['summary']['formalRails'],0)
  self.assertEqual(result['timeBasis'],'common_formal_t0')
  self.assertEqual(result['summary']['total'],{'radarFrames':5,'beltSamples':4,'duration':.08})
  self.assertEqual(result['summary']['formal'],{'radarFrames':2,'beltSamples':2,'duration':.04,'railHits':0,'outOfRange':0})
 def test_invalid_formal_window_is_rejected(self):
  for window in ({},{'start_ns':100,'end_ns':100},{'start_ns':True,'end_ns':200},{'start_ns':'100','end_ns':200}):
   (self.folder/'session.json').write_text(json.dumps({'state':'completed','formalWindow':window}))
   with self.assertRaisesRegex(ValueError,'T0/T1'):self.load()
 def test_bad_inputs_and_incomplete_recording(self):
  for body in ({'session':'../test-session'},{'session':'test-session','fixedBin':True},{'session':'test-session','low':1,'high':.7}):
   with self.assertRaises(ValueError):self.replay.load(body)
  (self.folder/'session.json').write_text('{"state":"recording"}')
  with self.assertRaises(ValueError):self.load()
 def test_mismatch_rejected(self):
  with (self.folder/'radar.bin').open('ab') as f:f.write(b'x')
  with self.assertRaisesRegex(ValueError,'BIN longer'):self.load()
 def test_belt_quality_and_failed_session(self):
  (self.folder/'session.json').write_text('{"state":"failed"}')
  with (self.folder/'belt_samples.csv').open('w',newline='') as f:
   w=csv.writer(f);w.writerow(['host_rx_monotonic_ns','value_raw','phase'])
   w.writerows([(10**15,0,'warmup'),(10**15+20000000,1023,'formal'),(10**15+40000000,2000,'formal')])
  r=self.load();self.assertEqual(r['summary']['formalRails'],1);self.assertEqual(r['summary']['formalOutOfRange'],1)
  self.assertEqual(r['belt'][1], [0,1023,2000]);self.assertTrue(r['warnings'])
 def test_http_concurrency_security_and_static_routes(self):
  http=ThreadingHTTPServer(('127.0.0.1',0),Handler);http.session=CaptureSession(root=self.root);http.replay=self.replay;http.token='t'
  thread=threading.Thread(target=http.serve_forever,daemon=True);thread.start();base=f'http://127.0.0.1:{http.server_port}'
  def post(path,body,token='t'):
   return urlopen(Request(base+path,data=json.dumps(body).encode(),headers={'X-Capture-Token':token,'Content-Type':'application/json'}),timeout=10)
  try:
   with urlopen(base+'/api/replay/sessions') as r:self.assertEqual(json.load(r)[0]['id'],'test-session')
   for path in ('/replay.html','/replay.js','/replay.css','/vendor/uPlot.iife.min.js'):
    with urlopen(base+path) as r:self.assertEqual(r.status,200)
   with self.assertRaises(HTTPError) as denied:post('/api/replay',{'session':'test-session'},'wrong')
   self.assertEqual(denied.exception.code,403)
   http.session.state='recording'
   with self.assertRaises(HTTPError) as active:post('/api/replay',{'session':'test-session'})
   self.assertEqual(active.exception.code,409)
   http.session.state='idle';self.replay.lock.acquire()
   try:
    with self.assertRaises(HTTPError) as occupied:post('/api/start',{})
    self.assertEqual(occupied.exception.code,409)
   finally:self.replay.lock.release()
  finally:http.shutdown();http.server_close();thread.join()

if __name__=='__main__':unittest.main()
