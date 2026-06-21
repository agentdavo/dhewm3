/* Per-stage render profiler for softgles2 (gated, default-off).
 *
 * Accumulates wall-clock ticks per pipeline stage on the recording/main hart and
 * emits one line per presented frame, so a real engine frame can be decomposed
 * into vertex / dispatch / raster(+fragment) / present. "Measure first."
 */
#ifndef SOFTGLES2_SG_PROF_H
#define SOFTGLES2_SG_PROF_H

#include <stdint.h>

#define SGL_PROF_MAXHARTS 8

extern int sgl_prof_enabled;
extern uint64_t sgl_prof_vertex;    /* vertex transform (sg_vertex_prog_cached) */
extern uint64_t sgl_prof_dispatch;  /* sg_mt_prog_dispatch (bin + parallel tile raster) */
extern uint64_t sgl_prof_raster;    /* single-hart emit_tri_prog raster (fragment-dominated) */
extern uint64_t sgl_prof_present;   /* present/blit */
extern uint64_t sgl_prof_draws;
extern uint64_t sgl_prof_tris;
extern uint64_t sgl_prof_frags;     /* shaded fragment lanes (vcpop of write mask; atomic) */

/* Dispatch sub-attribution (Lever 1): the [rprof] dispatch bucket conflates
 * serial binning with the blocking parallel raster wait. These split it so we
 * can target the real cost. release/mainbusy/wait are timed inside
 * sg_mt_sched_run; bin is timed in sg_mt_prog_dispatch before the fan-out. */
extern uint64_t sgl_prof_mt_bin;      /* prim build + tile bin + active scan (serial) */
extern uint64_t sgl_prof_mt_release;  /* gen-bump worker release */
extern uint64_t sgl_prof_mt_mainbusy; /* main hart's own tile shading */
extern uint64_t sgl_prof_mt_wait;     /* main hart join spin-wait on workers */
/* Per-hart utilisation (load balance): busy ticks + tiles consumed, indexed by
 * worker id (0 = main). A tiles_done spread >20% means the atomic cursor / tile
 * granularity is leaving harts idle. */
extern uint64_t sgl_prof_hart_busy[SGL_PROF_MAXHARTS];
extern uint64_t sgl_prof_hart_tiles[SGL_PROF_MAXHARTS];
/* Splits mainbusy/raster into the fragment-shader call (frag_simt: kernel or VM)
 * vs the raster wrapper around it (coverage, early-Z, varying interp, blend,
 * quantise, store). Tells us whether shader optimisation (kernel/gather) can
 * move the needle or whether the wrapper/contention dominates. Atomic (cross-
 * hart), gated on prof. */
extern uint64_t sgl_prof_shade;   /* time inside frag_simt across all harts */

void     sgl_prof_enable(int on);
uint64_t sgl_prof_now(void);        /* timer_ticks() wrapper (0 when disabled) */
void     sgl_prof_frame_report(void); /* print one [rprof] line + reset (call at present) */

#endif /* SOFTGLES2_SG_PROF_H */
