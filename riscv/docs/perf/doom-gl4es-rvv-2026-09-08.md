# Doom 3 gl4es / RVV optimization validation — 2026-09-08

Follow-up: the [visual audit](visual-audit-2026-09-08.md) adds thread-local
fragment state and stencil fixes, validates fog and full refraction, and
supersedes this initial pass's open visual gate and refraction-off default.

## Changes

* gl4es ARB parameter uploads stop at the highest linked slot and skip inactive
  locations. Reflection rebuilds these bounds on relink. Patch 0005 preserves the
  change outside the ignored vendor checkout. The sparse reflection regression
  exercises six active uniforms across 58 slots, instead of scanning 240 slots.
* softgles2 resolves kernel uniform, sampler, and attribute names once at link
  time. The immutable 172-byte binding table replaces a 1,412-byte lazy string
  cache. Sampler units and uniform values are still read live; relinking rebuilds
  attribute locations. This removes worker-side cache mutation and string scans.
* The 2× RGBA8 SSAA box resolve uses RVV segmented loads and packed integer
  channel sums, with LMUL 4. Results preserve the original integer rounding.
  Other scales and clamped edge blocks retain the scalar path.
* The four-width check exposed a pre-existing VLEN 128 SIMT matrix bug: uniform
  loading stopped at eight lane entries, truncating mat4 and mat3. A matrix member
  sharing the register's vector storage now holds all sixteen values regardless
  of lane count, without growing the register at the tested widths.

## Reproduction

Run with the compiler and MSYS2 UCRT64 DLL directories on PATH:

```powershell
python riscv/gl4es-bridge/test/run_arb_uniform_validate.py
python riscv/scripts/rvv_doom_matrix.py --jobs 8 --repetitions 3
```

The matrix builds separate binaries with `zvl128b`, `zvl256b`, `zvl512b`, and
`zvl1024b`, matching `SGL_SIMT_LANES` and QEMU's VLEN. Compilation is concurrent;
QEMU measurements run sequentially. Compiler: GCC 16.1.0; emulator: QEMU 11.1.1,
`rva23s64`, one hart, OpenSBI, 1 GiB RAM. Raw results and payloads are under the
ignored `riscv/build/doom-matrix/`; `results.json` records every repetition.

The interaction test compares the real gl4es Doom interaction VS and FS kernels
against the generic SIMT executor. Binding tests exercise sampler changes,
relinking, attribute remapping, and live matrix changes. Resolve tests compare an
independent scalar channel oracle, 56 scale/edge/tail cases and a full
1280×960 → 640×480 image, including destination canaries.

## Performance limits

Final matrix: **36/36 passed** (three payloads × four widths × three runs).
The interaction VS and FS comparisons report zero mismatches at every width.
Median results across three runs:

| VLEN | Interaction kernel ns/fragment | 2× resolve speedup over old scalar |
|---|---:|---:|
| 128 | 9,399 | 1.32× |
| 256 | 8,472 | 2.01× |
| 512 | 8,150 | 2.55× |
| 1024 | 3,581 | 247.46× |

Each resolve run times three rounds; the table takes the median of those
per-run median ratios. Interaction numbers describe the current kernel, not an
A/B measurement of the metadata optimization alone.

These are QEMU microbenchmarks, not hardware measurements or whole-game FPS.
The old scalar resolve becomes unusually expensive under this compiler/QEMU
combination at VLEN 1024; its large relative speedup is specific to that baseline.
Earlier VLEN 1024 engine smoke runs reduced measured presentation from roughly
1–1.5 seconds to 4–5 milliseconds with the initial RVV implementation. Matching
camera/frame whole-game A/B measurements were not established, so total frame
speedups are not claimed. Shader metadata timings were noisy and do not justify
a numerical speedup claim.

A derivative batching experiment failed exact arithmetic comparisons and was
excluded. Heat-haze deformation remains disabled by default. Existing glass
streaks and black geometry triangles remain open in `TODO.md`; these performance
changes are not evidence that those rendering defects are fixed. Full engine
smoke coverage uses VLEN 1024; the four-width matrix covers shader and resolve
payloads, not four complete engine builds.

The final engine rebuilt successfully at `C:/src/build-rv-win2/dhewm3` and
completed multiple Mars City frames with eight harts at VLEN 1024. The final
serial log is `riscv/build/doom-opt/engine-final-smoke.log`; the emulator was
then shut down through its monitor. Build log:
`riscv/build/doom-opt/engine-final-build.log`.
