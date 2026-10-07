"""Derive a common half-open [T0, T1) interval without rewriting raw sources."""
import csv
from pathlib import Path

def export_formal_window(directory, stats, require_belt=False):
    directory = Path(directory)
    t0, t1 = stats.get('formalStartNs', 0), stats.get('formalEndNs', 0)
    if not t0 or t1 <= t0:
        return None
    window = {'start_ns':t0, 'end_ns':t1, 'duration_s':(t1-t0)/1e9,
              'interval':'[start_ns, end_ns)', 'time_basis':'host_receive_QPC',
              'radar_frames':0, 'belt_samples':0, 'radar_first_file_frame':None}
    for source, target, kind in [('radar.bin.frames.csv','radar_formal.frames.csv','radar'),
                                 ('belt_samples.csv','belt_formal.samples.csv','belt')]:
        path = directory/source
        if not path.exists():
            if kind == 'radar' or require_belt:
                raise ValueError('正式区间缺少原始索引：'+source)
            continue
        first = last = None
        quality = dict(samples=0, rail_hits=0, out_of_range=0, min=None, max=None)
        with path.open(encoding='utf-8-sig',newline='') as src, (directory/target).open('w',encoding='utf-8',newline='') as dst:
            rows=csv.DictReader(src)
            writer=csv.DictWriter(dst,fieldnames=[*rows.fieldnames,'relative_time_s'])
            writer.writeheader()
            for row in rows:
                stamp=int(row['host_rx_monotonic_ns'])
                if last is not None and stamp < last:
                    raise ValueError(source+' 时间戳逆序')
                first=stamp if first is None else first
                last=stamp
                if not t0 <= stamp < t1:
                    continue
                row['relative_time_s']=f'{(stamp-t0)/1e9:.9f}'
                # The asynchronous live phase marker is not the experiment boundary.
                if kind=='belt':
                    row['phase']='formal'
                    value=int(row['value_raw']);quality['samples']+=1
                    quality['rail_hits']+=int(value in (0,1023))
                    quality['out_of_range']+=int(not 0<=value<=1023)
                    quality['min']=value if quality['min'] is None else min(value,quality['min'])
                    quality['max']=value if quality['max'] is None else max(value,quality['max'])
                    window['belt_samples']+=1
                else:
                    if window['radar_first_file_frame'] is None:
                        window['radar_first_file_frame']=int(row['file_frame'])
                        if stamp!=t0:raise ValueError('共同 T0 未对应雷达完整帧')
                    window['radar_frames']+=1
                writer.writerow(row)
        if kind=='belt':
            window['belt_quality']=quality
            window['belt_covers_window']=first is not None and first<=t0 and last>=t1
    if window['radar_frames']!=stats.get('formalFrames'):
        raise ValueError('正式帧统计与共同时间区间不一致')
    return window
