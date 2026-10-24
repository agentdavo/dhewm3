/* Work stealing / hart scheduling for MT raster. */
#ifndef SOFTGLES2_SG_MT_SCHED_H
#define SOFTGLES2_SG_MT_SCHED_H

#include "sg_mt_internal.h"

/* Work function contract:
 * `ctx` points at a scheduler-context object whose shared job payload is
 * dispatch-owned and immutable for the run:
 *   - draw/pv/prims/tile_offsets/tile_refs/active_tiles never mutate while workers run.
 *   - only `tile_next` may mutate, and only with atomic operations.
 */
typedef void (*sg_mt_work_fn)(void *ctx, int worker, int nworkers);

void sg_mt_sched_run(sg_mt_work_fn fn, void *ctx);

/* Non-zero while a parallel section is already running. sg_mt_sched_run() uses
 * one global handshake, so re-entering it would deadlock -- callers that may run
 * either inside or outside a parallel section must check first. */
int  sg_mt_sched_busy(void);

/* Harts available to sg_mt_sched_run (1 = single-hart). Triggers lazy init. */
int  sg_mt_sched_nharts(void);
/* Decide whether MT has enough visible work for a tile-queued path.
 * Callers pass the final dispatch layout/tile area and ownership counts;
 * this routine does not perform ownership checks beyond read-only contract checks
 * done in sg_mt_validate_job.
 */
int sg_mt_sched_should_mt(int nprim, int bin_refs, int active_count,
                         int fs_cost, int fs_simt_ok, const sgl_drawstate *st,
                         int tile_area);

/* Worker callback used by MT dispatch (runs tile-at-a-time).
 * The callback receives an immutable job payload through sg_mt_sched_ctx.
 */
void sg_mt_sched_tiles(void *vctx, int worker, int nworkers);

#endif /* SOFTGLES2_SG_MT_SCHED_H */
