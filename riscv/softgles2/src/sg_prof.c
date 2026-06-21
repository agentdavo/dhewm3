/* Per-stage render profiler (see header). */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdio.h>
#endif
#include "sg_prof.h"

extern uint64_t timer_ticks(void);

#if !defined(SGL_BAREMETAL)
/* Host builds have no rdtime cycle counter (it lives in boot/runtime on bare
 * metal). Profiling timings are 0 on host; the RVV target measures real cycles
 * on QEMU. Provided here so GLCORE links cleanly for host tests. */
uint64_t timer_ticks(void) { return 0; }
#endif

int      sgl_prof_enabled = 0;
uint64_t sgl_prof_vertex, sgl_prof_dispatch, sgl_prof_raster, sgl_prof_present;
uint64_t sgl_prof_draws, sgl_prof_tris, sgl_prof_frags;
uint64_t sgl_prof_mt_bin, sgl_prof_mt_release, sgl_prof_mt_mainbusy, sgl_prof_mt_wait;
uint64_t sgl_prof_hart_busy[SGL_PROF_MAXHARTS];
uint64_t sgl_prof_hart_tiles[SGL_PROF_MAXHARTS];
uint64_t sgl_prof_shade;

void sgl_prof_enable(int on) { sgl_prof_enabled = on; }

uint64_t sgl_prof_now(void) { return sgl_prof_enabled ? timer_ticks() : 0; }

void sgl_prof_frame_report(void)
{
    if (!sgl_prof_enabled) return;
    /* aclint-mtimer @ 10 MHz -> 1 tick = 100 ns -> us = ticks / 10 */
    uint64_t total = sgl_prof_vertex + sgl_prof_dispatch + sgl_prof_raster + sgl_prof_present;
    extern volatile unsigned long sgl_dbg_mt_engaged, sgl_dbg_mt_cover_admit;
    extern volatile unsigned long sgl_dbg_draw_skipped_transparent;
    printf("[rprof] draws=%lu tris=%lu frags=%lu skipxp=%lu mt_engaged=%lu mt_blits=%lu | vertex=%lu.%01lums dispatch=%lu.%01lums "
           "raster=%lu.%01lums present=%lu.%01lums total=%lu.%01lums\n",
           (unsigned long)sgl_prof_draws, (unsigned long)sgl_prof_tris, (unsigned long)sgl_prof_frags,
           (unsigned long)sgl_dbg_draw_skipped_transparent,
           (unsigned long)sgl_dbg_mt_engaged, (unsigned long)sgl_dbg_mt_cover_admit,
           (unsigned long)(sgl_prof_vertex / 10000),   (unsigned long)((sgl_prof_vertex / 1000) % 10),
           (unsigned long)(sgl_prof_dispatch / 10000), (unsigned long)((sgl_prof_dispatch / 1000) % 10),
           (unsigned long)(sgl_prof_raster / 10000),   (unsigned long)((sgl_prof_raster / 1000) % 10),
           (unsigned long)(sgl_prof_present / 10000),  (unsigned long)((sgl_prof_present / 1000) % 10),
           (unsigned long)(total / 10000),             (unsigned long)((total / 1000) % 10));
    /* dispatch sub-attribution + per-hart utilisation (Lever 1 measurement) */
    uint64_t mtsum = sgl_prof_mt_bin + sgl_prof_mt_release + sgl_prof_mt_mainbusy + sgl_prof_mt_wait;
    if (mtsum) {
        printf("[rprof-mt] bin=%lu.%01lums release=%lu.%01lums mainbusy=%lu.%01lums wait=%lu.%01lums | frags=%lu",
               (unsigned long)(sgl_prof_mt_bin / 10000),      (unsigned long)((sgl_prof_mt_bin / 1000) % 10),
               (unsigned long)(sgl_prof_mt_release / 10000),  (unsigned long)((sgl_prof_mt_release / 1000) % 10),
               (unsigned long)(sgl_prof_mt_mainbusy / 10000), (unsigned long)((sgl_prof_mt_mainbusy / 1000) % 10),
               (unsigned long)(sgl_prof_mt_wait / 10000),     (unsigned long)((sgl_prof_mt_wait / 1000) % 10),
               (unsigned long)sgl_prof_frags);
        printf(" | hart busy/tiles");
        uint64_t hartsum = 0;
        for (int h = 0; h < SGL_PROF_MAXHARTS; ++h) {
            hartsum += sgl_prof_hart_busy[h];
            if (sgl_prof_hart_tiles[h])
                printf(" h%d=%lu.%01lums/%lu", h,
                       (unsigned long)(sgl_prof_hart_busy[h] / 10000), (unsigned long)((sgl_prof_hart_busy[h] / 1000) % 10),
                       (unsigned long)sgl_prof_hart_tiles[h]);
        }
        /* shade = total time in frag_simt across harts; wrapper = the rest of the
         * per-batch raster work (coverage/early-Z/interp/blend/quantise/store). */
        uint64_t wrapper = hartsum > sgl_prof_shade ? hartsum - sgl_prof_shade : 0;
        unsigned long shpct = hartsum ? (unsigned long)(sgl_prof_shade * 100 / hartsum) : 0;
        printf(" | shade=%lu.%01lums (%lu%%) wrapper=%lu.%01lums\n",
               (unsigned long)(sgl_prof_shade / 10000), (unsigned long)((sgl_prof_shade / 1000) % 10), shpct,
               (unsigned long)(wrapper / 10000), (unsigned long)((wrapper / 1000) % 10));
    }

    /* Phase 0: every 120 frames, dump the shader-workload ranking (which programs
     * shade the most fragments, kernel vs VM) — the archetype-specialisation
     * work-list. No-op unless glsl_link_dump populated the registry. */
    { extern void glsl_dbg_workload_report(void); static unsigned _fswork_fr = 0;
      ++_fswork_fr;
      if (_fswork_fr == 2u || (_fswork_fr % 10u) == 0u) glsl_dbg_workload_report(); }

    sgl_prof_vertex = sgl_prof_dispatch = sgl_prof_raster = sgl_prof_present = 0;
    sgl_prof_draws = sgl_prof_tris = sgl_prof_frags = 0;
    sgl_dbg_mt_engaged = sgl_dbg_mt_cover_admit = 0;   /* per-frame engagement */
    { extern volatile unsigned long sgl_dbg_draw_skipped_transparent;
      sgl_dbg_draw_skipped_transparent = 0; }   /* per-frame transparent-layer skips */
    sgl_prof_mt_bin = sgl_prof_mt_release = sgl_prof_mt_mainbusy = sgl_prof_mt_wait = 0;
    sgl_prof_shade = 0;
    for (int h = 0; h < SGL_PROF_MAXHARTS; ++h) { sgl_prof_hart_busy[h] = 0; sgl_prof_hart_tiles[h] = 0; }
}
