#!/usr/bin/env python3
"""Symbolize all-hart first-frame PC samples; sampling runs are not benchmarks."""
from pathlib import Path
from collections import Counter
import argparse,json,subprocess
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('samples',type=Path)
p.add_argument('--elf',type=Path,default=root/'build-rv/dhewm3')
a=p.parse_args();samples=json.loads(a.samples.read_text())
addresses=sorted({x[k] for x in samples for k in ('pc','ra')})
addresses=sorted(set(addresses)|{addr-1 for x in samples for addr in x.get('stack',[])})
tool=root/'build/toolchain/install/bin/riscv64-unknown-elf-addr2line.exe'
symbols={}
for first in range(0,len(addresses),200):
    batch=addresses[first:first+200]
    lines=subprocess.check_output([str(tool),'-f','-C','-e',str(a.elf)]+[hex(x) for x in batch],text=True).splitlines()
    for i,addr in enumerate(batch):symbols[addr]=(lines[i*2],lines[i*2+1])
report=[]
for thread in sorted({x['thread'] for x in samples}):
    rows=[x for x in samples if x['thread']==thread]
    report.append(f'\nThread {thread}: {len(rows)} samples')
    for key in ('pc','ra'):
        report.append(key.upper())
        for (function,location),count in Counter(symbols[x[key]] for x in rows).most_common(20):
            report.append(f'{count:5d} {100*count/len(rows):6.2f}% {function} | {location}')
    if any('stack' in x for x in rows):
        report.append('STACK CHAINS (frame-pointer walk, may be incomplete)')
        chains=Counter(tuple(symbols[addr-1][0] for addr in x.get('stack',[])) for x in rows)
        for chain,count in chains.most_common(12):
            report.append(f'{count:5d} {100*count/len(rows):6.2f}% '+ ' <- '.join(chain))
text='\n'.join(report)+'\n';print(text)
a.samples.with_suffix('.txt').write_text(text)
