# Demorun harvest: load UI vs lit Mars City

Source: `demorun-tcg-profile.log`  
Config: `TCG_PROFILE=1` · `RENDER_PROF=1` · **VLEN=256** · **512×288** · SMP=6 · PIN_HARTS=1 · shadows off

## Two regimes

| Regime | Signature | Avg total | Avg frags | Dominant cost |
|--------|-----------|-----------|-----------|---------------|
| **Load / soft_particle UI** | draws ≤ 10 | ~**228 ms** | ~113k | dispatch/mainbusy; shade ~40–47%, wrapper ~53–60% |
| **Lit scenic** | draws > 50 | ~**13 s** | ~1.6M | **shade ~90–100%** of hart busy; raster multi-second |

Engine `frame:` bk on later frames: roughly **5–19 s** per frame at this resolution under TCG.

## Shader workload (cumulative, late `[fswork]`)

| Path | % frags | Notes |
|------|---------|--------|
| **interaction_gl4es** | **51%** | kernel 100% hit |
| **overlay** | **43%** | kernel 100% hit |
| envcombine | ~1% | kernel |
| overlay_atest | &lt;1% | kernel |
| **VM id=12** | ~0.5% | ninst=73 nvaryf=8 tex2d=2 pow=3 (heatHaze-like + gamma) |
| **VM id=10** | ~0.5% | ninst=81 nvaryf=12 cube=1 pow=3 |

**Kernel coverage ≈ 99% of frags.** Remaining VM is heatHaze / exotic paths.

## Vertex path (lit frames)

- VS **kernel batches** fire heavily (thousands/frame)
- Scalar verts near-zero — MVP/overlay/interaction VS specialisation is working

## MT / occupancy (lit)

- 6 harts roughly balanced
- occ **~55–82%** → sparse-lane pack (threshold was 50%) under-used
- present **0 ms** (headless / zero-copy) — not a bottleneck

## Implications

1. **Do not chase more tiny shaders first** — interaction+overlay already own the work.
2. **Lit bottleneck = fragment shade + fill** of interaction/overlay batches.
3. **Wrapper micro-opts** still help load UI and any single-hart path; on lit MT path shade ≫ wrapper.
4. **Raise sparse pack threshold** (e.g. 75%) for lit occ.
5. Optional: heatHaze kernel for the last VM crumbs + correctness.

## Code this session

- Sparse pack threshold **50% → 75%** (lit occ≈55–80%)
- Packed scatter: 4 rows when no discard (skip kill mask)
- Kernels: `modulate_cube`, `dual_proj_mul` (residual FPE VM crumbs)
- **heatHaze**: `IR_LOADFC` + raster lane map + `heathaze` kernel
- **heatHazeWithMask**: `heathaze_mask` (+ discard kill mask row 4)
- **`glsl_fs_simt_ok`**: true when any `fs_kernel` is bound (so mask path isn’t forced scalar)
- **interaction gamma**: skip `pow` when `env21.w == 1` (identity after clamp)

## Scenic A/B guidance

| Profile | Env | Use |
|---------|-----|-----|
| TCG iterate | `TCG_PROFILE=1` (256 / 512×288 / prof) | daily work |
| Shipping look | `RVV_VLEN=1024 FB_WIDTH=768 FB_HEIGHT=432` | visual QA |

Same scenic view + `bm_renderProf 1`; compare median `total=` and `frame: bk:`.
