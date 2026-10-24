# First-tick trace and four/eight-hart QEMU comparison

## Setup

One unchanged shipping ELF (`5be867b7f95c43274c1621d276868c72691a8eb673c3a53f333456201d170622`),
QEMU 11.1.1 multi-threaded TCG, VLEN 1024, 4 GiB guest RAM, 768x432,
full-quality source textures, temporary snapshots of `doom-truecolour.img`.
The renderer already supports eight harts. Each log confirms the actual
renderer hart count. Four harts remain the physical-target and launcher default.

The Windows host is an Intel Core Ultra 7 265 with 20 physical/logical cores.
Windows CPU-set information reports eight efficiency-class-1 cores at logical
CPU IDs 0, 1, 6, 7, 8, 9, 18, 19; the remaining twelve are class 0. The topology
record and its query script are under `build-rv/host-cpu-topology.*`.

Unpinned runs execute in 4/8/8/4 order. An exploratory run restricted the entire
QEMU process to CPUs 0,1,6,7 and regressed severely: 81.418 s map-ready,
431.700 s first-present, a 350.282 s interval. Helper threads also share that
restricted CPU set, so this is not a clean guest-core scaling experiment. The
subsequent process-affinity eight-hart run was deliberately stopped; no timing
result is claimed for it and remaining process-affinity runs were cancelled.

The harness also supports pinning only vCPU threads, using HMP `info cpus` to
obtain their host thread IDs. A four-hart run used 0,1,6,7 with helper threads
unrestricted. It was also much slower and was repurposed as a diagnostic caller
trace; it timed out without a completed first-frame measurement and is excluded
from the timing comparison. The queued eight-hart affinity
run was cancelled. Windows thread handles require query plus set-information
rights for this operation. There is no recommendation to pin CPUs based on these
exploratory runs; the completed comparison uses normal unrestricted scheduling.
No compilation,
other test guests or debugger sampling overlap measured boots. Host scheduling,
boost and memory-system variation are still possible.

## Completed unpinned comparison

| Run | Renderer harts | Map-ready | First-map-present | Map-ready → present |
|---|---:|---:|---:|---:|
| 4a | 4 | 70.025 s | 108.867 s | 38.842 s |
| 8a | 8 | 73.544 s | 141.242 s | 67.698 s |
| 8b | 8 | 62.387 s | 123.776 s | 61.389 s |
| 4b | 4 | 59.665 s | 92.680 s | 33.015 s |

All report the same first-present workload: 2 views, 138 draws, 12,584 triangles
(5,275 shadow triangles), and the same ELF hash. Eight harts are slower in both
observed boots. The two-run means are 100.774 s (four) versus 132.509 s (eight)
overall, and 35.929 s versus 64.544 s for the map-ready interval. This is an
observation on this Windows/QEMU host, not a prediction for physical RISC-V.
Raw evidence: `build-rv/hart-scale-{4a,8a,8b,4b}/results.json` and serial logs.

## Stack sampling

The separate diagnostic run collected 380 samples per hart between map-ready
and first-map-present. Frame-pointer chains show the main hart repeatedly
flushing the deferred framebuffer command queue:

* Roughly 57% of its samples have `glCopyTexSubImage2D` →
  `sgl2_glCopyTexSubImage2D` → `sg_framebin_flush` → tile rasterization.
* Roughly 37% have `realize_textures` → `sgl2_glGenerateMipmap` →
  `sg_framebin_flush` → tile rasterization.
* Worker harts predominantly run overlay shading, bilinear texture sampling,
  interpolation, blending and pixel stores.

These are sampled call-chain frequencies, not exact CPU-time percentages.
QEMU stop locations favor translated-block boundaries. The frame-pointer walk
is incomplete across gl4es routines that omit frame pointers. Sampling runs
are diagnostic and excluded from the timing comparison.

The important distinction is that the expensive work under copy/mipmap calls
is predominantly **queued rendering**, not demonstrated copy bandwidth or
mipmap-filter cost. Copying a framebuffer requires prior draws to complete;
texture mutation must protect queued draws that sample that texture. Removing
these barriers indiscriminately would violate rendering dependencies.

Artifacts: `build-rv/first-tick-stacks-4/1-source-samples.json` and its symbolized
`.txt`; the raw log and diagnostic timing are in the same directory.
The matching ELF is saved as `engine.elf` there. A later one-shot API breakpoint
attempt did not capture an entry before its timeout. It did not identify an
engine caller; objective screenshots remain a hypothesis, not a traced finding.
All experimental guests were shut down.

## Reproduction

The boot harness now accepts `--harts`, `--host-cpus`, `--vcpu-cpus`, and
`--sample-stacks`. For example:

```powershell
python riscv/scripts/texture_boot_ab.py source --harts 8 --vcpu-cpus 0 1 6 7 8 9 18 19 --label hart-vcpu-8-test
python riscv/scripts/texture_boot_ab.py source --sample-stacks --label first-tick-stack-test
python riscv/scripts/first_frame_samples.py build-rv/first-tick-stack-test/1-source-samples.json
```

Host CPU IDs above are specific to this machine. The unsuccessful one-shot
experiment is retained as a diagnostic artifact in `build-rv/capture-first-tick-api.py`,
not as a supported boot-harness option.

## Next code target

Investigate texture-reference tracking for queued draws. If no queued command
can sample a texture being mipmapped, an unconditional global flush may be
unnecessary. This requires checking descriptor/pointer lifetimes and validating
output equivalence; it is not safe simply to delete the barrier. A framebuffer
copy still requires relevant prior framebuffer writes to complete. No game,
gl4es or softgles2 production behavior was changed by this tracing/scaling pass.
