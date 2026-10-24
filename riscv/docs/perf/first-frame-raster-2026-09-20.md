# First-map rasterization follow-up

## Attribution

Diagnostic all-hart PC and return-address sampling between `map-ready` and
`first-map-present` found that this interval is dominated by softgles2 shading
and rasterization. The full-quality source run collected 333 samples per hart;
the native DDS run collected 133 per hart. Both used four harts and VLEN 1024.
Overlay shaders, bilinear sampling, varying interpolation and color stores
dominated. Worker wait occupied a minority of samples. This does not support
prioritizing gameplay or gl4es upload overhead for this interval.

QEMU stops concentrate at translated-block boundaries. Individual source-line
percentages are not instruction-cost measurements. Sampling interrupts the
guest, so these runs are diagnostic only and are excluded from timing results.

Artifacts: `build-rv/first-frame-profile/`, including the matching
`baseline.elf`, sample JSON and symbolized text. Reproduce with
`texture_boot_ab.py source native --sample-first-frame`; symbolize using
`first_frame_samples.py <samples.json> --elf <matching.elf>`.

## Changes

* Load exact Bayer thresholds from a repeated, contiguous float table instead
  of computing indices, gathering integers, converting and scaling every batch.
  All values are exactly representable binary fractions. The general path remains
  available for vectors wider than 64 lanes and for diagnostic comparison.
* Outline the generic fragment interpreter into a noinline helper. The shipping
  specialized-shader entry now reserves 80 bytes of stack, versus approximately
  100 KiB in the baseline. The large register file remains on the fallback path.

Projection, sampling, alpha, blending and fog calculations are unchanged.
Full-quality source textures remain the startup default.

An attempted overlay varying mask was removed. Although the kernel does not
read TexCoord.z and shader tests passed, toggling the mask in-game changed
21,297 RGBA bytes in the wall capture. A later unchanged wall repeat also differed,
so this does not establish that the mask caused a regression. It also does not
validate equivalent raster output; the renderer conservatively retains baseline
overlay interpolation behavior. A broader
all-kernel liveness toggle also changed the wall and is not an isolation of this
new optimization. Diagnostic script and captures remain under
`build-rv/first-frame-profile/overlay_mask_probe.py` and
`build-rv/first-frame-baseline-b/`.

## Validation

The game rebuilt successfully with the repository-local GCC 16.1 toolchain.
The matrix passed overlay/VM comparison, bindings, interaction shading, alpha,
stencil and dither at VLEN 128, 256, 512 and 1024: 24 passing payload runs.
The overlay harness requires `SGL_GATHER_RUNTIME_TOGGLE`; matrix builds now enable
that test-only switch. Production continues to fold the gather enable constant.

The new dither test compares the production old/new expressions byte for byte,
checks a scalar oracle and untouched tail lanes, and covers every VL from one
through VLMAX with dithering on/off and positive/negative coordinate phases.
Counts are 2,304 / 4,608 / 9,216 / 18,432: 34,560 total cases.
Logs and results: `build-rv/first-frame-tests/`.

## Boot measurement protocol

Fresh sequential full-quality source-texture boots, four harts, VLEN 1024,
768x432, QEMU 11.1.1 TCG, temporary disk snapshots under the repository. No
compilation, test guests or PC sampling overlap measured boots. Baseline and new
ELFs use the same asset disk and launch options. Timer epoch is C-runtime entry,
after firmware. First-map-present records a successful map presentation and
does not guarantee the cinematic has finished or player control is available.

The harness accepts `--elf` for a saved baseline and `--hold-seconds` for optional
visual review after the measured interval has ended.

## Exploratory timing (includes the subsequently removed overlay mask)

| Run | Map ready | First map present | Interval |
|---|---:|---:|---:|
| Baseline A | 63.873 s | 171.639 s | 107.766 s |
| Experimental A | 69.449 s | 159.411 s | 89.962 s |
| Experimental B | 56.240 s | 146.765 s | 90.525 s |
| Baseline B | 57.475 s | 166.067 s | 108.592 s |

These experimental gains must not be presented as the final retained-change
result. The mask was removed after visual validation. Experimental B did pass
fog on/off/restore: 330,185 changed RGB pixels when fog was disabled, zero RGBA
differences after restoration. The final build is measured and reviewed separately.

## Retained-build results

All three final runs used ELF SHA-256 starting `5be867b7f95c` and baseline overlay
interpolation. Runs are in `build-rv/first-frame-final-{a,b,c}/`.

| Run | Map ready | First map present | Interval |
|---|---:|---:|---:|
| Final A | 51.784 s | 159.867 s | 108.083 s |
| Final B | 57.505 s | 146.786 s | 89.281 s |
| Final C | 48.324 s | 130.237 s | 81.913 s |

The final first-present workload reports the same 138 draws / 12,584 triangles
as both baseline runs. Final timing varies substantially: one run matches the
107.766–108.592 s baseline interval; two are faster. This is promising, but is
not sufficient to assign a reliable percentage speedup to the retained changes.
Earlier PC samples and microbenchmarks must not substitute for this result.

Final visual validation used a stable hangar view. A repeat with no change,
switching to the original dither expression, and restoring the table each
produced **zero differing RGBA bytes**. Disabling fog changed 330,394 RGB pixels;
restoring fog produced zero differing RGBA bytes. See
`first-frame-final-c/dither-hangar-results.json`, `visual-results.json`,
`dither-hangar-{fast,repeat,general,restored}.png` and
`fog-{on,off,restored}.png`. Wall captures were inspected, but unchanged repeats
vary and are not a reliable byte-equivalence oracle. Still captures alone do not
establish absence of temporal tearing.

The capture helper now handles both file-local `g_scanout` symbols. One is null
without SSAA; relying on nm symbol order had selected the null instance after
relinking. The helper requires one distinct nonzero scanout pointer.

The next performance target remains interpolation/sampling/raster work. A stable
fixed-workload measurement and an equivalent interpolation path are needed before
retrying varying pruning. Gameplay, filtering quality, projection and fog were
not altered by this pass.
