import csv,tempfile,unittest
from pathlib import Path
from formal_window import export_formal_window
from server import ROOT
class WindowTests(unittest.TestCase):
 def test_half_open_window_preserves_raw_and_overrides_delayed_phase(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build') as tmp:
   p=Path(tmp)
   with (p/'radar.bin.frames.csv').open('w',newline='') as f:
    w=csv.writer(f);w.writerow(['file_frame','wire_frame','host_rx_monotonic_ns']);w.writerows([(0,0,100),(1,1,200),(2,2,300),(3,3,400)])
   with (p/'belt_samples.csv').open('w',newline='') as f:
    w=csv.writer(f);w.writerow(['received_index','host_rx_monotonic_ns','value_raw','phase']);w.writerows([(0,100,0,'formal'),(1,200,400,'ready'),(2,300,500,'ready'),(3,400,1023,'formal')])
   before=(p/'belt_samples.csv').read_bytes()
   result=export_formal_window(p,{'formalStartNs':200,'formalEndNs':400,'formalFrames':2},True)
   self.assertEqual(result['radar_frames'],2);self.assertEqual(result['belt_samples'],2)
   self.assertTrue(result['belt_covers_window']);self.assertEqual(result['belt_quality']['rail_hits'],0)
   self.assertEqual((p/'belt_samples.csv').read_bytes(),before)
   with (p/'belt_formal.samples.csv').open() as f:rows=list(csv.DictReader(f))
   self.assertEqual([r['phase'] for r in rows],['formal','formal'])
   self.assertEqual([r['received_index'] for r in rows],['1','2'])
   with self.assertRaisesRegex(ValueError,'统计'):
    export_formal_window(p,{'formalStartNs':200,'formalEndNs':400,'formalFrames':1},True)
 def test_absent_t0_never_fabricates_formal_window(self):
  with tempfile.TemporaryDirectory(dir=ROOT/'build') as tmp:
   self.assertIsNone(export_formal_window(tmp,{'formalStartNs':0,'formalEndNs':0},True))
if __name__=='__main__':unittest.main()
