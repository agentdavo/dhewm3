#include "sg_frag.h"

void sg_frag_ctx_init(sg_frag_ctx *ctx, glsl_program *gp, const sgl_tex *const *samplers)
{
    if (!ctx) return;
    ctx->gp = gp;
    ctx->ustore = 0;   /* immediate-mode: read gp->ustore live (FrameBin sets a snapshot) */
    for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) {
        ctx->samplers[i] = (samplers && i < GLSL_MAX_SAMPLERS) ? samplers[i] : 0;
    }
}

int sg_frag_cb_scalar(void *ctx, const float *vary, float rgba[4])
{
    sg_frag_ctx *fc = (sg_frag_ctx *)ctx;
    return glsl_run_fragment_u(fc->gp, vary, fc->samplers, fc->ustore, rgba);
}

int sg_frag_cb_simt(void *ctx, const float *varys, int vl, float *out)
{
    (void)vl;
    sg_frag_ctx *fc = (sg_frag_ctx *)ctx;
    return glsl_run_fragment_simt_u(fc->gp, varys, 0, vl, fc->samplers, fc->ustore, out);
}
