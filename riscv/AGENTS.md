# dhewm3 on bare-metal RISC-V64

Boot the [dhewm3](https://github.com/dhewm/dhewm3) Doom 3 source port **without an
operating system** on a RISC-V64 machine (QEMU `virt`), rendering through a
custom **software OpenGL ES 2.0** implementation that is optimised for the
**RISC-V Vector (RVV / "V") extension**.

The classic GL 1.x / ARB-program calls the engine emits are bridged to GL ES 2.0
by [gl4es](https://github.com/ptitSeb/gl4es), whose GLES2 backend then targets
our software rasteriser instead of a GPU.

```
  dhewm3 engine  ──qgl──▶  gl4es (GL 1.4/ARB ▶ GLES 2.0)  ──▶  softgles2 (SW rasteriser, RVV)
        │                                                              │
        └────────────── bare-metal sys layer (no OS) ─────────────────┘
                         OpenSBI · virtio-blk FS · timer · input
```

## Status

| Stage | Component | State |
|-------|-----------|-------|
| 1 | Boot + S-mode runtime (SBI console, heap, traps, libc, **RVV self-test**) | ✅ working, boots on QEMU |
| 2 | Freestanding C/C++ runtime (static ctors, `new`/`delete`, libm) | ✅ newlib/libstdc++ via `/opt/riscv`; engine C++ links + runs |
| 3 | `softgles2` software GL ES 2.0 (API surface + **RVV textured rasteriser** + GLSL VM/SIMT) | ✅ buffers/textures/attribs/draw + blend/depth/stencil + GLSL interp + SIMT kernels; host+QEMU conformance green |
| 4 | gl4es shim integration (GL 1.4/ARB → GLES2) | ✅ engine renders through gl4es → softgles2 |
| 5 | dhewm3 bare-metal sys layer + `HARDLINK_GAME` build | ✅ boots, `Sys_*` over SBI, SDL3-on-dOS input/GL |
| 6 | virtio-blk filesystem (read `.pk4` assets) | ✅ mounts `doom.img` (dosfs), loads `base/demo00.pk4` |
| 7 | `doom.img` boot image + `run.sh` (matches project brief) | ✅ `image/mkimage.sh`, `scripts/run.sh`, headless capture |
| 8 | **Playable Mars City walkthrough** (enter map + move) | ✅ boots `demo_mars_city1`, skips intro, player spawns + walks (see below) |

**The port boots dhewm3 with no OS on QEMU `virt` (`-cpu rva23s64`, OpenSBI), mounts
the asset disk over virtio-blk, loads `game/demo_mars_city1`, spawns a controllable
player in Mars City, and renders it 768×432 through gl4es → softgles2.** See
[ROADMAP.md](ROADMAP.md) for the phase detail and orchestration workflow.

## Headless Mars City walkthrough

Because the bare-metal SDL3 backend has no HID, a deterministic **scripted usercmd**
drives the player (no input events needed), and the intro cinematic is auto-skipped:

* `g_skipCinematic 1` — fast-forwards the `mars_city1` intro camera to player control
  (the skip also force-clears the left-over full-screen fade: `Game_local.cpp`).
* `in_scriptedMove 1` — injects a walk-forward + slow-yaw usercmd in
  `idUsercmdGenLocal::MakeCurrent()` (bypasses key bindings / the dead input path).
  Tunables: `in_scriptedFwd` (−127..127), `in_scriptedYawPerTic`, `in_scriptedRun`.
* All baked into the boot argv (`riscv/engine/engine_kmain.c`); the frame loop
  (`sys_dos.cpp`) logs `getviewpos` each frame and can `setviewpos`-warp to a scenic
  vantage.

Capture the walkthrough (waits for the multi-minute software load, then samples the
live gameplay so no shots are wasted on the loading screen):

```sh
riscv/scripts/walk_capture.sh          # -> riscv/shots_walk/shot_NN_fr<f>_re<r>.png
```

> **Perf note (TCG):** the world-media *load* (`EndLevelLoad` gl4es texture uploads)
> and the ~5M-frag/frame software shading are CPU-bound under QEMU TCG — a full map
> load is ~8–25 min and in-world frames are seconds each. This is the software
> renderer under emulation, not I/O (see `riscv/docs`).

## Quick start (stage 1)

Requires `riscv64-unknown-elf-gcc` (bare-metal newlib toolchain) and
`qemu-system-riscv64`.

```sh
cd riscv
make          # build build/payload.elf
make run      # boot it on QEMU virt via OpenSBI (Ctrl-A X to quit)
```

Expected output ends with the RVV self-test and a clean SBI power-off:

```
[rvv] V extension present: VLEN = 128 bits (vlenb=16)
[rvv] vector add: c[0]=1000 c[36]=1072  -> PASS
[ok] stage-1 runtime healthy; powering off via SBI.
```

## Toolchain

| Tool | Used for |
|------|----------|
| `/opt/riscv` gcc **16.1.0** (`riscv64-unknown-elf-`) | preferred: full **RVV v1.0** intrinsics (`__riscv_v_intrinsic=1000000`) + newlib `libm`/`libstdc++` (auto-detected by the Makefiles) |
| `riscv64-unknown-elf-gcc` (system) | fallback (v0.11 intrinsics, no libc) |
| `qemu-system-riscv64` (8.2 `-cpu max`, or locally-built 11 `-cpu rva23s64`) | run target |
| OpenSBI (`fw_dynamic`, bundled with QEMU) | M-mode firmware → S-mode handoff |

## Layout

```
riscv/
  boot/            bare-metal runtime: start.S, linker.ld, sbi.c, trap.c, runtime.c
  kmain.c          stage-1 bring-up / self-test entry
  Makefile         build the payload
  scripts/run.sh   launch on QEMU (auto-selects qemu 11 + rva23s64 if built)
  softgles2/       software OpenGL ES 2.0 renderer        (stage 3, see ROADMAP)
  gl4es/           vendored GL→GLES2 shim                 (stage 4)
  image/           doom.img builder + asset packing       (stage 7)
```

`riscv/scripts/run.sh` launches RISC-V SMP guests with multi-threaded TCG
(`-accel tcg,thread=multi,tb-size=256` by default; override with
`TCG_TB_SIZE=<MB>`). `SMP` is passed directly to QEMU, so topology strings like
`SMP=8,cores=4,threads=2` are supported; the launcher uses the leading count
for host pinning. For repeatable host-core placement, add `PIN_HARTS=1`; by
default this maps vCPU/hart threads to host CPUs `0..SMP_COUNT-1`. Override the
placement with `PIN_HART_CPUS=0,1,2,3`.

By default the launcher keeps the plain `virt,acpi=off` QEMU machine. To try
RISC-V AIA interrupt-controller topologies on compatible QEMU builds, pass
`AIA=aplic` or `AIA=aplic-imsic`; `demorun.sh` forwards the same env var to
`run.sh`.
