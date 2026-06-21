/* Generic GLES2 fixed-function blending: the full glBlendFuncSeparate /
 * glBlendEquationSeparate / glBlendColor matrix, per channel.
 *
 *   result = equation( src * srcFactor, dst * dstFactor )
 *
 * applied independently to RGB (eq_rgb, src_rgb, dst_rgb) and alpha (eq_a,
 * src_a, dst_a). Pure GLES2 semantics — no RVV/Doom3 specifics in the contract;
 * an RVV (m2) and a scalar implementation share one factor/equation table.
 * Clamping to [0,1] is left to the caller's quantise/pack step. */
#ifndef SOFTGLES2_SG_BLEND_H
#define SOFTGLES2_SG_BLEND_H

#include "sg_core.h"
#include "sgl_gl.h"   /* GL_ZERO .. GL_ONE_MINUS_CONSTANT_ALPHA, GL_FUNC_* */

/* One blend factor's coefficient for a single channel.
 *   cs/cd   = this channel's src/dst value      (for *_COLOR factors)
 *   sa/da   = src/dst alpha                      (for *_ALPHA factors)
 *   cc/ca   = constant colour channel / alpha    (for CONSTANT_* factors)
 *   is_alpha= blending the alpha channel (SRC_ALPHA_SATURATE -> 1 there) */
static inline float sgl_blend_coeff_f(unsigned f, float cs, float cd,
                                      float sa, float da, float cc, float ca, int is_alpha)
{
    switch (f) {
    case GL_ZERO:                     return 0.f;
    case GL_ONE:                      return 1.f;
    case GL_SRC_COLOR:                return cs;
    case GL_ONE_MINUS_SRC_COLOR:      return 1.f - cs;
    case GL_DST_COLOR:                return cd;
    case GL_ONE_MINUS_DST_COLOR:      return 1.f - cd;
    case GL_SRC_ALPHA:                return sa;
    case GL_ONE_MINUS_SRC_ALPHA:      return 1.f - sa;
    case GL_DST_ALPHA:                return da;
    case GL_ONE_MINUS_DST_ALPHA:      return 1.f - da;
    case GL_CONSTANT_COLOR:           return cc;
    case GL_ONE_MINUS_CONSTANT_COLOR: return 1.f - cc;
    case GL_CONSTANT_ALPHA:           return ca;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return 1.f - ca;
    case GL_SRC_ALPHA_SATURATE: {
        if (is_alpha) return 1.f;
        float t = 1.f - da;
        return sa < t ? sa : t;
    }
    default:                          return 0.f;
    }
}

static inline float sgl_blend_eq_f(unsigned eq, float s, float d)
{
    switch (eq) {
    case GL_FUNC_SUBTRACT:         return s - d;
    case GL_FUNC_REVERSE_SUBTRACT: return d - s;
    default:                       return s + d;   /* GL_FUNC_ADD */
    }
}

/* Blend src colour c[0..3] against dst d[0..3] in place (c becomes the result). */
static inline void sgl_blend_generic_f(float c[4], const float d[4], const sgl_blend_state *bs)
{
    float sa = c[3], da = d[3], ca = bs->color[3];
    float out[4];
    for (int ch = 0; ch < 3; ++ch) {
        float fs = sgl_blend_coeff_f(bs->src_rgb, c[ch], d[ch], sa, da, bs->color[ch], ca, 0);
        float fd = sgl_blend_coeff_f(bs->dst_rgb, c[ch], d[ch], sa, da, bs->color[ch], ca, 0);
        out[ch] = sgl_blend_eq_f(bs->eq_rgb, c[ch] * fs, d[ch] * fd);
    }
    {
        float fs = sgl_blend_coeff_f(bs->src_a, sa, da, sa, da, ca, ca, 1);
        float fd = sgl_blend_coeff_f(bs->dst_a, sa, da, sa, da, ca, ca, 1);
        out[3] = sgl_blend_eq_f(bs->eq_a, sa * fs, da * fd);
    }
    c[0] = out[0]; c[1] = out[1]; c[2] = out[2]; c[3] = out[3];
}

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#include <riscv_vector.h>

static inline vfloat32m2_t sgl_blend_coeff_m2(unsigned f,
        vfloat32m2_t cs, vfloat32m2_t cd, vfloat32m2_t sa, vfloat32m2_t da,
        float cc, float ca, int is_alpha, size_t vl)
{
    switch (f) {
    case GL_ZERO:                     return __riscv_vfmv_v_f_f32m2(0.f, vl);
    case GL_ONE:                      return __riscv_vfmv_v_f_f32m2(1.f, vl);
    case GL_SRC_COLOR:                return cs;
    case GL_ONE_MINUS_SRC_COLOR:      return __riscv_vfrsub_vf_f32m2(cs, 1.f, vl);
    case GL_DST_COLOR:                return cd;
    case GL_ONE_MINUS_DST_COLOR:      return __riscv_vfrsub_vf_f32m2(cd, 1.f, vl);
    case GL_SRC_ALPHA:                return sa;
    case GL_ONE_MINUS_SRC_ALPHA:      return __riscv_vfrsub_vf_f32m2(sa, 1.f, vl);
    case GL_DST_ALPHA:                return da;
    case GL_ONE_MINUS_DST_ALPHA:      return __riscv_vfrsub_vf_f32m2(da, 1.f, vl);
    case GL_CONSTANT_COLOR:           return __riscv_vfmv_v_f_f32m2(cc, vl);
    case GL_ONE_MINUS_CONSTANT_COLOR: return __riscv_vfmv_v_f_f32m2(1.f - cc, vl);
    case GL_CONSTANT_ALPHA:           return __riscv_vfmv_v_f_f32m2(ca, vl);
    case GL_ONE_MINUS_CONSTANT_ALPHA: return __riscv_vfmv_v_f_f32m2(1.f - ca, vl);
    case GL_SRC_ALPHA_SATURATE:
        if (is_alpha) return __riscv_vfmv_v_f_f32m2(1.f, vl);
        return __riscv_vfmin_vv_f32m2(sa, __riscv_vfrsub_vf_f32m2(da, 1.f, vl), vl);
    default:                          return __riscv_vfmv_v_f_f32m2(0.f, vl);
    }
}

/* equation(src_term, dst*fd) with the dst term fused (vfmacc/vfnmsac/vfmsac).
 * For GL_FUNC_ADD this is bit-identical to the old vfmacc(vfmul(s,fs),d,fd)
 * alpha/additive paths, so existing RGB output is unchanged. */
static inline vfloat32m2_t sgl_blend_combine_m2(unsigned eq, vfloat32m2_t src_term,
                                                vfloat32m2_t d, vfloat32m2_t fd, size_t vl)
{
    switch (eq) {
    case GL_FUNC_SUBTRACT:         return __riscv_vfnmsac_vv_f32m2(src_term, d, fd, vl); /* src_term - d*fd */
    case GL_FUNC_REVERSE_SUBTRACT: return __riscv_vfmsac_vv_f32m2(src_term, d, fd, vl);  /* d*fd - src_term */
    default:                       return __riscv_vfmacc_vv_f32m2(src_term, d, fd, vl);  /* src_term + d*fd */
    }
}

/* Blend src (cr,cg,cb,ca) against dst (dr,dg,db,da) in place. */
static inline void sgl_blend_generic_m2(
        vfloat32m2_t *cr, vfloat32m2_t *cg, vfloat32m2_t *cb, vfloat32m2_t *ca,
        vfloat32m2_t dr, vfloat32m2_t dg, vfloat32m2_t db, vfloat32m2_t da,
        const sgl_blend_state *bs, size_t vl)
{
    vfloat32m2_t sr = *cr, sg = *cg, sb = *cb, sa = *ca;
    const float kc = bs->color[3];

    vfloat32m2_t r = sgl_blend_combine_m2(bs->eq_rgb,
        __riscv_vfmul_vv_f32m2(sr, sgl_blend_coeff_m2(bs->src_rgb, sr, dr, sa, da, bs->color[0], kc, 0, vl), vl),
        dr, sgl_blend_coeff_m2(bs->dst_rgb, sr, dr, sa, da, bs->color[0], kc, 0, vl), vl);
    vfloat32m2_t g = sgl_blend_combine_m2(bs->eq_rgb,
        __riscv_vfmul_vv_f32m2(sg, sgl_blend_coeff_m2(bs->src_rgb, sg, dg, sa, da, bs->color[1], kc, 0, vl), vl),
        dg, sgl_blend_coeff_m2(bs->dst_rgb, sg, dg, sa, da, bs->color[1], kc, 0, vl), vl);
    vfloat32m2_t b = sgl_blend_combine_m2(bs->eq_rgb,
        __riscv_vfmul_vv_f32m2(sb, sgl_blend_coeff_m2(bs->src_rgb, sb, db, sa, da, bs->color[2], kc, 0, vl), vl),
        db, sgl_blend_coeff_m2(bs->dst_rgb, sb, db, sa, da, bs->color[2], kc, 0, vl), vl);
    vfloat32m2_t a = sgl_blend_combine_m2(bs->eq_a,
        __riscv_vfmul_vv_f32m2(sa, sgl_blend_coeff_m2(bs->src_a, sa, da, sa, da, kc, kc, 1, vl), vl),
        da, sgl_blend_coeff_m2(bs->dst_a, sa, da, sa, da, kc, kc, 1, vl), vl);

    *cr = r; *cg = g; *cb = b; *ca = a;
}
#endif /* __riscv_v_intrinsic */

#endif /* SOFTGLES2_SG_BLEND_H */
