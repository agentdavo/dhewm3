# DoomOS handover 10 — perf investigation + RISC-V modernisation

Written 2026-07-25. Branch `riscv64-baremetal`, on top of `9a6d6b6`.
Everything below is **uncommitted working-tree state**. Nothing has been committed.

---

## 0. TL;DR for whoever picks this up

1. **The documented bottleneck was wrong.** Lit frames are **~87% serial on ONE hart**,
   not "98% fragment shading". The 98% figure was measured *inside* the MT path,
   which is ~11% of the frame. See §2.
2. **FrameBin is now enabled and completely unvalidated at runtime.** It is the ~3x
   lever. It builds. It has never rendered a frame. Validate it first — §5.1.
3. **A measured 8.2% lit-frame win** is banked (11399.6ms -> 10463.1ms on *identical*
   geometry). An earlier "2.35x on load/UI frames" claim has been **WITHDRAWN** —
   see §3.1, it did not survive scrutiny. §3.
4. **Do not trust cross-run lit-frame medians from before this session.** The scenic
   camera rotated 24 deg/frame, so no two frames were comparable. Fixed via
   `bm_benchVantage`. §4 — read this before measuring anything.
5. Three subagents are researching RISC-V 2026+ gaps (AIA/APLIC/IMSIC/ACLINT/Sstc,
   RVA23 extension compliance, MMU/DTB/drivers). Their reports were not back when
   this was written. §7.

---

## 1. Build / run quickstart

```bash
# build (BAREMETAL branch of neo/CMakeLists.txt)
cmake -S neo -B build-rv && make -C build-rv dhewm3 -j8

# profiling run, deterministic vantage, 512x288, VLEN=256, 6 harts
RVV_VLEN=256 FB_WIDTH=512 FB_HEIGHT=288 RENDER_PROF=1 PIN_HARTS=1 SMP=6 \
  TIMEOUT=2400 NOBUILD=1 LOG=riscv/docs/perf/round3.log riscv/demorun.sh

# parse any serial log into per-regime stats
python3 riscv/scripts/rprof_stats.py riscv/docs/perf/round3.log

# kernel-vs-VM bit-exactness oracle (THE regression gate — see §6)
cd riscv/softgles2 && make iakgl4esrv RVV_VLEN=256 -j6
cd /home/djs/dhewm3 && RVV_VLEN=256 SMP=2 PIN_HARTS=1 TIMEOUT=120 \
  riscv/run.sh riscv/softgles2/build/iakern_gl4es_validate.elf
```

Map load is 8–25 min under TCG. A lit frame is ~10 s. Budget ~40 min per run.

---

## 2. The corrected performance picture

`sgl_prof_raster` accumulates **only when MT dispatch declined the draw** — in
`sg_draw.c` the MT path `return`s before that timer starts. So `raster` is pure
single-hart time, and `dispatch` is the (parallel) MT path. This was mislabelled in
every prior analysis.

Committed baseline (`demorun-tcg-profile.log`), lit frame median:

| bucket | ms | note |
|---|---|---|
| total | 12566 | |
| vertex | 403 | serial |
| **raster** | **10942** | **serial, 1 hart** |
| dispatch | 1374 | MT, 6 harts |
| **SERIAL total** | **11327** | **87.4% of frame** |

One frame showed `draws=795, mt_engaged=30` — **765 of 795 draws on one hart**,
~10% hart utilisation. Cause is structural: `sg_mt.c` requires 192+ verts/draw
(`SGL_MT_MIN_VERTS`) and Doom 3 averages ~122. Per-draw MT cannot serve a
many-small-draws workload. Frame-level binning (FrameBin) is the architectural fix.

`rprof_stats.py` now prints `SERIAL (vertex+raster, 1 hart) = N% of frame`. Watch it.

---

## 3. Measured results

### 3.1 WITHDRAWN: the load/UI "2.35x"

An earlier version of this document claimed a 2.35x win on load/UI frames
(254.9ms -> 108.5ms) on the grounds that `frags=112554` was identical across runs.
**That reasoning was wrong and the claim is retracted.** Three runs of the same
binary family give, all at `frags_med=112554`:

| run | load/UI total_med |
|---|---|
| committed baseline | 254.9 ms |
| fix-round1 | 108.5 ms |
| fix-round2 (*more* fixes) | **804.7 ms** |

Equal fragment count does **not** make these samples comparable. A UI frame's cost
is dominated by whatever else the run is doing while it is sampled — background map
load, heap state, allocator fragmentation — not by its own 112k fragments. The
three numbers straddle the "before" value in both directions, which is the tell.

**Lesson for the next session: `frags` equality is not sufficient evidence of
like-for-like work.** Match on the specific frame's full signature
(`draws`+`tris`+`frags`) AND on position in the boot sequence, as the lit-frame
comparison below does. If you want a trustworthy UI-frame number, measure it at a
pinned point (e.g. always frame N after map load completes), not by taking a median
over whatever UI frames a run happened to emit.

**Lit frame** (identical geometry: `draws=19 tris=4238 frags=523909`):

| | baseline | round 2 | delta |
|---|---|---|---|
| total | 11399.6 ms | **10463.1 ms** | **−8.2%** |
| raster (serial) | 6432.6 ms | 5533.5 ms | −14.0% |
| dispatch | 4490.2 ms | 4409.5 ms | −1.8% |
| vertex | 476.4 ms | 519.8 ms | +9.1% |
| mt_engaged | 9/19 | 9/19 | unchanged |

**Interaction kernel microbench**: 1.19x -> **1.62x** vs its VM oracle, bit-exact.

**Round 2 did not complete.** Its log ends with `terminating on signal 15 from pid
... (bash)` — a wait-loop hit its own 10-minute timeout and the SIGTERM propagated
to QEMU. It yielded exactly one lit frame (the one above) out of a planned 40-minute
run. If you re-run, detach QEMU from any polling wrapper's process group, or poll
with a loop that cannot be killed as a group with it.

Caveats, stated plainly:
- The 8.2% lands in `raster`, consistent with the kernel wins (Nx1 sampler, memo,
  q==1 divide). Attribution to any single change is not established.
- **The depth-fill colour mask is unattributed.** `frags` is identical because
  `sg_raster.c` increments the fragment counter *before* the `cmask==0` gate — so
  the counter cannot distinguish "mask worked" from "mask never engaged". To prove
  it, surface `sgl_dbg_frag_cmask0` in the `[rprof]` line.
- `mt_engaged` did not move, so the MT gate changes have not paid off yet.

---

## 4. MEASUREMENT METHODOLOGY — read before benchmarking

Three traps were hit this session. All are now fixed; do not re-introduce them.

**4.1 The camera used to move every frame.** `bm_scenicWarp` did
`yaw = (frame * 24) % 360` and re-issued `setviewpos`, which teleports the player
and rebuilds every entity interaction. Lit frames varied 25888–68271 tris and
492k–960k frags. Cross-run medians were meaningless.
Fixed: `bm_benchVantage` (sys_dos.cpp) places the camera **once** at frame 8 and
never touches it again, and suppresses the per-frame `getviewpos`. It **defaults to
1 whenever `BM_RENDER_PROF` is set**, so a profiling build cannot silently disagree
with itself. `bm_benchVantageYaw` sets the angle (default 96).

**4.2 The frame classifier silently dropped frames.** `rprof_stats.py` used
`draws>50` for "lit"; a real 11.4 s frame had `draws=19` and fell between buckets,
vanishing from the stats. Now classified on `tris >= 100`, the buckets partition,
and it prints a loud warning if any frame matches no bucket.

**4.3 Timings were divided by a hardcoded 10 MHz.** `sg_prof.c` had 19 hardcoded
`/10000` divisions. Correct on QEMU virt; **silently wrong by 10x or 2.4x on real
RVA23 silicon** clocking mtime at 1 MHz or 24 MHz. Now: `timer_init_from_dtb()`
in `runtime.c` walks the FDT for `timebase-frequency`, `timer_ns_from_ticks()` does
an overflow-safe exact conversion (the naive `ticks*1e9` wraps uint64 past ~30 min
of accumulated ticks), output is in microseconds, and `[rprof-timebase] N Hz` is
logged so the divisor is never an unstated assumption.

**Corollary still open:** ~15 other sites still use the compile-time `TIMEBASE_HZ`
(`riscv/smp/sched.c:171`, `riscv/smp/kmain.c:210`, `dos_syscalls.c` usleep/
gettimeofday/clock, `newlib_syscalls.c`, several `*_validate.c`). On non-10MHz
hardware those mis-scale: hart-bringup budgets shorten, sleeps run short, audio
paces wrong. **Convert them to `timer_hz()` before running on anything but QEMU
virt.**

---

## 5. Next actions, in priority order

### 5.1 Validate FrameBin — DO THIS FIRST
`sgl_framebin_enabled` now defaults to **1** (`SGL_FRAMEBIN_DEFAULT=0` reverts).
It builds and has **never been run**.

What was done: `sg_framebin_barrier()` added and called from **11 entry points** in
`sg_texture.c` (glTexImage2D, glTexSubImage2D, glCopyTexImage2D,
glCopyTexSubImage2D, glReadPixels, glGenerateMipmap, glDeleteTextures, 4x
glTexParameter*). Rationale: a recorded draw stores `sgl_tex` **pointers**, not
texel copies, so any mid-frame texture mutation retroactively changes draws already
recorded; and glReadPixels/glCopyTex* observe a framebuffer those draws have not
written yet. Doom 3 does exactly this for `_currentRender` (heat haze, mirrors).

HiZ is **force-disabled** on the deferral path (`sg_draw.c`). Reason: `glClear`
seeds the HiZ pyramid immediately while the clear itself only replays at flush,
which can leave pyramid entries *nearer* than the real depth buffer and cull valid
geometry. Stale-far would be harmlessly conservative; stale-near is corruption.
Revisit by replaying clears into the pyramid at flush.

Acceptance criteria:
1. `mt_engaged` should approach `draws` (was 9/19), and `raster` should collapse
   toward zero. That is the ~3x.
2. **Pixel-for-pixel frame comparison against immediate mode.** FrameBin replays
   per-tile refs in submission order specifically to be pixel-identical, so any
   difference is a bug, not a tradeoff. Build both (`SGL_FRAMEBIN_DEFAULT=0/1`),
   capture the same frame, diff.
3. Watch for heat-haze/mirror surfaces specifically — that is where the barriers
   matter.

### 5.2 Vertex path — measure, then choose
A `gather=` sub-timer was just added to `[rprof-vstime]`, splitting attribute
gather from transform. They were previously summed, so neither could be attributed.
Run once, then:
- `gather` dominant -> strided load. `sg_vertex.c:298` fetches through an
  **indirect callback** doing a bounds check and a `memcpy` per float per lane —
  64 indirect calls + ~256 memcpys per batch, unvectorisable by any compiler. Data
  is client-side AoS `idDrawVert` at 60-byte stride (engine reports `vbo:0` every
  frame) = one `__riscv_vlse32_v_f32m2(base + c*4, 60, vl)` per component.
- `simt` dominant -> index dedupe. `[rprof-vs]` shows `96288 verts` for
  `tris=32108` — exactly 3x, i.e. **zero index reuse exploited**. A post-transform
  cache exists at `sg_vertex.c:446` but is unreachable because the SIMT path
  returns first. Typical mesh reuse ~6 tris/vertex.

### 5.3 Other high-value items from the audits (not yet done)
- **pk4 repack, zero code change.** `demo00.pk4` is 11145/12234 entries DEFLATE,
  481MB -> 1127MB uncompressed, inflated single-threaded under TCG. `zip -0`
  should cut 40–90s off map load net of extra I/O.
- **`idDict::MatchPrefix` is O(n^2)** (`neo/idlib/Dict.cpp:527-551`) — the
  continuation calls `args.FindIndex(*lastMatch)` to re-find a position it was just
  handed. Suspected the unattributed **40.7 s** inside `media-cache` during map
  load (sub-timers account for only 0.9 s of 41.6 s). Instrument to confirm, then
  derive `start` by pointer arithmetic. One line.
- **69.5 s of `SpawnMapEntities` is unattributed** (35% of map load). Nothing
  measures it. Add per-entity-class timing before proposing any fix.
- **`interactionTable` is never allocated** — `Session.cpp:1760` skips
  `GenerateAllInteractions()` under `BAREMETAL`, which is the only allocation site,
  so `tr_light.cpp:593`'s fast path is dead forever and every frame walks the chain.
  Also leaves `generateAllInteractionsCalled` false, so `noDynamicInteractions`
  hints are ignored.
- **`HasShadows()` ignores `r_shadows`** (`Interaction.cpp:658`), so with shadows
  off the shadow *front-end* still runs: off-screen entities are pulled into the
  frame and skinned to produce zero drawsurfs.
- **MT admission**: `active` gate rejected the most work (14610 tris/frame from 85
  draws). Already relaxed (fixed 24 -> one tile per hart, adaptive grab size) but
  `mt_engaged` has not moved — recheck after FrameBin, which may make it moot.
  `SGL_MT_MIN_VERTS` was deliberately **not** lowered; see the comment in
  `sg_mt_internal.h` for why (admitting ~800 tiny draws/frame would cost more in
  per-draw binning than it saves).

---

## 6. Regression gates — RUN THESE

**The bit-exactness oracle caught a real bug I introduced this session.** Do not
skip it.

```bash
cd riscv/softgles2 && make clean && make iakgl4esrv RVV_VLEN=256 -j6
cd /home/djs/dhewm3 && RVV_VLEN=256 SMP=2 PIN_HARTS=1 TIMEOUT=120 \
  riscv/run.sh riscv/softgles2/build/iakern_gl4es_validate.elf
```
Expect `[gik-vs] maxdiff=0 bitmiss=0 OK` **and** `[gik] ... maxdiff=0 bitmiss=0
OK(bit-exact)`. Related gates: `iakernrun`, `ovkrun`, `atestrv`.

Note `-ffp-contract=fast` was added to `bm_sgl` and (bare-metal) `idlib`; both
sides of the oracle build with the same flag so they should stay consistent, but
re-run after touching flags.

### The bug, as a cautionary tale
The kernel name-lookup memo originally keyed on the **name pointer**, asserting in
its own comment that "every caller passes a string literal". False: the interaction
VS kernel built its 14 uniform names into one reused `char n[40]` stack buffer, so
all 14 aliased to a single memo entry (`bitmiss=272`), and storing that pointer also
left a dangling reference to popped stack. Fixed by storing a **copy** of the name,
and by replacing the runtime string construction with a static literal table.
Verified against pristine sources that the failure was mine before fixing.

---

## 7. RISC-V 2026+ modernisation (in flight)

Three subagents were dispatched to audit gaps for state-of-the-art RISC-V. Reports
were not back when this was written — **check for them, they may be the most
valuable input to the next session.**

### 7.1 AIA / interrupts / timers — REPORT IN, verified against QEMU 11 source

Ordered as the audit recommends: **A1 -> A5 -> A4 -> C3 + C2 -> C1 -> A2 + A3 + B1
-> B2/B3/B4 (one-liners, anytime)**.

**A1. The trap vector destroys vector and FP state. Blocks everything else here.**
`start.S:75-145` saves x1-x31 only — no `vstart/vtype/vl/vcsr`, no `v0-v31`, no
`f0-f31/fcsr`. `irq.c:167` sets `sstatus.SIE` on the boot hart (the only `sstatus`
write in the tree) and that hart is *also* raster worker 0. **Confirmed live, not
theoretical**: `objdump` of `riscv/build/payload.elf` shows `memset` at `0x80200408`
is RVV-compiled (`vsetvli`/`vmv.v.x`/`vse8.v`). GCC 16 at `-O2` autovectorises
freely, so any C on the IRQ path can clobber `vtype`/`vl`/`v0-31` under an
interrupted RVV raster loop -> silently wrong pixels or a `vill` fault.
Fix: branch on `sstatus.VS`/`FS` in `trap_vector`, save/restore to a per-hart
scratch (32 x `vlenb` = 4 KiB at VLEN=1024), restore `vstart` **last**. Cheaper
interim: build IRQ-path TUs with a `-march` without `v`, plus
`-fno-builtin-memset/-memcpy`.

**A2. `AIA=aplic` has never worked.** QEMU `virt.c:803-804` only emits the PLIC FDT
node when `aia_type == VIRT_AIA_TYPE_NONE`. So `AIA=aplic` yields a DTB with an
APLIC and **no PLIC**; both `fdt_contains` branches miss, `g_backend` stays
`IRQ_BACKEND_NONE`, and virtio-blk silently degrades to spin-polling
(`dos_virtio_blk.c:200-207`). Direct mode is also unimplemented: QEMU's
`riscv_aplic.c:492` defines `target[]` as `hart_idx<<18 | IPRIO`, but `irq.c:194-195`
writes `| source` (lands in IPRIO, not an EIID), and the per-hart IDC block
(`idelivery`/`ithreshold`/`claimi`) is never touched. Needs three backends:
`PLIC`, `APLIC_DIRECT`, `APLIC_MSI`.

**A3.** `fdt_contains` is a substring scan; MMIO bases (`0x0c000000`/`0x0d000000`)
and `context = hartid*2 + 1` are hardcoded QEMU-virt conventions. Promote the FDT
walker already written at `runtime.c:125-165` into a reusable API
(`fdt_next_node`/`fdt_getprop`/`fdt_node_is_compatible`/`fdt_reg`). Prerequisite
for B1, C5, C6.

**A4/A5.** `os_irq_init_from_dtb` early-returns on second call, so IMSIC
`eidelivery`/`eithreshold` and the `sie`/`sstatus` writes are **boot-hart only** —
needs an `os_irq_init_local()` called from `dos_hart_entry.S`. And worker harts
install the *fatal* `trap_vector` (`trap.c:39-47` prints and powers off), so the
first IPI after enabling SSIE panics the machine. A recoverable vector already
exists and works in the test-only `riscv/smp/sched_secondary.S:88-124`; lift it
into `boot/`.

**A6.** No SBI base-extension probing (`sbi_probe_extension`); HSM/TIME/SRST/IPI are
assumed. Firmware without HSM degrades to single-hart with no diagnostic. Bundled
firmware verified as OpenSBI **v1.7**.

**B1.** Hart discovery assumes dense hartids 0..7 (`dos_thread.c:91-96`,
`DOS_MAX_HARTS 8`). Real platforms use sparse/non-zero-based hartids; QEMU itself
sets per-socket `base_hartid`. Enumerate `/cpus/cpu@N` after A3.

**B2. `TIMEBASE_HZ` is still hardcoded in more places than §4 listed** — including
two that matter most:
- `runtime.c:169` `timer_ms()` divides by `TIMEBASE_HZ` **even though `g_timer_hz`
  is set 60 lines above**. One-line fix, and everything calls it.
- `sys_baremetal.cpp:99` `bm_timebase_hz = TIMEBASE_HZ`, never refreshed from
  `timer_hz()` — so **every Doom 3 frame time and `Sys_MillisecondsPrecise` is
  wrong by the timebase ratio on hardware.**
Plus `dos_syscalls.c:462,472-473,495,524`, `dos/dos_syscalls.c:46-47,60`,
`smp/sched.c:171`, `smp/kmain.c:210`, `sys_dos.cpp:69,75,438-439`, and several tests.

**B3.** `sstatus.FS`/`VS` are never initialised in either entry path — works only
because OpenSBI leaves them on. A hart may legally enter S-mode with `VS=Off`,
making the first `vsetvli` in `rv_detect_platform_caps` an illegal-instruction
panic. One line: `csrs sstatus, (3<<13)|(3<<9)`.

**B4.** `OS_IRQ_MAX_SOURCES 128` exceeds QEMU virt's `VIRT_IRQCHIP_NUM_SOURCES 96`;
routing source >=96 writes past the implemented arrays. Also `irq.c:195` uses
`boot_hartid` as the APLIC hart index, which QEMU treats as index *within socket*.

**C1. No `wfi` — the biggest perf item, and the mechanism is confirmed.**
QEMU 11 `helper_wfi` (`op_helper.c:555-573`) sets `halted=1`/`EXCP_HLT` and
`cpu_loop_exit()`, which under MTTCG genuinely parks the vCPU thread and returns
the host core. **Critically, `helper_wrs_nto` (`op_helper.c:575-584`) is a pure
permission check with no halt — so Zawrs does NOT yield under TCG**, and
Zihintpause decodes to a nop. `wfi` is the only instruction that yields. With
`PIN_HARTS=1`, every idle worker pegs its pinned host CPU between draws, stealing
host time from harts still rasterising. Port the working design from
`smp/sched.c:230-243` + `sched_secondary.S`; keep the bounded `rdtime` deadline as
the lost-wakeup net. Depends on A1 and A5.

**C2.** `sbi_send_ipi` is not wrapped in `boot/sbi.c` at all. Two verified faster
wakes than an SBI IPI: **IMSIC MSI** (single 32-bit store to
`0x28000000 + hartidx*4096` with `aia-guests=0`; `riscv_imsic.c:285-330` accepts it
from any initiator — **zero ecalls**; reserve EIID 1 for IPI, start devices at 8),
or **ACLINT SSWI** (`*(uint32_t*)(0x2F00000 + hart*4) = 1`, but needs
`-machine virt,aclint=on`, verified opt-in and TCG-only).

**C3. Sstc is unused and there is no timer interrupt in the system at all.**
`sbi_set_timer` exists (`sbi.c:54`) and is **called by nothing** — only two TODO
comments reference it. QEMU's rva23s64 profile sets `ext_sstc = true`
(`cpu.c:3295`) and Sstc is mandatory in RVA23S64, so `csrw stimecmp` replaces the
whole S->M->MMIO->S round trip. Add `timer_arm(deadline)` to `runtime.c` that uses
`stimecmp` when present (detect via the cpu node's `riscv,isa-extensions`) else
`sbi_set_timer`.

**C4.** Busy-wait sleeps (`sys_baremetal.cpp:118-132`, `dos_syscalls.c:489-500`,
`:521-527`) become arm+`wfi` after C3. **Latent hang**: `dos/dos_syscalls.c:62`
does `while (timer_ticks() < end) asm volatile("wfi");` with `sie.STIE` clear and
no timer armed — on any hart without SEIE that halts permanently. Check which copy
is in the game payload before touching.

**C5.** SBI PMU + Sscofpmf (enabled in QEMU's profile) would give per-hart
`instret`/`cycle`, separating guest instructions from host wall time in a way
`rdtime` cannot. Only worth it if shader-VM work continues.

**C6. Explicitly NOT worth doing** (so nobody re-proposes them): SBI SUSP/CPPC (no
DVFS under TCG); Zawrs *instead of* `wfi` (no-op under TCG — only as a
future-silicon hint alongside it); DBCN (already correct with a legacy fallback).
### 7.3 RVA23 extension compliance — REPORT IN

**Two things in my briefing were WRONG; do not propagate them:**
- The **SDL3 "medlow" worry is unfounded.** `readelf -r` shows `R_RISCV_PCREL_HI20`
  (medany) — the toolchain **defaults to `-mcmodel=medany`**, so the missing flag is
  harmless. SDL3 is a perf gap (no B/Zicond/Zcb/Zfa) and an `-fno-common`/
  `-fno-stack-protector` hygiene gap, **not** a link/crash gap.
- The **QEMU `-cpu` string in `run.sh:23` is complete.** Everything `BM_MARCH` adds
  is covered by the profile or by superset extensions. **Nothing the toolchain emits
  would trap.** Confirmed positively: the shipped binary already executes 4860
  `fli.s` (Zfa), 1575 `czero.eqz` (Zicond), 7 `vwsll.vi` (Zvbb), 5 `cbo.zero`
  (Zicboz), 13 `pause`.

**A1. `zvl1024b` makes the image unrunnable on real RVA23 silicon — biggest item.**
Zvl1024b is *optional*; RVA23 mandates only VLEN>=128. Shipping RVA23-class parts
are VLEN=256 (SpacemiT K1/M1) or 512 (SiFive X280/XM). A `zvl1024b` binary is
architecturally illegal on all of them — **and because of `-mrvv-vector-bits=zvl`,
even VLEN *larger* than 1024 gives wrong answers**, since fixed-length types assume
exactly 1024. `rv_detect_platform_caps()` reads `vlenb` and **never compares it to
anything**, and `run.sh`'s `RVV_VLEN` is an independent env var — so
`RVV_VLEN=256 ./run.sh` against a `zvl1024b` build produces **silently wrong pixels,
no trap**. Fix: panic in `_cstart` if `vlenb*8 != __riscv_v_min_vlen`; derive
`RVV_VLEN` from the ELF's `Tag_RISCV_arch`; offer a VLA (no `-mrvv-vector-bits`)
mode at a measured 8.8-22.6% cost.

**A2.** `cbo.zero`/`fli.s`/`vwsll` are emitted unconditionally with **no ISA or
firmware validation anywhere**. `cbo.zero` traps as illegal in S-mode unless M-mode
set `menvcfg.CBZE` (gate verified in QEMU `op_helper.c:186`). It works only because
the bundled OpenSBI happens to set it; other firmware turns a rasteriser inner loop
into a fatal trap with nothing but `scause=2`.

**A3. DMA buffers are on the STACK, and the buffer *placement* is the bug** — not
the missing cache ops. `dos_virtio_gpu.c:355` etc. declare device-writable response
structs as **stack locals** programmed with `VIRTQ_DESC_F_WRITE`; `savefs.c` DMAs to
`malloc`'d blocks whose first cache line holds the 16-byte in-band heap header;
`dos_virtio_input.c:72` packs **8 independent device-writable descriptors per 64-byte
line**; the scanout is ordinary cached heap at 128-byte alignment. On a non-coherent
SoC a `cbo.inval` on a stack-resident response destroys live saved registers in the
same line. **No amount of cache maintenance fixes this** — the objects must move to a
64-byte-aligned, size-rounded DMA arena first. That mechanical half is the
load-bearing one.

**A4.** Corroborates §7.2 A3 and sharpens it: the barrier in use is
`__sync_synchronize()` = `fence rw,rw`, which orders memory-vs-memory but **not**
prior memory writes before a subsequent **I/O** write. Needs `fence w,ow` at
doorbells. Also found two divergences among the copy-pasted drivers:
`dos_virtio_snd.c:296-299` is missing the post-reset fence its four siblings have,
and `:373` reads `used->idx` before any barrier.

**B2. The `bm_zero_range()` I wrote today miscompiles.** GCC turns the head loop
`while (p<end && ((uintptr_t)p & 7u))` into a **30-instruction RVV early-break loop**
(`vid.v`, seven `vadd.vv`, `vand`/`vmseq`/`vmor.mm`, `vcpop.m`) computing a per-byte
alignment predicate over 128 bytes before falling through to scalar. Harmless at
runtime (`__bss_start` is aligned, so it exits on the first `vcpop`) but pointless —
and it means **`_cstart` executes vector instructions before `.bss` is live**, which
works only because OpenSBI left `sstatus.VS` at Initial. Fix: `-fno-tree-vectorize`
on `runtime.c` (or a `#pragma` on the function), and use `cbo.zero` per 64B for the
body under `#ifdef __riscv_zicboz`. Do **not** hardcode 64 unless `Zic64b` is
asserted by A2's probe.

**B4. Zacas/Zabha are NOT on the QEMU CPU** (`cpu.c:1252,1254`, default **off**), so
adding them to `-march` without adding `zacas=true,zabha=true` to `run.sh:23` traps
immediately. **Both sides must move atomically.** Worth a `run.sh` assertion that
parses `Tag_RISCV_arch` from the ELF and validates it against the `-cpu` string.
Real LR/SC retry loops that would benefit: `smp/sched.c:211` (work-steal CAS, the hot
one), `dos_syscalls.c:188,224,231`.

**B5.** Zicbop `prefetch.r/w` already assembles under the current `-march` and is
unused. Zero cost under TCG, pure 2026-hardware prep for the texture sampler and
framebin tile walk.

**B9.** Profiler is `rdtime`-only. `Zihpm` (`rdcycle`/`rdinstret`) would separate
guest instructions from host wall time — the thing TCG measurements structurally
cannot tell you. Route through A2's probe since these trap if `mcounteren` is clear.

**B10.** QEMU's `RVA23S64` profile omits `ext_smstateen` (defaults false) though
Ssstateen is mandatory — a real QEMU-vs-conformant-hardware divergence, but **no
impact on DoomOS today** and not worth acting on.

**B11.** `pause` is hand-encoded as `.insn` in 8 places despite `zihintpause` being
in `-march`; two other spin loops use a bare `nop`. Consolidate into one `bm_pause()`
header. Vector-crypto (Zvkng/Zvksg/Zbkb/Zbkc/Zbkx) is dead weight — Doom 3 hashes
MD4/MD5, which none of it accelerates.

### 7.4 CONFLICT between two audits — resolve before acting

**Zawrs.** §7.3 B1 recommends replacing the raster spin (`sg_mt_sched.c:37`) with
`lr.w` + `wrs.nto`. §7.1 C1 verified that **QEMU's `helper_wrs_nto`
(`op_helper.c:575-584`) is a pure permission check with no halt** — it does *not*
yield under TCG, whereas `helper_wfi` genuinely parks the vCPU thread.

Both are right in their own domain: `wrs.nto` is the correct instruction on real
multicore silicon (retires when the reservation breaks, freeing issue slots for
sibling harts), and is worthless for the QEMU-hosted measurements this project
currently runs on. **Do not adopt `wrs.nto` as a substitute for `wfi`.** If adopted
at all, it belongs *alongside* an IPI+`wfi` path (§7.1 C1) as a future-silicon hint,
and it will show zero improvement in any TCG benchmark — so do not expect the
measurement to validate it.
### 7.2 MMU / DTB / drivers / boot — REPORT IN, verified against QEMU 11 source

**Would not boot or would silently corrupt on modern hardware**

- **A1. PLIC context assumes hartid base 0 and a single socket.** `irq.c:158`
  `context = boot_hartid*2 + 1` with `PLIC_BASE` fixed. QEMU `virt.c:1284` numbers
  contexts **relative to `base_hartid` within a socket**, and each socket gets its
  **own PLIC** at `base + socket*size`. Correct expression is
  `((boot_hartid - socket_base) * 2 + 1)`. On `-smp 8,sockets=2`, or any SoC whose
  application harts do not start at 0 (management core at hartid 0 is a common
  layout), DoomOS programs another hart's context. Failure mode is not clean:
  `vblk_wait_used()` takes the `g_irq_routed` path, enables a source nobody
  delivers, and spins in `wfi` forever -> **hang mid-asset-load, indistinguishable
  from the known flaky-boot symptom.** Fix by walking `interrupts-extended` for the
  entry with `cause == 9` belonging to this hart (what Linux's `plic_init` does).
- **A2. RAM base/size are link-time constants; ~2.1 GiB of a 4 GiB machine is
  unreachable.** `engine.ld:36` `LENGTH = 1920M`. Nothing reads `/memory`. Two
  consequences: (i) on a platform with less RAM or a different DRAM base, `_sbrk`
  hands out nonexistent addresses and the engine dies inside a map load with no
  diagnostic; (ii) at `MEM=4G`, RAM runs to `0x180000000` but the heap stops at
  `0xF8200000` — **2176 MiB permanently unusable.** The stated `-mcmodel=medany`
  ±2 GiB justification applies only to *symbol* addressing, not to a runtime-
  discovered arena reached through an ordinary pointer. Caution: QEMU places the
  DTB near the top of DRAM (`boot.c:359`), so any high arena must clamp below
  `boot_dtb`.
- **A3. Driver MMIO accessors emit no I/O fences** — `volatile` is only a compiler
  barrier. Copy-pasted into **five** drivers (blk, gpu, input, snd, vcon). Real
  reorder hazards: `QUEUE_SEL` then read `QUEUE_NUM_MAX`; `DEVICE_FEATURES_SEL`
  then read `DEVICE_FEATURES` (could conclude `VERSION_1` is absent). The
  `avail->idx` -> `QUEUE_NOTIFY` path *is* protected by `__sync_synchronize()`; the
  others are not. **TCG's memory model is far stronger than the architecture
  permits, so this class of bug can never reproduce under QEMU.** Fix: shared
  `virtio_mmio.h` with `fence ow,ow` before stores / `fence i,ir` after loads, and
  delete the five copies (they have already drifted).
- **A5.** `timer_ms()` (`runtime.c:169`) still divides by compile-time
  `TIMEBASE_HZ` while `timer_ns_from_ticks()` uses `g_timer_hz` — **two clocks that
  disagree is worse than one wrong clock.** Corroborates §7.1 B2. One line.

**Robustness**

- **B2. The 8-hart cap is the binding constraint on the project's actual
  bottleneck.** QEMU 11 `virt` allows **512** harts (`virt.h:28`). Indexing is
  already bounds-checked throughout, so raising it is safe; the work is making
  `dos_thread_create` iterate a DTB-discovered hartid **list** instead of `[0,8)`.
- Per-hart ISA is sampled on the boot hart and assumed homogeneous. Heterogeneous
  clusters are the 2026 norm, and `engine_kmain.c:47` already hard-panics on a VLEN
  mismatch — so a worker on a non-V hart is a guaranteed failure, not a degradation.
- **B5. There is no PCIe/ECAM support at all**, so `virtio-net-device` and
  `virtio-rng-pci` in `run.sh` are dead weight — and the net device consumes one of
  the 8 MMIO slots for nothing. 7 of 8 slots are in use; adding one more device
  silently pushes a driver's `scan_slots()` to find nothing. A boot-time slot census
  print would make that visible.
- **B6.** Worker stacks are `stacks[8][256KB]` contiguous with no red zone; the main
  8 MiB stack grows straight into `__heap_end`. dhewm3 uses `_alloca` heavily — the
  exact pattern that overflows without faulting.

**Performance / design — and a recommendation against reflex**

- **C1. Keep Bare mode. Do not add paging.** The agent's reasoning, which I find
  sound: there is one address space and one application; paging would add page-table
  walks to the most TLB-hostile workload in the system. **Superpages are damage
  control, not an optimisation** — in Bare mode there is no translation to miss, so
  gigapage-mapping DRAM gets *close* to Bare-mode performance, never better.
  "Superpages for the texture arena" as a speedup has the sign backwards. And **PMP
  is unavailable**: PMP/Smepmp are M-mode CSRs owned by OpenSBI; S-mode cannot
  configure them.
  What *is* indefensible is B6's silent corruption. Fix it **without** an MMU: pad
  `stacks[]` with a 4 KiB poisoned red zone, check it in `dos_thread_join()` and the
  profiler tick. ~40 lines, zero runtime cost. Only if real protection is wanted
  later: Sv39 identity map from **1 GiB gigapages** (4-5 PTEs, permanently TLB
  resident), splitting only the gigapage containing worker stacks down to 4 KiB.
  That is the sole shape in which turning on the MMU is defensible.

**Correction to an earlier audit finding.** A previous agent recommended adding
`la gp, __global_pointer$` to the entry paths. This audit found `gp` is currently
safe *precisely because* nothing references `__global_pointer$`, so the linker never
emits the relaxation (confirmed: zero `(gp)` operands in the payload disassembly).
**Adding a conventional `la gp` without `.option norelax` around it would be the
thing that breaks it.** If done at all, it must be
`.option push; .option norelax; la gp, __global_pointer$; .option pop`.

Also checked and found NOT to be defects (saves the next reader the trip):
`vgpu_ensure_scanout()` running before the virtio handshake only allocates a buffer,
so the ordering is fine.

Known state going in: `run.sh` accepts `AIA=aplic|aplic-imsic`; `irq.c:150` picks
APLIC+IMSIC vs PLIC by DTB **substring match** (cannot read property values);
`satp` is never written; the trap handler saves **no FP and no vector state**
(`start.S`) while the boot hart both takes interrupts and runs RVV raster loops —
that is a real corruption risk, flagged as C2 in the R1 audit; there is **no
`wfi`** anywhere (5 harts busy-spin ~35 hart-seconds per 11.4 s frame);
`sbi_send_ipi` is not wrapped.

### QEMU timebase experiment (discussed, not done)
`RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ = 10000000` at
`qemu-11.0.1/include/hw/intc/riscv_aclint.h:78`, used in 4 places in `virt.c`
including both the DTB emission (`:750`) and the ACLINT instantiation
(`:1584/1597/1611`) — so patching it keeps hardware and DTB consistent.

Running at 100 MHz would be a clean **validation of the DTB timebase parser**:
parser working -> frame times unchanged (ticks 10x faster, divisor 10x larger);
parser broken -> every reported time inflates 10x. But it **will** break the ~15
compile-time `TIMEBASE_HZ` sites in §4 first, so convert those before trying it.

Cheaper parser test needing no QEMU rebuild: temporarily set the fallback in
`runtime.c` to a sentinel (e.g. 12345678). `[rprof-timebase]` printing 10000000
proves the DTB read worked; printing the sentinel proves it did not. Currently the
fallback and QEMU's value are both 10 MHz, so **the parser is unverified.**

---

## 8. Complete change inventory (uncommitted)

**Renderer / perf**
- `neo/renderer/draw_common.cpp` — `RB_DepthFillColorMask()`; depth-fill pass
  colour-masked so softgles2's `cmask==0` fast path skips the shader. Guarded on
  the clear colour actually being **black** (`r_clear "1"` is purple, and the light
  passes blend additively onto whatever the fill leaves — masking then would tint
  the world). Also guarded on `numClipPlanes` and `r_showOverDraw`.
- `neo/renderer/RenderSystem_init.cpp`, `tr_local.h` — `r_useDepthOnlyFill` (default 1).
- `riscv/dos/dos_present.c` — **`sg_frame_reset()` at frame end.** Its only caller
  was `sgl_present()`, which the engine never uses, so the per-frame bump arena only
  ever grew (`[rprof-heap]` 191MB -> 670MB over 66 frames, ~7MB/frame).
- `riscv/softgles2/src/sg_mt_sched.c` — MT reject histogram; `MTR_ACTIVE` gate
  relaxed from fixed 24 tiles to one tile per hart; adaptive tile grab size
  (~2 claims per hart) so small draws actually spread.
- `riscv/softgles2/src/sg_mt_internal.h` — `MTR_*` enum, `SGL_MT_MIN_REF_SPAN_X10`
  18->10, and a comment on why `SGL_MT_MIN_VERTS` is deliberately unchanged.
- `riscv/softgles2/src/sg_mt.c` — reject counters at the pre-binning early-outs.
- `riscv/softgles2/src/sg_prof.c/.h` — `[rprof-mtrej]` line; timebase-derived ns;
  microsecond output; `[rprof-timebase]`; `gather=` sub-timer.
- `riscv/softgles2/src/sg_framebin.c/.h` — `sg_framebin_barrier()`; default ON.
- `riscv/softgles2/src/sg_texture.c` — 11 barrier call sites.
- `riscv/softgles2/src/sg_draw.c` — HiZ disabled on the deferral path.
- `riscv/softgles2/src/sg_vertex.c` — gather sub-timer.

**Kernels (all verified bit-exact)**
- `riscv/softgles2/src/sg_kernels.c` — name-lookup memo (stores copies);
  static literal table for VS uniform names; `q==1` projective-divide skip.
- `riscv/softgles2/src/sg_math.h` — Nx1/1xN degenerate-axis sampler fast path
  (`_specularTable` is 256x1, sampled on every interaction fragment; both rows
  clamp to 0 so half the gathers and lerps were provably dead).
- `riscv/softgles2/src/glsl_internal.h`, `glsl.c` — `kmemo[]` storage + reset on relink.

**Correctness**
- `neo/idlib/math/Simd_RVV.cpp` — **6x `vfrsqrt7` NaN guards.** `vfrsqrt7(+0)=+inf`
  then `0*inf=NaN` propagated into face planes and tangents; NaN comparisons return
  false, which silently flips facing tests in the shadow code. Generic
  `idMath::RSqrt(0)` returns a finite ~1.98e19.
- `riscv/softgles2/src/sg_mt_tiles.c` — front-to-back tile sort now guarded on
  `blend == NONE && depth_test`. It was unconditional, which changes output for
  blended draws (Doom 3 submits translucent surfaces and fog as multi-tri draws).
- `riscv/engine/dos_thread.c` — release fence before `sbi_hart_start`. TLS/stack
  were published with only a compiler barrier; best candidate for the intermittent
  post-OpenSBI boot hang (unproven, correct regardless).

**Boot / build / harness**
- `riscv/boot/runtime.c/.h` — FDT `timebase-frequency` walker; `timer_hz()`;
  `timer_ns_from_ticks()`; **widened `.bss`/`.tbss` zeroing** (was byte-at-a-time
  over 2MB of hart stacks + 128KB TLS, executed before any console output exists —
  likely part of the apparent boot hang). Deliberately `noinline` and hand-written
  so GCC cannot rewrite it into a `memset` call that would recurse into libc before
  `.bss` is live.
- `neo/CMakeLists.txt` — `-ffp-contract=fast` on `bm_sgl` and on bare-metal `idlib`
  (upstream's directory-wide `-ffp-contract=off` is a desktop determinism guard;
  `sg_raster.c` gains 44% more FMA sites with it on). `-DNDEBUG` for bare-metal
  (~2900 live assert sites; verified no side-effecting asserts in `neo/` outside
  vendored libs).
- `riscv/engine/sys_dos.cpp` — `bm_benchVantage` / `bm_benchVantageYaw`.
- `riscv/scripts/rprof_stats.py` — classifier fix, serial-share metric, drop warning.
- `riscv/scripts/vlen_ab_fullframe.sh` — VLEN A/B harness. **Documented caveat:**
  gl4es is a prebuilt archive pinned at `zvl256b` and is not rebuilt on VLEN change,
  so "only VLEN varies" is false for one of four linked components until
  `build-gl4es.sh` takes `RV_MARCH` from `RVV_VLEN`.

**Logs**: `riscv/docs/perf/fix-round1-*.log`, `fix-round2-bench.log`,
`vlen-ab-256-512x288.log` (partial, but contains the reference lit frame at line 271).

---

## 9. Open question deliberately left unanswered

The full-frame VLEN 256 vs 1024 A/B was **stopped mid-run and never redone**. Arm 1
duplicated the committed baseline config exactly, and measuring the VLEN default
against code that is 87% serial answers a question about code that is about to
change. Redo it **after** FrameBin lands, and fix the gl4es `-march` pinning first
or the comparison is confounded (tile geometry is also tied to VLEN: `TILE_W =
SGL_SIMT_LANES = VLEN/16`, so 512x288 gives 2304 tiles at VLEN=256 but only 144 at
VLEN=1024 — that alone changes binning and straggler behaviour for reasons having
nothing to do with vector width).
