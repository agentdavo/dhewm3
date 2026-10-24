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
> renderer under emulation, not I/O (see `riscv/docs/perf/`).
>
> **TCG iteration profile:** `TCG_PROFILE=1 riscv/demorun.sh` builds with
> `RVV_VLEN=256`, `512×288`, `bm_renderProf=1`, `PIN_HARTS=1`. VLEN A/B microbench:
> `riscv/scripts/tcg_vlen_bench.sh`. Shipping default remains VLEN=1024 / 768×432.

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

## Physical hardware target

Treat QEMU as a prototype for a physical implementation. Direct `-kernel`
loading and virtio-blk do not validate physical boot-ROM, SD-controller, DDR
initialization or flash/XIP behavior. Select boot media and device drivers to
match the intended hardware; SD boot into RAM is an option alongside XIP.
The physical target follows the QEMU `virt` DTB/DTS hardware contract, with
four harts and the RVA23S64 ISA profile. Default game launches use four harts;
retain larger software capacity for explicit scaling tests. The current build
also requires VLEN=1024 and the optional extensions in the build/launcher ISA
strings. Default memory remains 4 GiB. Use the generated tree for peripheral
addresses and interrupts. Storage currently uses virtio-blk; the SD controller
and physical boot firmware remain to be specified if SD boot/assets are used.

## Local build outputs

Firmware-backed disk boot is documented in [docs/sdcard-boot.md](docs/sdcard-boot.md).
`image/mksdcard.py` builds `build-rv/sdcard/doomos-sdcard.img` with the executable,
assets and save journal. `scripts/sdcard_boot.py` launches it with a separate
OpenSBI/U-Boot firmware bundle and no host-loaded game ELF. This validates
QEMU virtio-blk disk boot, not a physical SD controller or board boot ROM.

A second, **scalar** target builds the same image for baseline `rv64imafdc`
(== `rv64gc`: no V, no Zb*/Zk*, no Zfh/Zfa/Zicboz): `BM_VECTOR=0 build-win.sh`
-> `build-rv-scalar/`, launched with `sdcard_boot.py --isa rv64imafdc`. It is a
portability reference, not a shipping configuration — without RVV the software
renderer falls back to softgles2's non-vector reference paths. See
[docs/scalar-rv64imafdc.md](docs/scalar-rv64imafdc.md).

DTB placement is a platform constraint, not a DoomOS detail: the blob must sit
above `__heap_end` or `ram_init_from_dtb()`'s clamp subtracts it from the heap
one-for-one, silently. See [docs/dtb-placement-constraint.md](docs/dtb-placement-constraint.md).

Keep generated builds, scratch disk images, logs and captures inside this repository.
The Windows engine build defaults to `build-rv/`; validation artifacts belong in
`riscv/build/`. Keep the custom medany compiler, downloads and intermediates in
`build/toolchain/` (installed compiler: `build/toolchain/install/bin/`).
Do not create task build directories elsewhere on the system.

## Toolchain

| Tool | Used for |
|------|----------|
| `/opt/riscv` gcc **16.1.0** (`riscv64-unknown-elf-`) | preferred: full **RVV v1.0** intrinsics (`__riscv_v_intrinsic=1000000`) + newlib `libm`/`libstdc++` (auto-detected by the Makefiles) |
| `riscv64-unknown-elf-gcc` (system) | fallback (v0.11 intrinsics, no libc) |
| `qemu-system-riscv64` (8.2 `-cpu max`, or locally-built 11 `-cpu rva23s64`) | run target |
| OpenSBI (`fw_dynamic`, bundled with QEMU) | M-mode firmware → S-mode handoff |

### Windows (MSYS2 UCRT64) — anchored to `c:\msys64`

Windows hosts build and run entirely from the MSYS2 stack at **`c:\msys64`** plus a
one-time medany runtime-library build. See **[docs/windows-build.md](docs/windows-build.md)**
for the full recipe; the short version:

| Tool | Source |
|------|--------|
| `qemu-system-riscv64` **11.1.1** | pacman `mingw-w64-ucrt-x86_64-qemu` — accepts `-cpu rva23s64` + bundled OpenSBI v1.8.1 (verified booting to S-mode @ `0x80200000`) |
| binutils 2.47, host gcc, gmp/mpfr/mpc, ninja, cmake, texinfo | pacman (UCRT64) |
| **medany** `riscv64-unknown-elf-gcc` **16.1.0** + newlib 4.6.0 + libgcc/libstdc++ | built once into **`C:\projects\dhewm3\build\toolchain\install`** (recipe: `C:\projects\dhewm3\riscv\build-toolchain-win.sh`) |

> **Why a separate toolchain:** the pacman `riscv64-unknown-elf-*` package ships
> **medlow** prebuilt archives (`libc`/`libm`/`libgcc`/`libsupc++`/`libstdc++`).
> The image links at `0x80200000`, past medlow's ±2 GiB reach, so every one of
> them overflows (`relocation truncated to fit: R_RISCV_HI20`). C++ exceptions +
> RTTI are load-bearing (`common->Error()` throws), so `libsupc++`/`libstdc++`/
> `libgcc` must be medany too — a "rebuild newlib only" shortcut does **not**
> work. The medany build reuses `c:\msys64`'s binutils/host-gcc/gmp-mpfr-mpc and
> only replaces the compiler + runtime libs.

Build + run from a **UCRT64 shell** (cc1plus needs the ucrt64 DLLs on `PATH`):

```sh
riscv/build-win.sh                                  # -> C:/projects/dhewm3/build-rv/dhewm3
cp doom.img build-rv/doom-scratch.img                 # engine writes to the disk
DISK="$PWD/build-rv/doom-scratch.img" riscv/run-win.sh
```

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
