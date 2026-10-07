"""Protocol, handoff evidence, and serial lifecycle regression tests (no hardware)."""
import csv
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest

from belt import SessionContext
from hkh11c import Hkh11cDevice
from hkh11c_protocol import Hkh11cParser, command_bytes

ROOT = Path(__file__).resolve().parents[2]

def sample(value):
    tail = bytes([0xA0, value >> 8, value & 255])
    return bytes([255,204,5,(5+sum(tail))&255])+tail

IDENTITY = bytes.fromhex('ff cc 07 84 a2 fe 01 10 cc')
ACK_GAIN = bytes.fromhex('ff cc 03 a7 a4')

class ProtocolTests(unittest.TestCase):
    def test_all_split_boundaries_and_coalesced_frames(self):
        raw=IDENTITY+sample(26331)+ACK_GAIN+sample(0)+sample(1023)+sample(512)+command_bytes(0xA1)
        expected=[IDENTITY,sample(26331),ACK_GAIN,sample(0),sample(1023),sample(512),command_bytes(0xA1)]
        for split in range(1,len(raw)):
            parser=Hkh11cParser()
            frames=parser.feed(raw[:split],0,100)+parser.feed(raw[split:],1,200)
            self.assertEqual([f.raw for f in frames],expected)
            self.assertEqual([f.value for f in frames if f.command==0xA0],[26331,0,1023,512])
            for f in frames:
                self.assertEqual(f.first_chunk,0 if f.raw_offset<split else 1)
                self.assertEqual(f.last_chunk,0 if f.raw_offset+len(f.raw)<=split else 1)
                self.assertEqual(f.rx_ns,100 if f.last_chunk==0 else 200)
            self.assertEqual(parser.statistics()['incomplete_tail_bytes'],0)
        parser=Hkh11cParser(); frames=[]
        for i,b in enumerate(raw):frames.extend(parser.feed(bytes([b]),i,i))
        self.assertEqual([f.raw for f in frames],expected)
        for f in frames:self.assertEqual(f.rx_ns,f.raw_offset+len(f.raw)-1)

    def test_bad_checksum_and_length_recover_without_stalling(self):
        bad=bytearray(sample(25));bad[3]^=1
        raw=b'noise\xff'+bytes(bad)+b'\xff\xcc\x07\x00\xa0'+sample(749)+b'\xff\xcc\x05'
        parser=Hkh11cParser();frames=parser.feed(raw,0,123)
        self.assertEqual([f.value for f in frames],[749])
        self.assertEqual(parser.checksum_errors,1)
        self.assertEqual(parser.length_errors,1)
        self.assertEqual(parser.statistics()['incomplete_tail_bytes'],3)
        self.assertGreater(parser.skipped_bytes,0)
        self.assertEqual(command_bytes(0xA4,3),bytes.fromhex('ff cc 04 ab a4 03'))

    def test_handoff_raw_sample_and_chunk_regression(self):
        handoff=Path(os.environ.get('HKH11C_HANDOFF',str(ROOT.parent/'resp-belt-handoff')))
        if not handoff.exists():self.skipTest('handoff evidence not available on this machine')
        for run in ('20261007-140128','20261007-140546-gain'):
            folder=handoff/'evidence'/run
            raw=(folder/'raw.bin').read_bytes()
            with (folder/'chunks.csv').open(encoding='utf-8-sig') as f:chunks=list(csv.DictReader(f))
            with (folder/'samples.csv').open(encoding='utf-8-sig') as f:expected=list(csv.DictReader(f))
            parser=Hkh11cParser();frames=[]
            for i,c in enumerate(chunks):
                offset=int(c['offset']);length=int(c['length']);stamp=round(float(c['elapsed_s'])*1e9)
                frames.extend(parser.feed(raw[offset:offset+length],i,stamp))
            actual=[f for f in frames if f.command==0xA0]
            self.assertEqual([(f.raw_offset,f.value) for f in actual],[(int(r['offset']),int(r['value'])) for r in expected])
            for frame,row in zip(actual,expected):self.assertAlmostEqual(frame.rx_ns/1e9,float(row['time']),places=6)
            self.assertEqual(parser.checksum_errors,0)
            if 'gain' in run:
                formal=[f.value for f in actual if chunks[f.last_chunk]['phase']=='formal']
                self.assertEqual((len(formal),min(formal),max(formal)),(1523,244,749))
                self.assertEqual([f.raw for f in frames if f.command==0xA2],[IDENTITY])

class FakeSerial:
    def __init__(self, stop_ack=True, gain_ack=True, stream=True, disconnect=False):
        self.is_open=False;self.buffer=bytearray();self.cv=threading.Condition()
        self.measuring=False;self.writes=[];self.stop_ack=stop_ack;self.gain_ack=gain_ack
        self.stream=stream;self.disconnect=disconnect;self.counter=0

    def open(self):
        self.is_open=True
        self.thread=threading.Thread(target=self._pump,daemon=True);self.thread.start()

    def _queue(self,data):
        with self.cv:self.buffer.extend(data);self.cv.notify_all()

    def _pump(self):
        while self.is_open:
            if self.measuring and self.stream:
                self._queue(sample(26331 if self.counter==0 else 400+self.counter%20));self.counter+=1
            time.sleep(.005)

    @property
    def in_waiting(self):
        if self.disconnect:raise OSError('device unplugged')
        with self.cv:return len(self.buffer)

    def read(self,n):
        with self.cv:
            if not self.buffer:self.cv.wait(.02)
            data=bytes(self.buffer[:n]);del self.buffer[:n];return data

    def write(self,data):
        self.writes.append(bytes(data));cmd=data[4]
        if cmd==0xA2:self._queue(IDENTITY)
        elif cmd==0xA0:self.measuring=True
        elif cmd==0xA4 and self.gain_ack:self._queue(ACK_GAIN+sample(511))
        elif cmd==0xA1:
            self.measuring=False
            if self.stop_ack:self._queue(command_bytes(0xA1))
        return len(data)

    def cancel_read(self):
        with self.cv:self.cv.notify_all()

    def close(self):
        self.is_open=False
        if hasattr(self,'thread'):self.thread.join(1)

class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=ROOT/'build')
        self.directory=Path(self.tmp.name)
        self.serial=FakeSerial()
        self.device=Hkh11cDevice(lambda:self.serial)
        self.device.SETTLE_SECONDS=.03;self.device.COMMAND_TIMEOUT=.15;self.device.DATA_TIMEOUT=.2
        self.context=SessionContext(self.directory,time.perf_counter_ns(),time.time_ns(),time.perf_counter_ns,{'port':'COM99','gain':3})

    def tearDown(self):
        self.device.stop();self.tmp.cleanup()

    def test_gain_identity_raw_trace_and_idempotent_stop(self):
        self.device.prepare(self.context);self.device.start();self.device.mark_phase('formal')
        time.sleep(.04);before=time.perf_counter_ns();result=self.device.stop()
        self.assertEqual(result['error'],'');self.assertTrue(result['stop_acknowledged'])
        self.assertEqual(result['device_id_hex'],'FE 01 10 CC')
        self.assertEqual(result['gain_commanded'],3)
        self.assertFalse(self.serial.dtr);self.assertFalse(self.serial.rts)
        self.assertIs(self.device.stop(),result)
        self.assertEqual([c[4] for c in self.serial.writes],[0xA2,0xA0,0xA4,0xA1])
        raw=(self.directory/'belt_raw.bin').read_bytes()
        with (self.directory/'belt_samples.csv').open() as f:rows=list(csv.DictReader(f))
        self.assertEqual(len(rows),result['samples'])
        self.assertEqual(int(rows[0]['value_raw']),26331);self.assertEqual(rows[0]['range_ok'],'0')
        for row in rows:
            start=int(row['raw_byte_offset']);self.assertEqual(raw[start:start+7],sample(int(row['value_raw'])))
            self.assertLessEqual(int(row['host_rx_monotonic_ns']),time.perf_counter_ns())
            self.assertEqual(row['device_timestamp'],'')
        self.assertTrue(any(r['phase']=='formal' for r in rows))
        self.assertTrue(all(r['gain_commanded']=='3' for r in rows if r['phase']=='formal'))
        self.assertEqual(result['phase_stats']['formal']['out_of_range'],0)
        self.assertTrue((self.directory/'belt_quality.json').is_file())

    def test_missing_gain_ack_attempts_stop_and_keeps_raw(self):
        self.serial.gain_ack=False
        self.device.prepare(self.context)
        with self.assertRaisesRegex(RuntimeError,'A4'):
            self.device.start()
        result=self.device.stop()
        self.assertTrue(result['stop_acknowledged'])
        self.assertFalse(self.serial.is_open)
        self.assertGreater((self.directory/'belt_raw.bin').stat().st_size,0)

    def test_no_data_start_timeout_and_stop_timeout(self):
        self.serial.stream=False;self.serial.stop_ack=False
        self.device.prepare(self.context)
        with self.assertRaisesRegex(RuntimeError,'A0'):self.device.start()
        begin=time.monotonic();result=self.device.stop()
        self.assertLess(time.monotonic()-begin,1)
        self.assertIn('A0',result['error']);self.assertFalse(result['stop_acknowledged'])
        self.assertIn('A1 应答超时', (self.directory/'belt_events.csv').read_text(encoding='utf-8'))

    def test_disconnect_reports_error_and_closes(self):
        self.device.prepare(self.context);self.device.start();self.serial.disconnect=True
        time.sleep(.08)
        self.assertIn('unplugged',self.device.status()['error'])
        self.device.stop();self.assertFalse(self.serial.is_open)

    def test_log_failure_does_not_prevent_stop_command(self):
        self.device.prepare(self.context);self.device.start()
        self.device.event_file.close()
        result=self.device.stop()
        self.assertEqual(self.serial.writes[-1],command_bytes(0xA1))
        self.assertTrue(result['stop_acknowledged']);self.assertIn('事件写入失败',result['error'])

if __name__=='__main__':unittest.main()
