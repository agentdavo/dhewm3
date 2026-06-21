#include "sg_mt_internal.h"
#include "sg_mt_raster.h"
#include "sg_mt_sched.h"
#include "sg_prof.h"

#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
extern void dos_threads_init(void) __attribute__((weak));
extern void *dos_thread_create(int (*fn)(void *), void *arg, const char *name) __attribute__((weak));

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
    __asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#else
    __asm__ __volatile__("nop" ::: "memory");
#endif
}

static int sg_mt_worker(void *arg)
{
    int worker = (int)(long)arg;        /* 1 .. nharts-1 */
    unsigned seen = 0;
    for (;;) {
        while (__atomic_load_n(&g_mt.gen, __ATOMIC_ACQUIRE) == seen) sg_mt_relax();
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
    g_mt.fn = fn;
    g_mt.ctx = ctx;
    __atomic_store_n(&g_mt.done, 0, __ATOMIC_RELEASE);
    uint64_t t0 = sgl_prof_now();
    __atomic_fetch_add(&g_mt.gen, 1, __ATOMIC_ACQ_REL);   /* release workers */
    uint64_t t1 = sgl_prof_now();
    fn(ctx, 0, g_mt.nharts);                              /* main worker */
    uint64_t t2 = sgl_prof_now();
    while (__atomic_load_n(&g_mt.done, __ATOMIC_ACQUIRE) != g_mt.nharts - 1)
        sg_mt_relax();
    uint64_t t3 = sgl_prof_now();
    if (sgl_prof_enabled) {
        sgl_prof_mt_release  += t1 - t0;
        sgl_prof_mt_mainbusy += t2 - t1;
        sgl_prof_mt_wait     += t3 - t2;
    }
#endif
}

int sg_mt_sched_should_mt(int nprim, int bin_refs, int active_count,
                         int fs_cost, int fs_simt_ok, const sgl_drawstate *st,
                         int tile_area)
{
#if defined(SGL_BAREMETAL)
    if (active_count < SGL_MT_MIN_ACTIVE_TILES) return 0;
    if (bin_refs < SGL_MT_MIN_BIN_REFS) return 0;

    int ref_span_x10 = nprim > 0 ? (bin_refs * 10) / nprim : 0;
    if (ref_span_x10 < SGL_MT_MIN_REF_SPAN_X10) return 0;

    int eff_cost = fs_cost;
    if (st->color_mask == 0) eff_cost = 8;       /* depth/stencil-only: shader is skipped */
    else if (!fs_simt_ok && eff_cost < 96) eff_cost = 96;

    if (tile_area < 1) tile_area = SGL_MT_TILE_W * SGL_MT_TILE_H;
    long score = (long)bin_refs * (long)eff_cost +
                 (long)active_count * (long)tile_area;
    return score >= SGL_MT_MIN_WORK_SCORE;
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
    for (;;) {
        int base = __atomic_fetch_add(&sctx->tile_next, SG_MT_GRAB, __ATOMIC_RELAXED);
        if (base >= total) break;
        int end = base + SG_MT_GRAB; if (end > total) end = total;
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
