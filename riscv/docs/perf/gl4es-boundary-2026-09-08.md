# gl4es latency and memory attribution — 8 September 2026

Follow-up: [allocation and copy optimization results](gl4es-optimization-2026-09-08.md).
The measurements below describe the earlier baseline.

## Scope

Measured the current Doom 3 → gl4es → RVV softgles2 build in QEMU 11.1.1,
VLEN 1024, eight harts, 1280×960 internal rendering resolved to 640×480.
Shadows, fog, postprocessing and refraction remain enabled. These are emulated
wall-clock times, not predictions of latency on physical RISC-V hardware.

Desktop GL entry/exit wrappers subtract nested GLES execution for each call.
This separates gl4es conversion/state handling from downstream shader compilation,
draw execution and deferred rasterization. The existing `[rprof-mt] wrapper`
counter describes the **softgles2 raster loop**, not gl4es.

See [instrumentation details](../../gl4es-bridge/PROFILE.md). No desktop API
lookups were left unwrapped in this run. Allocation ownership is tracked by
remapping allocation calls in a diagnostic copy of the gl4es archive, leaving
engine and softgles2 allocations out of the shim figures. All measured frames
have zero untracked frees, table overflows and unfinished desktop spans.
The independent host test passes nested timing exclusion, allocation conservation
and frame-counter reset checks.

## Fixed window view

Ten consecutive completed shim frames 44–53, after camera freeze and screenshot
capture, at `setviewpos -700 -1380 176 240`:

| Metric | Measurement |
|---|---:|
| gl4es exclusive time, median | 25.160 ms/frame |
| gl4es exclusive range | 22.220–33.286 ms/frame |
| Complete engine frame, median | 4198 ms |
| Median paired gl4es share of frame time | 0.596% |
| Desktop GL calls | 11,460–11,461/frame |
| gl4es requested live allocations | 11,418,061 bytes (10.89 MiB) |
| gl4es requested allocation high-water mark | 21,248,461 bytes (20.26 MiB) |
| Allocation/free events | 565 each/frame |
| Requested allocation traffic | 10,038,274 bytes/frame (9.57 MiB) |
| Entire guest committed heap | 573 MiB |

The largest shim entry points are `glDrawElements` (median 15.261 ms total over
559 calls) and `glCopyTexSubImage2D` (4.644 ms over three calls). These figures
exclude the GLES calls they invoke. Uploading ARB environment parameters takes
about 0.962 ms in total despite 3,688 calls. Per-entry figures come from each
frame's top-five ranking, so an entry absent from that ranking is not reported
as zero.

## Fixed hangar view

Ten consecutive completed shim frames 67–76, after warming and excluding the
screenshot pause, at `setviewpos 300 -1700 176 0`:

| Metric | Measurement |
|---|---:|
| gl4es exclusive time, median | 46.111 ms/frame |
| gl4es exclusive range | 43.684–58.620 ms/frame |
| Complete engine frame, median | 10,896 ms |
| Median paired gl4es share of frame time | 0.435% |
| Desktop GL calls | 23,556/frame |
| gl4es requested live allocations | 11,805,413 bytes (11.26 MiB) |
| gl4es requested allocation high-water mark | 21,635,813 bytes (20.63 MiB) |
| Allocation/free events | 1096 each/frame |
| Requested allocation traffic | 10,546,294 bytes/frame (10.06 MiB) |
| Entire guest committed heap | 573 MiB |

Retained allocations are constant across all ten samples. The largest exclusive
API totals are `glDrawElements` (33.298 ms median over 1090 calls) and
`glCopyTexSubImage2D` (4.177 ms over three calls).

## Interpretation and limits

Direct gl4es overhead is a small part of this workload's frame time. Most time
inside desktop GL calls is spent below the shim in the software renderer.
Removing only the measured shim work would have a small effect on whole-frame
latency. Changes to gl4es-generated shaders could still reduce downstream work;
this experiment does not measure an alternative shader generator or GLES engine
backend.

The retained allocation total is stable in the measured window frames. The
allocation/free traffic is nevertheless a useful optimization target; it is
not a leak, nor is allocated-byte traffic a measurement of memory bandwidth.
Draw conversion/state preparation and texture-copy fallback paths are the next
gl4es areas to inspect. Fragment shading and the raster loop remain much larger
whole-frame targets.

Memory values are requested allocation sizes, excluding allocator metadata and
fragmentation. The entire original gl4es archive's text/data/BSS totals
1,195,740 bytes (1.14 MiB); this is an upper bound on those archive sections
retained in the linked image, not its heap usage. The diagnostic allocation and
API tables consume a separate 2,109,440 static bytes and are not counted as
gl4es allocations. The tables are absent from normal builds.

Diagnostic timing has not been calibrated to remove wrapper overhead. Short
gl4es spans include timing-hook cost and host scheduling noise; reported
exclusive times are instrumented estimates. Screenshots and camera changes were
excluded from timing ranges. Engine frame totals include work outside rendering.

## Artifacts

Under `riscv/build/visual-audit/`:

- `shim-profile.log`: complete serial trace, including startup and scene changes.
- `shim-window-results.json`: all ten window rows and summary statistics.
- `shim-hangar-results.json`: all ten hangar rows and summary statistics.
- `shim-window.png`, `shim-hangar.png`: reviewed screenshots of measured scenes.
- `dhewm3-shim-profile.elf`: saved diagnostic executable matching the log.

Normal production builds use `BAREMETAL_SHIM_PROFILE=OFF`. The profiler changes
are diagnostic infrastructure and do not alter gl4es rendering behavior.
The normal engine was rebuilt successfully with profiling OFF after measurement;
its symbol table contains neither the boundary wrapper nor allocation tracker.
