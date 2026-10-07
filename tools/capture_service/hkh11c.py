"""HKH-11C session adapter; pySerial is loaded only when the belt is opened."""
import csv
import json
from pathlib import Path
import threading
import time

from hkh11c_protocol import Hkh11cParser, command_bytes


def enumerate_belt_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        return {'ports': [], 'error': '双设备模式需要 pySerial，请安装 requirements.txt 中的依赖。'}
    try:
        ports = [dict(port=p.device, description=p.description, serial_number=p.serial_number,
                      hwid=p.hwid, location=p.location) for p in list_ports.comports()
                 if p.vid == 0x10C4 and p.pid == 0xEA60]
        return {'ports': sorted(ports, key=lambda p: p['port']), 'error': ''}
    except OSError as exc:
        return {'ports': [], 'error': str(exc)}


class Hkh11cDevice:
    SETTLE_SECONDS = 3.0
    COMMAND_TIMEOUT = 2.0
    DATA_TIMEOUT = 3.0

    def __init__(self, serial_factory=None):
        self.serial_factory = serial_factory
        self.port = None
        self.context = None
        self.parser = Hkh11cParser()
        self.cv = threading.Condition(threading.RLock())
        self.command_lock = threading.Lock()
        self.closing = threading.Event()
        self.receiver = None
        self.files = []
        self.error = ''
        self.phase = 'prepare'
        self.gain = None
        self.device_id = ''
        self.samples = 0
        self.latest = None
        self.first_ns = self.last_ns = self.last_valid_ns = None
        self.phase_stats = {}
        self.pending = None
        self.ack = None
        self.measuring = False
        self.stop_acknowledged = False
        self.final_result = None

    def _event(self, event, *, stamp=None, command='', gain='', raw_offset='', raw=b'', detail=''):
        with self.cv:
            if not hasattr(self, 'events'):
                return
            try:
                self.events.writerow([self.context.clock_ns() if stamp is None else stamp,
                                      event, self.phase, command, gain, raw_offset, raw.hex(' '), detail])
                self.event_file.flush()
            except (OSError, ValueError) as exc:
                # A failed log must not prevent sending the emergency STOP.
                self._fail(f'呼吸带事件写入失败：{exc}')

    def _fail(self, message):
        with self.cv:
            self.error = self.error or str(message)
            self.cv.notify_all()

    def _check(self, allow_cancel=True):
        if self.error:
            raise RuntimeError(self.error)
        if allow_cancel and self.context.is_cancelled():
            raise RuntimeError('呼吸带启动已取消。')

    def prepare(self, context):
        self.context = context
        try:
            self._prepare(context)
        except Exception as exc:
            self._fail(f'呼吸带准备失败：{exc}')
            self._event('error', detail=self.error)
            raise

    def _prepare(self, context):
        options = context.belt_options
        port_name = options['port']
        self.requested_gain = options.get('gain', 3)
        if self.requested_gain not in (1, 3):
            raise ValueError('HKH-11C 当前仅启用已验证档位 1 和 3。')
        def file(name, mode):
            handle = (context.directory/name).open(mode, **({} if 'b' in mode else {'encoding':'utf-8','newline':''}))
            self.files.append(handle)
            return handle
        self.raw = file('belt_raw.bin', 'xb')
        self.chunk_file = file('belt_chunks.csv', 'x')
        self.chunks = csv.writer(self.chunk_file)
        self.chunks.writerow(['chunk_id','raw_offset','byte_count','host_rx_monotonic_ns'])
        self.sample_file = file('belt_samples.csv', 'x')
        self.sample_csv = csv.writer(self.sample_file)
        self.sample_csv.writerow(['received_index','raw_byte_offset','first_chunk_id','last_chunk_id',
                                  'host_rx_monotonic_ns','device_timestamp','value_raw','range_ok',
                                  'rail_hit','gain_commanded','phase'])
        self.event_file = file('belt_events.csv', 'x')
        self.events = csv.writer(self.event_file)
        self.events.writerow(['host_monotonic_ns','event','phase','command','gain_commanded','raw_byte_offset','hex','detail'])
        if self.serial_factory is None:
            try:
                import serial
            except ImportError as exc:
                raise RuntimeError('缺少 pySerial；请用项目 .venv 启动，或安装 tools/capture_service/requirements.txt。') from exc
            self.port = serial.Serial(port=None)
        else:
            self.port = self.serial_factory()
        # Configure lines BEFORE open (pySerial otherwise defaults DTR/RTS to True).
        self.port.port = port_name
        self.port.baudrate = 115200
        self.port.bytesize = 8
        self.port.parity = 'N'
        self.port.stopbits = 1
        self.port.xonxoff = self.port.rtscts = self.port.dsrdtr = False
        self.port.dtr = self.port.rts = False
        self.port.timeout = .05
        self.port.write_timeout = 1.0
        self.port.open()
        self._event('port_open', detail=port_name)
        self.receiver = threading.Thread(target=self._receive, name='HKH11C-receive', daemon=True)
        self.receiver.start()
        frame = self._command(0xA2)
        self.device_id = frame.raw[5:9].hex(' ').upper()
        self._event('identity', raw=frame.raw, raw_offset=frame.raw_offset, detail=self.device_id)

    def _command(self, cmd, parameter=None, *, cleanup=False):
        with self.command_lock:
            with self.cv:
                if not cleanup:
                    self._check()
                raw = command_bytes(cmd, parameter)
                self.pending = (cmd, parameter)
                self.ack = None
                try:
                    self._event('command_tx', command=f'{cmd:02X}', gain='' if parameter is None else parameter, raw=raw)
                    if not cleanup:
                        self._check()
                    if self.port.write(raw) != len(raw):
                        raise RuntimeError('呼吸带串口未完整发送命令。')
                    deadline = time.monotonic()+self.COMMAND_TIMEOUT
                    while self.ack is None:
                        if not cleanup:
                            self._check()
                        if self.receiver and not self.receiver.is_alive():
                            raise RuntimeError('呼吸带接收线程已退出，无法确认命令。')
                        remain = deadline-time.monotonic()
                        if remain <= 0:
                            raise RuntimeError(f'呼吸带命令 {cmd:02X} 应答超时。')
                        self.cv.wait(min(remain, .05))
                    frame = self.ack
                    if cmd == 0xA4:
                        self.gain = parameter
                        self._event('gain_confirmed', stamp=frame.rx_ns, command='A4', gain=parameter,
                                    raw_offset=frame.raw_offset, raw=frame.raw)
                    return frame
                except Exception as exc:
                    self._event('command_failed', command=f'{cmd:02X}', detail=str(exc))
                    if not cleanup:
                        self._fail(str(exc))
                    raise
                finally:
                    self.pending = None
                    self.ack = None

    def start(self):
        try:
            self._start()
        except Exception as exc:
            self._fail(f'呼吸带启动失败：{exc}')
            self._event('error', detail=self.error)
            raise

    def _start(self):
        self.mark_phase('warmup')
        self.measuring = True  # also stop if start ACK/first sample is lost
        self._command(0xA0)    # A0 is acknowledged by the first checksummed sample
        self._command(0xA4, self.requested_gain)
        deadline = time.monotonic()+self.SETTLE_SECONDS
        with self.cv:
            while time.monotonic() < deadline:
                self._check()
                self.cv.wait(min(.05, max(0, deadline-time.monotonic())))
            if self.last_valid_ns is None or self.context.clock_ns()-self.last_valid_ns > int(self.DATA_TIMEOUT*1e9):
                raise RuntimeError('呼吸带稳定段没有持续收到有效量程样本。')
        self.mark_phase('ready')

    def mark_phase(self, phase):
        with self.cv:
            self.phase = phase
            self._event('phase')

    def _receive(self):
        chunk_id = 0
        last_flush = time.monotonic()
        try:
            while not self.closing.is_set():
                data = self.port.read(max(1, min(self.port.in_waiting, 4096)))
                stamp = self.context.clock_ns()  # before parsing or any disk write
                if not data:
                    continue
                with self.cv:
                    offset = self.parser.received
                    self.raw.write(data)
                    self.chunks.writerow([chunk_id, offset, len(data), stamp])
                    previous = self.parser.statistics()
                    frames = self.parser.feed(data, chunk_id, stamp)
                    for frame in frames:
                        if frame.command == 0xA0:
                            value = frame.value
                            range_ok = value <= 1023
                            rail = value in (0, 1023)
                            self.sample_csv.writerow([self.samples, frame.raw_offset, frame.first_chunk,
                                frame.last_chunk, frame.rx_ns, '', value, int(range_ok), int(rail),
                                '' if self.gain is None else self.gain, self.phase])
                            phase = self.phase_stats.setdefault(self.phase, dict(samples=0, out_of_range=0, rail_hits=0, min=None, max=None))
                            phase['samples'] += 1
                            phase['out_of_range'] += int(not range_ok)
                            phase['rail_hits'] += int(rail)
                            phase['min'] = value if phase['min'] is None else min(value, phase['min'])
                            phase['max'] = value if phase['max'] is None else max(value, phase['max'])
                            self.samples += 1
                            self.latest = value
                            self.first_ns = frame.rx_ns if self.first_ns is None else self.first_ns
                            self.last_ns = frame.rx_ns
                            if range_ok:
                                self.last_valid_ns = frame.rx_ns
                        else:
                            self._event('response_rx', stamp=frame.rx_ns, command=f'{frame.command:02X}',
                                        raw_offset=frame.raw_offset, raw=frame.raw)
                        if self.pending and self.pending[0] == frame.command and self.ack is None:
                            if frame.command == 0xA4:
                                self.gain = self.pending[1]
                            self.ack = frame
                            self.cv.notify_all()
                    current = self.parser.statistics()
                    if any(current[k] != previous[k] for k in ('checksum_errors','length_errors','unknown_commands')):
                        self._event('parser_resync', stamp=stamp, raw_offset=offset, detail=json.dumps(current))
                    if time.monotonic()-last_flush >= 1:
                        for handle in self.files:
                            handle.flush()
                        last_flush = time.monotonic()
                    chunk_id += 1
        except Exception as exc:
            self._fail(f'呼吸带接收或落盘失败：{exc}')
            try:
                self._event('error', detail=self.error)
            except OSError:
                pass

    def status(self):
        with self.cv:
            if self.measuring and self.phase in ('ready', 'formal'):
                if self.last_ns is None or self.context.clock_ns()-self.last_ns > int(self.DATA_TIMEOUT*1e9):
                    self._fail('呼吸带连续 3 秒没有完整有效校验帧（设备断开或无数据）。')
            span = (self.last_ns-self.first_ns)/1e9 if self.first_ns is not None and self.last_ns is not None else 0
            return dict(samples=self.samples, latest=self.latest, phase=self.phase, gain_commanded=self.gain,
                        device_id_hex=self.device_id, port=self.context.belt_options['port'] if self.context else '',
                        observed_hz=(self.samples-1)/span if span else None,
                        phase_stats={key:dict(value) for key,value in self.phase_stats.items()},
                        first_rx_monotonic_ns=self.first_ns, last_rx_monotonic_ns=self.last_ns,
                        stop_acknowledged=self.stop_acknowledged,
                        **self.parser.statistics(), error=self.error)

    def stop(self):
        if self.final_result is not None:
            return self.final_result
        try:
            if self.port and self.port.is_open and self.measuring:
                self.mark_phase('stopping')
                try:
                    self._command(0xA1, cleanup=True)
                    self.stop_acknowledged = True
                    time.sleep(.15)  # bounded tail receive, never purge raw bytes
                except Exception as exc:
                    self._fail(f'呼吸带停止未确认：{exc}')
            self.measuring = False
        finally:
            self.closing.set()
            if self.port and self.port.is_open:
                try:
                    self.port.cancel_read()
                except (OSError, AttributeError):
                    pass
            if self.receiver:
                self.receiver.join(timeout=1)
            if self.port:
                try:
                    self.port.close()
                except Exception as exc:
                    self._fail(f'呼吸带端口关闭失败：{exc}')
            if self.receiver and self.receiver.is_alive():
                self.receiver.join(timeout=1)
                if self.receiver.is_alive():
                    self._fail('呼吸带接收线程退出超时。')
            for handle in self.files:
                try:
                    handle.close()
                except OSError as exc:
                    self._fail(f'呼吸带文件关闭失败：{exc}')
        self.final_result = self.status()
        self.final_result['device_sample_loss_detectable'] = False
        self.final_result['timestamp_kind'] = 'host receive; device provides no timestamp or sequence'
        if self.context:
            try:
                (self.context.directory/'belt_quality.json').write_text(json.dumps(self.final_result, ensure_ascii=False, indent=2), encoding='utf-8')
            except OSError as exc:
                self.final_result['error'] = self.final_result['error'] or f'呼吸带统计写入失败：{exc}'
        return self.final_result
