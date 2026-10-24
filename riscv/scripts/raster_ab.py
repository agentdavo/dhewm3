#!/usr/bin/env python3
"""Alternate a raster diagnostic switch in an already frozen QEMU scene.

Disable sgl_prof_enabled before use. Requires the visual_probe.py debugger ports.
Capture pauses and two subsequent frames are excluded from every timing block.
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('log',type=Path)
p.add_argument('--prefix',type=Path,required=True)
p.add_argument('--samples',type=int,default=10)
p.add_argument('--rounds',type=int,default=2)
p.add_argument('--symbol',action='append',help='Repeat to toggle several switches together')
a=p.parse_args()
symbols=a.symbol or ['sgl_alpha_only_store']
probe=Path(__file__).with_name('visual_probe.py')

def rows():
    return [{'frame':int(f),'engine_ms':int(ms)} for f,ms in
            re.findall(r'^frame:(\d+) all:\s*(\d+)',a.log.read_text(errors='replace'),re.M)]

def wait_frame(target):
    deadline=time.monotonic()+180
    while time.monotonic()<deadline:
        data=rows()
        if data and data[-1]['frame']>=target:return data
        time.sleep(.5)
    raise RuntimeError(f'No completed frame {target} before timeout')

def invoke(*args):
    subprocess.run([sys.executable,str(probe),*args],check=True)

phases=[]
for round in range(a.rounds):
    for enabled in ((1,0) if round%2==0 else (0,1)):
        for symbol in symbols:invoke('--set-int',symbol,str(enabled))
        wait_frame(rows()[-1]['frame']+2)
        shot=Path(f'{a.prefix}-{round}-{enabled}.png')
        invoke('--capture',str(shot))
        start=rows()[-1]['frame']+3
        data=wait_frame(start+a.samples-1)
        chosen=[r for r in data if start<=r['frame']<start+a.samples]
        assert len(chosen)==a.samples
        phase={'round':round,'enabled':enabled,'capture':str(shot),'rows':chosen,
               'median_ms':statistics.median(r['engine_ms'] for r in chosen)}
        phases.append(phase)
        print(json.dumps(phase),flush=True)
        Path(f'{a.prefix}-results.json').write_text(json.dumps(phases,indent=2)+'\n')
for symbol in symbols:invoke('--set-int',symbol,'1')
print('A/B complete; optimized switch restored.',flush=True)
