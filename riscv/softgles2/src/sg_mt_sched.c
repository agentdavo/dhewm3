#include "sg_mt_internal.h"
#include "sg_mt_raster.h"
#include "sg_mt_sched.h"
#include "sg_prof.h"
#include "boot/bm_pause.h"

#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
extern void dos_threads_init(void) __attribute__((weak));
extern void *dos_thread_create(int (*fn)(void *), void *arg, const char *name) __attribute__((weak));

/* Idle-hart parking. Weak so softgles2 still links against a host/test build or
 * any environment that does not provide the bare-metal runtime. */
extern unsigned long dos_thread_self_id(void) __attribute__((weak));
extern void          bm_wait_until(uint64_t deadline_ticks) __attribute__((weak));
extern uint64_t      timer_ticks(void) __attribute__((weak));
extern uint64_t      timer_ticks_from_us(uint64_t us) __attribute__((weak));
/* os_irq_ipi() picks the cheapest wake available: an IMSIC MSI store (no ecall)
 * under the AIA MSI backend, else an SBI IPI. Weak, with the SBI call as the
 * fallback, so this file still links where the OS layer is absent. */
extern int           os_irq_ipi(unsigned long hartid) __attribute__((weak));
extern long          sbi_send_ipi_hart(unsigned long hartid) __attribute__((weak));

typedef struct {
    int                   inited;
    int                   nharts;       /* 1 (main) + number of worker harts */
    volatile sg_mt_work_fn fn;
    void *volatile        ctx;
    volatile unsigned     gen;          /* bumped to release the workers */
    volatile int          done;         /* workers ++ when their work finishes */
} sg_mt_state;

static sg_mt_state g_mt;

static inline void sg_mt_relax(void)
{
    /* Zihintpause if present on RVV, else a plain compiler/memory barrier. */
#if defined(__riscv)
    bm_pause();
#else
    __asm__ __volatile__("nop" ::: "memory");
#endif
}

/*
 * Zawrs backoff for the SHORT spin, as a future-silicon hint only.
 *
 * `wrs.nto` retires when the reservation on the watched word is broken, which on
 * real multicore silicon frees issue slots for sibling harts instead of burning
 * them on a poll loop. RVA23U64 mandates Zawrs, so this is the architecturally
 * correct way to spin.
 *
 * It is deliberately NOT a substitute for the `wfi` park below, and two audits
 * disagreed about this until QEMU settled it: helper_wrs_nto (op_helper.c) is a
 * pure permission check that falls through with NO halt, whereas helper_wfi sets
 * EXCP_HLT and cpu_loop_exit()s. So under TCG -- which is every measurement this
 * project takes -- wrs.nto yields nothing and `wfi` is the only instruction that
 * hands a host core back. Expect ZERO improvement from this in any benchmark
 * here; it earns its place on hardware, not in the emulator.
 *
 * lr.w arms the reservation on the word we are waiting on, so the hint has
 * something to watch. Falls back to `pause` when Zawrs is not compiled in.
 */
/* OFF by default, and that is a measurement decision, not timidity. Under TCG
 * `pause` decodes to a nop while lr.w and wrs.nto are each a helper call, so
 * enabling this turns a free spin iteration into two helper dispatches -- in a
 * 4000-iteration backoff that is a real slowdown on the very emulator every
 * number in this project comes from. Turn it on for hardware bring-up
 * (-DSGL_MT_USE_ZAWRS=1), leave it off for TCG benchmarking. */
#ifndef SGL_MT_USE_ZAWRS
#define SGL_MT_USE_ZAWRS 0
#endif

static inline void sg_mt_relax_on(volatile unsigned *addr)
{
#if SGL_MT_USE_ZAWRS && defined(__riscv) && defined(__riscv_zawrs)
    unsigned tmp;
    __asm__ __volatile__("lr.w %0, (%1)\n\twrs.nto"
                         : "=&r"(tmp) : "r"(addr) : "memory");
    (void)tmp;
#else
    (void)addr;
    sg_mt_relax();
#endif
}

/*
 * Idle workers used to spin on g_mt.gen forever. With PIN_HARTS=1 that means
 * every idle worker pegs its pinned host CPU for the whole gap between draws,
 * stealing host time from the harts still rasterising -- and because a lit frame
 * is largely single-hart, those gaps are most of the frame.
 *
 * `wfi` is the only instruction that gives the host core back under QEMU MTTCG:
 * helper_wfi() sets EXCP_HLT and cpu_loop_exit()s, genuinely parking the vCPU
 * thread. Zawrs' wrs.nto is a pure permission check with no halt and Zihintpause
 * decodes to a nop, so neither is a substitute here (they are future-silicon
 * hints only, and will show zero improvement in any TCG measurement).
 *
 * Spin first, park second: a short spin covers the common case where the next
 * draw is already coming, without paying an ecall. The bounded deadline is a
 * lost-wakeup net, not the expected exit -- correctness never depends on the IPI
 * arriving, only latency does.
 */
#define SGL_MT_SPINS_BEFORE_PARK 4000
#define SGL_MT_PARK_US           2000

static volatile unsigned long g_mt_hartid[SGL_MT_MAXHARTS];
static volatile int           g_mt_parked[SGL_MT_MAXHARTS];

static int sg_mt_can_park(void)
{
    return bm_wait_until && timer_ticks && timer_ticks_from_us &&
           (os_irq_ipi || sbi_send_ipi_hart);
}

static void sg_mt_ipi(unsigned long hartid)
{
    if (os_irq_ipi) os_irq_ipi(hartid);
    else if (sbi_send_ipi_hart) sbi_send_ipi_hart(hartid);
}

static void sg_mt_wait_gen(int worker, unsigned seen)
{
    unsigned spins = 0;

    while (__atomic_load_n(&g_mt.gen, __ATOMIC_ACQUIRE) == seen) {
        if (spins < SGL_MT_SPINS_BEFORE_PARK || !sg_mt_can_park()) {
            ++spins;
            sg_mt_relax_on((volatile unsigned *)&g_mt.gen);
            continue;
        }
        __atomic_store_n(&g_mt_parked[worker], 1, __ATOMIC_RELEASE);
        /* Re-check AFTER publishing parked=1. Without this re-read the submitter
         * could observe parked==0, skip the IPI, and bump gen -- all in the
         * window before we halt -- and we would sleep through the work. */
        if (__atomic_load_n(&g_mt.gen, __ATOMIC_ACQUIRE) == seen)
            bm_wait_until(timer_ticks() + timer_ticks_from_us(SGL_MT_PARK_US));
        __atomic_store_n(&g_mt_parked[worker], 0, __ATOMIC_RELEASE);
        spins = 0;
    }
}

/* Pull any parked worker out of `wfi`. Only IPIs harts that say they are
 * parked, so the steady-state busy case costs one relaxed load per worker
 * rather than an ecall. */
static void sg_mt_wake_workers(void)
{
    if (!sg_mt_can_park()) return;
    for (int b = 1; b < g_mt.nharts; ++b) {
        if (__atomic_load_n(&g_mt_parked[b], __ATOMIC_ACQUIRE))
            sg_mt_ipi(g_mt_hartid[b]);
    }
}

static int sg_mt_worker(void *arg)
{
    int worker = (int)(long)arg;        /* 1 .. nharts-1 */
    unsigned seen = 0;
    if (dos_thread_self_id && worker < SGL_MT_MAXHARTS)
        g_mt_hartid[worker] = dos_thread_self_id();
    for (;;) {
        sg_mt_wait_gen(worker, seen);
        seen = __atomic_load_n(&g_mt.gen, __ATOMIC_ACQUIRE);
        if (worker < g_mt.nharts && g_mt.fn) g_mt.fn(g_mt.ctx, worker, g_mt.nharts);
        __atomic_fetch_add(&g_mt.done, 1, __ATOMIC_ACQ_REL);
    }
    return 0;
}

static void sg_mt_lazy_init(void)
{
    if (g_mt.inited) return;
    g_mt.inited = 1;
    g_mt.nharts = 1;                     /* main hart is always worker 0 */
    if (!dos_thread_create) {
        printf("[sgl-mt] dos_thread_create unavailable; tile raster stays single-hart\n");
        return;
    }
    if (dos_threads_init) dos_threads_init();
    for (int b = 1; b < SGL_MT_MAXHARTS; ++b) {
        if (!dos_thread_create(sg_mt_worker, (void *)(long)b, "sgl-tile")) break;
        g_mt.nharts = b + 1;
    }
    printf("[sgl-mt] tile-parallel raster: %d harts (%d workers, %dx%d tiles)\n",
           g_mt.nharts, g_mt.nharts - 1, SGL_MT_TILE_W, SGL_MT_TILE_H);
}
#endif

/* Non-zero while a parallel section is in flight. sg_mt_sched_run() keeps a
 * single global handshake (g_mt), so calling it from inside itself would
 * deadlock on the gen/done counters. The vertex stage dispatches large draws
 * through it and must be able to ask. */
static volatile int g_mt_busy;

int sg_mt_sched_busy(void)
{
#if defined(SGL_BAREMETAL)
    return __atomic_load_n(&g_mt_busy, __ATOMIC_ACQUIRE);
#else
    return 0;
#endif
}

int sg_mt_sched_nharts(void)
{
#if defined(SGL_BAREMETAL)
    sg_mt_lazy_init();
    return g_mt.nharts;
#else
    return 1;
#endif
}

void sg_mt_sched_run(sg_mt_work_fn fn, void *ctx)
{
#if !defined(SGL_BAREMETAL)
    fn(ctx, 0, 1);
#else
    sg_mt_lazy_init();
    if (g_mt.nharts < 2) {
        fn(ctx, 0, 1);
        return;
    }
    __atomic_store_n(&g_mt_busy, 1, __ATOMIC_RELEASE);
    g_mt.fn = fn;
    g_mt.ctx = ctx;
    __atomic_store_n(&g_mt.done, 0, __ATOMIC_RELEASE);
    uint64_t t0 = sgl_prof_now();
    __atomic_fetch_add(&g_mt.gen, 1, __ATOMIC_ACQ_REL);   /* release workers */
    sg_mt_wake_workers();                                 /* IPI any parked hart */
    uint64_t t1 = sgl_prof_now();
    fn(ctx, 0, g_mt.nharts);                              /* main worker */
    uint64_t t2 = sgl_prof_now();
    while (__atomic_load_n(&g_mt.done, __ATOMIC_ACQUIRE) != g_mt.nharts - 1)
        sg_mt_relax_on((volatile unsigned *)&g_mt.done);
    uint64_t t3 = sgl_prof_now();
    __atomic_store_n(&g_mt_busy, 0, __ATOMIC_RELEASE);
    if (sgl_prof_enabled) {
        sgl_prof_mt_release  += t1 - t0;
        sgl_prof_mt_mainbusy += t2 - t1;
        sgl_prof_mt_wait     += t3 - t2;
    }
#endif
}

/* Which gate declined a draw, and how much geometry went single-hart because of
 * it. A rejected draw is not free: it falls back to emit_tri_prog on one hart
 * while every other hart spins. Profiling showed 10 of 19 draws in a lit frame
 * being rejected here, costing 6.4s of a 11.4s frame, so "why" is worth counting.
 * Indexed by MTR_*; reported in [rprof-mt]. */
volatile unsigned long sgl_dbg_mt_reject[MTR_N];
volatile unsigned long sgl_dbg_mt_reject_prims[MTR_N];

static int mt_reject(int why, int nprim)
{
    sgl_dbg_mt_reject[why]++;
    sgl_dbg_mt_reject_prims[why] += (unsigned long)(nprim > 0 ? nprim : 0);
    return 0;
}

int sg_mt_sched_should_mt(int nprim, int bin_refs, int active_count,
                         int fs_cost, int fs_simt_ok, const sgl_drawstate *st,
                         int tile_area)
{
#if defined(SGL_BAREMETAL)
    /* Admit down to one tile per hart instead of a fixed 24. The old floor was
     * "one SG_MT_GRAB-sized claim per hart" (4 x 6 harts = 24), which rejected any
     * draw touching fewer tiles no matter how much geometry it carried -- measured
     * at 14610 triangles/frame pushed onto the single-hart path from only 85
     * draws (~172 tris each), the largest single rejection reason in the frame.
     * A draw covering >= nharts tiles can still occupy every hart; the grab size
     * in sg_mt_sched_tiles() adapts so those tiles actually spread out rather
     * than being swallowed by the first claim. */
    int nh = g_mt.nharts;
    int min_tiles = (nh > 1) ? nh : SGL_MT_MIN_ACTIVE_TILES;
    if (active_count < min_tiles) return mt_reject(MTR_ACTIVE, nprim);
    if (bin_refs < SGL_MT_MIN_BIN_REFS) return mt_reject(MTR_BINREFS, nprim);

    int ref_span_x10 = nprim > 0 ? (bin_refs * 10) / nprim : 0;
    if (ref_span_x10 < SGL_MT_MIN_REF_SPAN_X10) return mt_reject(MTR_SPAN, nprim);

    int eff_cost = fs_cost;
    if (st->color_mask == 0) eff_cost = 8;       /* depth/stencil-only: shader is skipped */
    else if (!fs_simt_ok && eff_cost < 96) eff_cost = 96;

    if (tile_area < 1) tile_area = SGL_MT_TILE_W * SGL_MT_TILE_H;
    long score = (long)bin_refs * (long)eff_cost +
                 (long)active_count * (long)tile_area;
    if (score < SGL_MT_MIN_WORK_SCORE) return mt_reject(MTR_SCORE, nprim);
    return 1;
#else
    (void)nprim; (void)bin_refs; (void)active_count; (void)fs_cost; (void)fs_simt_ok; (void)st;
    (void)tile_area;
    return 0;
#endif
}

void sg_mt_sched_tiles(void *vctx, int worker, int nworkers)
{
    (void)worker; (void)nworkers;
    sg_mt_sched_ctx *sctx = (sg_mt_sched_ctx *)vctx;
    const sg_prog_job *j = sctx ? sctx->job : 0;
#if defined(SGL_BAREMETAL)
    if (!j) return;
    /* only mutable scheduler cursor is advanced atomically; job-backed arrays are
     * read-only by design and should not be written here. */
    int total = j->active_tiles ? j->active_count : j->tiles_x * j->tiles_y;
    uint64_t busy0 = sgl_prof_now();   /* per-hart utilisation (load balance) */
    int mytiles = 0;
    /* Batch-grab the shared tile cursor: one RELAXED fetch_add claims SG_MT_GRAB
     * tiles instead of an ACQ_REL fetch_add per tile. With ~494 tiles x 8 harts
     * the old code ran ~62 fenced atomics/hart, all bouncing the cursor cache
     * line — the dominant MTTCG inflation. Grabbing in small blocks cuts that
     * ~Gx; the granule stays small (4) so load balance barely changes (a
     * straggler does at most GRAB-1 extra tiles). Disjoint tiles -> bit-exact. */
    /* Adapt the claim size to the job. A fixed grab of 4 is right for a
     * few-hundred-tile draw, but on a small draw (now admitted -- see the
     * MTR_ACTIVE gate) it hands the whole job to the first one or two harts and
     * leaves the rest idle: 8 tiles / grab 4 = 2 claims for 6 harts. Target ~2
     * claims per hart so every hart gets work, while keeping the granule big
     * enough on large jobs to amortise the cursor cache-line bounce. Harts need
     * not agree on the value -- the cursor is just a counter, each claims what it
     * asks for -- so this stays bit-exact and race-free. */
    int grab = SG_MT_GRAB;
    if (nworkers > 1) {
        int g = total / (nworkers * 2);
        grab = (g < 1) ? 1 : (g > SG_MT_GRAB ? SG_MT_GRAB : g);
    }
    for (;;) {
        int base = __atomic_fetch_add(&sctx->tile_next, grab, __ATOMIC_RELAXED);
        if (base >= total) break;
        int end = base + grab; if (end > total) end = total;
        for (int t = base; t < end; ++t) {
            int tile = j->active_tiles ? j->active_tiles[t] : t;
            if (tile < 0 || tile >= j->tiles_x * j->tiles_y) {
                continue;
            }
            sg_mt_tile_emit(j, tile % j->tiles_x, tile / j->tiles_x);
            mytiles++;
        }
    }
    if (sgl_prof_enabled && worker >= 0 && worker < SGL_PROF_MAXHARTS) {
        sgl_prof_hart_busy[worker]  += sgl_prof_now() - busy0;
        sgl_prof_hart_tiles[worker] += (uint64_t)mytiles;
    }
#else
    (void)j;
#endif
}
