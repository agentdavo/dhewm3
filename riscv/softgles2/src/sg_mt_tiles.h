/* Tile binning helpers for the programmable draw MT path. */
#ifndef SOFTGLES2_SG_MT_TILES_H
#define SOFTGLES2_SG_MT_TILES_H

#include "sg_mt_internal.h"

/* Tile binning contracts:
 * - offsets_out/refs_out must outlive the dispatch on success; ownership stays
 *   in the dispatch scope until all worker harts finish.
 * - layout, tiles_x/y must match the dispatch-side layout choice for
 *   deterministic tile ownership.
 */
int sg_mt_tiles_build(const sgl_pvert *pv, const sg_prim_desc *prims, int nprim,
                     const sgl_drawstate *st, int fbw, int fbh,
                     int tiles_x, int tiles_y,
                     const sg_mt_tile_layout *layout,
                     int cull_face, int cull_mode, int front_ccw,
                     int **offsets_out, int **refs_out);

#endif /* SOFTGLES2_SG_MT_TILES_H */
