# gl4es ↔ softgles2 bridge

Bridges the engine's desktop OpenGL 1.x / ARB-program calls down to our software
OpenGL ES 2.0 (`softgles2`) via [gl4es](https://github.com/ptitSeb/gl4es).

```
  dhewm3 (qgl)  ──▶  gl4es  (GL 1.4/ARB → GLES2)  ──set_getprocaddress──▶  softgles2 (sgl2_*)
                     libGL.a (desktop gl*)            bridge.c shim          RVV SW rasteriser
```

## How it works

gl4es exposes the **desktop** GL API (`glBegin`, `glDrawArrays`,
`glProgramStringARB`, …) and reaches the GLES2 driver **only** through the
function pointers returned by its `set_getprocaddress()` callback — never by
direct symbol reference. softgles2 implements those same GLES2 names, so:

- **Symbol namespacing** (`sgl2_names.h`): the softgles2 backend is compiled
  with `-include sgl2_names.h`, renaming its exported GLES2 entry points to
  `sgl2_*` so they don't collide with gl4es's desktop `gl*` when statically
  linked.
- **Proc-address shim** (`bridge.c`): `sgl2_getprocaddress(name)` maps each
  GLES2 name string to the matching `sgl2_*` function; unimplemented GLES2
  functions resolve to a no-op so gl4es never jumps through NULL.
- **Bring-up** (`sgl_bridge_init(fb)`): binds a softgles2 framebuffer as the
  surface, then `set_getprocaddress` + `set_getmainfbsize` + `initialize_gl4es`.

## Build flags (from design doc 14)

```
-DSTATICLIB -DNO_LOADER -DNOEGL -DNO_INIT_CONSTRUCTOR -DNOX11 -DNO_GBM
```
No dlopen, no EGL/X11/GBM; manual init. `./build-gl4es.sh host` produces
`../gl4es/lib/libGL.a` (3.3 MB).

## Status

| | State |
|---|---|
| gl4es static lib builds with freestanding flags | ✅ |
| Symbol namespacing (`sgl2_*`) + proc-address shim | ✅ |
| gl4es initialises against softgles2; **maxed caps reported** (8 attribs, 8 varyings, 4/8 tex units, 4096 tex) | ✅ host |
| `glClear` renders through the full chain | ✅ (`make host-smoke`) |
| gl4es-generated **GLSL ES shader compiles + links** in our interpreter | ✅ (`SGL_TRACE=1` shows `link -> GLSL`) |
| `glBindAttribLocation` + app-bound attribute locations | ✅ |
| gl4es reaches softgles2 `glDrawArrays` | ✅ (trace `[drawarr] count=3`) |
| Immediate-mode **geometry renders** | ✅ full chain: `glBegin/glVertex/glColor/glEnd` → gl4es → softgles2 draws the triangle. Fix: implement program introspection (`glGetProgramiv(GL_ACTIVE_ATTRIBUTES)` + `glGetActiveAttrib`/`glGetActiveUniform`) so gl4es fills `va_size[]` and wires `glVertexAttribPointer` (`fpe.c:1435`) |
| ARB vertex/fragment program path (`draw_arb2.cpp`) | ⏳ (gl4es `arbconverter` → GLSL ES → our interpreter) |
| Bare-metal RISC-V gl4es build | ✅ `build-gl4es.sh riscv` → `libGL.a` (riscv:rv64, rva23s64) |
| **Bare-metal bridge payload (no OS)** | ✅ `make rv-run`: boots on QEMU (OpenSBI, `-cpu rva23s64`), gl4es+softgles2+newlib link, renders the triangle over serial. Needed a **TLS fix** (set `tp`/`.tdata`/`.tbss` — gl4es's proc loader uses `THREAD_LOCAL` guards) + newlib syscalls (`newlib_syscalls.c`) |
| `qgl*` → gl4es mapping in dhewm3 | ⏳ Phase 5 |

## Debugging

`SGL_TRACE=1 ./build/bridge_smoke` dumps softgles2-side calls (link result +
shader source on fallback, `glDrawArrays`, attribute/buffer setup, transformed
v0/v1/v2). `BRIDGE_TRACE=1` dumps which GLES2 procs gl4es resolves (`impl` vs
`STUB`). Both are env-gated and compiled out of bare-metal builds.

## Next steps (geometry)

1. Trace gl4es's renderlist → GLES2 realize path (`render.c` / `listdraw.c` /
   `fpe.c` `fpe_glDrawArrays`) to find why the `gles_glVertexAttribPointer`
   array-bind branch is skipped for an immediate-mode triangle (candidate:
   `glstate->vao->vertex` / client-array vs VBO decision; possibly needs
   `glVertexAttrib4fv` / `glVertexAttribIPointer` implemented, currently STUB).
2. Implement the remaining STUBbed GLES2 entry points gl4es calls
   (`glFinish`, `glDeleteShader`, `glGetProgramInfoLog`, `glGetShaderInfoLog`,
   `glGetActiveAttrib/Uniform`, `glGetShaderPrecisionFormat`, `glVertexAttrib4fv`,
   `glVertexAttribIPointer`).
3. Re-run `make host-smoke`; expect the RGB triangle, then move to a
   vertex-array path and the ARB-program path.

## Bare-metal stubs (link the engine image with these)

newlib supplies most libc; on top, provide: `getenv` (hardcode `LIBGL_NOPSA=1`,
`LIBGL_NOBANNER=1`, `LIBGL_ES=2`), `getcwd`→NULL, `fopen/fread/fwrite/fclose`
→no-op (shader cache disabled), `gettimeofday`→0 (or SBI rdtime). Link
`/opt/riscv` `libm`; build gl4es with `-D_FORTIFY_SOURCE=0`.

## Files
```
build-gl4es.sh   clone + build gl4es static (host; riscv WIP)
sgl2_names.h     GLES2 → sgl2_* rename (namespacing)
bridge.c/.h      proc-address shim + sgl_bridge_init
Makefile         host-smoke target
test/bridge_smoke.c   desktop-GL → gl4es → softgles2 smoke test
```
