# DoomOS handover 21 — executing handover 20's §7 list

Written 2026-07-26. Branch `riscv64-baremetal`, on top of `3807656`.

Handover 20 left a five-item list. Items 1–4 are done. The headline is not that
the list got shorter: it is that **two of the instruments the list depended on
were measuring nothing**, and a hang that had been written off as flakiness for
three handovers turned out to be a deterministic bug.

---

## 0. TL;DR

1. **FrameBin is pixel-identical to immediate mode — measured, not argued.** But
   not by `framebin_ab.sh`, which *cannot* answer the question. New in-process
   gate: `make framebinrun`. §2
2. **The "flaky early boot hang" is a real bug and is fixed.** A `wfi` was
   re-executed after the trap handler disarmed the very interrupt meant to wake
   it. Caught red-handed on a hung guest. §1
3. **`com_fixedTic` was never reaching the engine.** `bm_argc` was hardcoded and
   3 short, silently dropping exactly that triple — so every `[fbhash]`
   comparison ever run used wall-clock game time. §2.1
4. **Both AIA backends work** (`aplic` direct-IDC and `aplic+imsic` MSI). They had
   never been executed. §4
5. **The whole `riscv/smp` harness was unbuildable and unrunnable.** Five separate
   defects. It now builds and passes, including on a non-zero boot hart. §3

---

## 1. The `wfi` re-execution hang (was: "virtio-blk flaky early boot")

### 1.1 What it actually is

`bm_wait_until()` armed the S-timer and executed `wfi`. With `sstatus.SIE` set,
the timer fires as a **trap** whose `sepc` points AT the `wfi`. `os_handle_irq()`
disarms the timer on delivery — clears `STIE`, pushes `stimecmp` to `~0`. `sret`
then resumes **at the `wfi` and re-executes it**, now with nothing armed that can
wake it. The hart halts forever.

Register state from a hung guest, via the QEMU monitor:

```
pc        0x8073306c   <- wfi+4; QEMU advances pc then halts, i.e. halted IN wfi
sepc      0x80733068   <- the wfi itself: a trap resumed AT it
scause    0x8000000000000005   <- supervisor timer
stimecmp  0xffffffffffffffff   <- disarmed
sie       0x202        <- SSIE|SEIE, STIE CLEAR
sip       0x0          <- nothing pending
```

Reproduced 3/3 on boot harts 1, 2 and 4, always stalling at the identical log
offset (4934 B). It is not flaky and it is not virtio-blk.

**Looping around the `wfi` does not help** — the re-execution happens at the
instruction level, so control never returns to the loop to re-arm. This is why
the bug survived a version of `bm_wait_until` that looped.

### 1.2 Fix

Mask `sstatus.SIE` across the park. `wfi` resumes on anything enabled in `sie`
regardless of `sstatus.SIE`, so the wake still happens — it just falls through
instead of trapping, and the caller disarms normally. Pending interrupts are
taken when SIE is restored. `csrrc` to read-and-clear, restoring only what was
there, so a caller that deliberately masked interrupts is not overridden.

Three sites had this shape:
- `bm_wait_until()` (`riscv/boot/runtime.c`) — the engine's park, used by
  `vblk_wait_used()` and softgles2's `sg_mt_sched.c`.
- `sched.c`'s submitter barrier and worker park (`park_wfi()`), waiting on an
  **IPI**: the handler clears SSIP, `sret` re-executes the `wfi`, and the hart
  sleeps until some later IPI. `sched.c`'s header comment asserted the opposite
  ("the `wfi` simply falls through to re-check") — corrected.

`sched_secondary.S`'s `.Lirq` clears SSIP/STIP and `sret`s **without advancing
sepc**, which is what makes the C-side masking necessary. The remaining bare
`wfi`s (`kmain.c:114`, `secondary.S`, `sbi.c`) are terminal `for(;;)` halts where
re-execution is the intent — those are correct.

### 1.3 Corroboration from an independent path

`riscv/smp/Makefile` contains a retry loop commented `>>> (boot hart != 0;
retrying)`. Someone had already noticed a boot-hart-dependent failure here and
worked around it. After the fix the scheduler test passes 4/4 **including boot
hart 3** — the case the retry existed for.

### 1.4 Also fixed on the way

- `vblk_wait_used()`'s bare `wfi` → bounded park (handover 20 §7 item 4). Note
  handover 20 §4's reasoning that a timer wake "turns a hang into a poll that
  completes" is wrong in the observed case: it polls and `used->idx` still never
  advances, because the hart never wakes at all.
- `bm_wait_until()` never exited early. Its header documented "until an interrupt
  **or** the deadline, whichever comes first", but the body looped to the
  deadline, re-arming after every early wakeup — making the deadline the expected
  exit and adding a full slice of latency to every completion. Now single-shot;
  both callers re-test their own predicate in an outer loop.
- `strstr` was missing from the bare-metal libc. `boot/fdt.c`'s `riscv,isa`
  fallback calls it; every existing target happened to let `--gc-sections` drop
  `fdt_cpu_has_extension`, so it only surfaced once a build kept that path.

---

## 2. FrameBin pixel identity — ANSWERED, and the old gate retired

**`riscv/scripts/framebin_ab.sh` cannot answer this question.** It compares
per-frame `[fbhash]` digests between two whole-engine QEMU runs, but frame
content there is a function of WALL CLOCK, not frame index.

Control experiment: **the same binary, run twice, diverges from frame 6** (13 of
19 common frames differed). So the script's FAIL carries no information about
FrameBin. Handover 20 §3.3's "6 identical, frame 6 differs" is that baseline
noise — frames 0–5 are the static intro, and divergence starts exactly where
animation starts.

### 2.1 Why `com_fixedTic` did not rescue it

Two independent reasons:

1. **It was never passed.** `bm_argc` in `engine_kmain.c` was hardcoded (79 menu
   / 81 map) while the arrays held 82 / 84 entries — exactly 3 short, dropping
   the `+set com_fixedTic 1` triple appended last. Now derived with
   `sizeof(argv)/sizeof(argv[0]) - 1`.
2. **Even when passed, it is not sufficient.** `com_fixedTic` sets
   `minTic = latchedTicNumber` and `lastGameTic = latchedTicNumber - N`, fixing
   usercmds-per-frame — but `latchedTicNumber` is `com_ticNumber`, which still
   advances with real time. With it genuinely active, only **6 of 105** distinct
   reference images appeared anywhere in the candidate's 635 frames.

### 2.2 The gate that does work

`riscv/softgles2/test/framebin_validate.c` — the same scene rendered **immediate
and deferred in ONE process**, compared byte for byte. No clock, no engine, no
pacing.

```
[framebin] 149x101  30 draws + mid-scene scissored clear + blend
           immediate vs deferred: PASS (bit-exact)
```

The scene is built to break a binner: dimensions not a multiple of the tile size
so draws straddle bins, 30 overlapping draws, a mid-scene **scissored clear**
recorded into the bins, depth testing, and an **order-dependent blended pass**
where any replay reorder changes pixels. 0 of 15049 pixels differ.

```bash
cd riscv/softgles2 && make framebinrun RVV_VLEN=256
```

---

## 3. Vertex work validated (handover 20 §7 item 2)

Both claims were "bit-exact by construction", which is an argument, not a
measurement.

- **Dedupe**: `make vsdeduprun` — same indexed mesh with dedupe off and on in one
  process. 9600 corners → 1681 uniques (5.7x reuse). **PASS (bit-exact).**
- **Parallel vertex**: needs a hart pool (`dos_thread_create` is engine-supplied),
  so it cannot run standalone. But the claim rests entirely on the range split
  covering `[0,count)` exactly once. `sg_vs_mt_range()` was moved into its own
  dependency-free TU (`src/sg_vs_range.c`) so the host test links **the real
  function, not a transcription**. `make vsmt_ranges`: 560 (count, nworkers)
  combinations x 5 lane widths, every vertex owned by exactly one worker.
  **ALL PASS.** The rounding is non-obvious — `per` rounds UP to a lane multiple,
  pushing later workers past the end.

`sgl_vs_dedup_min_verts` and `sgl_vs_mt_min_verts` are now runtime-settable
(read once per draw, not per vertex) precisely so this kind of A/B is possible.

---

## 4. AIA validated (handover 20 §7 item 3)

Both backends boot, mount dosfs, and render — neither had ever been executed.

| `AIA=` | reported backend | result |
|---|---|---|
| `aplic` | `aplic (direct)` | boots, renders |
| `aplic-imsic` | `aplic+imsic (msi)` | boots, dosfs+savefs, 12 frames |

Reaching multi-hart rendering also exercises the zero-ecall IMSIC IPI wake path.

PLIC context selection was separately verified against QEMU's own DTB
(`-machine virt,dumpdtb=`): the DTB-derived context and the legacy `2h+1`
fallback agree for every hart, so that code is correct either way.

---

## 5. The `riscv/smp` harness was completely broken

Five defects, none related to each other:

1. `boot/runtime.h` vs `sched_sbi.h`/`smp_sbi.h` declare the SBI calls with
   different return types — including both is a hard error. 4 files did. Fixed
   with `smp_timer.h` (declares only what the harness needs).
2. `fdt.o` missing from `COMMON_OBJS` → link error (`runtime.c` calls
   `fdt_find_compatible` etc). Same gap handover 20 §2.3 fixed for softgles2.
3. `SCRIPTS := ../scripts` — that directory does not exist; `run.sh` is at
   `riscv/run.sh`. Every `run*`/`srun*` target failed with "No such file".
4. `RVV_VLEN` mismatch: images build `zvl256b`, `run.sh` defaults 1024 → boot
   panic. Now derived from `ARCH`'s `zvl<N>b` so they cannot desynchronise.
5. The `park_wfi` bug in §1.

---

## 6. Harness defects fixed (all were silent)

- `framebin_ab.sh` summarised with plain `diff`, which calls unequal frame counts
  a FAIL — the exact trap handover 20 §1 documents. Now uses `fbhash_cmp.py`.
- `demorun.sh` clones the asset disk to ONE fixed `SCRATCH` path while commenting
  that runs are "decoupled from any concurrent run". A leftover QEMU from an
  earlier session held that image's write lock and **killed both arms of an A/B
  instantly** — while still producing logs, just with no `[fbhash]` in them.
  Per-arm `SCRATCH` now, plus an explicit "arm produced no frames" check.
- `demorun.sh` never forwarded `MONITOR_SOCKET`, so the one tool that can
  diagnose a hung guest was unreachable from exactly the long runs that hang.
  Forwarded now — this is what made §1 solvable.
- `fbhash_cmp.py` crashed on a missing log, burying the real state in a
  traceback.
- `framebin_ab.sh` reported `mt_engaged`, which handover 20 §3.1 had already
  corrected away. Now reports hart balance and `total`.

**QEMU monitor gotcha:** the HMP unix socket is single-client and `nc` never
exits on its own. Send ALL commands in ONE `nc -U` session bounded by `timeout`;
killing `nc` mid-session wedges the monitor so no later query works.

---

## 6b. FrameBin frame cost (handover 20 §7 item 5) — ~2.5x, stated the safe way

A cross-run speedup could NOT be stated: an unrelated Verilator workload on the
host restarted mid-measurement and the two rounds disagreed (2.71x vs 2.07x).
Interleaving the arms did not rescue it, and the reason is worth recording —
**interleaving only cancels drift when both arms are equally sensitive to it.**
The off arm rasterises largely serially and barely notices free-core count (2%
spread across rounds); the on arm spans 6 harts and swung 1.7x.

So the number comes from the WITHIN-RUN stage composition instead, where host
speed cancels exactly — each stage as a percentage of its OWN frame's total:

| stage | FrameBin ON | FrameBin OFF |
|---|---|---|
| vertex   | 2.7%  | 1.2%  |
| dispatch | 0.0%  | **38.8%** |
| raster   | 0.0%  | **21.3%** |
| mt (parallel) | 97.2% | 38.7% |
| present  | 0.1%  | 0.0%  |
| total_med | 2231.8ms (n=249) | 5976.6ms (n=157) |

**60.1% of an immediate-mode frame is `dispatch`+`raster`, and both read exactly
0.0% under FrameBin.** Amdahl on 60.1% predicts ~2.5x, and the noisy cross-run
medians bracket it at 2.07-2.71x — two independent lines of evidence agreeing.
The absolute `mt` time is comparable either way (2168ms vs 2330ms), which is the
expected shape: the same rasterisation work, minus the serial stages.

Harness: `riscv/scripts/perf_ab.sh` + `perf_cmp.py`. `perf_cmp.py` REFUSES to
state a ratio when rounds disagree by >15%; that guard fired on its first use and
is the only reason a "1.29x" and a "2.29x" from the same binaries were not
reported as results.

---

## 7. What to do next

1. **Re-take the cross-run frame-time ratio** if a genuinely idle host ever
   appears, to tighten 2.07-2.71x. The within-run composition above is the
   trustworthy statement and does not need re-taking. Note any number must be
   taken FRESH: the `wfi` fix changes park behaviour, so a figure spanning it is
   not like-for-like.
2. **Delete or repurpose `framebin_ab.sh`.** It cannot validate FrameBin (§2). It
   is still useful as a *soak* test — it caught the boot hang — but it must not
   be presented as a pixel gate.
3. **Consider a deterministic clock** for bare metal (`Sys_MillisecondsPrecise`
   driven by a frame counter when `BM_FB_HASH` is on). That is what would make
   whole-engine frame comparison possible at all. Not attempted here: it changes
   every timing path in the engine and wants its own session.
4. Then: lit-scene `[rprof-vshist]`, `SpawnMapEntities` per-class data,
   `MatchPrefix` counters, pk4 repack (all still untouched from handover 20 §7.6).

---

## 8. Files added

```
riscv/softgles2/test/framebin_validate.c   FrameBin equivalence gate (in-process)
riscv/softgles2/test/vsdedup_validate.c    vertex dedupe equivalence gate
riscv/softgles2/test/vsmt_ranges.c         range-split coverage sweep (host)
riscv/softgles2/src/sg_vs_range.c          sg_vs_mt_range, dependency-free for test
riscv/smp/smp_timer.h                      breaks the runtime.h/smp_sbi.h clash
riscv/docs/handover-21.md                  this file
```

New make targets: `framebinrv`/`framebinrun`, `vsdeduprv`/`vsdeduprun`,
`vsmt_ranges`.

---

## 9. The recurring failure mode, again

Handover 20 §9 said: **before trusting a check, confirm it can fail.** This
session found two checks that were measuring nothing — a hardcoded `bm_argc`
truncating the flag that made the comparison valid, and a whole-engine digest
diff that reports differences between two runs of the same binary. Both looked
authoritative. Both had been believed across multiple handovers.

Every gate added here self-checks that it can fail: the pixel gates corrupt one
pixel and confirm the comparator reports exactly one difference; the range sweep
simulates an off-by-one. A passing check nobody has watched fail is not evidence.

Two smaller instances from tonight, same shape:
- `pgrep -f <pattern>` matches the shell running it, so "is it still alive?"
  answered yes about itself. Use `ps -C`.
- A Make variable comment on the same line (`SCRIPTS := ..    # note`) keeps the
  trailing whitespace in the value.
