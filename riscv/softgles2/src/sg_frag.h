/* Fragment execution stage helpers for programmable draws. */
#ifndef SOFTGLES2_SG_FRAG_H
#define SOFTGLES2_SG_FRAG_H

#include "glsl.h"

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

#endif /* SOFTGLES2_SG_FRAG_H */
