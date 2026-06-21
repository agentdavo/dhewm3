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
#define SGL_MT_MIN_VERTS  192    /* ~64 triangles: below this, dispatch cost wins */
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
#define SGL_MT_MIN_REF_SPAN_X10 18
#define SGL_MT_MIN_WORK_SCORE   180000L
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
