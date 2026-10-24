#!/usr/bin/env python3
"""Compare matched-work raster payloads across VLEN; never sum unlike workloads."""
import argparse,json,re,statistics
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path('riscv/build/doom-matrix'))
p.add_argument('--repetition',type=int,default=0)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();rows=[];components=[]
for vlen in (128,256,512,1024):
    folder=a.root/str(vlen);text=(folder/f'setup-{a.repetition}.log').read_text(errors='replace')
    groups={}
    for nv,small,color,draws,r,base,opt in re.findall(r'\[setup-perf\] nvary=(\d+) small=(\d+) color=(\d+) draws=(\d+) round=(\d+) baseline_ticks=(\d+) optimized_ticks=(\d+)',text):
        if int(r)==0:continue
        key=(int(nv),int(small),int(color),int(draws));groups.setdefault(key,[]).append((int(base),int(opt)))
    for (nv,small,color,draws),pairs in groups.items():
        # QEMU virt timebase is 10 MHz in these payloads: 10 ticks per us.
        rows.append(dict(vlen=vlen,nvary=nv,small=small,color=color,draws=draws,
            baseline_us_per_draw=statistics.median(x for x,y in pairs)/10/draws,
            optimized_us_per_draw=statistics.median(y for x,y in pairs)/10/draws,
            paired_speedup=statistics.median(x/y for x,y in pairs)))
    text=(folder/f'interaction-{a.repetition}.log').read_text(errors='replace')
    k=re.findall(r'\[gik\] kernel (\d+) ns/frag',text)
    text=(folder/f'resolve-{a.repetition}.log').read_text(errors='replace')
    resolve=re.findall(r'rvv_ticks=(\d+)',text)
    components.append(dict(vlen=vlen,interaction_kernel_ns_per_fragment=int(k[-1]),
        resolve_640x480_us=statistics.median(map(int,resolve))/10))
layouts=[{(r['nvary'],r['small'],r['color']) for r in rows if r['vlen']==v} for v in (128,256,512,1024)]
if not layouts[0] or any(x!=layouts[0] for x in layouts):raise SystemExit('Incomplete or mismatched VLEN workload sets')
a.output.write_text(json.dumps(dict(raster=rows,components=components,
    caveat='QEMU worker wall time; matched workloads, not whole-game shares or physical-hardware throughput.'),indent=2)+'\n')
for row in rows:
    print(f"VLEN={row['vlen']:4} n={row['nvary']:2} small={row['small']} color={row['color']} baseline={row['baseline_us_per_draw']:.3f}us optimized={row['optimized_us_per_draw']:.3f}us speedup={row['paired_speedup']:.3f}")
print(json.dumps(components,indent=2))
