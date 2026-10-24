#!/usr/bin/env python3
"""Exercise production batch-state helpers concurrently at all supported widths."""
import os
from pathlib import Path
import subprocess

sg = Path(__file__).resolve().parents[1]
out = sg.parent / 'build/visual-audit'
out.mkdir(parents=True, exist_ok=True)
for lanes in (8, 16, 32, 64):
    exe = out / f'frag-tls-{lanes}{".exe" if os.name == "nt" else ""}'
    # UCRT64 GCC 16.2 IV optimization emits ordinary .refptr addresses for
    # external TLS array induction variables. Disable that host-only pass;
    # the RISC-V production flags are unchanged. O0 also passes this regression.
    flags = ['-fno-ivopts'] if os.name == 'nt' else []
    subprocess.run([os.environ.get('CC', 'gcc'), '-O2', *flags,
                    f'-DSGL_SIMT_LANES={lanes}', '-I' + str(sg / 'src'),
                    '-I' + str(sg / 'include'), str(sg / 'src/glsl_simt.c'),
                    str(sg / 'test/frag_tls_validate.c'), '-pthread', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
