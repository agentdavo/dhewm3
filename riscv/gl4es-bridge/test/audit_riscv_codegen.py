#!/usr/bin/env python3
"""Recompile gl4es with plain RV64GCV; retain objects and vector reports locally.

Run build-gl4es.sh riscv first to export the configured compile commands.
This audit never replaces the shipping library or its RVA23 configuration.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json, os, shlex, subprocess

root = Path(__file__).resolve().parents[3]
out = root/'build-rv/gl4es-rv64gcv-audit'
out.mkdir(parents=True, exist_ok=True)
os.environ['TMP'] = os.environ['TEMP'] = str(out)
os.environ['PATH'] = str(root/'build/toolchain/install/bin') + ';C:/msys64/ucrt64/bin;' + os.environ['PATH']
commands = json.loads((root/'build-rv/gl4es/compile_commands.json').read_text())
def compile_one(item):
    name = Path(item['file']).stem
    args = shlex.split(item['command'].replace('\\', '/'))
    args = ['-march=rv64gcv' if a.startswith('-march=') else a for a in args]
    args[args.index('-o') + 1] = str(out/(name+'.o'))
    report = out/(name+'.vec')
    report.write_text('')
    args += ['-fopt-info-vec-all='+str(report)]
    p = subprocess.run(args, cwd=item['directory'], capture_output=True, text=True)
    (out/(name+'.log')).write_text(p.stdout+p.stderr)
    return {'file': item['file'], 'returncode': p.returncode,
            'vectorized_loops': report.read_text().count('optimized: loop vectorized')}
with ThreadPoolExecutor(max_workers=6) as pool:
    results = list(pool.map(compile_one, commands))
(out/'summary.json').write_text(json.dumps(results, indent=2))
failures = [r for r in results if r['returncode']]
print(f"RV64GCV: {len(results)} files, {len(failures)} failures; reports in {out}")
if failures:
    print(failures)
    raise SystemExit(1)
