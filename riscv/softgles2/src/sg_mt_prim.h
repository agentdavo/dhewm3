/* Primitive assembly and screen-space primitive-tile bounds helpers for MT path. */
#ifndef SOFTGLES2_SG_MT_PRIM_H
#define SOFTGLES2_SG_MT_PRIM_H

#include "sg_mt_internal.h"

int sg_mt_prim_build(GLenum mode, GLsizei count, sg_prim_desc *prims);

/* Triangle/coverage bounds projection into the caller-owned tile grid.
 * Inputs (`pv`/`prims`/`layout`/`tiles_x/y`) are immutable within the dispatch
 * interval; outputs feed the dispatch-owned binning arrays in `sg_mt_tiles_build`.
 */
sg_tile_range sg_mt_prim_tiles(const sgl_pvert *pv, const sg_prim_desc *p,
                              const sgl_drawstate *st, int fbw, int fbh,
                              int tiles_x, int tiles_y,
                              const sg_mt_tile_layout *layout,
                              int cull_face,
                              int cull_mode, int front_ccw);

#endif /* SOFTGLES2_SG_MT_PRIM_H */
