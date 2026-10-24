# Doom 3 to softgles2 Pareto — 8 September 2026

## Whole-renderer boundary check

Two complete frozen-hangar frames in the final production build (the partial
frame immediately after enabling profiling is excluded) attribute **97.4%** of
render-profile wall time to parallel raster/shading, **2.6%** to vertex work,
and **0.04%** to presentation. This is a renderer total, not total engine frame
latency. The normal build starts with profiling disabled; it was enabled only
for these diagnostic frames, then disabled before QEMU exited.

The whole-worker profiler's final frame attributes 27% of summed worker busy
time to shader calls and 73% to their surrounding work. The sampled-batch table
below has a narrower denominator and a different diagnostic build: it excludes
triangle/row setup and scheduling. It must not be substituted for the complete
worker breakdown. Further attribution of that excluded setup work remains a
priority alongside shader execution.

## Measured ranking

Frozen Mars City reception and hangar, QEMU 11.1.1, VLEN 1024, eight harts,
1280×960 internal rendering with 2× resolve to 640×480. Normal renderer/shim
profilers were disabled; the separate sampled raster probe was enabled.
Reception uses completed frames 25–34; hangar uses 38–45.

| RVV batch stage | Reception | Hangar |
|---|---:|---:|
| Fragment shader call | 71.43% | 49.21% |
| Coverage, depth and stencil | 12.91% | 35.30% |
| Scatter, discard, blend, quantization and stores | 8.00% | 8.71% |
| Varying interpolation and lane packing | 6.97% | 6.10% |
| Derivatives and fragment-coordinate setup | 0.68% | 0.68% |

These two leading categories account for about 84% of sampled batch time in
both views. More specifically, full-color shaders plus color-masked-off
coverage/depth/stencil account for 80.27% in reception and 79.47% in the hangar.
That identifies shader execution and depth/shadow-volume batch handling as the
first targets. The earlier gl4es boundary audit measured only about 0.4–0.6%
exclusive shim time; that is historical wall-time context, not another row in
this table. Historical fragment counts identify `interaction_gl4es` as the
largest shader workload; fragment counts are not shader CPU-time attribution.

### Interpretation limits

The probe samples approximately one batch in 64, using independent worker LCG
streams. Counters belong to each worker and are combined only after the join.
Intervals use the guest timebase and are worker wall time, not CPU cycles or
additive frame latency. Table denominators include only sampled batch intervals:
triangle setup, row-span setup, scheduling, vertex work, engine work and probe
selection are excluded. Diagnostic instrumentation can change register
allocation. These shares do **not** supersede the older whole-worker
shader/wrapper split, whose wrapper also includes work outside these intervals.
QEMU host scheduling further limits absolute timing and full-frame FPS claims.

## Retained change

Stencil ALWAYS bypasses the stored-stencil comparison, failed-stencil handling
and redundant coverage reduction. Depth-dependent stencil operations remain.
`sgl_stencil_always_fast=0` restores the baseline for live A/B; the default is one.
The earlier alpha-only output optimization remains enabled.

## Rejected shader candidate

A cubemap shortcut returned immediately when one face supplied all vector lanes.
It passed all synthetic tests and reception images, but the hangar differed in
4,636 RGB pixels (16,470 RGBA bytes; maximum channel difference 29). Disabling
only the cubemap shortcut reproduced the baseline exactly with the stencil
optimization still enabled. The candidate was **removed**, including its switch
and test registration. Its rendering discrepancy remains unresolved.

Consequently, shader execution is still the largest remaining measured batch
cost; this pass improves the second-ranked coverage/depth/stencil category.
The rejected candidate ELF, sampler header, payload source and benchmark logs
remain under the ignored evidence directory for investigation.

## Reproduction

Configure the engine with `-DBAREMETAL_RASTER_PARETO=ON` for diagnostic compilation,
then set `sgl_pareto_enabled=1` through `visual_probe.py`. Normal builds default to
OFF and compile the per-batch probe completely out. Independent base renderer
profiling is not required.

```
python riscv/scripts/pareto_analyze.py riscv/build/visual-audit/pareto-window.log --first 25 --last 34 --output riscv/build/visual-audit/pareto-window.json
python riscv/scripts/pareto_analyze.py riscv/build/visual-audit/pareto-window.log --first 38 --last 45 --output riscv/build/visual-audit/pareto-hangar.json
python riscv/scripts/rvv_doom_matrix.py --jobs 8
```

Raw sampled timings and JSON are under ignored `riscv/build/visual-audit/`.

## Validation and isolated timing

The rejected-candidate matrix passed 28/28 payloads: interaction shader, bindings,
resolve, stencil, texture copy, alpha-only stores and cubemap sampling at VLEN
128/256/512/1024. Stencil checks cover 1,096 cases per VLEN. Cubemap checks cover
432/864/1,728/3,456 cases respectively, comparing against the original merge path
at every vector-tail length, all six faces, mixed faces, zero directions, major
axis ties, missing faces and nearest/bilinear filtering.

The stencil ALWAYS benchmark uses a color-masked-off pass with wrapping stencil
increments, 4,000 draws per arm, alternating order over four rounds. Median paired
speedups after the first warm-up round are:

| VLEN | Stencil ALWAYS raster speedup |
|---:|---:|
| 128 | 2.278× |
| 256 | 1.215× |
| 512 | 1.182× |
| 1024 | 1.139× |

These are final retained-source isolated QEMU workloads, not overall game speedups. The larger 128-bit
result is specific to that compiled test and must not be extrapolated to other
VLENs or hardware.

Cubemap timings were extended from 4,000 to 40,000 calls per arm, with warm-up,
alternating order, and separate first-face, last-face and mixed-face workloads.
This exposed substantial host-time drift and some slower arms. The rejected shortcut was
workload-dependent; no universal cubemap or full-game speedup was established.
The raw per-arm evidence is retained as `cube-1.log` at each VLEN, alongside the
initial matrix logs. The rejected implementation skipped operations only for full-face batches; mixed
batches retained the partition/merge algorithm plus a shortcut check.

Extended cubemap median baseline/optimized ratios (rounds 1–3; above 1 is faster):

| VLEN | First face nearest / bilinear | Last face nearest / bilinear | Mixed nearest / bilinear |
|---:|---:|---:|---:|
| 128 | 1.144 / 1.415 | 1.055 / 0.874 | 0.920 / 0.966 |
| 256 | 1.112 / 1.130 | 0.999 / 1.003 | 0.995 / 1.082 |
| 512 | 1.318 / 1.106 | 1.012 / 1.029 | 1.124 / 0.916 |
| 1024 | 1.194 / 1.224 | 0.977 / 1.005 | 1.002 / 0.993 |

First-face batches consistently benefit in this run. The other columns include
regressions and drift, so they do not establish a broad win. Actual RISC-V
hardware measurements and longer controlled game runs remain necessary before
claiming a general shader-throughput gain.

## In-game isolation and fog evidence

The candidate build's four reception captures were byte-identical with both
switches on/off in ABBA order. Its short three-frame block medians were
2,664 / 2,752 / 2,845 / 2,793 ms (on/off/off/on). The hangar candidate medians were
7,071 ms on and 7,218 ms off, but its images differed: these timing improvements
**cannot be credited to retained code**, because that candidate included the
rejected cubemap shortcut.

With the cubemap candidate disabled, stencil-only hangar output matched the
baseline byte for byte. Fog-off changed 305,440 RGB pixels; restoring fog exactly
reproduced the stencil-only capture. These are fixed-view scanout-buffer checks;
they do not certify every map or physical-display scanout timing.

The final build has `BAREMETAL_RASTER_PARETO=OFF`, `BAREMETAL_RENDER_PROF=0` and
`BAREMETAL_SHIM_PROFILE=OFF`. The per-batch timing probe is compiled out.

The retained-source matrix was rebuilt after removing the cubemap candidate and
passed **24/24** payloads (six payloads at each VLEN). Final logs are
`pareto-retained-matrix-build.log`, `pareto-retained-matrix.log`, and the ordinary
per-VLEN `stencil-0.log` files. The stencil table above uses this final run.

## Final production visual check

A fresh boot of `dhewm3-pareto-final.elf` passed stencil on/off comparisons with
**zero differing RGBA bytes** in both reception and hangar. Disabling fog changed
**305,261 RGB pixels** in this fresh scene; restoring fog gave zero differences.
The frozen ship pose differs from the earlier boot, hence the different fog-pixel
count. Final captures are `pareto-final-reception-{1,0}.png`,
`pareto-final-hangar-{1,0}.png` and `pareto-final-fog-{off,restored}.png`; results
are recorded in `pareto-final-visual.json`. Both optimized views were visually
inspected. QEMU was shut down after verification.

No retained whole-game FPS improvement is claimed. The confirmed performance
result is the isolated stencil ALWAYS workload; shader execution and setup
around batches remain major opportunities.
