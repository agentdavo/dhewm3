# Scalar `rv64imafdc` (no-vector) target

A second build of the same DoomOS image for the RISC-V64 **baseline** ISA:

```
rv64imafdc   ==  rv64gc  ==  I + M + A + F + D + C  (+ Zicsr, Zifencei)
```

No V, no Zba/Zbb/Zbs/Zbc/Zbk*, no Zfh/Zvfh, no Zicboz, no Zfa, no Zihintpause,
no fixed-VLEN codegen. It exists to prove the port is not welded to RVA23S64 and
to give a reference target for parts that have no vector unit.

> **`rv64imacfdc` is not a legal `-march` string.** GCC parses the single-letter
> extensions in canonical order (`i m a f d q c`) and rejects the second `c`:
> `error: '-march=rv64imacfdc': extension 'c' appear more than one time`.
> `rv64imafdc` names the identical ISA.

## Build

```sh
BM_VECTOR=0 riscv/build-win.sh            # -> build-rv-scalar/dhewm3
```

`BM_VECTOR=0` selects `riscv/toolchain/baremetal-rv64imafdc.cmake`, points gl4es
at `-march=rv64imafdc`, and uses a separate build directory so it never shares a
CMake cache with the shipping vector tree. Everything else — CRT, linker script,
medany model, newlib/libstdc++, SDL3 — is unchanged.

Then the disk and a run:

```sh
riscv/image/mkimage.sh                    # -> doom.img (assets + savefs)
wsl -d Ubuntu -- bash riscv/image/build-boot-firmware.sh
C:/Python314/python.exe riscv/image/mksdcard.py \
    --elf build-rv-scalar/dhewm3 --assets doom.img \
    --out build-rv-scalar/sdcard/doomos-sdcard-rv64imafdc.img
C:/Python314/python.exe riscv/scripts/sdcard_boot.py --isa rv64imafdc \
    --image build-rv-scalar/sdcard/doomos-sdcard-rv64imafdc.img
```

The OpenSBI/U-Boot firmware bundle is ISA-independent and is shared with the
vector image. `--isa rv64imafdc` launches QEMU with

```
-cpu rv64,v=false,zicboz=false,zfa=false,zba=false,zbb=false,zbs=false
```

The optional extensions are turned **off**, not merely left unused, so anything
that slipped into the image lands as an illegal instruction instead of quietly
executing.

## What had to change

softgles2 already carried non-RVV reference paths for its host builds — all 30
sources compiled scalar untouched — and `libSDL3-riscv.a` was already built
`rv64imafdc`. The toolchain's newlib/libgcc/libstdc++ are `rv64gc` (the compiler
was configured `--with-arch=rv64gc --disable-multilib`), so none of them carry
vector code either. The work was in the hand-written assembly and the places
that assumed a vector unit exists at run time:

| File | Change |
|---|---|
| `boot/start.S` | Trap-handler `v0-v31`/`vtype`/`vl`/`vcsr` spill+reload behind `#ifdef __riscv_vector`; `sstatus.VS` is read-only zero without V, so the mask folds to 0 and the FS/VS restore becomes a plain FS restore |
| `boot/runtime.c` | `rv_detect_platform_caps()` (`csrr vlenb`, `vsetvli`) guarded, geometry zeroed instead; the `cbo.zero`/`fli.s`/`vwsll.vi` probes now compile only when `__riscv_zicboz`/`__riscv_zfa`/`__riscv_zvbb` are defined |
| `kmain.c` | Stage-1 RVV self-test guarded; scalar builds print that V is absent rather than silently skipping |
| `boot/bm_pause.h` | `pause` is Zihintpause and will not assemble without it. PAUSE is `FENCE pred=W, succ=(none), fm=0, rd=rs1=x0` == `0x0100000F`, which a hart without Zihintpause retires as that FENCE, so the raw encoding is emitted instead. Verified byte-identical to `pause` assembled with `-march=..._zihintpause` |
| six spin loops | `dos_syscalls.c`, `dos_thread.c`, `sys_baremetal.cpp`, `sys_dos.cpp`, `sg_mt_sched.c`, `dos_virtio_gpu.c` each hand-rolled `asm("pause")`; all now use the existing `bm_pause()` helper, which previously had no callers |
| `engine_kmain.c` | The boot banner asserted `vlmax_e32m2 == SGL_SIMT_LANES`. With no `vsetvl` to interrogate that is `0 != 64` and panicked on every boot; guarded, and the banner now reports the scalar configuration |
| `softgles2/src/glsl_simt.c`, `sg_kernels.c` | `sgl_mip_enabled`, `sgl_mip_trilinear_enabled`, `sgl_heathaze_deform`, `sgl_dbg_iak_light_unity` sat inside `#if GLSL_SIMT_RVV` but are written unconditionally by `BM_SyncRendererCvars()`. Now defined unconditionally, matching `sgl_dither_enabled` / `sgl_lod_deriv_enabled` |
| `gl4es-bridge/build-gl4es.sh` | Emits `libGL-riscv-scalar.a` for a non-vector `-march`, so a vector `libGL.a` can never be linked into a scalar image. The ISA is decided by asking the compiler for `__riscv_vector`, not by pattern-matching the `-march` string (every `rv64*` name contains the `v` of `rv`) |

Separately, `riscv/Makefile` was missing `boot/fdt.c` from `SRCS`. That is a
pre-existing bug on the vector build too — it only ever linked because
`--gc-sections` happened to drop `timer_probe_sstc`/`ram_init_from_dtb`.

## Scope and caveats

* **This is a portability target, not a shipping configuration.** Losing V means
  losing every softgles2 vector kernel; the build falls back to the reference
  paths (`glsl_simt.c`: *"host / non-RVV: correct reference (loop scalar IR /
  tree-walk per lane)"*). Those are written for correctness, not throughput.
* `SGL_SIMT_LANES` stays 64 on the scalar build. Without `vsetvl` it is only the
  SoA stride the reference loops walk, not a hardware lane count; keeping 64
  also keeps the multithread tile height (`SGL_SIMT_LANES/4`) at the validated 16.
* The same QEMU-virt caveats as the vector image apply: this validates
  virtio-blk disk boot, not a physical SD controller or board boot ROM.

## Validation on 2026-09-21

Image: `build-rv-scalar/sdcard/doomos-sdcard-rv64imafdc.img`, 619,708,416 bytes
(591 MiB), sha256 `2cc9d0bd…a028e41a`. Firmware bundle shared with the vector
image, sha256 `6bd44489…67601ac1`.

* **The built ELF contains no vector code.** `objdump -d` over all 1,763,531
  instructions in `build-rv-scalar/dhewm3` finds 0 vector instructions and 0
  Zicboz/Zfa/Zb instructions. Its `.riscv.attributes` reads
  `rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0_zcd1p0`.
* **Firmware-only cold boot succeeded** with V, Zicboz, Zfa, Zba, Zbb and Zbs all
  disabled on the QEMU CPU; OpenSBI reported `Boot HART Base ISA: rv64imafdch`.
  No illegal-instruction trap anywhere in the run. Result and logs in
  `build-rv-scalar/sdcard/check/`.
* Host-observed: U-Boot 0.22 s, disk handoff 0.44 s, DOSFS mount 0.66 s,
  map-ready 51.8 s, **first map frame 330.8 s**. Four render harts came up
  (`[sgl-mt] tile-parallel raster: 4 harts (3 workers, 64x16 tiles)`), savefs
  mounted, and the CPU banner correctly reported
  `[doomos-cpu] no V extension (scalar build); SoftGLES batch=64 lanes` with
  `isa probe: zicboz=NO zfa=NO zvbb=NO`.
* Asset load is essentially unaffected (map-ready 51.8 s here vs 50.3 s for the
  vector run in [sdcard-boot.md](sdcard-boot.md)) — that stage is I/O and CPU
  work the vector kernels never touched. The cost lands on rendering:
  **279 s from map-ready to first frame, against 103.5 s** for the vector run,
  ≈2.7×.

  Treat that ratio as indicative, not a measurement. The two runs are from
  different sessions rather than a controlled A/B, and roughly the last 80 s of
  the scalar first-frame stage overlapped a concurrent 6-job host build. A real
  comparison needs both images run back to back on an otherwise idle host.
* The shipping vector build was rebuilt from the same tree afterwards and is
  unchanged in kind: it links and still contains 15,853 `vset*` instructions.

### Note on rebuilding `doom.img`

`mkimage.sh` is not byte-reproducible: two runs over the same `base/` produced
different SHA-256s (`a7f67a3b…` then `f4a27bd9…`). The image manifest
`doomos-sdcard-rv64imafdc.json` records the assets hash that actually went into
the delivered disk; a later rebuild will not match it. The delivered disk itself
is stable and was re-verified as `2cc9d0bd…a028e41a` after the run.

## Heap high-water (measured 2026-09-21)

Measured on the scalar build with `RENDER_PROF=1`, `demo_mars_city1`, 768x432,
4 harts, via `[rprof-heap]` (`dos_heap_committed_bytes()` = the `_sbrk`
high-water, which is the whole engine heap: newlib malloc sits on `_sbrk`).

| Stage | committed |
|---|---:|
| loading screen | 200 MB |
| game-load-start | 279 MB |
| textures-upload-start | 557 MB |
| after map-ready (plateau) | **750 MB** |

Do not quote the early samples as the high-water: the first `[rprof-heap]` line
appears during loading-screen frames, long before texture upload. The figure
plateaus at 750 MB and is flat across the remaining samples.

Caveat: `RENDER_PROF=1` defaults `bm_benchVantage`/`bm_benchFreeze` to 1, so the
camera sits at the scenic bench vantage rather than the spawn point. That biases
the number up if anything, so it is conservative for sizing, but it is not the
plain-spawn figure.

### `dos_heap_limit()` clamps, and no longer extends

The DTB can now only move the heap limit *down*. Both halves of the old
behaviour were wrong:

* **It never clamped.** On a machine with less DRAM than the 1920 MiB link
  window it returned `__heap_end` unchanged and `_sbrk` handed out addresses
  past physical memory.
* **It extended, and extending is unsound.** `__heap_end` *is*
  `__stack_bottom` -- the 8 MiB boot stack sits immediately above it, and on a
  part with a framebuffer carved from the top of DRAM, so does that aperture.
  `_sbrk` bounds the break by `[__heap_start, limit]` and nothing else; sbrk
  cannot express a hole. So any limit above `__heap_end` places the stack
  *inside* the heap arena. That was true on every path including QEMU, which
  merely had 2.44x of margin rather than 1.24x and so never hit it.

Reclaiming RAM past the link window was the stated reason for extending, but
it was never a measured need -- peak demand is 750 MB against a 1832 MiB linked
heap. The limit per path:

| Path | `ram_end()` | old limit | new limit | margin over peak |
|---|---|---:|---:|---:|
| `sdcard_boot.py` (`boot.scr` `fdt move ... 0x100000000`) | `0x100000000` | 1966 MB | 1832 MB | 2.44x |
| `run-win.sh` direct `-kernel`, `-m 4G` | `0x17fe00000` | 4012 MB | 1832 MB | 2.44x |
| 1 GiB part, 1020 MiB link, `/memory` = full 1 GiB | `0xc0000000` | 1024 MB (spans stack + fb) | 932 MB | 1.24x |

If a workload ever genuinely needs more than the linked window, the honest knob
is `LENGTH(RAM)` in `engine.ld` -- a deliberate act, bounded by medany's +/-2 GiB
reach -- not an accident of where firmware left the DTB. `bm_report_ram_window()` also
prints the window at boot and warns loudly when `__stack_top` is above DRAM --
the heap can follow the DTB at run time, but the stack is linked above the heap
and cannot be moved, so that case needs a relink and nothing else will do.

### Sizing a smaller machine

A 1 GiB part linked with a 1020 MiB window gives ~933 MiB of heap. Against the
measured 750 MB that is 183 MiB spare (80% utilised) -- workable, not roomy, and
that is the *demo* map. Any such target must also place its DTB **above**
`__heap_end`: `ram_init_from_dtb()` deliberately clamps `ram_end()` below the
blob, so a DTB sitting inside the heap range silently costs exactly as much heap
as it sits low by. `boot.scr`'s `fdt move` is the existing pattern for that.

### What this image does NOT need

Verified against the linked ELF, not inferred: the image never enables the MMU.
No `satp` write, no `sfence.vma`, no `sinval.vma` and no page-table construction
anywhere in `riscv/{boot,engine,smp,dos,softgles2}` -- the single tree-wide
mention is a comment in `riscv/smp/secondary.S:8` noting that `satp == 0` on
entry -- and `objdump` over the 60 MB ELF finds zero of those opcodes. It runs
in bare mode on physical addresses throughout, which is what `-mcmodel=medany`
plus the fixed `0x80200000` link implies.

So Sv39 paging, NAPOT leaves (Svnapot), TLB invalidation semantics (Svinval) and
any TLB implementation are irrelevant to whether this image boots. What it does
need from a target: DRAM at `0x80000000` with the payload at `0x80200000`; SBI
HSM/TIME/IPI/SRST/DBCN/PMU (all six are probed and printed at boot); a DTB placed
**above** `__heap_end`; virtio-blk; and four harts.

## Retargeting the RAM window (1 GiB parts)

`riscv/engine/engine-raven.ld` is `engine.ld` with `LENGTH = 1920M` changed to
`1020M` and nothing else -- verified by diffing the bodies, 122 lines each, one
line differing. The script derives `__stack_top`, `__stack_bottom` and
`__heap_end` from `ORIGIN(RAM) + LENGTH(RAM)`, so retargeting really is one
number.

```sh
BM_VECTOR=0 BUILD=<dir> ENGINE_LD=riscv/engine/engine-raven.ld riscv/build-win.sh
```

`ENGINE_LD` is a cache variable, so **use a separate `BUILD` dir** -- reusing a
tree silently keeps whichever link it was configured with. Verified from the
linked ELF, not the script: `__heap_start 0x85154380`, `__heap_end/__stack_bottom
0xbf600000`, `__stack_top 0xbfe00000`, all inside a 1 GiB part.

Booted at `-m 1G` on the no-vector CPU: `usable=0x80000000..0xbfe00000 (1022MB)
heap=932MB`, map-ready 47.7 s, **72 world frames rendered, zero allocation
failures**. So the measured 750 MB peak does fit in 932 MiB in practice, not
just in arithmetic. QEMU at `-m 1G` places the DTB at `0xbfe00000` by itself --
exactly the placement derived as correct, immediately above the stack.

### The SD path needs `--fdt-addr` on a small part

`boot.scr` relocates the DTB to `0x100000000` (4 GiB), which is above the whole
1920 MiB window and correct for QEMU virt at `-m 4G`. **That address does not
exist on a 1 GiB part and `fdt move` fails outright.** `mksdcard.py --fdt-addr`
now selects it; the default is unchanged and produces a byte-identical
`boot.scr`, so existing image hashes are undisturbed.

For a 1 GiB part linked with `engine-raven.ld`, the blob goes in the gap between
`__stack_top` and the framebuffer aperture -- `0xbfe00000..0xbfed4000`, 848 KiB:

```sh
riscv/image/mksdcard.py --fdt-addr 0xbfe00000 ...
```

Placing it low instead costs heap one-for-one: a DTB at `0x9fe00000` yields a
428 MiB heap against the 750 MB requirement, and the boot line says so
(`heap CLAMPED below DTB/DRAM end`, plus the stack warning).

### Reading the marker, and a harness trap

`stage=first-map-present` requires a virtio-gpu **scanout**. `run-win.sh` has no
virtio-gpu unless `VIDEO=gpu`, so the marker never fires there no matter how
long it runs -- a `-m 4G` probe rendered 224 world frames without it. Do not
read its absence as "the first frame never completed"; count `^views:` lines
instead. `sdcard_boot.py` does attach a virtio-gpu, which is why the marker is
meaningful in the `--check` result.
