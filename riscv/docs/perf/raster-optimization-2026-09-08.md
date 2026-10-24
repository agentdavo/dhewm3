# Downstream raster optimization — 8 September 2026

## Retained change

The alpha-only store path removes RGB unpacking, blending, dithering and RGBA8
conversion when the color mask writes only alpha. It retains the existing blend
dispatch and alpha arithmetic, including the specialized blend cases. Depth and
discard handling run first; masked stores preserve destination RGB. Detailed
diagnostics use the original path to retain their RGB diagnostic counters.
`sgl_alpha_only_store=0` selects the original four-channel path for A/B.

The Windows build script now defaults `RENDER_PROF` to zero, consistent with
CMake. Per-stage counters and shader dumps are opt-in with
`RENDER_PROF=1 riscv/build-win.sh`. The production image is configured with
`BAREMETAL_RENDER_PROF=0` and `BAREMETAL_SHIM_PROFILE=OFF`.

## Correctness and isolated performance

The final matrix passes **24/24 payloads**: interaction shader, bindings, resolve,
stencil, texture copy and alpha-only store at VLEN **128, 256, 512 and 1024**.
Each alpha payload compares 2,700 cases against the unchanged four-channel path:
blend factors/equations, NaN/infinite alpha, preserved destination RGB, mixed
early-Z coverage, discard and vector tails. The stencil test includes 1,096 cases
covering both faces and partial/zero write masks.

The isolated alpha-store benchmark alternates baseline/optimized order for four
rounds, 1,000 draws per arm. Median paired speedups from the final retained matrix,
excluding the first warm-up round, were:

| VLEN | Alpha-only raster speedup |
|---:|---:|
| 128 | 1.635× |
| 256 | 1.709× |
| 512 | 1.731× |
| 1024 | 1.801× |

That is roughly 39–44% less time in this test workload. These are QEMU 11.1.1
wall-clock measurements, **not whole Doom frames or physical RISC-V latency**.
Full-game timings drifted substantially even after restoring the optimized
settings, so no reliable overall FPS improvement is claimed.

## Visual checks and rejected candidate

The initial reception A/B produced four byte-identical RGBA captures. In the
hangar, alpha-only optimization with the derivative cache disabled matched the
conservative baseline exactly across all 1,228,800 bytes.

The final retained production build was then booted afresh with both profilers
off. Its window and hangar on/off captures are independently byte-identical.
Disabling hangar fog changes 305,373 RGB pixels; restoring fog reproduces the
optimized frame byte for byte. These checks cover the tested Mars City views,
not every map or physical-display scanout behavior. Final captures are named
`raster-production-window-{on,off}`, `raster-production-hangar-{on,off}` and
`raster-production-fog-{off,restored}` under the evidence directory.

A per-triangle derivative-gradient cache was tried and **removed**. A scaled
gradient first changed rounding because GCC fused arithmetic across the proposed
cache boundary. A revised unscaled cache passed 20,051 arithmetic/raster tests
at each VLEN, but the actual hangar still differed in 6,992 RGB pixels. Isolation
showed the alpha-only path matched and the derivative cache did not. The remaining
cause was not resolved; the existing derivative calculations are retained.
This is why synthetic passes alone were not accepted as visual validation.

Stencil KEEP/zero-write-mask pruning was evaluated separately. The reception
images matched, but ten-frame medians were 2,810.5 ms enabled and 2,770.5 ms
disabled. No stencil-only game speedup is established by that noisy sample.

## Reproduction

```powershell
python riscv/scripts/rvv_doom_matrix.py --jobs 8
```

For in-game A/B, freeze game time/camera and disable renderer profiling, then use:

```powershell
python riscv/scripts/raster_ab.py riscv/build/visual-audit/raster-production.log --prefix riscv/build/visual-audit/raster-window --symbol sgl_alpha_only_store
```

The harness excludes capture pauses and two subsequent frames from each sample
block, saves raw RGBA captures and per-frame JSON, and restores the optimized
setting on successful completion. It leaves shader/fog settings unchanged.

Local evidence is under ignored `riscv/build/visual-audit/`: the initial
`raster-alpha-matrix.log`, `raster-alpha-game.log`, `raster-combined-*` captures
and JSON, `raster-hangar-alpha-only.png`, and the isolated candidate ELF
`dhewm3-raster-candidate.elf`. Rejected derivative sources are saved there under
`rejected-derivative-*`. Final verification uses `raster-retained-*` logs and
`raster-production.log`; individual matrix logs are in
`riscv/build/doom-matrix/<VLEN>/`.
