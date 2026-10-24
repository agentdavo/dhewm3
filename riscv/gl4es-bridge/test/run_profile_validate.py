#!/usr/bin/env python3
from pathlib import Path
import os
import re
import subprocess
import tempfile

here=Path(__file__).resolve().parent
cc=os.environ.get('CC', 'C:/msys64/ucrt64/bin/gcc.exe' if os.name=='nt' else 'cc')
if os.name=='nt':
    os.environ['PATH']=str(Path(cc).parent)+os.pathsep+os.environ.get('PATH','')
with tempfile.TemporaryDirectory() as tmp:
    exe=Path(tmp)/'profile-test.exe'
    subprocess.run([cc,'-O2',str(here/'profile_validate.c'),str(here.parent/'profile.c'),'-o',str(exe)],check=True)
    output=subprocess.check_output([str(exe)],text=True)
    rows=[dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',line))
          for line in output.splitlines() if line.startswith('[shim]')]
    a,b=rows
    assert a['inclusive_us']==5 and a['driver_us']==3 and a['exclusive_us']==2, output
    assert a['calls']==1 and a['live_bytes']==200 and a['blocks']==1, output
    assert a['allocated_bytes']-a['freed_bytes']==a['live_bytes'], output
    assert b['live_bytes']==0 and b['blocks']==0 and b['calls']==0, output
    assert not any(r['untracked_frees'] or r['overflow'] or r['depth'] for r in rows), output
    print('PASS: nested GLES timing excluded once; allocations conserved; frame counters reset')
