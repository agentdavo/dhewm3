# DoomOS / softgles2 performance plan

Target: maximise **wall-clock FPS** of dhewm3 → gl4es → softgles2 on **QEMU 11
TCG** (`rva23s64`), while keeping a real-hardware profile (VLEN=1024, 768×432)
as the shipping default.

## Order of work

| Phase | Item | Status |
|-------|------|--------|
| 0 | Measure + VLEN A/B tooling (`tcg_vlen_bench.sh`, `TCG_PROFILE=1`) | done |
| 1a | RVV `textureCube` (`sgl_rvv_sample_cube_m2`) | done |
| 1b | Scalar-exponent gamma pow (`sgl_rvv_pow_sf_m2` for 0/1/2) | done |
| 1c | Resolution ladder (`BAREMETAL_FB_WIDTH/HEIGHT`, TCG 512×288) | done |
| 2a | MVP VS + colour FS kernels (depth/solid) | done |
| 2b | Additive ONE/ONE blend simplify + ONE/ZERO alpha | done |
| 2c | pow(0/1/2/16) successive-square path (kernel+VM) | done |
| 2d | envcombine binds overlay VS; live_lanes==0 early-out | done |
| 2e | HiZ 32x32 + deferred dirty coalesce; skip EQUAL refresh | done |
| 2f | modulate2d FS kernel (Color * texture2D) | done |
| 2g | Present zero-copy | already done when scanout-bound |
| 3a | Demorun harvest (lit vs load) | done → demorun-lit-vs-load.md |
| 3b | Sparse pack 75%; packed scatter 4-row | done |
| 3c | modulate_cube + dual_proj_mul kernels | done |
| 3d | gl_FragCoord SIMT (IR_LOADFC) + heathaze kernel | done |
| 3e | scenic A/B helper script | done → scripts/scenic_ab.sh |
| 3f | heathaze_mask + simt_ok if fs_kernel | done |
| 3g | interaction gamma skip when gw==1 | done |
| 4 | Full DoomOS platform review | tasks / REVIEW-TASKS.md |

## How to measure (QEMU 11 local)

```bash
# Microbench: interaction kernel across VLENs
riscv/scripts/tcg_vlen_bench.sh

# Full engine, TCG iteration profile
TCG_PROFILE=1 TIMEOUT=600 PIN_HARTS=1 riscv/demorun.sh

# Shipping-like profile
RVV_VLEN=1024 FB_WIDTH=768 FB_HEIGHT=432 RENDER_PROF=1 \
  TIMEOUT=900 PIN_HARTS=1 riscv/demorun.sh
```

QEMU binary: `qemu-11.0.1/build/qemu-system-riscv64` (auto-selected by `run.sh`).

## Review checklist (full DoomOS suitability)

See scheduled tasks + writeups:

1. **Boot / SMP / heap** — multi-hart tile workers + AsyncThread, atomics, malloc
2. **softgles2** — kernel coverage, TCG-expensive ops, HiZ/MT
3. **Engine + gl4es** — frame loop, uploads, SIMD, present
4. **QEMU 11 TCG** — VLEN default, SMP, pin, tb-size

Weekly regression: softgles2 `iakern` at VLEN 256 and 1024.
