/* Internal SoftGLES2 tile raster interfaces.
 *
 * This header captures cross-unit structures and constants that should stay private
 * to the multi-hart programmable draw path.
 */
#ifndef SOFTGLES2_SG_MT_INTERNAL_H
#define SOFTGLES2_SG_MT_INTERNAL_H

#include "sg_mt.h"
#include "sg_core.h"

#if !defined(SGL_BAREMETAL)
#  include <stddef.h>
#endif

/* Tunable constants and batching geometry for the MT path.
 *
 * Tile geometry is a policy decision for workload balance versus binning cost.
 * Keep defaults matching the old behavior in comments/tests while exposing the
 * shape as an explicit runtime layout object.
 */
#define SGL_MT_MAXHARTS   8      /* main hart + up to 7 worker harts (Lever 4).
                                  * Self-gating: sg_mt_lazy_init stops spawning
                                  * when dos_thread_create fails (no more present
                                  * harts), so this is min(8, guest harts). The
                                  * guest hart count = QEMU -smp; keep -smp <= host
                                  * cores to avoid MTTCG oversubscription. */
/* ~64 triangles: below this, dispatch cost wins.
 * DELIBERATELY NOT LOWERED. It is the most-hit rejection by draw COUNT (808 of
 * 975 in a measured lit frame) so it looks like the obvious lever, but those 808
 * draws carry only 6800 triangles between them -- and admitting a draw costs a
 * full bin pass plus 7 mallocs. Measured binning is ~165ms for ~30 admitted
 * draws, so paying it on ~800 tiny draws would add seconds per frame and lose
 * far more than the single-hart shading it saves. Many-small-draws is exactly
 * the workload per-draw MT cannot serve; the fix is frame-level binning
 * (sg_framebin.c), not a lower threshold here. */
#define SGL_MT_MIN_VERTS  192
/* ...UNLESS the draw covers a large screen area: a low-vertex/high-coverage
 * blit (the gl4es 2D overlay is a 4-vertex GL_TRIANGLE_FAN filling the screen)
 * is huge fragment work the vertex count hides. Admit such draws to binning so
 * they fan across harts instead of shading single-hart. 64x64 px = the floor;
 * sg_mt_sched_should_mt() makes the real call after binning. */
#define SGL_MT_MIN_COVER_PX 16384
/* Tiles claimed per atomic fetch_add on the shared work-stealing cursor. >1
 * amortises the cursor cache-line bounce (the dominant MTTCG contention) while
 * staying small enough that load balance is unaffected. */
#define SG_MT_GRAB 4
/* Legacy defaults retained for behavior compatibility. */
#define SGL_MT_TILE_W     SGL_SIMT_LANES
#define SGL_MT_TILE_H     (SGL_SIMT_LANES / 4)
#define SGL_MT_MIN_ACTIVE_TILES 24
#define SGL_MT_MIN_BIN_REFS     512
/* Minimum average tiles touched per primitive, x10. This gate asks "is this
 * draw spread out enough to be worth fanning across harts?", but tile-span per
 * primitive is the wrong proxy for that: with a 16x4 tile at VLEN=256 there are
 * 2304 tiles on a 512x288 screen, so a draw of 4000 one-tile triangles has
 * ample tile-parallelism yet scores span=10 and is rejected. Total work is
 * already captured by SGL_MT_MIN_WORK_SCORE below, which is the gate that
 * should be making this call. Lowered 18 -> 10 so single-tile primitives are
 * admitted on the strength of their aggregate work; see [rprof-mt] mtrej=. */
#define SGL_MT_MIN_REF_SPAN_X10 10
#define SGL_MT_MIN_WORK_SCORE   180000L

/* Reasons a draw is declined to the single-hart path. The first group fires in
 * sg_mt_prog_dispatch() BEFORE binning, the second in sg_mt_sched_should_mt()
 * after. Instrumenting only the second group would have been misleading: Doom 3
 * averages ~122 verts/draw against SGL_MT_MIN_VERTS=192, so most draws are
 * rejected by MTR_SMALL and never reach the scheduler's gates at all. */
enum {
    MTR_SMALL = 0,    /* under SGL_MT_MIN_VERTS and under SGL_MT_MIN_COVER_PX */
    MTR_NOPRIM,       /* no primitives built (or malloc failed) */
    MTR_NOREFS,       /* binning produced no tile references */
    MTR_ACTIVE,       /* too few active tiles */
    MTR_BINREFS,      /* too few tile references */
    MTR_SPAN,         /* tiles-per-primitive below SGL_MT_MIN_REF_SPAN_X10 */
    MTR_SCORE,        /* aggregate work below SGL_MT_MIN_WORK_SCORE */
    MTR_N
};
extern volatile unsigned long sgl_dbg_mt_reject[MTR_N];
extern volatile unsigned long sgl_dbg_mt_reject_prims[MTR_N];
/* Geometry tuning for large primitive batches.
 * Keep unchanged unless profiling justifies a different split point.
 */
#define SGL_MT_TILE_SPLIT_PRIMS 4096

typedef struct {
    int w;
    int h;
} sg_mt_tile_layout;

typedef struct {
    int split_prim_threshold;  /* nprim >= threshold before split candidates are considered */
    int split_area_div;        /* minimum candidate area = base_area / split_area_div */
    int min_active_tiles;      /* require at least this many base tiles before splitting */
} sg_mt_layout_policy;

typedef struct {
    int tx0, ty0;
    int tx1, ty1; /* inclusive tile bounds */
} sg_tile_range;

/* Immutable job payload shared by all workers.
 *
 * Ownership/ownership boundary:
 * 1) Dispatch owns the backing allocations for pv/prims/tile_offsets/tile_refs/active_tiles
 *    until all workers complete.
 * 2) Workers only consume pointers through immutable reads.
 * 3) Dispatcher must finalize this structure before launching sg_mt_sched_run.
 */

typedef struct {
    int a, b, c;
    float zmin;
} sg_prim_desc;

typedef struct {
    const sg_draw_packet *draw;          /* shared, immutable per-draw state */
    const sgl_pvert      *pv;           /* shared vertex stream, read-only by workers */
    const sg_prim_desc    *prims;        /* shared primitive descriptors, read-only */
    int                   prim_count;    /* number of valid entries in prims */
    const int             *tile_offsets;  /* [tiles_x*tiles_y + 1], dispatch-owned */
    const int             *tile_refs;     /* [tile_offsets[last]] primitive indices, dispatch-owned */
    const int             *active_tiles;  /* optional compact active tile list, dispatch-owned */
    int                   active_count;   /* number of entries in active_tiles */
    int                   tiles_x;
    int                   tiles_y;
    int                   fbh;
    int                   fbw;
    sg_mt_tile_layout     layout;         /* tile geometry for this draw */
} sg_prog_job;

/* Dispatch-scoped mutable scheduler state.
 * Keep this separate so the shared payload stays immutable across harts.
 */
typedef struct {
    const sg_prog_job *job;
    volatile int       tile_next;
} sg_mt_sched_ctx;

/* Keep tile-split behavior configurable and probeable.
 * The default policy preserves existing production defaults.
 */
sg_mt_layout_policy sg_mt_layout_policy_default(void);
sg_mt_tile_layout    sg_mt_layout_choose(int nprim, int fbw, int fbh,
                                        const sg_mt_layout_policy *policy);

/* Lightweight defensive check used by both dispatch and tests.
 * Returns non-zero when the immutable job payload is internally consistent.
 */
int sg_mt_validate_job(const sg_prog_job *job, int total_tiles, int bin_refs);

#endif /* SOFTGLES2_SG_MT_INTERNAL_H */
