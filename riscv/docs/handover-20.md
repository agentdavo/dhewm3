# DoomOS handover 20 — executing handover 10, plus the RVA23S64 roadmap

Written 2026-07-25/26. Branch `riscv64-baremetal`, on top of `9a6d6b6`.
Everything is **uncommitted working-tree state**.

Supersedes handover 10's §5 and §7 action lists, and replaces the interim
handover 11 (deleted; its content is folded in here). Read handover 10 for the
*analysis*; read this for what was done to it and what is now known.

---

## 0. TL;DR — read these five things

1. **The DTB timebase parser is VERIFIED working.** Handover 10 §4.3 left this
   explicitly unproven, and it mattered because QEMU virt's timebase equals the
   compile-time fallback, so a broken parse was indistinguishable from a working
   one. Every boot now prints `source=dtb` vs `COMPILE-TIME FALLBACK`. §2.1
2. **FrameBin works and eliminates the serial raster stage entirely** —
   `raster = 0.000 ms` in all 488 profiled frames, against handover 10's baseline
   of 10942 ms (87% of a lit frame), with six harts balanced to 0.4%. §3
3. **The profiler was lying about frame cost, in the same way handover 10 caught
   it lying before.** `[rprof] total` omitted the whole parallel section under
   FrameBin: 604–843 ms reported against a 3458–3949 ms real backend. Fixed. §3.2
4. **The pixel-identity verdict is NOT yet established, and the reason is a
   harness defect, not FrameBin.** Frame content was a function of wall-clock
   time, so two builds at different speeds render different content at the same
   frame index. `com_fixedTic` is now wired in. §3.3 — **finish this first.**
5. The four-step RVA23S64 roadmap (full AIA, PMU counters, parallel vertex,
   Zawrs) is implemented and compiling but **unvalidated at runtime**. §6

---

## 1. Gates

```bash
# bit-exactness oracle (THE gate) -- currently PASSING
cd riscv/softgles2 && make iakgl4esrv RVV_VLEN=256 -j6
cd /home/djs/dhewm3 && RVV_VLEN=256 SMP=2 PIN_HARTS=1 TIMEOUT=180 \
  riscv/run.sh riscv/softgles2/build/iakern_gl4es_validate.elf
# expect: [gik-vs] maxdiff=0 bitmiss=0 OK   and   [gik] ... OK(bit-exact)

# frame-digest gate (pixel identity between two builds)
riscv/scripts/framebin_ab.sh              # both arms
ARMS=off riscv/scripts/framebin_ab.sh     # re-run one arm
python3 riscv/scripts/fbhash_cmp.py <ref.log> <cand.log> "label"
```

**Do not read `speedup=` from a contended host.** Two runs of the identical
binary minutes apart reported 2.10x and 0.92x purely because a QEMU was running
alongside the second. Same class of error as handover 10 §3.1's retracted claim.

**Three harness rules, each learned the hard way tonight:**

- **Run QEMU under `setsid`.** A run died at minute 13 of 30 with
  `terminating on signal 15 from pid ... (bash)` because an unrelated wrapper in
  the same process group timed out. Handover 10 §3.1 lost a whole 40-minute run
  to exactly this and said so; I reproduced it anyway. `framebin_ab.sh` now does
  it.
- **Compare only frames present in BOTH runs.** Runs are cut off by wall-clock
  timeout, so unequal frame counts are normal. `fbhash_cmp.py` compares the
  intersection and reports the non-overlap separately, so a thin overlap cannot
  be mistaken for a strong pass. It is tested three ways (identical → PASS, one
  mutated frame → FAIL naming the frame, truncated → PASS with a note).
- **Never edit sources while a build runs, and check whether a "timed out" build
  is actually dead.** Two `make`s ended up compiling in `build-rv-dd1`
  simultaneously (the harness backgrounded one I assumed was killed), and a later
  build consumed a half-written `irq.c`. Both produce binaries that link fine and
  cannot be trusted.

---

## 2. Boot / platform (handover 10 §7)

### 2.1 One clock, and it is the real one
`timer_ms()` divided by the compile-time `TIMEBASE_HZ` while
`timer_ns_from_ticks()` used the discovered value — two clocks that disagree,
worse than one that is wrong. All runtime sites now use `timer_hz()`:
`runtime.c` (plus new `timer_us()`, `timer_ticks_from_us()`), `sys_baremetal.cpp`
(`bm_timebase_hz` was never refreshed, so **every Doom 3 frame time and every
`Sys_MillisecondsPrecise()` was wrong by the timebase ratio on hardware**), both
`dos_syscalls.c`, `sys_dos.cpp`, `smp/sched.c`, `smp/kmain.c`,
`gl4es-bridge/newlib_syscalls.c`. Remaining `TIMEBASE_HZ` uses are in
`softgles2/test/*` benchmark harnesses, which only scale their own printouts.

### 2.2 Trap vector saves vector and FP state (7.1 A1) — the blocker
`trap_vector` saved x1–x31 and nothing else while the boot hart both enables
`sstatus.SIE` and runs RVV raster loops. Now: V state spilled only when
`sstatus.VS != Off` and FP only when `FS != Off`; the vector area is sized from
`vlenb` **at run time** (`32*vlenb` — 512 B at VLEN=128, 4 KiB at VLEN=1024) and
lives on the trap stack, so it is per-hart and re-entrant with no scratch array;
`vstart` zeroed before the spill and restored **last**; `FS`/`VS` forced on for
the handler (so autovectorised C on the IRQ path cannot take a nested illegal
instruction) then restored exactly, since `sret` does not restore them.
Verified in the disassembly: 4x `vse8.v`/`vle8.v` at LMUL=8, `vsetvl zero,t1,t2`
for the vtype/vl restore, `csrw vstart` last.

`sstatus.FS/VS` are now also explicitly enabled in **both** entry paths rather
than relying on OpenSBI leaving them on (7.1 B3).

### 2.3 A real FDT reader (7.1 A3)
New `riscv/boot/fdt.c`/`.h`. The previous consumers could not read a property:
`irq.c` substring-scanned the raw blob and `runtime.c` had a private walker
hardcoded to `timebase-frequency`, so every MMIO base, interrupt context and
hartid was a hardcoded QEMU-virt convention. API: `fdt_next_node`, `fdt_getprop`,
`fdt_node_is_compatible`, `fdt_find_compatible`, `fdt_parent`, `fdt_reg`,
`fdt_getprop_u32`, `fdt_cpu_has_extension`. Added to
`riscv/softgles2/Makefile`'s `RT` list — it linked without it only because
`--gc-sections` happened to drop the timer path.

### 2.4 Interrupt controllers — see §6.1 for the full AIA work
PLIC is DTB-driven end to end: base from `reg`, **context from
`interrupts-extended`** (the index of this hart's `riscv,cpu-intc` phandle with
cause 9), source count from `riscv,ndev`. The old `boot_hartid*2+1` is right only
for a single socket based at hart 0 — and the boot hart is a lottery, observed as
0, 1, 3 and 5 across tonight's runs.

`os_handle_irq()` now handles **software and timer interrupts recoverably**.
Previously anything that was not an external interrupt fell through to `trap.c`'s
FATAL TRAP and powered the machine off, so the first IPI a hart ever received
killed the run. `sie.SSIE` is set alongside `SEIE`, or a `wfi`-parked hart can
never be woken. Both are preconditions for §4.

### 2.5 RAM discovered, not assumed (7.2 A2)
`ram_init_from_dtb()` walks `/memory` for the bank containing the image and
clamps below the DTB blob. `_sbrk` extends its ceiling accordingly, recovering
**2176 MiB** at `MEM=4G` (RAM runs to `0x180000000`, the linker window stopped at
`0xF8200000`). `engine.ld`'s ±2 GiB medany argument constrains *symbol*
addressing, not a runtime arena reached through ordinary pointers.

### 2.6 Stack red zone instead of an MMU (7.2 B6/C1)
Worker stacks were contiguous and unguarded. Each now carries a 4 KiB poisoned
red zone below its low end, checked in `dos_thread_join()` and exposed as
`dos_thread_stack_overflowed()` — following the audit's advice to fix B6 without
turning on paging.

### 2.7 Sparse hartids (7.2 B1/B2)
`dos_discover_harts()` enumerates `/cpus` (skipping `disabled` and harts HSM will
not manage) instead of `for (hart = 0; hart < 8; ++hart)`, falling back to the
dense scan when there is no usable DTB.

### 2.8 Boot census (7.1 A6, 7.2 B5) — every line was once a silent failure
```
[doomos-plat] timebase 10000000 Hz (from dtb)
[doomos-plat] isa probe: zicboz=yes zfa=yes zvbb=yes rdcycle=yes rdinstret=yes
[doomos-plat] sbi ext: HSM=yes TIME=yes sPI=yes SRST=yes DBCN=yes PMU=yes
[doomos-plat] virtio-mmio slots: ... (N used, M free)
[doomos-irq]  backend=plic hart=0 sources=96
```
`sources=96` is `riscv,ndev`-derived and confirms the old hardcoded 128 was
overrunning QEMU virt's implemented arrays (7.1 B4).

### 2.9 ISA probed by execution (7.3 A2)
`cbo.zero`, `fli.s`, `vwsll`, `rdcycle`, `rdinstret` are each probed by
*executing* them behind a trap hook that steps over a fault
(`rv_probe_trap_hook`). Necessary because `cbo.zero` traps as illegal in S-mode
unless M-mode set `menvcfg.CBZE`, `menvcfg` is not readable from S-mode, and it
works today only because the bundled OpenSBI happens to enable it.

### 2.10 Other
- **VLEN mismatch panics** (7.3 A1). With `-mrvv-vector-bits=zvl`, running a
  `zvl1024b` image at any other VLEN produced silently wrong pixels and no trap.
- **`runtime.c` built `-fno-tree-vectorize`** (7.3 B2) — GCC autovectorised
  `bm_zero_range()`'s alignment head loop into 30 RVV instructions that executed
  *before* `.bss` was live.
- **`pause` mnemonic** replaces the hand-encoded `.insn` in all six bare-metal
  sites (7.3 B11); `riscv/boot/bm_pause.h` documents that it does **not** yield
  under TCG.

### 2.11 Driver ordering and DMA placement (7.2 A3, 7.3 A3/A4)
`riscv/boot/virtio_mmio.h` gives fenced accessors (`fence o,o` before device
stores, `fence i,ir` after device loads) replacing bare `volatile`, which is only
a *compiler* barrier. Doorbells use `fence w,o`, not `__sync_synchronize()` —
`fence rw,rw` orders memory against memory and does **not** order a memory write
before a subsequent I/O write, so the device could read a stale `avail->idx`.
`vgpu_submit()` bounces through a static 64-byte-aligned staging buffer: every
caller passed **stack locals** as `VIRTQ_DESC_F_WRITE` targets, and on a
non-coherent SoC invalidating a stack-resident response destroys the live saved
registers sharing that line — a *placement* bug no cache maintenance can fix.
`savefs.c` DMA payloads no longer sit on `malloc`'d blocks whose first line holds
the heap header. Rules in `riscv/boot/dma.h`.

**None of this class of bug can reproduce under QEMU** — TCG's memory model is
far stronger than the architecture permits. Correctness comes from the ordering
rules, not from a passing run.

---

## 3. FrameBin

### 3.1 The serial raster stage is gone — the headline result
Across **all 488 profiled frames, `raster` is 0.000 ms**. Handover 10's baseline
measured `raster = 10942 ms` serial, 87% of a lit frame. Hart balance:

```
h0=2489.6ms/440  h1=2489.9ms/444  h2=2490.9ms/468
h3=2485.7ms/464  h4=2492.6ms/434  h5=2495.0ms/456      wait=13.2ms
```

Every hart within 0.4%, straggler wait in single-digit ms. The structural problem
handover 10 diagnosed — 765 of 795 draws single-hart because `SGL_MT_MIN_VERTS`
needs 192+ verts and Doom 3 averages ~122 — is solved by binning at frame level.

**Correction to handover 10's acceptance criterion.** It says "`mt_engaged` should
approach `draws`". It does not (`draws=1003 mt_engaged=2`), and that is not a
failure: under FrameBin draws are *recorded* and replayed in one frame-level
flush, so `mt_engaged` counts flushes. The criterion was written for the per-draw
path. **Read the per-hart busy/tiles table instead.**

### 3.2 The profiler mis-attributed the frame AGAIN — fixed
| | value |
|---|---|
| engine backend wall clock (`[speeds] bk`) | 3458–3949 ms/frame |
| old `[rprof] total` | 604–843 ms |
| `mainbusy`, omitted from that total | 2163–2588 ms |

`total` was `vertex+dispatch+raster+present`. FrameBin moves raster into the
deferred flush, so `raster` reads 0 and the entire parallel section fell outside
the total. `total + mtsum` reconciles with the engine's own clock. Anyone diffing
the old `total` across FrameBin on/off would have read a ~10x speedup that does
not exist, and `rprof_stats.py` reported `total_med=342.8ms SERIAL=99.4%` for
frames that actually took ~3.5–4 s.

Fixed: `[rprof]` prints an `mt=` bucket and includes it in `total`.
`rprof_stats.py` parses `mt=` as **optional** so older logs still parse. Per-hart
busy figures are deliberately *not* summed in — that would count the same wall
time once per hart, the mirror image of the original error.

So "raster = 0" means the single-hart path is gone; it does **not** mean that
work vanished. No clean before/after frame time has been taken.

### 3.3 Pixel identity — STILL OPEN, and here is exactly where it stands
This is the most important unfinished item. Do not treat FrameBin as validated.

Attempts and what each showed:
- **Map target**: immediate mode is too slow to reach lit frames. Arm "on"
  compiled 16 shaders and rendered 492 frames in 30 min; arm "off" compiled 2 and
  produced nothing further in 50 min at 28% of one core. Aborted.
- **Menu target**: arm on 114 frames, arm off 7. Of the **7 common frames, 6 are
  bit-identical and frame 6 differs.**

**Frame 6's divergence is NOT attributable to FrameBin**, because the comparison
was not controlled for time: `com_fixedTic` was 0, so the engine advances game
time from the wall clock and the two arms run at very different speeds — they
render *different content* at the same frame index. Frames 0–5 matching is
consistent with those frames being static. I checked whether arm-off's frame-6
image appears anywhere in arm-on's 114 frames (it does not), but that test is
inconclusive: continuous animation means neither run samples the same instants.

**Fixed for the next run:** `BAREMETAL_FIXED_TIC` (defaults ON whenever
`BAREMETAL_FB_HASH` is on) passes `+set com_fixedTic 1`, making frame content a
function of frame index rather than wall clock. With that, **few frames suffice** —
the slow arm's 7 deterministic frames become a valid comparison.

A second signal worth keeping: fragment counts differ slightly even on frames
whose hashes are identical (1328324 vs 1328292). So `frags` is not a reliable
equality signal — the same lesson as handover 10 §3.1, in a new place.

If a real divergence survives determinism, prime suspects in order: the 11
`sg_framebin_barrier()` sites in `sg_texture.c` (a recorded draw stores `sgl_tex`
**pointers**, so mid-frame texture mutation retroactively changes recorded
draws), then heat-haze/mirror `_currentRender`, then the force-disabled HiZ path.

---

## 4. Idle harts park (7.1 C1/C2/C3)

- `sbi_send_ipi`/`sbi_send_ipi_hart` wrapped in `boot/sbi.c` (were not wrapped).
- `timer_arm()`/`timer_disarm()` use **Sstc `stimecmp`** when the DTB advertises
  it, else `sbi_set_timer` — which existed and was **called by nothing**.
- `bm_wait_until(deadline)` arms the timer then `wfi`. Arming first is what makes
  it safe: `wfi` resumes only on interrupts *enabled in `sie`*, so a bare `wfi`
  with `STIE` clear halts forever — the latent hang in the old `select()`/
  `usleep` loops, now converted.
- Workers spin ~4000 iterations then park with a 2 ms bounded deadline;
  `sg_mt_sched_run()` IPIs only harts that published `parked=1`. The flag is
  re-checked *after* publishing to close the observe-0/skip-IPI/bump-gen race,
  and the deadline is a lost-wakeup net, not the expected exit.

Evidence it works: an idle 6-hart run sits at ~28–72% of ONE core total, where
handover 10 measured 5 harts busy-spinning ~35 hart-seconds per 11.4 s frame.

**Latent bug left in place, worth fixing early:** `vblk_wait_used()` still
contains a **bare `wfi` with no timer armed**. If a completion interrupt is ever
lost or mis-routed it hangs forever with no diagnostic — precisely handover 10
§7.2 A1's predicted "hang mid-asset-load". `bm_wait_until()` exists to make that
pattern safe; converting that call site is a one-line change.

---

## 5. Engine-side (handover 10 §5.3)

- **`idDict::MatchPrefix` is no longer O(n²)** — `lastMatch` is always a pointer
  this function returned, i.e. into `args`, so its index is pointer arithmetic.
  It was calling `args.FindIndex(*lastMatch)`, a linear scan comparing
  `idKeyValue` **by value** (two string compares per element) to re-find a
  position it had just been handed. Falls back to the search if the pointer is
  not within `args`. Counters `idDict::matchPrefixCalls/matchPrefixScanned` added
  so the suspected 40.7 s can be confirmed rather than assumed.
- **`interactionTable` is allocated on bare metal.** `GenerateAllInteractions()`
  now skips only its *eager* loop under `BAREMETAL` and still builds the table
  (`AllocInteractionTable()`). Skipping the whole call left the table NULL
  forever — its only allocation site — so `tr_light.cpp:593`'s fast path was dead
  code and every frame walked the interaction chain, and
  `generateAllInteractionsCalled` stayed false so every `noDynamicInteractions`
  hint was ignored. **This changes which geometry is drawn — do not compare
  `[fbhash]` across it.**
- **`HasShadows()` honours `r_shadows`** — with shadows off the shadow *front
  end* still ran, pulling off-screen entities into the frame and skinning them to
  produce drawsurfs the back end discarded.
- **`SpawnMapEntities` instrumented** (35% of map load, previously unmeasured):
  per-phase (epairs copy / inhibit / spawnDef) plus a sorted per-classname table.
  Uses `Sys_MillisecondsPrecise` deliberately — with integer `Sys_Milliseconds`
  every entity floors to 0 and the sum undercounts by essentially everything.
- **NOT done: pk4 repack.** `zip -0` on `demo00.pk4` should cut 40–90 s off map
  load with zero code change, but it rewrites a game data file the user owns.

---

## 6. RVA23S64 roadmap — implemented, UNVALIDATED at runtime

RVA23S64 is by definition the **supervisor-mode** profile, which settles a
question worth not re-opening: a state-of-the-art RVA23S64 DoomOS *is* an S-mode
OS under firmware. Dropping OpenSBI for M-mode would move away from the target.
Its cost is now near zero anyway — Sstc removed the timer ecall, DBCN batches the
console, `hart_start` runs once, and §6.1 removes the last hot ecall.

### 6.1 Full AIA
Three real backends replace one lumped `AIA`:
- `PLIC`, `APLIC_DIRECT`, `APLIC_MSI`, chosen from the DTB (`msi-parent`
  presence, matching how QEMU sets `DOMAINCFG.DM`).
- **Direct mode drives the per-hart IDC block** (`idelivery`/`ithreshold`/
  `claimi` at `0x4000 + idx*32`) — never touched before, which is why
  `AIA=aplic` had never worked.
- They are separate backends because the target register's low bits mean
  **EIID in MSI mode and IPRIO in direct mode**; conflating them mis-prioritises
  silently rather than failing.
- **Correction to handover 10 §7.1 A2**, which calls the `| source` target write
  a flat bug. It is mode-conditional: verified in `riscv_aplic.c` that MSI mode
  decodes `eiid = target & 0x7ff`, so `| source` was already correct there.
  Propagating handover 10's wording would send someone to "fix" working code.
- **Zero-ecall IPI**: `os_irq_ipi()` writes an EIID straight to the target's
  IMSIC page (`base + idx*4096`); QEMU's `riscv_imsic_write()` accepts an MSI
  from any initiator. Replaces an SBI ecall on the raster wake path. EIID 1
  reserved for IPI, devices from 8. Falls back to SBI IPI when no IMSIC.
- hartid→controller-index built from `interrupts-extended` position, not assumed.

**`AIA=aplic`/`aplic-imsic` have not been run.** Every run used the default
(PLIC). The code is reasoned-correct and compiles; it has not executed.

### 6.2 PMU counters
`rdcycle`/`rdinstret` probed by execution, exposed as `bm_rdcycle()`/
`bm_rdinstret()`, reported per frame as `[rprof-instret]` with insn/fragment.
This closes a blind spot that caused real damage: every other number derives from
`rdtime`, i.e. **host** wall time, which cannot tell "the guest did less work"
from "the host was quieter" — the exact failure behind the 2.10x/0.92x pair.
Measured on the menu: ~3.9 G instructions/frame, ~3000 insn/fragment.
Full Sscofpmf sampling is not implemented; `sbi ext: PMU=yes` confirms it is
available.

### 6.3 Parallel vertex — the last serial stage
`vertex` is now the only non-scaling part of a frame (~800 ms serial vs ~2489 ms
parallel raster). It is eager because Doom 3 uses **client-side vertex arrays**
(`vbo:0` every frame) that the engine refills between draws, so the fetch cannot
simply be deferred to flush.

`[rprof-vshist]` was added to choose the fix, and the mean actively misleads:
27012 verts / ~105 draws averages 257, which looks far too small to dispatch —
but **four draws of ≥4096 verts carry 93% of all vertices**. So the cheap fix
applies: `sg_vertex_prog_simt_mt()` splits a large draw's range across the hart
pool (threshold 2048). Bit-exact by construction — each vertex depends only on
its own attributes and the uniforms, and ranges are disjoint in `pv[]`/`vb[]`.
Guarded by `sg_mt_sched_busy()` because `sg_mt_sched_run()` has one global
handshake and re-entering deadlocks.

**Caveat: that histogram is from the MENU.** The lit scene has a different shape
(91761 verts / 994 draws) and its distribution has not been measured. The
threshold may reach less there; `[rprof-vshist]` will say on the next lit run.

Also landed earlier: **index dedupe** (`sg_vertex_prog_simt_dedup`). Indexed
draws arrive one entry per triangle CORNER, measured at exactly 3.0x redundancy
(91761 corners for 30599 triangles) with no reuse exploited — the post-transform
cache at the bottom of `sg_vertex_prog_cached()` is unreachable because the SIMT
path returns first. Dedupe hashes the index list, transforms each distinct vertex
once through the unmodified SIMT batcher, then expands. Disable with
`-DBAREMETAL_VS_DEDUP=0` for an A/B.

### 6.4 Zawrs
`lr.w` + `wrs.nto` in the short spin, alongside and never replacing the `wfi`
park — QEMU's `helper_wrs_nto` is a pure permission check with **no halt**, while
`helper_wfi` sets `EXCP_HLT` and `cpu_loop_exit()`s (both verified in source).
**Default OFF** (`SGL_MT_USE_ZAWRS=0`), and that is a measurement decision: under
TCG `pause` is a nop while `lr.w`/`wrs.nto` are each a helper call, so enabling it
turns a free spin iteration into two helper dispatches across a 4000-iteration
backoff — a real slowdown on the emulator every number here comes from. Turn on
for hardware bring-up only.

---

## 7. What to do next, in order

1. **Finish the FrameBin pixel verdict** with `com_fixedTic` (§3.3). Everything
   else about FrameBin is settled; this is the one open correctness question.
2. **Validate the vertex work** the same way — dedupe and parallel-vertex are
   both claimed bit-exact by construction, which is an argument, not a
   measurement. `-DBAREMETAL_VS_DEDUP=0/1` and a large/small
   `SGL_VS_MT_MIN_VERTS` give clean A/Bs.
3. **Run `AIA=aplic` and `AIA=aplic-imsic`** (§6.1). Entirely unexercised.
4. **Convert `vblk_wait_used()`'s bare `wfi`** to `bm_wait_until()` (§4).
5. **Take a clean before/after frame time** on a quiet host with the corrected
   profiler, so FrameBin's win can finally be stated as a number.
6. Then: lit-scene `[rprof-vshist]`, `SpawnMapEntities` per-class data,
   `MatchPrefix` counters, pk4 repack.

---

## 8. Files added

```
riscv/boot/fdt.c, fdt.h          minimal FDT reader
riscv/boot/virtio_mmio.h         fenced MMIO accessors
riscv/boot/dma.h                 DMA buffer placement rules
riscv/boot/bm_pause.h            spin hint, in one place
riscv/scripts/framebin_ab.sh     FrameBin A/B harness (setsid; ARMS=on|off)
riscv/scripts/fbhash_cmp.py      frame-digest comparator (intersection-aware)
riscv/docs/handover-20.md        this file
```

New CMake options: `BAREMETAL_FRAMEBIN` (1), `BAREMETAL_FB_HASH` (0),
`BAREMETAL_VS_DEDUP` (1), `BAREMETAL_FIXED_TIC` (defaults to `FB_HASH`).

---

## 9. The recurring failure mode on this project

Handover 10 retracted a 2.35x claim because equal fragment counts were mistaken
for like-for-like work. Tonight produced four more instances of the same shape —
a number or check that looks authoritative but measures something other than its
name:

- `[rprof] total` omitted the parallel section (4–5x undercount).
- `frags` differs between arms even on bit-identical frames.
- A frame-digest FAIL that was really "one arm ran faster".
- A build check that passed because `cmp` on two *nonexistent* files also
  reports "differ".

The lesson is not "be careful". It is: **before trusting a check, confirm it can
fail.** `fbhash_cmp.py` is tested against a deliberately mutated log for exactly
this reason.
