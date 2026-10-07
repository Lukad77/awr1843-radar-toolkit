"""Read-only sources, reproducible offline derivatives under session/analysis/replay-v1."""
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import threading

VERSION = 1
SOURCE_NAMES = ('session.json','radar.cfg','radar.bin','radar.bin.frames.csv','belt_samples.csv','belt_quality.json')

class Replay:
    def __init__(self, root, executable, environment):
        self.root=Path(root).resolve()
        self.executable=Path(executable)
        self.environment=environment
        self.lock=threading.Lock()

    def directory(self, name):
        if not isinstance(name,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,100}',name):
            raise ValueError('会话名称无效。')
        parent=(self.root/'captures').resolve(); folder=parent/name
        if folder.resolve().parent != parent or folder.is_symlink() or not folder.is_dir():
            raise ValueError('会话不存在或路径不安全。')
        for item in SOURCE_NAMES:
            source=folder/item
            if source.exists() and (source.resolve().parent!=folder.resolve() or not source.is_file()):
                raise ValueError('会话文件路径不安全。')
        return folder

    def sessions(self):
        result=[]
        parent=self.root/'captures'
        if not parent.exists():return result
        for folder in sorted(parent.iterdir(),reverse=True):
            if not (folder/'session.json').is_file():continue
            try:
                folder=self.directory(folder.name)
                m=json.loads((folder/'session.json').read_text(encoding='utf-8-sig'))
                result.append({'id':folder.name,'state':m.get('state','unknown'),'mode':m.get('mode','unknown'),
                    'bytes':(folder/'radar.bin').stat().st_size if (folder/'radar.bin').exists() else 0,
                    'hasBelt':(folder/'belt_samples.csv').is_file()})
            except (OSError,ValueError):continue
        return result

    @staticmethod
    def rows(path, limit):
        with path.open(encoding='utf-8-sig',newline='') as f:
            result=[]
            for row in csv.DictReader(f):
                result.append(row)
                if len(result)>limit:raise ValueError(f'{path.name} 超出预览上限 {limit} 行。')
        return result

    def load(self, body):
        import numpy as np
        from scipy.signal import butter, sosfiltfilt, detrend
        if not isinstance(body,dict) or set(body)-{'session','fixedBin','dc','low','high'}:
            raise ValueError('回放参数无效。')
        folder=self.directory(body.get('session'))
        fixed=body.get('fixedBin',-1);dc=body.get('dc',True)
        if type(fixed) is not int or not -1<=fixed<=4095 or type(dc) is not bool:
            raise ValueError('距离单元或 DC 参数无效。')
        low=body.get('low',.1);high=body.get('high',.7)
        if type(low) not in (int,float) or type(high) not in (int,float) or not .03<=low<high<=2:
            raise ValueError('滤波范围需满足 0.03 ≤ 下限 < 上限 ≤ 2 Hz。')
        manifest=json.loads((folder/'session.json').read_text(encoding='utf-8-sig'))
        if manifest.get('state') not in ('completed','failed'):
            raise ValueError('会话尚未结束，不能生成离线预览。')
        formal_window=manifest.get('formalWindow')
        if formal_window is not None:
            if not isinstance(formal_window,dict) or any(type(formal_window.get(k)) is not int for k in ('start_ns','end_ns')) or not 0<formal_window['start_ns']<formal_window['end_ns']:
                raise ValueError('共同正式区间 T0/T1 无效，不能确定回放时间基准。')
        index=self.rows(folder/'radar.bin.frames.csv',30000)
        if not index:raise ValueError('本次会话没有雷达完整帧，无法回放。')
        ticks=[int(r['host_rx_monotonic_ns']) for r in index]
        wires=[int(r['wire_frame']) for r in index]
        if any(int(r['file_frame'])!=i for i,r in enumerate(index)) or any(b<a for a,b in zip(ticks,ticks[1:])) or any(b<=a for a,b in zip(wires,wires[1:])):
            raise ValueError('雷达索引或接收时间顺序异常。')
        # Physical byte layout is validated again against the actual CFG by C++.
        if not self.executable.is_file():raise ValueError('缺少 radar_replay_extract，请重新编译项目。')
        fingerprint={name:[(folder/name).stat().st_size,str((folder/name).stat().st_mtime_ns)] for name in SOURCE_NAMES if (folder/name).exists()}
        fingerprint['cfg_sha256']=hashlib.sha256((folder/'radar.cfg').read_bytes()).hexdigest()
        fingerprint['extractor_sha256']=hashlib.sha256(self.executable.read_bytes()).hexdigest()
        fingerprint['backend_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        settings={'version':VERSION,'fixedBin':fixed,'dc':dc,'low':low,'high':high,'rx':0,'neighborSpan':1,'gridHz':50}
        key=hashlib.sha256(json.dumps([fingerprint,settings],sort_keys=True).encode()).hexdigest()[:24]
        analysis=folder/'analysis'
        if analysis.exists() and analysis.resolve().parent!=folder.resolve():raise ValueError('分析目录路径不安全。')
        cache=analysis/'replay-v1'/key
        if cache.resolve().is_relative_to(folder.resolve()) is False:raise ValueError('缓存路径不安全。')
        cache.mkdir(parents=True,exist_ok=True)
        result_path=cache/'preview.json'
        for name in ('preview.json','preview.tmp','phase.tmp','radar_phase.csv','processing.json','preview.csv'):
            if (cache/name).resolve().parent != cache.resolve():raise ValueError('缓存文件路径不安全。')
        if result_path.is_file():return json.loads(result_path.read_text(encoding='utf-8'))
        phase=cache/'radar_phase.csv';temporary=cache/'phase.tmp'
        run=subprocess.run([str(self.executable),str(folder),str(temporary),str(fixed),'1' if dc else '0'],
            env=self.environment(),capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=180,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if run.returncode:raise ValueError('雷达相位提取失败：'+run.stderr.strip())
        temporary.replace(phase)
        ph=self.rows(phase,30000)
        if len(ph)!=len(index):raise ValueError('相位数据与帧索引长度不一致。')
        origin=formal_window['start_ns'] if formal_window else ticks[0];rt=np.array([(v-origin)/1e9 for v in ticks]);ry=np.array([float(r['displacement_mm']) for r in ph])
        bins=[int(r['track_bin']) for r in ph];amps=[float(r['track_amp']) for r in ph]
        if not np.all(np.isfinite(ry)) or not all(math.isfinite(a) for a in amps):raise ValueError('相位或幅度存在非有限数值。')
        gap_indices=[i for i in range(1,len(wires)) if wires[i]!=wires[i-1]+1]
        switches=[float(rt[i]) for i in range(1,len(bins)) if bins[i]!=bins[i-1] and i not in gap_indices]
        belt_rows=self.rows(folder/'belt_samples.csv',300000) if (folder/'belt_samples.csv').is_file() else []
        bt=np.array([(int(r['host_rx_monotonic_ns'])-origin)/1e9 for r in belt_rows]);by=np.array([int(r['value_raw']) for r in belt_rows],dtype=float)
        if len(bt)>1 and np.any(np.diff(bt)<0):raise ValueError('呼吸带接收时间顺序异常。')
        warnings=[]
        if manifest['state']=='failed':warnings.append('该会话采集失败，预览包含部分数据，不能视为完整实验。')
        if gap_indices:warnings.append('存在雷达帧缺口；缺口处分段解缠和滤波，图中不跨缺口连线。')
        if not belt_rows:warnings.append('本会话没有呼吸带数据，仅展示雷达。')
        begin=min(float(rt[0]),float(bt[0])) if len(bt) else float(rt[0])
        end=max(float(rt[-1]),float(bt[-1]) if len(bt) else 0)
        if end-begin>=2000:raise ValueError('预览时间范围超过 2000 秒，请使用较短会话。')
        grid=np.arange(begin,end+.001,.02)
        sos=butter(3,[low,high],fs=50,btype='bandpass',output='sos')
        def filtered(t,y,breaks,valid):
            out=np.full(len(grid),np.nan)
            if not len(t):return out
            positive=np.diff(t);positive=positive[positive>0]
            limit=max(.25,3*float(np.median(positive))) if len(positive) else .25
            boundaries=set(breaks)|{i for i in range(1,len(t)) if t[i]-t[i-1]>limit}
            begin=None
            for i in range(len(t)+1):
                if begin is not None and (i==len(t) or i in boundaries or not valid[i]):
                    x=t[begin:i];v=y[begin:i];unique,inv=np.unique(x,return_inverse=True)
                    means=np.bincount(inv,weights=v)/np.bincount(inv)
                    pick=(grid>=x[0])&(grid<=x[-1])
                    if pick.sum()>24:
                        out[pick]=sosfiltfilt(sos,detrend(np.interp(grid[pick],unique,means)))
                    begin=None
                if i<len(t) and valid[i] and begin is None:begin=i
            good=np.isfinite(out)
            if good.sum()>1 and out[good].std()>1e-12:out[good]=(out[good]-out[good].mean())/out[good].std()
            else:out[:]=np.nan
            return out
        rb=filtered(rt,ry,gap_indices,np.ones(len(rt),dtype=bool))
        bb=filtered(bt,by,[],(by>=0)&(by<=1023))
        if not np.any(np.isfinite(rb)):warnings.append('雷达片段过短或无变化，无法生成滤波预览。')
        if rt[-1]-rt[0]<2/low:warnings.append('记录不足所选最低频率的两个周期；低频与滤波边界结果需谨慎解释。')
        def clean(values):return [float(v) if np.isfinite(v) else None for v in values]
        def with_breaks(t,y,breaks):
            breaks=set(breaks)
            xx=[];yy=[]
            for i,(x,v) in enumerate(zip(t,y)):
                if i in breaks:xx.append(float((t[i-1]+x)/2));yy.append(None)
                xx.append(float(x));yy.append(float(v))
            return [xx,yy]
        phases=[]
        for row,t in zip(belt_rows,bt):
            if not phases or phases[-1]['phase']!=row['phase']:phases.append({'time':float(t),'phase':row['phase']})
        rail=sum(v in (0,1023) for v in by);outside=int(np.sum((by<0)|(by>1023)))
        if rail or outside:warnings.append(f'呼吸带全程端点命中 {rail} 个、超量程 {outside} 个；原值保留，请结合阶段审计。')
        formal=[int(r['value_raw']) for r in belt_rows if
                (formal_window['start_ns']<=int(r['host_rx_monotonic_ns'])<formal_window['end_ns']
                 if formal_window else r['phase']=='formal')]
        if formal_window:
            phases=[marker for marker in phases if marker['phase'] not in ('formal','tail')]
            phases.extend([{'time':0.,'phase':'formal'},
                           {'time':(formal_window['end_ns']-origin)/1e9,'phase':'tail'}])
            phases.sort(key=lambda marker:marker['time'])
        formal_summary=None
        if formal_window:
            formal_summary={'duration':(formal_window['end_ns']-formal_window['start_ns'])/1e9,
                            'radarFrames':sum(formal_window['start_ns']<=t<formal_window['end_ns'] for t in ticks),
                            'beltSamples':len(formal),'railHits':sum(v in (0,1023) for v in formal),
                            'outOfRange':sum(not 0<=v<=1023 for v in formal)}
        result={'session':folder.name,'state':manifest['state'],'settings':settings,'cacheKey':key,'cacheDirectory':str(cache),
          'timeBasis':'common_formal_t0' if formal_window else 'first_radar_frame',
          'summary':{'radarFrames':len(rt),'beltSamples':len(bt),'duration':float(rt[-1]-rt[0]),'switches':len(switches),
          'total':{'radarFrames':len(rt),'beltSamples':len(bt),'duration':end-begin},'formal':formal_summary,
          'radarGaps':len(gap_indices),'formalSamples':len(formal),'formalRails':sum(v in (0,1023) for v in formal),
          'formalOutOfRange':sum(not 0<=v<=1023 for v in formal),'radarStats':manifest.get('radarStats',{}),'beltQuality':manifest.get('beltResult',{})},
          'warnings':warnings,'radar':with_breaks(rt,ry,gap_indices),'belt':with_breaks(bt,by,[]),
          'comparison':[clean(grid),clean(rb),clean(bb)],'track':[rt.tolist(),bins,amps],
          'markers':{'formalWindow':[0.,(formal_window['end_ns']-origin)/1e9] if formal_window else [],'switches':switches,'gaps':[float(rt[i]) for i in gap_indices],
          'rails':[float(t) for t,v in zip(bt,by) if v in (0,1023)],'outOfRange':[float(t) for t,v in zip(bt,by) if not 0<=v<=1023],'phases':phases},
          'provenance':{'sources':fingerprint,'extractorLog':run.stdout.strip(),
            'processing':'RX0; actual cfg; chirp averaging, neighbor ±1; optional adaptive DC; gap resets; 50Hz display interpolation; linear detrend + 3rd order zero-phase Butterworth + separate z-score. No lag fitting or polarity flipping.',
            'clock':('Host receive QPC, seconds relative to common formal T0; negative times are pre-roll.' if formal_window else 'Host receive QPC, seconds relative to first radar complete frame.')+' Not calibrated hardware synchronization.'}}
        import scipy
        result['provenance']['libraries']={'numpy':np.__version__,'scipy':scipy.__version__}
        for name in SOURCE_NAMES:
            if name in fingerprint and [(folder/name).stat().st_size,str((folder/name).stat().st_mtime_ns)] != fingerprint[name]:
                raise RuntimeError('分析期间源文件发生变化，请重新加载。')
        (cache/'processing.json').write_text(json.dumps({'settings':settings,'provenance':result['provenance']},ensure_ascii=False,indent=2),encoding='utf-8')
        with (cache/'preview.csv').open('w',newline='',encoding='utf-8') as f:
            writer=csv.writer(f);writer.writerow(['relative_time_s','radar_bandpass_z','belt_bandpass_z']);writer.writerows(zip(*result['comparison']))
        temp=result_path.with_suffix('.tmp');temp.write_text(json.dumps(result,ensure_ascii=False,allow_nan=False),encoding='utf-8');temp.replace(result_path)
        return result
