#!/usr/bin/env python3
"""Summarize a settled shim profile range (shim frame numbers, inclusive)."""
import argparse
import json
from pathlib import Path
import re
from statistics import median

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--first',type=int,required=True)
p.add_argument('--last',type=int,required=True)
p.add_argument('--output',type=Path)
a=p.parse_args()
rows=[]
render_ms=None
heap_mb=None
for line in a.log.read_text(errors='replace').splitlines():
    if line.startswith('[rprof]'):
        render_ms=float(re.search(r' total=([\d.]+)ms',line)[1])
    if line.startswith('[rprof-heap]'):
        heap_mb=int(re.search(r'committed=(\d+)MB',line)[1])
    if line.startswith('[shim]'):
        row={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
        row['frame_allocated_bytes']=row['allocated_bytes']-(rows[-1]['allocated_bytes'] if rows else 0)
        row['frame_freed_bytes']=row['freed_bytes']-(rows[-1]['freed_bytes'] if rows else 0)
        row['render_ms']=render_ms
        row['heap_mb']=heap_mb
        row['apis']=[]
        rows.append(row)
    elif line.startswith('[shim-api]') and rows:
        rows[-1]['apis'].append({'name':line.split()[1],**{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}})
    elif line.startswith('frame:') and rows:
        rows[-1]['engine_ms']=int(re.search(r' all:\s*(\d+)',line)[1])
chosen=[r for r in rows if a.first<=r['frame']<=a.last]
assert len(chosen)==a.last-a.first+1, 'Incomplete sample range'
assert all(not(r['untracked_frees'] or r['overflow'] or r['depth']) for r in chosen)
result={'samples':len(chosen),'first':a.first,'last':a.last}
for key in ['exclusive_us','inclusive_us','driver_us','calls','live_bytes','peak_bytes','blocks','allocs','frees','frame_allocated_bytes','frame_freed_bytes','render_ms','engine_ms','heap_mb']:
    vals=[r[key] for r in chosen]
    result[key]={'min':min(vals),'median':median(vals),'max':max(vals)}
result['exclusive_percent_engine_median']=median(r['exclusive_us']/1000/r['engine_ms']*100 for r in chosen)
result['rows']=chosen
text=json.dumps(result,indent=2)
if a.output:a.output.write_text(text+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))
