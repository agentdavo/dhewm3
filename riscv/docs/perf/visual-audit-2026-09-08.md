# Mars City visual and performance audit — 2026-09-08

## Correctness changes

* Fragment-coordinate and analytic-derivative arrays are thread-local. Previously
  tile workers could overwrite each other's screen-sampling coordinates and LOD
  inputs. Derivative arrays now hold all 32 varying components independently of
  VLEN; their former lane-sized allocation also overflowed at VLEN 128 and 256.
* Main and worker TLS are aligned to 64 bytes. Both linker scripts reserve the
  main thread's TLS zero-fill storage before ordinary BSS. Tests exposed an
  overlap in small payloads that an ordinary location-counter assignment did not
  prevent; BSS now has an explicit address after TBSS.
* Stencil comparisons use `reference FUNC stored`, with the read mask applied
  to both operands. Both scalar and RVV paths previously reversed ordered
  comparisons, including Doom's `GL_GEQUAL` lighting test. The old host test
  encoded the same mistaken expectation and is corrected. See the
  [Khronos stencil comparison reference](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glStencilFunc.xhtml).
* RVV `GL_INCR` uses saturating addition; adding in eight bits and then taking
  the minimum with 255 had already wrapped. Wrapping operations remain wrapping.
* Normal-map heat-haze refraction is enabled by default after visual validation.
  Renderer cvars synchronize between frames so refraction, mipmapping and
  dithering changes actually take effect during a run.

## Evidence

Raw captures and logs are under ignored `riscv/build/visual-audit/`:

| Capture | What was checked |
|---|---|
| `room240-no-post.png` | Before stencil correction: disabling post-processing still leaves jagged shadow triangles above the windows. |
| `room240-shadows-off.png` | Diagnostic shadow-off comparison removes those triangles. |
| `stencil-fixed-room240-final.png` | Corrected comparisons, with shadows and post-processing enabled: sawtooth artifacts gone. |
| `refraction-on-room240.png` | Full normal-map refraction, with coherent glass and no streaks. |
| `stable-0.png` through `stable-3.png` | Four consecutive completed frames, byte-identical RGBA captures at a frozen camera. |
| `prune-off.png` | Conservative shader setup is pixel-identical to the optimized frozen capture. |
| `host-room240.png` | QEMU's actual display capture also shows the corrected scene. |
| `final-room96.png` | Second reception camera angle, full shadows and refraction. |
| `final-hangar-fog-on.png`, `final-hangar-fog-off.png` | Same hangar camera with fog toggled; visible warm haze affects 305,425 RGB pixels. |

Screenshots validate these Mars City views; they are not exhaustive coverage of
every map or material. The initial all-black captures were taken before the map
intro fade completed. The intro can teleport the player after an early camera
command; wait until it has settled before establishing a benchmark camera.

## Performance

The dominant gl4es interaction kernel consumes analytic derivatives for six
varying components. Only those six are now calculated, instead of every live
varying. Shaders that do not consume analytic derivatives skip that setup, and
shaders without a `gl_FragCoord` read skip filling the screen-coordinate arrays.
Arithmetic for retained inputs is unchanged. Draw-state snapshots carry the
flags by value, with no increase in structure size.

`sgl_batch_inputs_prune=0` restores conservative setup for same-process A/B.
The frozen-frame comparison is pixel-identical. Five-sample median total times
were approximately 4066 ms conservative versus 4042 ms optimized; this small
difference is within the observed QEMU/host timing variation, so no reliable
whole-frame speedup is claimed. The reduced calculation count is structural.
The earlier RVV SSAA resolve improvement remains in place.

## Reproduction and validation

```powershell
python riscv/softgles2/test/run_frag_tls_validate.py
python riscv/scripts/rvv_doom_matrix.py --jobs 8
```

The host concurrency test checks four threads over 1,000 synchronized rounds
at 8, 16, 32 and 64 lanes. UCRT64 GCC 16.2 miscompiles TLS-array induction at
`-O2`; that host-only test disables IV optimization. The RISC-V production flags
are unchanged. The QEMU matrix passes all 16 payload runs: shader, binding,
resolve and stencil tests at VLEN 128, 256, 512 and 1024. Each stencil payload
checks 328 combinations through the fixed and programmable raster paths.

Launch with `GDB_PORT=4469 MONITOR_PORT=4468 VIDEO=gpu`. The debugger interface
is bound to localhost. The command mailbox executes between frames:

```powershell
python riscv/scripts/visual_probe.py --command "in_scriptedMove 0; bm_scenicWarp 0; setviewpos -700 -1380 176 240; g_stopTime 1; com_showFPS 0"
python riscv/scripts/visual_probe.py --capture riscv/build/visual-audit/window.png
python riscv/scripts/visual_probe.py --set-int sgl_batch_inputs_prune 0
```

Wait for `[debug-command] completed=...` and a subsequent completed frame before
capturing a change. Normal captures briefly stop the guest to read stable RAM,
then resume it; profiling samples during that pause should be excluded. Command
text must pass through Doom's command buffer so semicolon-separated commands
all execute; `CMD_EXEC_NOW` alone executes only the first command.

## Final fresh-boot smoke check

The rebuilt engine was booted again with refraction enabled by default. The
console confirmed `bm_heathazeDeform` current/default values of 1. The window
capture `shipping-window.png` is clean in the tested view. Changing the live
cvar to 0 changed 10,404 RGB pixels; restoring it to 1 reproduced the original
RGBA frame byte for byte (`shipping-refraction-restored.png`). This also checks
the new per-frame renderer-cvar synchronization. `shipping-hangar-fog.png`
records the final build with fog and shadows enabled. These files and the
`shipping.log` serial record are in `riscv/build/visual-audit/`.
