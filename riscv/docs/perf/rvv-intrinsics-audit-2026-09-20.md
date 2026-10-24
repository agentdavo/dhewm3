# softgles2 RVV audit — 20 September 2026

## Scope and approach

Inventoried all C/header units under `riscv/softgles2/src`, inspected the explicit
intrinsic-bearing units and their dispatch/layout contracts, and inspected the
current GCC 16.1 generated code for the principal shader, SIMT, raster, vertex
and texture units. The supplied RVV intrinsic document informed the checks:
actual VL versus requested AVL, LMUL/register pressure, tail/mask policy,
indexed versus contiguous memory access, and optional-extension assumptions.
This is an optimization audit, not a proof that every shader or VLEN is optimal.

## Findings by pipeline area

| Area | Current implementation / evidence | Decision |
|---|---|---|
| Texture upload and DXT (`sg_texture.c`) | Upload still converted matching RGBA byte by byte; DXT color decoder requested 16 e32m1 lanes once | Retained direct copies, aligned packed RVV opaque-alpha uploads, and a VL-safe DXT loop |
| Sampling/math (`sg_math.h`) | RVV nearest/bilinear/trilinear/aniso, coherent contiguous-load detection, per-channel bilinear unpack, reciprocal-square-root refinements already exist | Keep arithmetic and filtering; sampler register lifetime is a leading next target |
| Shader kernels (`sg_kernels.c`) | Specialized Doom/gl4es shaders use FMA and SoA; large inlined sampler expansion causes extensive stack traffic | Tested whole-sampler outlining; rejected on measured runtime |
| GLSL SIMT (`glsl_simt.c`) | Vector operations already cover the IR executor; large register-file and spill traffic remains | Prefer shader specialization and reduced live intermediates; avoid blanket replacement with approximate math |
| Raster and blend (`sg_raster.c`, `sg_blend.h`) | RVV coverage, early depth/stencil, lane packing, masked writes, alpha-only path | Already vectorized; next work should target setup/packing overhead and register lifetime, backed by frame samples |
| Vertex (`sg_vertex.c`, `sg_vs_range.c`) | Vector postprocessing/SoA transpose, cached/deduplicated transforms, parallel ranges | Indexed attribute gathering is a possible lower-priority target; compiler already vectorizes some scalar-looking loops |
| Framebuffer/resolve/state (`sg_fb.c`, `sg_framebin.c`, `sg_state.c`) | Wide stores and packed integer resolve already present | Preserve; larger LMUL is useful for simple memory loops but not automatically for complex shaders |
| API, parser, clipping, arena, scheduler, diagnostics | Predominantly state/branch/ownership work; small or irregular loops | No evidence that adding intrinsics broadly helps; retain scalar control paths |

The historical per-frame Pareto in `pipeline-pareto-2026-09-08.md` still motivates
shader/raster work, but its eight-hart/high-resolution percentages are not a
fresh profile of this four-hart 768x432 build. Startup measurements are separate:
normal-map construction and file read/inflate dominate that workload.

## Retained changes and correctness

1. RGBA input for RGBA storage now copies rows directly on little-endian targets.
2. Aligned RGBA input for RGB storage uses `vle32`, `vor 0xff000000`, `vse32`,
   strip-mined with e32m8. It combines copying and alpha correction in one pass.
   Unaligned input retains the byte-safe path; other formats retain conversion.
   Unpack padding, subrectangle offsets, destination stride and alpha semantics
   are preserved. `SGL_UPLOAD_RVV=0` builds the baseline for comparison.
3. DXT color decoding advances by returned VL until all 16 pixels are produced.
   The former code failed the new literal-palette oracle at pixel 4 on VLEN128;
   e32m1 also cannot fit 16 pixels at VLEN256. This affects DXT1/3/5 decoding,
   although the current full-quality game configuration does not use DDS.

Production API validation passes **213 cases at each VLEN 128, 256, 512, 1024**,
covering odd widths, tails, guards, source alignment, unpack alignments 1/2/4/8,
RGB forced alpha, RGBA preservation/redefinition, depth/color copies and DXT.
The DXT cases independently specify expected colors, including transparent DXT1.
The same cases pass with the new RVV upload disabled.

Run `python riscv/scripts/texture_upload_validate.py` and repeat with `--scalar`.
Artifacts stay in `build-rv/texture-upload-validation` and `texture-upload-scalar`.

## Isolated upload timings

5,263,360 opaque RGBA texels, stock QEMU, one hart, aligned input. Three runs per
variant/VLEN, alternating order, pinned to host logical CPU 2; table shows medians.
The separate four-hart game was paused during these measurements.

| VLEN | Existing conversion (ms) | Packed RVV (ms) | Ratio |
|---:|---:|---:|---:|
| 128 | 98.299 | 83.194 | 1.18x |
| 256 | 94.239 | 11.337 | 8.31x |
| 512 | 97.233 | 9.789 | 9.93x |
| 1024 | 107.618 | 8.146 | 13.21x |

These are microbenchmark ratios, not game or physical-hardware speedups. Host
variation was substantial: the earlier unpinned one-shot run measured approximately
1.15x / 2.59x / 3.24x / 4.14x respectively. Even the pinned repetitions contain
outliers, recorded in `paired-results.json`. Smaller VLEN128 gains are modest;
the large-width improvement is clear in these tests, but real-hardware latency
and end-to-end game impact need their own measurements.

## Rejected experiments

- Byte-wise opaque-alpha RVV load/mask/merge/store was correct but slower than
  the baseline at every VLEN in the initial QEMU comparison. It was removed.
- Outlining `sgl_rvv_sample2d_m2` in an isolated source copy reduced the VLEN1024
  interaction-test ELF from 373,728 to 132,104 bytes. All paired outputs remained
  bit-exact against the SIMT interpreter, but kernel ns/fragment were:

| Pair | Inline | Outlined |
|---:|---:|---:|
| 0 | 20,076 | 21,954 |
| 1 | 10,010 | 10,400 |
| 2 | 9,667 | 10,922 |

The absolute times drifted, but outlining lost in each pair. It is **not** in
production. Sources, build/run scripts and logs remain in `build-rv/sampler-outline*`.
The test exercises the interaction shader; it does not establish performance of
all mip/aniso configurations. No source-wide LMUL or mask-policy change was made.

## Generated-code evidence and next priorities

Static whole-register memory instruction counts in the original VLEN1024 object:
interaction_gl4es 1,046; modulate_cube 1,048; SIMT IR executor 914. Inspection
confirmed frame-relative spill addresses in the interaction shader. These are
static counts across all branches, **not stores executed per fragment**. Reports
are in `build-rv/*-rvv-audit.txt`.

The best remaining renderer experiments are shortening live vector ranges in
interaction/cubemap kernels, selective lower-LMUL kernels, and moving only cold
mip/aniso branches out of line while preserving the common path. Each needs
paired timing and image/numerical checks: the rejected full outlining experiment
shows code size alone is not a reliable performance predictor. Sampling locality
and raster setup remain important alongside arithmetic intrinsics.

For startup, the measured next bottleneck is TGA/PK4 read/inflate, not the GL shim.
A bounded preparation pipeline or lossless cooked cache should precede invasive
concurrent GL submission. Whole-image parallel loading still needs parser,
archive cursor, allocator and GL-state ownership changes.

Full-game normal-map parallel validation and screenshot evidence are documented
in `texture-startup-2026-09-20.md`. The shipping ELF remains VLEN1024-specific;
the four-VLEN results above are focused renderer payloads, not four complete
DoomOS boot validations.
