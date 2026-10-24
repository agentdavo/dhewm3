# gl4es allocation and copy optimization — 8 September 2026

The retained pass eliminates steady-state gl4es allocation traffic in two frozen
Mars City views. It does not establish a general frame-rate improvement: window
shim time improved, while the hangar shim median was slightly higher.

## Retained changes

* Per-context index-conversion scratch storage replaces immediate-draw allocation
  and free. The existing index conversion and resulting index format are retained.
  Display lists/intercepted draws retain owned storage; nested draws and failed
  buffer growth fall back safely. Context destruction releases the scratch buffer.
* A per-cache most-recent shader-state entry avoids repeated hash lookups when
  the requested state matches. Cache isolation and disposal are unchanged.
* Eligible level-zero RGB/unsigned-byte framebuffer copies go directly to GLES.
  Resized, ratio-adjusted, shadow-copy and other conversion cases retain the old
  path. This removes the intermediate RGB and RGBA image allocations.
* softgles2 copies contiguous color rows using RVV, including tail lanes, and
  forces alpha to 255 for RGB textures. RGBA and depth semantics are preserved.
* Client-index draws explicitly clear a stale GLES element-buffer binding.

Vendor changes are recorded in
[patch 0006](../../gl4es-bridge/patches/0006-doom-draw-copy-fastpaths.patch).
The Windows build script now finds Git for Windows in MSYS login shells, verifies
all six patches, and uses a dedicated bare-metal CMake build directory.

A native 32-bit-index passthrough candidate was **withheld**. One build produced
a small glass/refraction image mismatch that could not be consistently reproduced
or explained. The final pass instead reuses storage for the existing conversion.

## Same-process measurements

QEMU 11.1.1, VLEN 1024, eight harts, 1280×960 internal rendering, 640×480 scanout.
Fog, shadows and heat-haze refraction enabled; game time and camera frozen.
Each row summarizes ten completed frames, excluding switch/capture transients.
These are emulated wall-clock measurements with boundary profiling enabled,
not hardware-cycle estimates. Nested GLES execution is excluded from shim time.

| Metric | Window: off → on | Hangar: off → on |
|---|---:|---:|
| gl4es exclusive median | 21.679 → 17.602 ms | 41.487 → 44.250 ms |
| gl4es exclusive range | 20.483–28.332 → 16.056–20.628 ms | 39.940–56.540 → 33.853–46.900 ms |
| Allocations and frees per frame | 566 → 0 each | 992 → 0 each |
| Allocated bytes per frame | 10,039,150 → 0 | 10,510,552 → 0 |
| GL calls per frame | 11,467 → 11,467 | 21,547 → 21,547 |
| Whole-engine median | 4,246 → 4,051.5 ms | 9,765 → 6,847 ms |

The window shim median decreased 18.8%; the hangar shim median increased 6.7%.
Substantial host/QEMU scheduling variation, especially in whole-engine times,
precludes claiming a reliable general CPU or frame-rate gain from these samples.
The zero-allocation result is exact in every measured optimized frame. Shader
creation, resource uploads, buffer growth and other views can still allocate.

Retained gl4es allocations are 11,620,573 bytes in the window and 12,179,125 bytes
in the optimized hangar. The scratch buffer remains allocated when its diagnostic
switch is disabled, so this A/B is not a fresh-process memory-footprint comparison.
Scratch capacity follows the largest converted immediate draw in each context.
It trades retained memory for allocation reuse. Allocation traffic is not resident
memory: eliminating 10 MB/frame does not mean saving 10 MB of persistent memory.

All sampled frames report zero untracked frees, attribution overflows and open
desktop timing spans. The saved profiling ELF's static table report omits the
6,144-byte call-site histogram; source now includes that histogram in the total.
This diagnostic storage is absent from the production build.

## Visual and regression evidence

* Window and hangar optimized/conservative captures are byte-identical across
  all 1,228,800 RGBA bytes per image.
* Toggling hangar fog changes 304,803 RGB pixels. Restoring fog reproduces the
  optimized image byte for byte. These captures validate the tested scenes;
  they do not establish tear-free presentation on every map or physical display.
* QEMU matrix: **20/20 passes** — interaction, bindings, resolve, stencil and
  texture copy at VLEN **128, 256, 512 and 1024**. The copy payload checks 33
  RGB/RGBA/depth cases, offsets, guard pixels and vector tails.
* Host production-extraction checks pass shader-cache behavior, client/EBO
  binding, index-buffer ownership, nested draws and failed growth.
* Independent boundary-profiler and ARB-uniform checks pass.
* Production rebuilt with `BAREMETAL_SHIM_PROFILE=OFF`; symbol inspection finds
  no profiling entry wrapper, `shim_malloc` or `shim_profile_report`.
  A fresh boot reached gameplay; the window was visually inspected and two
  successive frozen production captures are byte-identical.
* All six vendor patches apply sequentially to a clean upstream archive and
  pass the installed-tree reverse checks.

Local evidence is under ignored `riscv/build/visual-audit/`: `opt-pool.log`,
`dhewm3-opt-pool-profile.elf`, `pool-{window,hangar}-{on,off}.png` and `.rgba`,
the four corresponding `*-results.json` files, `pool-hangar-fog-off.png`,
`pool-hangar-fog-restored.png`, `opt-matrix.log`, and production build/boot logs.
Sample ranges: window on 83–92, off 98–107; hangar off 123–132, on 144–153.

```powershell
python riscv/gl4es-bridge/test/run_draw_fastpaths_validate.py
python riscv/gl4es-bridge/test/run_profile_validate.py
python riscv/gl4es-bridge/test/run_arb_uniform_validate.py
python riscv/scripts/rvv_doom_matrix.py --jobs 8
```

The next performance target is downstream fragment shading and rasterization.
Follow-up: [alpha-only raster optimization and visual validation](raster-optimization-2026-09-08.md).
gl4es remains below one percent of total frame time in these samples; further
shim work should be justified by measured hot paths rather than assumed overhead.
