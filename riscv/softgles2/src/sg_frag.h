/* Fragment execution stage helpers for programmable draws. */
#ifndef SOFTGLES2_SG_FRAG_H
#define SOFTGLES2_SG_FRAG_H

#include "glsl.h"

#if defined(_MSC_VER)
#define SG_FRAG_TLS __declspec(thread)
#else
#define SG_FRAG_TLS __thread
#endif

typedef struct {
    glsl_program *gp;
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS];
    /* Uniform snapshot for deferred (FrameBin) shading: NULL -> read gp->ustore
     * live (immediate-mode); otherwise a per-draw snapshot so a worker hart
     * shades a recorded command with that command's uniforms. */
    const float *ustore;
} sg_frag_ctx;

void sg_frag_ctx_init(sg_frag_ctx *ctx, glsl_program *gp, const sgl_tex *const *samplers);
int sg_frag_cb_scalar(void *ctx, const float *vary, float rgba[4]);
int sg_frag_cb_simt(void *ctx, const float *varys, int vl, float *out);

/* Per-lane gl_FragCoord (pixel centre) for the active SIMT batch. Raster fills
 * these before calling the fragment hook; kernels/VM read them for heatHaze etc. */
extern SG_FRAG_TLS float sgl_simt_frag_x[SGL_SIMT_LANES];
extern SG_FRAG_TLS float sgl_simt_frag_y[SGL_SIMT_LANES];
extern SG_FRAG_TLS float sgl_scalar_frag_x, sgl_scalar_frag_y;
void sgl_simt_set_fragcoord_run(int x0, int y, int vl);
/* xs[i] is offset within the current horizontal run; pixel x = x0 + xs[i]. */
void sgl_simt_set_fragcoord_packed(int x0, const uint32_t *xs, int y, int vl);
void sgl_scalar_set_fragcoord(int x, int y);

/* Per-batch analytic screen-space derivative of every interpolated varying, in
 * normalised texcoord units (d(vary)/dx, d(vary)/dy). The rasteriser fills these
 * once per lane-batch (evaluated at the batch's first pixel) right before the
 * fragment hook, exactly like gl_FragCoord above; the interaction kernels read
 * the pair belonging to a sampler's texcoord varyings to drive mip-LOD + aniso.
 * sgl_simt_deriv_valid == 0 => not computed this batch (mip/aniso path disabled)
 * -> kernels pass NULL and the sampler keeps its prior behaviour. */
/* These are indexed by varying component, not by vector lane. All batch state
 * is thread-local: independent tiles execute fragment shaders concurrently. */
extern SG_FRAG_TLS float sgl_simt_vary_ddx[SGL_MAX_VARY];
extern SG_FRAG_TLS float sgl_simt_vary_ddy[SGL_MAX_VARY];
extern SG_FRAG_TLS int sgl_simt_deriv_valid;
/* Master gate for computing per-batch analytic derivatives (mip-LOD + aniso).
 * Tracks sgl_mip_enabled at boot; when off the rasteriser skips derivative work
 * entirely so the mip-OFF path costs exactly what it did before. */
extern int   sgl_lod_deriv_enabled;

#endif /* SOFTGLES2_SG_FRAG_H */
