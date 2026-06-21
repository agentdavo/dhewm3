/* Raster execution for one tile or one programmable triangle dispatch unit. */
#ifndef SOFTGLES2_SG_MT_RASTER_H
#define SOFTGLES2_SG_MT_RASTER_H

#include "sg_mt_internal.h"

/* Tile raster path receives an immutable job pointer; all visible state changes
 * are via a private copy of draw.state (clip rectangle and face state only).
 */
void sg_mt_emit_triangle_prog(const sgl_pvert *a, const sgl_pvert *b, const sgl_pvert *c,
                             const sg_draw_packet *draw);

void sg_mt_tile_emit(const sg_prog_job *job, int tx, int ty);

#endif /* SOFTGLES2_SG_MT_RASTER_H */
