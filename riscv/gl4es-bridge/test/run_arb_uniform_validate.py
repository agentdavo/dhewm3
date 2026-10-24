#!/usr/bin/env python3
"""Test production gl4es reflection without a GL context (CC defaults to gcc).

Extract the reflection section verbatim: PE linkers otherwise retain unrelated
exported driver entry points. Matrix-name recognition is outside this ARB test.
"""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[3]
vendor = root / "riscv/gl4es"
out = root / "riscv/build/doom-opt"
out.mkdir(parents=True, exist_ok=True)
source = (vendor / "src/gl/fpe.c").read_text()
section = source[source.index("void builtin_Init("):source.index("int builtin_CheckVertexAttrib(")]
unit = out / "fpe-metadata.c"
unit.write_text('#include <stdio.h>\n#include <string.h>\n#include "fpe.h"\n'
                '#include "../glx/hardext.h"\n'
                'int isBuiltinMatrix(const char *s) { (void)s; return -1; }\n' + section)
exe = out / ("arb_uniform_validate.exe" if os.name == "nt" else "arb_uniform_validate")
subprocess.run([os.environ.get("CC", "gcc"), "-O2", "-DNOX11", "-DNOEGL", "-DSTATICLIB",
                "-I" + str(vendor / "include"), "-I" + str(vendor / "src/gl"),
                str(Path(__file__).with_name("arb_uniform_validate.c")), str(unit),
                "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
