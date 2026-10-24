# Matched raster workloads across VLEN — 8 September 2026

## Scope

The new setup payload uses identical geometry and draw counts at VLEN 128, 256,
512 and 1024. It exercises 4/8/12/32-float varying layouts, tiny triangles and long
spans, and both depth/stencil-only and shaded draws. The shaded control uses a
small interpolated-color shader; it is not a replacement for Doom's interaction
shader. Separate interaction-kernel and resolve timings retain their own units;
these different workloads must not be summed into a game-time Pareto.

The optimization skips per-triangle perspective-varying preparation only when
all color channels are masked off and the shader cannot discard. Such draws
already exit before interpolation/shading. Discard shaders still prepare their
inputs, since their result decides whether depth is written. No shader math,
vector packing threshold or derivative arithmetic changes.

`sgl_depth_only_setup_prune=0` restores the original preparation for A/B. It
defaults to one. Normal production profiling remains disabled.

The validation compares all color, depth and stencil bytes and shader-call
counts for both windings, small/large geometry, 8/12/32 varying layouts,
stencil on/off, discard/non-discard and color masked/enabled: initially 96 cases per VLEN, expanded to 128 when the 4-float layout was added.
Performance runs alternate order for four rounds, with warm-up. The analyzer
uses the median paired ratio from rounds 1–3 and reports time per draw. The
same draw counts and geometry make raster rows comparable across widths.

## Reproduction

```
python riscv/scripts/rvv_doom_matrix.py --jobs 8 --timeout 300
python riscv/scripts/vlen_workload_analyze.py --output riscv/build/visual-audit/vlen-workloads.json
```

Measurements use QEMU 11.1.1 with one hart per payload, serial execution and no
concurrent game or compilation. Times are guest wall time using QEMU virt's
10 MHz timebase, not physical RVV throughput. The full game remains configured
for VLEN 1024; payload validation covers all four matching-VLEN builds.

## Controlled follow-up and retained result

The initial full workload matrix passed **28/28 payloads**, but QEMU timings
showed drift even in unchanged shaded controls. A repeated matrix pinned each
single-hart QEMU process to **logical CPU 2** (Windows process affinity), with
no concurrent game or compilation. These 12 additional payload runs passed.
The full matched matrix is in `vlen-workloads.json`, with 48 raster rows and
separate shader/resolve units. Some short A/B groups remained sensitive to order.

The decisive follow-up used the 32-varying tiny-triangle depth/stencil workload,
200,000 draws per arm and eight alternating rounds. Dropping the first two
rounds leaves six balanced rounds: three baseline-first and three optimized-first.
All four follow-up payloads passed their 96 correctness cases again.

| VLEN | Baseline µs/draw | Optimized µs/draw | Median paired speedup |
|---:|---:|---:|---:|
| 128 | 5.820 | 5.254 | 1.110× |
| 256 | 5.754 | 5.328 | 1.080× |
| 512 | 5.656 | 5.325 | 1.063× |
| 1024 | 5.630 | 5.331 | 1.054× |

The long balanced run supports retaining the same pruning at all four widths;
there is no justification here for a VLEN-specific cutoff. The benefit is about
5–11% faster in this targeted workload and diminishes with wider vectors.
Long spans gain much less, since setup is amortized over more raster work.
Shaded/discard paths retain their input preparation. No whole-game FPS gain or
physical-hardware throughput claim follows from these microbenchmarks.

To reproduce the longer focused benchmark from the checked-in payload:

```
python riscv/scripts/rvv_doom_matrix.py --setup-focused --tests setup --cpu 2 --jobs 8 --timeout 300
```

For a full pinned workload matrix, omit `--setup-focused` and use `--cpu 2`.
Use a logical CPU allowed on the test machine. The runner fails explicitly if
it cannot apply the requested affinity. CPU pinning affects only the benchmark
QEMU process; it changes no system-wide power or scheduling settings.

Local evidence: `vlen-setup-matrix.log`, `vlen-pinned.log`,
`vlen-pinned-results.json`, `vlen-focused.log`, `vlen-focused-summary.json`, and
`riscv/build/vlen-focused/<VLEN>/setup-1.log`. Focused logs were produced from an
equivalent focused copy of the payload; `--setup-focused` now exposes that same
loop configuration directly. Normal production renderer/shim profiling stays off.

## Actual Doom depth/stencil layouts and final check

The live layout diagnostic sampled two complete frozen-hangar frames. Each
contained 143 non-discard, color-masked draws with **4 varying floats** and 103
with **8 varying floats**. Another 18 eight-varying color-masked draws could
discard and correctly retained their setup. These are draw counts, not CPU-time
shares. The 32-varying benchmark above must not be presented as the gain for
these observed Doom passes.

The payload was extended to include 4-float varyings, bringing validation to
**128 cases per VLEN**. Additional long, pinned runs for the actual 4/8 layouts
passed at all four widths. Using the same six balanced rounds, their median
baseline/optimized ratios were:

| VLEN | 4-varying speedup | 8-varying speedup |
|---:|---:|---:|
| 128 | 1.014× | 1.010× |
| 256 | 1.016× | 1.066× |
| 512 | 1.017× | 1.019× |
| 1024 | 1.021× | 1.031× |

The individual 4/8-varying rounds include slower arms and outliers. These small
positive medians do **not** establish a reliable percentage improvement for Doom.
The change removes unused work and preserves results, but no whole-game FPS gain
is claimed. There is insufficient evidence for different VLEN-specific thresholds.
The clearest isolated result remains the 32-varying case, not the observed hangar
layouts. Per-VLEN shader/register-pressure and batch setup work remain open.

Reproduce the actual-layout focused checks with `--setup-focused 4` and
`--setup-focused 8` (each with `--tests setup --cpu 2 --jobs 8 --timeout 300`).
Their logs are under `riscv/build/vlen-focused-4/` and `vlen-focused-8/`; summaries
are `vlen-doom-layout-summary.json` and `vlen-draw-layouts.json` in the evidence
folder. The initial full workload JSON contains 48 raster rows; rebuilding the
expanded default payload produces 64 rows, including the new 4-varying layout.

The final production VLEN-1024 reception and hangar images have **zero differing
RGBA bytes** with setup pruning on/off. Fog-off changes **305,261 RGB pixels**;
restoring fog reproduces the original image exactly. Both optimized views were
visually inspected. Captures and results are `vlen-setup-{reception,hangar}-{1,0}`,
`vlen-setup-fog-{off,restored}` and `vlen-setup-visual.json` under the evidence
folder. These fixed-view checks do not certify every scene or physical scanout.

The game ELF is saved as `dhewm3-vlen-setup-final.elf`. The layout diagnostic and
all profilers were disabled before QEMU exited. Normal builds have no per-batch
profiling; the new layout line is inside the existing opt-in draw diagnostic.
