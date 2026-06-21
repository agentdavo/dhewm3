/* SoftGLES2 programmable draw tile scheduler contracts.
 *
 * These helpers isolate the multi-hart tiling path from the GL API wiring so
 * primitive build + bin + per-hart scheduling have explicit handoff structs.
 */
#ifndef SOFTGLES2_SG_MT_H
#define SOFTGLES2_SG_MT_H

#include "sgl_gl.h"
#include "sg_core.h"

/* Immutable per-draw state visible to the tile path.
 * - state: raster draw state for this draw call.
 * - nvary: number of fragment-varying floats.
 * - fs_cost/fs_simt_ok: scalar cost and SIMD-compatibility hints.
 * - front_ccw/cull_face/cull_mode: face orientation and cull state.
 * - frag_scalar: scalar fragment hook (always available).
 * - frag_simt: optional SIMD fragment hook.
 * - frag_ctx: shader-specific context pointer passed to hooks.
 * - fb/state/fragment hooks are immutable for the duration of dispatch.
 */
typedef struct {
    const sgl_framebuffer *fb;
    sgl_drawstate   state;
    int             nvary;
    int             fs_cost;
    int             fs_simt_ok;
    int             front_ccw;
    int             cull_face;
    GLenum          cull_mode;
    sgl_frag_fn     fs_scalar;
    sgl_frag_simt_fn fs_simt;
    void           *frag_ctx;
} sg_draw_packet;

/* Attempt tile-parallel programmable draw. Returns non-zero when the call was
 * handled by the MT path and the caller should skip scalar raster emission.
 *
 * width/height are framebuffer dimensions used for tile sizing and active-tile
 * heuristics.
 */
int sg_mt_prog_dispatch(const sg_draw_packet *draw, GLenum mode, GLsizei count,
                        sgl_pvert *pv, int fbw, int fbh, int enabled);

#endif /* SOFTGLES2_SG_MT_H */
