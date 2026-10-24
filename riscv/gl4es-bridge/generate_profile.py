#!/usr/bin/env python3
"""Generate typed, diagnostic-only GL/GLES boundary wrappers from local headers."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)

def args(params):
    if params.strip() in ('', 'void'):
        return ''
    names = []
    for p in params.split(','):
        m = re.search(r'(\w+)\s*(?:\[[^]]*\])?\s*$', p)
        if not m:
            raise ValueError(f'Unsupported argument: {p}')
        names.append(m[1])
    return ', '.join(names)

front = [(name, ret.strip(), params) for name, ret, params in re.findall(
    r'^QGLPROC\((\w+),\s*([^,]+),\s*\(([^\n]*)\)\)',
    (root/'neo/renderer/qgl_proc.h').read_text(), re.M)]
qgl = (root/'neo/renderer/qgl.h').read_text()
ext_names = set(re.findall(r'\bq(gl\w+)', qgl.split('#if defined(D3_GLPROF)')[0]))
glext = (root/'riscv/gl4es/include/GL/glext.h').read_text()
known = {n for n, _, _ in front}
for ret, name, params in re.findall(r'GLAPI\s+([^;\n]+?)\s+APIENTRY\s+(gl\w+)\s*\(([^;]*)\);', glext):
    if name in ext_names and name not in known and 'Callback' not in name:
        front.append((name, ret.strip(), params.strip()))
        known.add(name)
assert len(front) < 512
code = ['#include <GL/gl.h>', '#include <GL/glext.h>', '#include <string.h>',
        '#include <stdio.h>', '#include "profile.h"',
        'extern void *__real_gl4es_GetProcAddress(const char *);']
for i, (name, ret, params) in enumerate(front):
    code += [f'static {ret} (*real_{name})({params});',
             f'static {ret} prof_{name}({params}) {{',
             'shim_span shim_saved_span=shim_enter();',
             ('' if ret == 'void' else f'{ret} result=') + f'real_{name}({args(params)});',
             f'shim_leave(shim_saved_span,{i},"{name}");',
             '' if ret == 'void' else 'return result;', '}']
code += ['void *__wrap_gl4es_GetProcAddress(const char *name) {',
         'void *p=__real_gl4es_GetProcAddress(name);', 'if(!p)return p;']
for name, _, _ in front:
    code += [f'if(!strcmp(name,"{name}")) {{ real_{name}=p; return (void*)prof_{name}; }}']
code += ['printf("[shim-unwrapped] %s\\n",name); return p;', '}']
(out/'profile_front.c').write_text('\n'.join(code)+'\n')

header = re.sub(r'/\*.*?\*/', '', (root/'riscv/softgles2/include/sgl_gl.h').read_text(), flags=re.S)
protos = {name: (ret.strip(), params.strip()) for ret, name, params in re.findall(
    r'^([\w\s*]+?)\b(gl\w+)\s*\(([^;{}]*)\)\s*;', header, re.M)}
names = re.findall(r'\{\s*"(gl\w+)"', (root/'riscv/gl4es-bridge/bridge.c').read_text())
code = ['#include "sgl_gl.h"', '#include "profile.h"']
for name in names:
    ret, params = protos[name]
    sym = 'sgl2_' + name
    code += [f'extern {ret} __real_{sym}({params});',
             f'{ret} __wrap_{sym}({params}) {{', 'uint64_t t=shim_driver_enter();',
             ('' if ret == 'void' else f'{ret} result=') + f'__real_{sym}({args(params)});',
             'shim_driver_leave(t);', '' if ret == 'void' else 'return result;', '}']
(out/'profile_driver.c').write_text('\n'.join(code)+'\n')
(out/'profile_link.cmake').write_text('set(SHIM_WRAP_FLAGS\n  -Wl,--wrap=gl4es_GetProcAddress\n' +
    ''.join(f'  -Wl,--wrap=sgl2_{n}\n' for n in names) + ')\n')
(out/'profile_alloc.syms').write_text(''.join(f'{n} shim_{n}\n' for n in ['malloc','calloc','realloc','free','strdup']))
print(f'Generated {len(front)} GL and {len(names)} GLES wrappers')
