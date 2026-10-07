"""Local Windows capture service; pySerial is optional for HKH-11C."""
import argparse
from collections import deque
from datetime import datetime, timezone
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import threading
import time
import uuid
from urllib.parse import urlparse

from belt import BELT_ADAPTERS, SessionContext
from hkh11c import enumerate_belt_ports

ROOT = Path(__file__).resolve().parents[2]

def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.replace(path)

def command_environment():
    env = os.environ.copy()
    # MinGW executables need the same runtime as the compiler used by CMake.
    cache = ROOT / 'build/CMakeCache.txt'
    if cache.exists():
        for line in cache.read_text(encoding='utf-8').splitlines():
            if line.startswith('CMAKE_CXX_COMPILER:FILEPATH='):
                env['PATH'] = str(Path(line.split('=', 1)[1]).parent) + os.pathsep + env.get('PATH', '')
    return env

class CaptureSession:
    def __init__(self, root=ROOT, executable=None, adapters=None):
        self.root = Path(root)
        self.executable = Path(executable or ROOT / 'build/radar_capture.exe')
        self.adapters = BELT_ADAPTERS if adapters is None else adapters
        self.lock = threading.RLock()
        self.worker = None
        self.state = 'idle'
        self.logs = deque(maxlen=100)
        self.result = {}
        self.frames = 0
        self.bytes = 0
        self.directory = None
        self.error = ''
        self.belt_status = None

    def snapshot(self):
        with self.lock:
            return dict(state=self.state, frames=self.frames, bytes=self.bytes,
                        directory=str(self.directory or ''), error=self.error,
                        belt=self.belt_status, result=self.result, logs=list(self.logs))

    def start(self, request):
        with self.lock:
            if self.worker and self.worker.is_alive():
                raise RuntimeError('已有采集会话，请先结束并等待保存完成。')
            if not self.executable.is_file():
                raise ValueError('请先编译 build/radar_capture.exe。')
            if not isinstance(request, dict):
                raise ValueError('请求必须是 JSON 对象。')
            allowed = {'cfgPath', 'serial', 'bindIp', 'dcaIp', 'dataPort', 'configPort', 'packetDelayUs', 'lvdsLanes', 'maxFrames', 'belt', 'beltPort', 'beltGain'}
            if set(request) - allowed:
                raise ValueError('未知配置字段：' + ', '.join(sorted(set(request)-allowed)))
            belt = request.get('belt', '')
            if not isinstance(belt, str) or (belt and belt not in self.adapters):
                raise ValueError('呼吸带适配器尚未集成，请选择仅雷达采集。')
            source = Path(str(request.get('cfgPath', ''))).expanduser()
            if not source.is_absolute():
                source = self.root / source
            if source.suffix.lower() != '.cfg' or not source.is_file():
                raise ValueError('CFG 路径不存在或不是 .cfg 文件。')
            if source.stat().st_size > 1024*1024:
                raise ValueError('CFG 文件不得超过 1 MiB。')
            serial = str(request.get('serial', 'COM8')).upper()
            if not serial.startswith('COM') or not serial[3:].isdigit() or int(serial[3:]) < 1:
                raise ValueError('串口格式应为 COM8、COM10 等。')
            belt_config = None
            if belt:
                belt_port = str(request.get('beltPort', '')).upper().strip()
                gain = request.get('beltGain', 3)
                if belt == 'HKH-11C':
                    if not belt_port.startswith('COM') or not belt_port[3:].isdigit() or int(belt_port[3:]) < 1:
                        raise ValueError('请选择当前呼吸带串口，不能固定沿用历史 COM9。')
                    if int(belt_port[3:]) == int(serial[3:]):
                        raise ValueError('呼吸带串口与雷达 CLI 串口不能相同。')
                    if type(gain) is not int or gain not in (1, 3):
                        raise ValueError('呼吸带仅支持已验证档位 1 或 3。')
                belt_config = {'port':belt_port, 'gain':gain, 'settleSeconds':3}
                if belt == 'HKH-11C':
                    belt_config['usb'] = next((p for p in enumerate_belt_ports()['ports']
                                               if p['port'].upper() == belt_port), None)
            cfg = dict(serial=serial, bindIp=str(request.get('bindIp', '192.168.33.30')),
                       dcaIp=str(request.get('dcaIp', '192.168.33.180')), rcvbuf=16777216)
            for key in ('bindIp', 'dcaIp'):
                ipaddress.IPv4Address(cfg[key])
            for key, default, low, high in [('dataPort',4098,1,65535), ('configPort',4096,1,65535),
                     ('packetDelayUs',50,5,500), ('lvdsLanes',2,2,4), ('maxFrames',0,0,1000000000)]:
                value = request.get(key, default)
                if type(value) is not int or not low <= value <= high:
                    raise ValueError(f'{key} 必须是 {low}..{high} 的整数。')
                cfg[key] = value
            if cfg['lvdsLanes'] not in (2,4) or cfg['dataPort'] == cfg['configPort']:
                raise ValueError('LVDS 通道应为 2/4，命令端口与数据端口不能相同。')
            name = datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
            directory = self.root / 'captures' / name
            directory.mkdir(parents=True, exist_ok=False)
            shutil.copyfile(source, directory / 'radar.cfg')
            validation = subprocess.run([str(self.executable), '--validate-cfg', str(directory/'radar.cfg')],
                         cwd=self.root, env=command_environment(), capture_output=True, text=True,
                         encoding='utf-8', errors='replace', timeout=10,
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if validation.returncode:
                raise ValueError('CFG 校验失败：' + validation.stderr.strip())
            cfg.update(cfg=str(directory / 'radar.cfg'), output=str(directory / 'radar.bin'))
            write_json(directory/'capture.json', {'capture':cfg})
            origin = time.perf_counter_ns()
            manifest = dict(session=name, state='starting', mode='radar+belt' if belt else 'radar',
                            cfgSource=str(source.resolve()), capture=cfg, beltAdapter=belt or None,
                            beltConfig=belt_config,
                            clock={'kind':'Windows QPC / Python perf_counter_ns', 'origin_monotonic_ns':origin,
                                   'origin_utc_ns':time.time_ns(), 'meaning':'host receive time, not hardware trigger'},
                            events=[{'name':'session_start_requested', 'monotonic_ns':origin}])
            write_json(directory/'session.json', manifest)
            self.state = 'starting'; self.frames = self.bytes = 0
            self.result = {}; self.error = ''; self.belt_status = None
            self.logs.clear(); self.directory = directory
            self.worker = threading.Thread(target=self._run, args=(directory, manifest, belt), daemon=False)
            self.worker.start()
            return self.snapshot()

    def stop(self):
        with self.lock:
            if self.state in ('starting', 'recording'):
                self.state = 'stopping'
            return self.snapshot()

    def _cancelled(self):
        with self.lock:
            return self.state == 'stopping'

    def _log(self, line):
        with self.lock:
            self.logs.append(line.rstrip())
            parts = line.split()
            if parts and parts[0] == 'PROGRESS' and len(parts) == 3:
                self.frames, self.bytes = int(parts[1]), int(parts[2])
            if parts and parts[0] == 'READY' and self.state == 'starting':
                self.state = 'recording'

    def _run(self, directory, manifest, belt_name):
        process = None; adapter = None; reader = None; failure = ''; code = None
        reader_errors = []
        try:
            if belt_name:
                adapter = self.adapters[belt_name]()
                with self.lock:
                    self.belt_status = {'phase':'prepare', 'samples':0}
                adapter.prepare(SessionContext(directory, manifest['clock']['origin_monotonic_ns'],
                                manifest['clock']['origin_utc_ns'], time.perf_counter_ns,
                                manifest['beltConfig'], self._cancelled))
                with self.lock: self.belt_status = adapter.status()
                adapter.start()
                with self.lock: self.belt_status = adapter.status()
                manifest['events'].append({'name':'belt_started', 'monotonic_ns':time.perf_counter_ns()})
            with self.lock:
                if self.state == 'stopping':
                    raise RuntimeError('采集在启动前已取消。')
            process = subprocess.Popen([str(self.executable), '--json', str(directory/'capture.json')],
                      cwd=self.root, env=command_environment(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                      stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace', bufsize=1,
                      creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            def read_output():
                try:
                    with (directory/'capture.log').open('w', encoding='utf-8') as log:
                        for line in process.stdout:
                            log.write(line); log.flush(); self._log(line)
                            parts = line.split()
                            if len(parts) == 2 and parts[0] in ('ARMED', 'READY'):
                                manifest['events'].append({'name':'radar_'+parts[0].lower(), 'monotonic_ns':int(parts[1])})
                                if parts[0] == 'READY' and adapter and hasattr(adapter, 'mark_phase') and not self._cancelled():
                                    adapter.mark_phase('formal')
                                    manifest['events'].append({'name':'formal_start', 'monotonic_ns':time.perf_counter_ns()})
                except Exception as exc:
                    reader_errors.append(str(exc)); self.stop()
            reader = threading.Thread(target=read_output, daemon=True); reader.start()
            stop_sent = None
            while process.poll() is None:
                if adapter:
                    status = adapter.status()
                    with self.lock: self.belt_status = status
                    if status.get('error'):
                        failure = '呼吸带采集失败：' + str(status['error']); self.stop()
                with self.lock: stopping = self.state == 'stopping'
                if stopping and stop_sent is None:
                    stop_sent = time.monotonic()
                    manifest['events'].append({'name':'stop_requested', 'monotonic_ns':time.perf_counter_ns()})
                    if adapter and hasattr(adapter, 'mark_phase'):
                        adapter.mark_phase('tail')
                    try: process.stdin.write('stop\n'); process.stdin.flush()
                    except (BrokenPipeError, OSError): pass
                if stop_sent is not None and time.monotonic() - stop_sent > 45:
                    failure = '停止超时，进程已终止；请检查雷达是否需要手动复位。'
                    process.kill()
                time.sleep(.1)
            code = process.wait()
            reader.join()
            if reader_errors: failure = '日志写入失败：' + reader_errors[0]
            if code != 0 and not failure:
                failure = f'采集未完整成功（退出码 {code}），请查看日志和丢包统计。'
        except Exception as exc:
            failure = str(exc)
        finally:
            if process and process.poll() is None:
                try:
                    process.stdin.write('stop\n'); process.stdin.flush(); process.wait(timeout=45)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill(); process.wait()
                    failure += '；雷达停止未确认，请检查设备。'
            if reader: reader.join(timeout=5)
            if any(event['name'] == 'formal_start' for event in manifest['events']):
                manifest['events'].append({'name':'formal_end', 'monotonic_ns':time.perf_counter_ns()})
            if process:
                for stream in (process.stdin, process.stdout):
                    try: stream.close()
                    except OSError: pass
            if adapter:
                try:
                    result = adapter.stop()
                    manifest['beltResult'] = result
                    with self.lock: self.belt_status = result
                    if result.get('error'): failure = failure or str(result['error'])
                except Exception as exc: failure = failure or f'呼吸带收尾失败：{exc}'
            stats = {}
            stats_path = directory/'radar.bin.stats.txt'
            try:
                if stats_path.exists():
                    for line in stats_path.read_text().splitlines():
                        key, value = line.split('=',1); stats[key] = int(value)
            except (OSError, ValueError) as exc:
                failure = failure or f'统计文件不完整：{exc}'
            manifest.update(state='failed' if failure else 'completed', error=failure,
                            exitCode=code, radarStats=stats, finishedUtc=datetime.now(timezone.utc).isoformat())
            manifest['events'].append({'name':'session_finished', 'monotonic_ns':time.perf_counter_ns()})
            manifest['events'].sort(key=lambda event: event['monotonic_ns'])
            try: write_json(directory/'session.json', manifest)
            except OSError as exc: failure = failure or f'会话记录保存失败：{exc}'
            with self.lock:
                self.result = stats; self.error = failure
                self.state = 'failed' if failure else 'completed'
                if failure: self.logs.append(failure)

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT/'web/capture'), **kwargs)

    def _trusted(self):
        host = self.headers.get('Host', '')
        allowed = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
        origin = self.headers.get('Origin')
        return host in allowed and (not origin or origin in {'http://'+h for h in allowed})

    def json_response(self, code, body):
        data = json.dumps(body, ensure_ascii=False).encode('utf-8')
        self.send_response(code); self.send_header('Content-Type','application/json; charset=utf-8')
        self.send_header('Cache-Control','no-store'); self.send_header('Content-Length',str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def do_GET(self):
        if not self._trusted(): return self.json_response(403, {'error':'仅允许本机同源访问。'})
        path = urlparse(self.path).path
        if path == '/api/status': return self.json_response(200, self.server.session.snapshot())
        if path == '/api/options':
            return self.json_response(200, {'token':self.server.token, 'cfgPath':str(ROOT/'awr1843.windows.cfg'),
                    'outputRoot':str(ROOT/'captures'), 'belts':list(self.server.session.adapters),
                    'beltPorts':enumerate_belt_ports()})
        if path == '/api/belt-ports': return self.json_response(200, enumerate_belt_ports())
        if path not in ('/', '/index.html', '/app.js', '/style.css'):
            return self.json_response(404, {'error':'Not found'})
        return super().do_GET()

    def do_POST(self):
        if not self._trusted() or not secrets.compare_digest(self.headers.get('X-Capture-Token',''), self.server.token):
            return self.json_response(403, {'error':'仅允许本机控制页面发起请求。'})
        try:
            length = int(self.headers.get('Content-Length','0'))
            if not 0 < length <= 1500000: raise ValueError('请求大小超出限制。')
            body = json.loads(self.rfile.read(length))
            path = urlparse(self.path).path
            if path == '/api/start': result = self.server.session.start(body)
            elif path == '/api/stop': result = self.server.session.stop()
            elif path == '/api/cfg':
                content = body.get('content')
                if not isinstance(content, str) or len(content.encode('utf-8')) > 1048576:
                    raise ValueError('CFG 文件不得超过 1 MiB。')
                target = ROOT/'captures'/'configs'/(uuid.uuid4().hex+'.cfg')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(content.lstrip('\ufeff'), encoding='utf-8')
                result = {'path':str(target)}
            else: return self.json_response(404, {'error':'Not found'})
            return self.json_response(200,result)
        except RuntimeError as exc: return self.json_response(409,{'error':str(exc)})
        except (ValueError, TypeError, OSError, AttributeError) as exc: return self.json_response(400,{'error':str(exc)})

    def log_message(self, *_): pass

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8090)
    args = parser.parse_args()
    if os.name != 'nt': parser.error('此服务使用 Windows 原生采集程序。')
    session = CaptureSession()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.session = session; server.token = secrets.token_urlsafe(32)
    print(f'Capture control: http://127.0.0.1:{args.port}', flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally:
        session.stop()
        if session.worker: session.worker.join()
        server.server_close()

if __name__ == '__main__': main()
