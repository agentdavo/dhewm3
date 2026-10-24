# DoomOS / softgles2 — open problems

## Visual audit update — 2026-09-08

The reception-window streaking and sawtooth shadow artifacts reproduced below
are now resolved in the checked views. The fragment coordinates and derivative
scratch arrays were shared by all render workers; they are now thread-local,
with derivative storage sized by varying count. Stencil comparisons also had
their operands reversed (including the old test's expectation), and RVV INCR
wrapped at 255 instead of saturating. Both raster paths now follow GL semantics.

Full refraction is enabled by default after the window capture passed. Four
consecutive frozen-frame captures were byte-identical. Real QEMU display
captures also work after the intro fade finishes. Fog on/off captures in the
hangar confirm visible fog, with shadows and post-processing enabled.

The new four-VLEN matrix passes shader, binding, resolve, and stencil checks;
each width executes 328 stencil cases. See
[visual audit and evidence](riscv/docs/perf/visual-audit-2026-09-08.md).
The older notes below are retained as investigation history, not current
claims that disabling refraction or adjusting the perspective reciprocal fixes
the symptoms. Other maps have not received the same screenshot coverage.

Tracked issues from the bare-metal RISC-V Doom 3 (softgles2 + gl4es) effort.
Fog #2 and the "overhead corruption" (= aliasing, addressed by SSAA) are RESOLVED
and are not listed here; see memory notes.

---

## 1. heatHaze — RESOLVED in tested views; investigation history

**Verified 2026-09-08:** fixed gl4es `clone_gl_pointer` retaining the generic
attribute's old `.buffer`. Conventional pointers already include the current
VBO shadow-data base. `realize_glenv` added the stale `.buffer->data` a second
time on the client path. The macro now clears `.buffer = NULL`.

The reproducible vendor change is
`riscv/gl4es-bridge/patches/0004-conventional-array-stale-buffer.patch`.
The Windows rebuild script now resolves native compiler/include paths and
supports a separate `GL4ES_BUILD_DIR`; see `riscv/docs/windows-build.md`.

**Runtime evidence:** rebuilt gl4es and dhewm3, booted Mars City on QEMU with
`bm_heathazeDeform 1` and draw logging enabled. All 54 client-array heatHaze
draws in the sampled log had different first/second UV pairs (zero equal pairs).
For example draw 646: `buf=0 ptr=0xa6c65c3c path=c`,
`v0=(-1,-1) v1=(0,0)`. VBO draws 2164/2167 still fetched `(0,0)` / `(1,1)`.
This validates the attribute handoff fix; it does not validate all refraction math.

**Visual result:** completed-frame scanout captures still show streaking and
black geometry artifacts around the reception glass/frames. Full refraction is
therefore NOT declared fixed. `bm_heathazeDeform` remains **0 by default**.
The final engine was rebuilt with draw logging disabled. The unsafe `HHbind`
probe was removed: `vertexCache.Position()` can return a VBO offset (including
zero), so treating `ac` as a CPU pointer was invalid even with a null guard.

**Next investigation:** correlate these now-correct input UVs with the SAME
draw's post-vertex `TexCoord_1` and `TexCoord_2`, then its raster/fragment inputs.
Compare refraction on/off at a fixed camera before changing the default. Earlier
claims that downstream stages were ruled out must not substitute for this
same-draw check. Do not reintroduce the disproven enable-bit workaround or
change Doom's shipping heatHaze program to compensate for a renderer bug.

The earlier `sgl_ram_hi()` range correction is retained: valid engine client
allocations can extend above `__heap_end` to `ram_end()`. However, a pointer
falling inside RAM is not evidence that it refers to the intended allocation.

Diagnostic artifacts are under the ignored `riscv/build/hz-verification/`:
`hz-fixed.log`, the build logs, and `hz-view-*.png`. QEMU HMP `screendump`
returned black in this run; captures instead read the actual RGBA scanout after
a completed `[rprof]` frame, with the guest briefly paused during the read.

---

## 2. Black shadow triangles — RESOLVED in tested views; investigation history

**Retest (2026-09-08):** black triangles remain visible in the reception scanout
with the gl4es UV fix and the reciprocal guard both present. The guard alone
is not a verified fix; the diagnosis below remains a hypothesis.

**Earlier guard (2026-09-08):** Root cause was an **unguarded perspective reciprocal** on grazing
overhead slivers. The barycentric weights come from float32 edge-function accumulation
across thousands of pixels; on a long thin foreshortened triangle the coverage test
`e>=0` admits lanes whose true barycentric is marginally negative, so the perspective
denominator `iw = w0*a.invw + w1*b.invw + w2*c.invw` rounds to ~0 or slightly negative.
`vfrec7` (7-bit seed + 1 Newton step) / `1.f/iw` of that returns a huge/negative `s`
which multiplies every perspective-corrected varying → the shader inputs explode →
`clamp01(NaN/huge)` → 0 → black fragments in the exact shape of the offending triangle.
Guard-band clip bounds screen coords but NOT the invw range of an on-screen
foreshortened tri, so clipping didn't prevent it.
**Change:** `riscv/softgles2/src/sg_raster.c` — floor `iw` to `1e-7f` before the
reciprocal in both raster paths: SIMT (`__riscv_vfmax_vf_f32m2(iw, 1e-7f, shade_vl)`
before `vfrec7`, ~line 1063) and scalar (`1.f/(iw>1e-7f?iw:1e-7f)`, ~line 1288). True
in-triangle `iw` is always >= the smallest vertex invw, so the floor is a no-op for
well-formed coverage — pure robustness guard.
**Status:** compiles + boots; reception renders with no obvious floating shards. A hard
A/B (revert the guard, rebuild, capture same vantage) was NOT run — at ~94 s/frame under
TCG the visual A/B is expensive. Guard is low-risk regardless. If a definitive before/
after is wanted, that's the remaining step.

**Original symptom (for reference):** Distinct black jagged/triangular shards floating
near the top of the reception scene (ceiling, above the window frames) — separate from
the heatHaze streaks. Looks like broken/degenerate triangles.

---

## Windows debug-harness gotchas (for whoever resumes)

Later gl4es pass (2026-09-08): reusable index storage, shader-cache MRU and direct
RGB copies remove all steady-state shim allocations in two frozen Mars City
views. Both A/B images match byte for byte; fog restoration also matches.
The expanded four-VLEN QEMU matrix passes 20/20. CPU timings are mixed;
native UINT passthrough was withheld. See
[measured results](riscv/docs/perf/gl4es-optimization-2026-09-08.md).

Optimization validation (2026-09-08): gl4es ARB uniform bounds, immutable
softgles2 kernel bindings, and exact RVV 2× SSAA resolve are implemented.
The VLEN 128/256/512/1024 QEMU matrix passed all 36 checks after fixing truncated
SIMT matrix loads at VLEN 128. See
[the report](riscv/docs/perf/doom-gl4es-rvv-2026-09-08.md) and rerun with
`python riscv/scripts/rvv_doom_matrix.py --jobs 8 --repetitions 3`.
Next performance work should isolate fragment shading and rasterizer overhead
in a fixed-camera capture; the visual defects above remain unresolved.

- The Claude Bash tool is Git Bash, NOT MSYS2. Prefix `PATH` with
  `/c/msys64/ucrt64/bin` for the medany gcc probe AND the qemu run, and pass
  `QEMU=/c/msys64/ucrt64/bin/qemu-system-riscv64.exe` explicitly (run-win.sh's
  `/ucrt64/bin` fallback path does not resolve in Git Bash).
- Kill stray qemu before each boot (`taskkill //F //IM qemu-system-riscv64.exe`); a
  zombie qemu holding monitor port 4455 causes new boots to fail + stale-log reads.
- Use a UNIQUE `LOG=` per run and screendump only after a fresh `[rprof]` frame
  (settle ~0.7s) or you capture the mid-frame clear (all black).
- A `%f` printf from the multi-hart fragment kernels crashes in newlib `_dtoa`/
  `_Balloc` (float formatting isn't reentrant across harts). Keep bare-metal probes
  integer-only.
- Deterministic turntable at the `-700 -1380` vantage needs `FB_HASH=1` (fixed-tic +
  deterministic clock); otherwise the pan angle drifts between boots.
