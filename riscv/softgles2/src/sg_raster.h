/* Rasteriser execution contracts for softgles2.
 *
 * These are the per-triangle raster entry points used by the GL dispatch
 * (gles2.c) and the multi-hart tile path (sg_mt_raster.c). The implementations
 * live in sg_raster.c; raster.c is a thin build-system shim.
 */
#ifndef SOFTGLES2_SG_RASTER_H
#define SOFTGLES2_SG_RASTER_H

#include "sg_core.h"

/* Fixed-function triangle texture+colour pipeline (a.k.a. the "built-in"
 * GL shader-less path).  Perspective-correct barycentric interpolation
 * of colour/texcoord, depth test, stencil, alpha/additive blending. */
void sgl_raster_tri(sgl_framebuffer *fb,
                    sgl_scrvert a, sgl_scrvert b, sgl_scrvert c,
                    const sgl_drawstate *st);

/* Programmable triangle: interpolates arbitrary varyings (perspective-
 * correct) and invokes the scalar or batched-SIMT fragment hook per
 * fragment batch.  nvary is the number of float varyings (<= SGL_MAX_VARY);
 * frag / frag_simt are the pixel shader callbacks, ctx is passed through. */
void sgl_raster_tri_prog(sgl_framebuffer *fb,
                         const sgl_pvert *a, const sgl_pvert *b, const sgl_pvert *c,
                         int nvary, const sgl_drawstate *st,
                         sgl_frag_fn frag, sgl_frag_simt_fn frag_simt, void *ctx);

/* Hierarchical-Z whole-triangle occlusion cull (LESS/LEQUAL). One coarse
 * depth-max per 16x16 tile. sgl_hiz_clear seeds it (call from glClear depth);
 * sgl_hiz_refresh recomputes it from the depth buffer (call once per draw,
 * after the tile fan-out joins). sgl_hiz_enabled gates the whole feature. */
extern int  sgl_hiz_enabled;
void sgl_hiz_clear(const sgl_framebuffer *fb, float depth);
void sgl_hiz_refresh(const sgl_framebuffer *fb);
/* Refresh only the tiles overlapping the draw's screen bbox (inclusive). */
void sgl_hiz_refresh_rect(const sgl_framebuffer *fb, int rx0, int ry0, int rx1, int ry1);

/* Scalar texture sample (nearest or bilinear). Shared by the rasteriser
 * scalar path and the GLSL fragment interpreter (texture2D). */
void sgl_sample(const sgl_tex *t, float u, float v, float out[4]);

#endif /* SOFTGLES2_SG_RASTER_H */
