#!/usr/bin/env python3
"""Summarize sampled worker-wall intervals, without treating them as frame time."""
import argparse, json, re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path); p.add_argument('--first',type=int,default=0)
p.add_argument('--last',type=int,default=1000000000); p.add_argument('--output',type=Path,required=True)
a=p.parse_args(); totals={}; samples={}; pending=[]; frames=[]
for line in a.log.read_text(errors='replace').splitlines():
    if line.startswith('[pareto]'):
        pending.append({k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)})
    m=re.match(r'frame:(\d+)',line)
    if not m: continue
    if a.first<=int(m[1])<=a.last and pending:
        frames.append(int(m[1]))
        for d in pending:
            mask=d['mask']; samples[mask]=samples.get(mask,0)+d['samples']
            for stage in ('coverage','interp','deriv','shader','output'):
                key=f"{stage}/mask{mask}"; totals[key]=totals.get(key,0)+d[stage+'_us']
    pending=[]
if not sum(totals.values()): p.error('No completed sampled frames in selected range')
all_us=sum(totals.values()); cumulative=0; rows=[]
for key,t in sorted(totals.items(),key=lambda kv:-kv[1]):
    cumulative+=t
    rows.append(dict(stage_mask=key,sampled_us=t,share_percent=100*t/all_us,cumulative_percent=100*cumulative/all_us))
stages={k:sum(v for key,v in totals.items() if key.startswith(k+'/')) for k in ('coverage','interp','deriv','shader','output')}
result=dict(log=str(a.log),first=a.first,last=a.last,completed_frames=frames,samples=samples,stage_us=stages,rows=rows,
    caveat='Sampled worker wall time; excludes triangle/row setup and probe selection; diagnostic build perturbs register allocation; not frame latency or CPU cycles.')
a.output.write_text(json.dumps(result,indent=2)+'\n')
for k,v in sorted(stages.items(),key=lambda kv:-kv[1]): print(f'{k:12} {100*v/all_us:6.2f}%')
for row in rows[:12]: print(f"{row['stage_mask']:22} {row['share_percent']:6.2f}% cumulative {row['cumulative_percent']:6.2f}%")
