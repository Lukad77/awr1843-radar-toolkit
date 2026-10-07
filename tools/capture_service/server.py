"""Local Windows capture service; pySerial is optional for HKH-11C."""
import argparse
from collections import deque
from datetime import datetime, timezone
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import math
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
from replay import Replay
from formal_window import export_formal_window

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
        self.total_frames = 0
        self.formal_window = None

    def snapshot(self):
        with self.lock:
            return dict(state=self.state, frames=self.frames, bytes=self.bytes,
                        directory=str(self.directory or ''), error=self.error,
                        belt=self.belt_status, result=self.result, logs=list(self.logs),
                        totalFrames=self.total_frames, formalWindow=dict(self.formal_window) if self.formal_window else None)

    def start(self, request):
        with self.lock:
            if self.worker and self.worker.is_alive():
                raise RuntimeError('已有采集会话，请先结束并等待保存完成。')
            if not self.executable.is_file():
                raise ValueError('请先编译 build/radar_capture.exe。')
            if not isinstance(request, dict):
                raise ValueError('请求必须是 JSON 对象。')
            allowed = {'cfgPath', 'serial', 'bindIp', 'dcaIp', 'dataPort', 'configPort', 'packetDelayUs', 'lvdsLanes', 'maxFrames', 'belt', 'beltPort', 'beltGain', 'durationSeconds'}
            if set(request) - allowed:
                raise ValueError('未知配置字段：' + ', '.join(sorted(set(request)-allowed)))
            duration = request.get('durationSeconds', 30)
            if type(duration) not in (int,float) or not math.isfinite(duration) or not 0 <= duration <= 86400:
                raise ValueError('正式采集时长必须为 0..86400 秒，0 表示手动结束。')
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
            validation_fields = dict(line.split('=',1) for line in validation.stdout.splitlines() if '=' in line)
            if validation_fields.get('numFrames') != '0':
                raise ValueError('共同区间采集要求 CFG 的 frameCfg 帧数为 0（连续采集）；请在网页设置正式时长/帧数。')
            cfg.update(cfg=str(directory / 'radar.cfg'), output=str(directory / 'radar.bin'))
            write_json(directory/'capture.json', {'capture':cfg})
            origin = time.perf_counter_ns()
            manifest = dict(session=name, state='starting', mode='radar+belt' if belt else 'radar',
                            cfgSource=str(source.resolve()), capture=cfg, beltAdapter=belt or None,
                            beltConfig=belt_config, syncVersion=2, formalWindow=None,
                            formalPolicy={'durationSeconds':duration, 'maxFrames':cfg['maxFrames'],
                                          'minimumConsecutiveRadarFrames':3},
                            clock={'kind':'Windows QPC / Python perf_counter_ns', 'origin_monotonic_ns':origin,
                                   'origin_utc_ns':time.time_ns(), 'meaning':'host receive time, not hardware trigger'},
                            events=[{'name':'session_start_requested', 'monotonic_ns':origin}])
            write_json(directory/'session.json', manifest)
            self.state = 'starting'; self.frames = self.bytes = self.total_frames = 0
            self.formal_window = None
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
                self.total_frames, self.bytes = int(parts[1]), int(parts[2])
            if parts and parts[0] == 'FORMAL_PROGRESS' and len(parts) == 2:
                self.frames = int(parts[1])

    def _run(self, directory, manifest, belt_name):
        process = None; adapter = None; reader = None; failure = ''; code = None
        reader_errors = []
        gates = {name:threading.Event() for name in ('ARMED','READY','STABLE')}
        def send(command):
            process.stdin.write(command+'\n'); process.stdin.flush()
        def remember(name, stamp=None):
            manifest['events'].append({'name':name,'monotonic_ns':time.perf_counter_ns() if stamp is None else stamp})
        def read_output():
            try:
                with (directory/'capture.log').open('w',encoding='utf-8') as log:
                    for line in process.stdout:
                        log.write(line); log.flush(); self._log(line)
                        parts=line.split()
                        if len(parts)<2 or not parts[1].isdigit():continue
                        name,stamp=parts[0],int(parts[1])
                        if name in gates:
                            remember('radar_'+name.lower(),stamp);gates[name].set()
                        elif name in ('FIRST_PACKET','FIRST_FRAME','FORMAL_GATE','DCA_START_BEGIN','DCA_START_ACK',
                                      'SENSOR_START_BEGIN','SENSOR_START_ACK','DCA_STOP_BEGIN','DCA_STOP_ACK',
                                      'SENSOR_STOP_BEGIN','SENSOR_STOP_ACK','SENSOR_START_TX_BEGIN','SENSOR_START_TX_END',
                                      'SENSOR_START_RESPONSE','SENSOR_STOP_TX_BEGIN','SENSOR_STOP_TX_END','SENSOR_STOP_RESPONSE'):
                            remember('radar_'+name.lower(),stamp)
                        elif name=='FORMAL_START':
                            remember('formal_start',stamp)
                            with self.lock:
                                self.formal_window={'start_ns':stamp,'end_ns':None,'radar_first_file_frame':int(parts[2])}
                                if self.state=='starting':self.state='recording'
                            if adapter and hasattr(adapter,'mark_phase') and not self._cancelled():adapter.mark_phase('formal')
                        elif name=='FORMAL_END':
                            remember('formal_end',stamp)
                            with self.lock:
                                if self.formal_window:self.formal_window['end_ns']=stamp
                                self.state='stopping'
                            if adapter and hasattr(adapter,'mark_phase'):adapter.mark_phase('tail')
            except Exception as exc:
                reader_errors.append(str(exc));self.stop()
        try:
            env=command_environment();env['RADAR_SESSION_CONTROL']='1'
            process=subprocess.Popen([str(self.executable),'--json',str(directory/'capture.json')],
                    cwd=self.root,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                    text=True,encoding='utf-8',errors='replace',bufsize=1,
                    creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            reader=threading.Thread(target=read_output,daemon=True);reader.start()
            deadline=time.monotonic()+60
            while not gates['ARMED'].wait(.05):
                if self._cancelled():raise RuntimeError('采集在准备阶段已取消。')
                if process.poll() is not None:raise RuntimeError('雷达准备失败，请查看设备日志。')
                if time.monotonic()>deadline:raise RuntimeError('雷达准备超时。')
            if self._cancelled():raise RuntimeError('采集在准备阶段已取消。')
            if belt_name:
                adapter=self.adapters[belt_name]()
                with self.lock:self.belt_status={'phase':'prepare','samples':0}
                adapter.prepare(SessionContext(directory,manifest['clock']['origin_monotonic_ns'],
                    manifest['clock']['origin_utc_ns'],time.perf_counter_ns,manifest['beltConfig'],self._cancelled))
            # Radar preparation is complete. Start continuous pre-roll on both devices.
            remember('preroll_start_requested');send('start')
            if adapter:
                adapter.start()  # acknowledges gain and waits the bounded settle period
                remember('belt_stable')
            stop_sent=None;formal_sent=False
            while process.poll() is None:
                status=adapter.status() if adapter else None
                if status is not None:
                    with self.lock:self.belt_status=status
                    if status.get('error'):
                        failure='呼吸带采集失败：'+str(status['error']);self.stop()
                stopping=self._cancelled()
                if stopping and stop_sent is None:
                    stop_sent=time.monotonic();remember('stop_requested')
                    if adapter and hasattr(adapter,'mark_phase'):adapter.mark_phase('tail')
                    try:send('stop')
                    except (BrokenPipeError,OSError):pass
                if not stopping and not formal_sent and gates['READY'].is_set() and gates['STABLE'].is_set():
                    fresh=not adapter or (status.get('samples',0)>0 and
                        0<=time.perf_counter_ns()-status.get('last_valid_rx_monotonic_ns',status.get('last_rx_monotonic_ns',0))<=250000000)
                    if fresh:
                        stamp=time.perf_counter_ns();remember('formal_requested',stamp)
                        send(f"formal {stamp} {round(manifest['formalPolicy']['durationSeconds']*1e9)}")
                        formal_sent=True
                if stop_sent is not None and time.monotonic()-stop_sent>45:
                    failure='停止超时，进程已终止；请检查雷达是否需要手动复位。';process.kill()
                time.sleep(.02)
            code=process.wait();reader.join()
            if reader_errors:failure='日志写入失败：'+reader_errors[0]
            if code!=0 and not failure:
                failure='采集在预采集阶段已取消。' if self._cancelled() and not self.formal_window else f'采集未完整成功（退出码 {code}），请查看日志和丢包统计。'
        except Exception as exc:
            failure=str(exc)
        finally:
            if process and process.poll() is None:
                try:send('stop');process.wait(timeout=45)
                except (OSError,subprocess.TimeoutExpired):
                    process.kill();process.wait();failure+='；雷达停止未确认，请检查设备。'
            if reader:reader.join(timeout=5)
            if process:
                for stream in (process.stdin,process.stdout):
                    try:stream.close()
                    except OSError:pass
            if adapter:
                try:
                    result=adapter.stop();manifest['beltResult']=result
                    with self.lock:self.belt_status=result
                    if result.get('error'):failure=failure or str(result['error'])
                except Exception as exc:failure=failure or f'呼吸带收尾失败：{exc}'
            stats={}
            try:
                path=directory/'radar.bin.stats.txt'
                if path.exists():
                    stats={k:int(v) for k,v in (line.split('=',1) for line in path.read_text().splitlines())}
                window=export_formal_window(directory,stats,require_belt=belt_name=='HKH-11C')
                manifest['formalWindow']=window
                with self.lock:self.formal_window=window
                if window and 'belt_quality' in window:
                    manifest['beltResult']['formal_window_stats']=window['belt_quality']
                    write_json(directory/'belt_quality.json',manifest['beltResult'])
                    if not window['belt_covers_window']:failure=failure or '呼吸带接收时间未完整覆盖共同正式区间。'
                if not window and not failure:failure='没有建立共同正式采集区间。'
            except (OSError,ValueError,KeyError) as exc:failure=failure or f'共同区间核验失败：{exc}'
            manifest.update(state='failed' if failure else 'completed',error=failure,exitCode=code,
                radarStats=stats,finishedUtc=datetime.now(timezone.utc).isoformat())
            remember('session_finished');manifest['events'].sort(key=lambda event:event['monotonic_ns'])
            try:write_json(directory/'session.json',manifest)
            except OSError as exc:failure=failure or f'会话记录保存失败：{exc}'
            with self.lock:
                self.result=stats;self.error=failure;self.frames=stats.get('formalFrames',0)
                self.total_frames=stats.get('savedFrames',0)
                self.state='failed' if failure else 'completed'
                if failure:self.logs.append(failure)

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
        if path == '/api/replay/sessions': return self.json_response(200, self.server.replay.sessions())
        if path == '/api/options':
            return self.json_response(200, {'token':self.server.token, 'cfgPath':str(ROOT/'awr1843.windows.cfg'),
                    'outputRoot':str(ROOT/'captures'), 'belts':list(self.server.session.adapters),
                    'beltPorts':enumerate_belt_ports()})
        if path == '/api/belt-ports': return self.json_response(200, enumerate_belt_ports())
        if path in ('/vendor/uPlot.iife.min.js', '/vendor/uPlot.min.css'):
            data=(ROOT/'web'/path.lstrip('/')).read_bytes()
            self.send_response(200)
            self.send_header('Content-Type','text/javascript' if path.endswith('.js') else 'text/css')
            self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
            return
        if path not in ('/', '/index.html', '/app.js', '/style.css','/replay.html','/replay.js','/replay.css','/replay-scope.js'):
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
            if path in ('/api/start','/api/replay'):
                replay = getattr(self.server, 'replay', None)
                if replay and not replay.lock.acquire(blocking=False):
                    raise RuntimeError('离线分析正在进行，请等待完成后再操作。')
                try:
                    if path == '/api/start': result = self.server.session.start(body)
                    else:
                        if self.server.session.snapshot()['state'] in ('starting','recording','stopping'):
                            raise RuntimeError('请先结束实时采集，再生成离线预览。')
                        result = replay.load(body)
                finally:
                    if replay: replay.lock.release()
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
        except ImportError: return self.json_response(400,{'error':'离线回放需要 numpy/scipy，请安装 requirements-replay.txt。'})
        except subprocess.TimeoutExpired: return self.json_response(400,{'error':'离线提取超过 180 秒，请使用较短会话。'})
        except (KeyError, IndexError) as exc: return self.json_response(400,{'error':'会话文件字段缺失或损坏：'+str(exc)})
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
    server.replay = Replay(ROOT, ROOT/'build/radar_replay_extract.exe', command_environment)
    print(f'Capture control: http://127.0.0.1:{args.port}', flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally:
        session.stop()
        if session.worker: session.worker.join()
        server.server_close()

if __name__ == '__main__': main()
