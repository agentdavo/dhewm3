# Boot timing

The engine emits `[boot-time] total_ms=... delta_ms=... stage=...` at startup.
`total_ms` starts at DoomOS C-runtime entry, before BSS clearing, platform
probing and constructors. OpenSBI, host process launch and a physical ROM/SD
loader are outside this measurement. Times use `rdtime` and the DTB timebase,
not `Sys_Milliseconds` or the deterministic gameplay clock.

Milestones cover runtime setup, constructors, threading, asset disk, save
replay, engine initialization, map preparation, world geometry/materials,
game/entity loading, texture uploads and interaction generation.

`first-display-present` is the first successful virtio GPU transfer/flush,
normally a loading screen. `map-ready` means map spawning completed.
`first-map-present` is the first successful transfer/flush after that signal;
it may still show the intro cinematic or a fade. It is not proof of player
control, physical display scanout completion, or time-to-photon latency.
Without a functioning virtio GPU, this milestone deliberately remains absent.
The two present markers are emitted only once per boot.

Summarize a captured serial log from the repository root:

```powershell
python riscv/scripts/boot_timing.py build-rv/doom-boot-timing.log
```

The summary stops at the first map present and ranks the largest intervals.
An incomplete boot is identified explicitly. Deltas are wall-clock intervals
between markers, not summed per-hart CPU time or exclusive function profiles.
They include QEMU translation/emulation, scheduling and any diagnostic I/O.
Individual integer-millisecond rounding can make delta sums differ slightly
from the final cumulative time.

Compare the same ELF, assets, quality, VLEN, hart count and scene. Full-source
textures and compressed/downsampled textures have different startup costs.
Do not report these TCG measurements as physical hardware performance.

## First measured full-quality boot (2026-09-20)

Stock MSYS2 QEMU 11.1.1, RVA23S64, four harts, VLEN1024, 4 GiB RAM,
768x432, full-resolution uncompressed textures, corrected gamma math:

| Milestone or interval | Time |
| --- | ---: |
| Storage and save replay complete (cumulative) | 0.112 s |
| First display present (cumulative) | 1.956 s |
| Engine init returned (cumulative) | 12.395 s |
| Game/entity load interval | 25.664 s |
| Texture upload interval | 51.767 s |
| Map ready (cumulative) | 102.358 s |
| Map ready to first map present interval | 132.227 s |
| First map present (cumulative) | 234.585 s |

The last interval accounts for 56.4% of startup, texture upload 22.1%, and
game/entity loading 10.9%. These are one-run observations, not benchmark
medians. The last interval includes work before/during the first render and
GPU transfer; finer profiling is needed before assigning it all to shading.
Raw log: `build-rv/doom-boot-timing.log`; summary:
`build-rv/boot-timing-summary.txt`.
