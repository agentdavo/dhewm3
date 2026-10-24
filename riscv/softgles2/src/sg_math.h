/*
 * sg_math.h — shared RVV math kernels for softgles2.
 *
 * This file centralizes the vector math kernels used by the A2/A3 executors and
 * standalone RVV validation so we can tune SIMD register-group usage without
 * duplicating arithmetic across files.
 */
#ifndef SOFTGLES2_SG_MATH_H
#define SOFTGLES2_SG_MATH_H

#include "sg_core.h"

#if defined(__GNUC__) || defined(__clang__)
#  define SGL_RVV_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#  define SGL_RVV_ALWAYS_INLINE static inline
#endif

/* Scalar fallback kernels used by scalar/host paths and by scalar sections in
 * the GLSL interpreter. These use explicit FMA chains to preserve deterministic
 * ordering parity with the vectorized equivalents where required by tests.
 */
static inline float sgl_math_sqrt_f(float x)
{
    return __builtin_sqrtf(x);
}

static inline float sgl_math_abs_f(float x)
{
    return __builtin_fabsf(x);
}

static inline float sgl_math_floor_f(float x)
{
    int i = (int)x;
    return (x < 0.f && (float)i != x) ? (float)(i - 1) : (float)i;
}

static inline float sgl_math_ceil_f(float x)
{
    return -sgl_math_floor_f(-x);
}

static inline float sgl_math_log2_f(float x)
{
    union { float f; unsigned u; } v;
    v.f = x;
    float e = (float)((int)((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f;
    /* log2(m) = 2/ln(2) * atanh((m-1)/(m+1)); |t| <= 1/3.
     * The old polynomial approximated ln(m), causing jumps at powers of two. */
    float t = (m - 1.f) / (m + 1.f), t2 = t * t;
    float p = 1.f + t2 * (1.f/3.f + t2 * (1.f/5.f + t2 * (1.f/7.f + t2/9.f)));
    return e + (t * p) * 2.8853900817779268f;
}

static inline float sgl_math_exp2_f(float x)
{
	/* Shader colours below the normal float range round to zero in RGBA8.
	 * Prevent exponent-bit wraparound from producing huge positive values. */
	if (x < -126.f) return 0.f;
    float xi = sgl_math_floor_f(x);
    float xf = x - xi;
    float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * (0.0555041f + xf * (0.0096181291f + xf * (0.0013333558f + xf * 0.0001540353f)))));
    union { float f; unsigned u; } v;
    v.f = p;
    v.u += ((unsigned)(int)xi << 23);
    return v.f;
}

static inline float sgl_math_pow_f(float x, float y)
{
    if (x <= 0.f) return (y == 0.f) ? 1.f : 0.f;
    return sgl_math_exp2_f(y * sgl_math_log2_f(x));
}

static inline float sgl_math_dot_f(const float *a, const float *b, int n)
{
    if (n <= 0) return 0.0f;
    float s = a[0] * b[0];
    for (int i = 1; i < n; ++i) s = __builtin_fmaf(a[i], b[i], s);
    return s;
}

static inline void sgl_math_matvec_f(const float *m, const float *v, float *out, int n)
{
    for (int i = 0; i < n; ++i) {
        float r = m[0 * n + i] * v[0];
        for (int j = 1; j < n; ++j) r = __builtin_fmaf(m[j * n + i], v[j], r);
        out[i] = r;
    }
}

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#include <riscv_vector.h>

/* Kept for compatibility with existing docs/tests. */
#define SGL_RVV_PI_2         1.5707963267948966f
#define SGL_RVV_TWO_OVER_PI  0.63661977236758134f
#define SGL_RVV_S1          (-0.16666667f)
#define SGL_RVV_S2           0.0083333310f
#define SGL_RVV_S3          (-0.00019840874f)
#define SGL_RVV_C1          (-0.5f)
#define SGL_RVV_C2          0.041666418f
#define SGL_RVV_C3          (-0.0013888397f)

/* ---------------- m1 kernels ---------------- */

/* rsqrt: vfrsqrt7 seed + one Newton-Raphson refinement.
 * y' = y * (1.5 - 0.5*x*y*y)
 */
static inline vfloat32m1_t sgl_rvv_rsqrt_fast_m1(vfloat32m1_t x, size_t vl)
{
    vfloat32m1_t y = __riscv_vfrsqrt7_v_f32m1(x, vl);
    vfloat32m1_t hx = __riscv_vfmul_vf_f32m1(x, 0.5f, vl);
    vfloat32m1_t t = __riscv_vfmul_vv_f32m1(y, y, vl);
    vfloat32m1_t e = __riscv_vfnmsac_vv_f32m1(__riscv_vfmv_v_f_f32m1(1.5f, vl), hx, t, vl);
    return __riscv_vfmul_vv_f32m1(y, e, vl);
}

/* one more NR step: ~14-bit -> near FP32 at short cost */
static inline vfloat32m1_t sgl_rvv_rsqrt_accurate_m1(vfloat32m1_t x, size_t vl)
{
    vfloat32m1_t y = __riscv_vfrsqrt7_v_f32m1(x, vl);
    vfloat32m1_t hx = __riscv_vfmul_vf_f32m1(x, 0.5f, vl);
    vfloat32m1_t t = __riscv_vfmul_vv_f32m1(y, y, vl);
    vfloat32m1_t e = __riscv_vfnmsac_vv_f32m1(__riscv_vfmv_v_f_f32m1(1.5f, vl), hx, t, vl);
    y = __riscv_vfmul_vv_f32m1(y, e, vl);
    t = __riscv_vfmul_vv_f32m1(y, y, vl);
    e = __riscv_vfnmsac_vv_f32m1(__riscv_vfmv_v_f_f32m1(1.5f, vl), hx, t, vl);
    return __riscv_vfmul_vv_f32m1(y, e, vl);
}

/* dot4: acc = a0*b0 + a1*b1 + a2*b2 + a3*b3 */
static inline vfloat32m1_t sgl_rvv_dot4_v1_m1(vfloat32m1_t a0, vfloat32m1_t a1,
                                           vfloat32m1_t a2, vfloat32m1_t a3,
                                           vfloat32m1_t b0, vfloat32m1_t b1,
                                           vfloat32m1_t b2, vfloat32m1_t b3,
                                           size_t vl)
{
    vfloat32m1_t acc = __riscv_vfmul_vv_f32m1(a0, b0, vl);
    acc = __riscv_vfmacc_vv_f32m1(acc, a1, b1, vl);
    acc = __riscv_vfmacc_vv_f32m1(acc, a2, b2, vl);
    acc = __riscv_vfmacc_vv_f32m1(acc, a3, b3, vl);
    return acc;
}

static inline vfloat32m1_t sgl_rvv_dot_rows_v1(const float a[][SGL_SIMT_LANES],
                                               const float b[][SGL_SIMT_LANES],
                                               int n, size_t vl)
{
    if (n <= 0) return __riscv_vfmv_v_f_f32m1(0.f, vl);
    vfloat32m1_t acc = __riscv_vfmul_vv_f32m1(__riscv_vle32_v_f32m1(a[0], vl),
                                              __riscv_vle32_v_f32m1(b[0], vl), vl);
    if (n > 1) acc = __riscv_vfmacc_vv_f32m1(acc, __riscv_vle32_v_f32m1(a[1], vl), __riscv_vle32_v_f32m1(b[1], vl), vl);
    if (n > 2) acc = __riscv_vfmacc_vv_f32m1(acc, __riscv_vle32_v_f32m1(a[2], vl), __riscv_vle32_v_f32m1(b[2], vl), vl);
    if (n > 3) acc = __riscv_vfmacc_vv_f32m1(acc, __riscv_vle32_v_f32m1(a[3], vl), __riscv_vle32_v_f32m1(b[3], vl), vl);
    return acc;
}

/* dot(accumulated sum) over vector arrays:
 *   acc = Σ a[i] * b[i], with n <= 4.
 * Keeps the same op count order as the shader kernels for deterministic results.
 */
static inline vfloat32m1_t sgl_rvv_dot_rows_v1_clipped(const float a[][SGL_SIMT_LANES],
                                                       const float b[][SGL_SIMT_LANES],
                                                       int n, size_t vl, int nmax)
{
    if (n > nmax) n = nmax;
    return sgl_rvv_dot_rows_v1(a, b, n, vl);
}

static inline vfloat32m1_t sgl_rvv_dot4_m1(const float *a0, const float *a1,
                                           const float *a2, const float *a3,
                                           const float *b0, const float *b1,
                                           const float *b2, const float *b3,
                                           size_t vl)
{
    return sgl_rvv_dot4_v1_m1(__riscv_vle32_v_f32m1(a0, vl), __riscv_vle32_v_f32m1(a1, vl),
                             __riscv_vle32_v_f32m1(a2, vl), __riscv_vle32_v_f32m1(a3, vl),
                             __riscv_vle32_v_f32m1(b0, vl), __riscv_vle32_v_f32m1(b1, vl),
                             __riscv_vle32_v_f32m1(b2, vl), __riscv_vle32_v_f32m1(b3, vl),
                             vl);
}

/* col-major M[16], out_i = sum_j M[j*4+i] * v_j, same j-order as simt path */
static inline void sgl_rvv_mat4xvec4_m1(vfloat32m1_t *out0, vfloat32m1_t *out1,
                                        vfloat32m1_t *out2, vfloat32m1_t *out3,
                                        const float M[16],
                                        vfloat32m1_t x, vfloat32m1_t y,
                                        vfloat32m1_t z, vfloat32m1_t w, size_t vl)
{
    vfloat32m1_t r0 = __riscv_vfmul_vf_f32m1(x, M[0], vl);
    r0 = __riscv_vfmacc_vf_f32m1(r0, M[4], y, vl);
    r0 = __riscv_vfmacc_vf_f32m1(r0, M[8], z, vl);
    r0 = __riscv_vfmacc_vf_f32m1(r0, M[12], w, vl);
    vfloat32m1_t r1 = __riscv_vfmul_vf_f32m1(x, M[1], vl);
    r1 = __riscv_vfmacc_vf_f32m1(r1, M[5], y, vl);
    r1 = __riscv_vfmacc_vf_f32m1(r1, M[9], z, vl);
    r1 = __riscv_vfmacc_vf_f32m1(r1, M[13], w, vl);
    vfloat32m1_t r2 = __riscv_vfmul_vf_f32m1(x, M[2], vl);
    r2 = __riscv_vfmacc_vf_f32m1(r2, M[6], y, vl);
    r2 = __riscv_vfmacc_vf_f32m1(r2, M[10], z, vl);
    r2 = __riscv_vfmacc_vf_f32m1(r2, M[14], w, vl);
    vfloat32m1_t r3 = __riscv_vfmul_vf_f32m1(x, M[3], vl);
    r3 = __riscv_vfmacc_vf_f32m1(r3, M[7], y, vl);
    r3 = __riscv_vfmacc_vf_f32m1(r3, M[11], z, vl);
    r3 = __riscv_vfmacc_vf_f32m1(r3, M[15], w, vl);
    *out0 = r0; *out1 = r1; *out2 = r2; *out3 = r3;
}

/* mat3xvec3, same order as IR path: col-major M[9], rows are
 * (0,3,6), (1,4,7), (2,5,8).
 */
static inline void sgl_rvv_mat3xvec3_m1(vfloat32m1_t *out0, vfloat32m1_t *out1,
                                       vfloat32m1_t *out2, const float M[9],
                                       vfloat32m1_t x, vfloat32m1_t y,
                                       vfloat32m1_t z, size_t vl)
{
    vfloat32m1_t r0 = __riscv_vfmul_vf_f32m1(x, M[0], vl);
    r0 = __riscv_vfmacc_vf_f32m1(r0, M[3], y, vl);
    r0 = __riscv_vfmacc_vf_f32m1(r0, M[6], z, vl);
    vfloat32m1_t r1 = __riscv_vfmul_vf_f32m1(x, M[1], vl);
    r1 = __riscv_vfmacc_vf_f32m1(r1, M[4], y, vl);
    r1 = __riscv_vfmacc_vf_f32m1(r1, M[7], z, vl);
    vfloat32m1_t r2 = __riscv_vfmul_vf_f32m1(x, M[2], vl);
    r2 = __riscv_vfmacc_vf_f32m1(r2, M[5], y, vl);
    r2 = __riscv_vfmacc_vf_f32m1(r2, M[8], z, vl);
    *out0 = r0; *out1 = r1; *out2 = r2;
}

/* sin/cos in radians, branchless quadrant select, f32m1 precision (minimax). */
static inline void sgl_rvv_sincos_f_m1(vfloat32m1_t *sin_out, vfloat32m1_t *cos_out,
                                       vfloat32m1_t a, size_t vl)
{
    vint32m1_t   ki = __riscv_vfcvt_x_f_v_i32m1(__riscv_vfmul_vf_f32m1(a, SGL_RVV_TWO_OVER_PI, vl), vl);
    vfloat32m1_t kf = __riscv_vfcvt_f_x_v_f32m1(ki, vl);
    vfloat32m1_t r  = __riscv_vfnmsac_vf_f32m1(a, SGL_RVV_PI_2, kf, vl);
    vfloat32m1_t r2 = __riscv_vfmul_vv_f32m1(r, r, vl);

    vfloat32m1_t acc = __riscv_vfmv_v_f_f32m1(SGL_RVV_C3, vl);
    acc = __riscv_vfmacc_vv_f32m1(__riscv_vfmv_v_f_f32m1(SGL_RVV_C2, vl), r2, acc, vl);
    acc = __riscv_vfmacc_vv_f32m1(__riscv_vfmv_v_f_f32m1(SGL_RVV_C1, vl), r2, acc, vl);
    vfloat32m1_t cosv = __riscv_vfmacc_vv_f32m1(__riscv_vfmv_v_f_f32m1(1.0f, vl), r2, acc, vl);

    vfloat32m1_t sa = __riscv_vfmv_v_f_f32m1(SGL_RVV_S3, vl);
    sa = __riscv_vfmacc_vv_f32m1(__riscv_vfmv_v_f_f32m1(SGL_RVV_S2, vl), r2, sa, vl);
    sa = __riscv_vfmacc_vv_f32m1(__riscv_vfmv_v_f_f32m1(SGL_RVV_S1, vl), r2, sa, vl);
    vfloat32m1_t rr2 = __riscv_vfmul_vv_f32m1(r, r2, vl);
    vfloat32m1_t sinv = __riscv_vfmacc_vv_f32m1(r, rr2, sa, vl);

    vint32m1_t kq = __riscv_vand_vx_i32m1(ki, 3, vl);
    vint32m1_t b0 = __riscv_vand_vx_i32m1(kq, 1, vl);
    vint32m1_t b2 = __riscv_vand_vx_i32m1(kq, 2, vl);
    vbool32_t swap   = __riscv_vmsne_vx_i32m1_b32(b0, 0, vl);
    vbool32_t sinneg = __riscv_vmsne_vx_i32m1_b32(b2, 0, vl);
    vbool32_t cosneg = __riscv_vmsne_vx_i32m1_b32(
        __riscv_vxor_vv_i32m1(__riscv_vsll_vx_i32m1(b0, 1, vl), b2, vl), 0, vl);

    vfloat32m1_t s_sel = __riscv_vmerge_vvm_f32m1(sinv, cosv, swap, vl);
    vfloat32m1_t c_sel = __riscv_vmerge_vvm_f32m1(cosv, sinv, swap, vl);
    *sin_out = __riscv_vmerge_vvm_f32m1(s_sel, __riscv_vfneg_v_f32m1(s_sel, vl), sinneg, vl);
    *cos_out = __riscv_vmerge_vvm_f32m1(c_sel, __riscv_vfneg_v_f32m1(c_sel, vl), cosneg, vl);
}

/* ---------------- m2 kernels (used by the RVV SIMT fragment/vertex executors) ---------------- */

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_rsqrt_fast_m2(vfloat32m2_t x, size_t vl)
{
    vfloat32m2_t y = __riscv_vfrsqrt7_v_f32m2(x, vl);
    vfloat32m2_t hx = __riscv_vfmul_vf_f32m2(x, 0.5f, vl);
    vfloat32m2_t t = __riscv_vfmul_vv_f32m2(y, y, vl);
    vfloat32m2_t e = __riscv_vfnmsac_vv_f32m2(__riscv_vfmv_v_f_f32m2(1.5f, vl), hx, t, vl);
    return __riscv_vfmul_vv_f32m2(y, e, vl);
}

/* Vectorised log2/exp2/pow — lane-parallel replicas of the scalar
 * sgl_math_log2_f / sgl_math_exp2_f / sgl_math_pow_f (same constants, same
 * operation order). Used by the SIMT fragment executor for BI_POW so specular
 * pow() no longer falls back to the per-lane scalar path. Bit-exact up to
 * fma-contraction (well within the simt_validate 1e-4 tolerance). */
SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_log2_m2(vfloat32m2_t x, size_t vl)
{
    vuint32m2_t u = __riscv_vreinterpret_v_f32m2_u32m2(x);
    /* e = (float)(((u >> 23) & 0xFF) - 127) */
    vint32m2_t ei = __riscv_vreinterpret_v_u32m2_i32m2(
                        __riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(u, 23, vl), 0xFFu, vl));
    vfloat32m2_t e = __riscv_vfcvt_f_x_v_f32m2(__riscv_vsub_vx_i32m2(ei, 127, vl), vl);
    /* m = bits with exponent forced to 127 -> mantissa in [1,2) */
    vfloat32m2_t m = __riscv_vreinterpret_v_u32m2_f32m2(
                        __riscv_vor_vx_u32m2(__riscv_vand_vx_u32m2(u, 0x007FFFFFu, vl), 0x3F800000u, vl));
    vfloat32m2_t t = __riscv_vfdiv_vv_f32m2(__riscv_vfsub_vf_f32m2(m, 1.0f, vl),
                                            __riscv_vfadd_vf_f32m2(m, 1.0f, vl), vl);
    vfloat32m2_t t2 = __riscv_vfmul_vv_f32m2(t, t, vl);
    vfloat32m2_t s = __riscv_vfmv_v_f_f32m2(0.11111111f, vl);
    s = __riscv_vfmadd_vv_f32m2(s, t2, __riscv_vfmv_v_f_f32m2(0.14285715f, vl), vl);
    s = __riscv_vfmadd_vv_f32m2(s, t2, __riscv_vfmv_v_f_f32m2(0.2f, vl), vl);
    s = __riscv_vfmadd_vv_f32m2(s, t2, __riscv_vfmv_v_f_f32m2(0.33333334f, vl), vl);
    s = __riscv_vfmadd_vv_f32m2(s, t2, __riscv_vfmv_v_f_f32m2(1.0f, vl), vl);
    vfloat32m2_t ts = __riscv_vfmul_vv_f32m2(t, s, vl);
    return __riscv_vfadd_vv_f32m2(e, __riscv_vfmul_vf_f32m2(ts, 2.8853900817779268f, vl), vl);
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_exp2_m2(vfloat32m2_t x, size_t vl)
{
    vbool16_t underflow = __riscv_vmflt_vf_f32m2_b16(x, -126.f, vl);
    x = __riscv_vfmax_vf_f32m2(x, -126.f, vl);
    /* xi = floor(x); xf = x - xi */
    vint32m2_t xii = __riscv_vfcvt_x_f_v_i32m2_rm(x, __RISCV_FRM_RDN, vl);
    vfloat32m2_t xf = __riscv_vfsub_vv_f32m2(x, __riscv_vfcvt_f_x_v_f32m2(xii, vl), vl);
    /* Sixth-order exp2 polynomial: keep exponent-boundary error below an RGBA8 step. */
    vfloat32m2_t p = __riscv_vfmv_v_f_f32m2(0.0001540353f, vl);
    p = __riscv_vfmadd_vv_f32m2(p, xf, __riscv_vfmv_v_f_f32m2(0.0013333558f, vl), vl);
    p = __riscv_vfmadd_vv_f32m2(p, xf, __riscv_vfmv_v_f_f32m2(0.0096181291f, vl), vl);
    p = __riscv_vfmadd_vv_f32m2(p, xf, __riscv_vfmv_v_f_f32m2(0.0555041f, vl), vl);
    p = __riscv_vfmadd_vv_f32m2(p, xf, __riscv_vfmv_v_f_f32m2(0.2402265f, vl), vl);
    p = __riscv_vfmadd_vv_f32m2(p, xf, __riscv_vfmv_v_f_f32m2(0.6931472f, vl), vl);
    p = __riscv_vfmadd_vv_f32m2(p, xf, __riscv_vfmv_v_f_f32m2(1.0f, vl), vl);
    /* p.u += xi << 23  (scale by 2^xi) */
    vint32m2_t pii = __riscv_vadd_vv_i32m2(__riscv_vreinterpret_v_f32m2_i32m2(p),
                                           __riscv_vsll_vx_i32m2(xii, 23, vl), vl);
    return __riscv_vmerge_vvm_f32m2(__riscv_vreinterpret_v_i32m2_f32m2(pii),
        __riscv_vfmv_v_f_f32m2(0.f, vl), underflow, vl);
}

/* Exact successive-square path for common integer exponents. Shared by the
 * scalar-exponent helper and the generic pow so kernel vs SIMT VM stay identical. */
SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_pow_posint_m2(vfloat32m2_t x, int n, size_t vl)
{
    vbool16_t xle0 = __riscv_vmfle_vf_f32m2_b16(x, 0.f, vl);
    vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, vl);
    if (n == 0) return __riscv_vfmv_v_f_f32m2(1.f, vl);
    if (n == 1) return __riscv_vmerge_vvm_f32m2(x, z, xle0, vl);
    vfloat32m2_t r = x;
    if (n == 2) {
        r = __riscv_vfmul_vv_f32m2(x, x, vl);
    } else if (n == 16) {
        r = __riscv_vfmul_vv_f32m2(x, x, vl);   /* x^2 */
        r = __riscv_vfmul_vv_f32m2(r, r, vl);   /* x^4 */
        r = __riscv_vfmul_vv_f32m2(r, r, vl);   /* x^8 */
        r = __riscv_vfmul_vv_f32m2(r, r, vl);   /* x^16 */
    } else {
        /* Fallback: repeated multiply (rare). */
        r = __riscv_vfmv_v_f_f32m2(1.f, vl);
        for (int i = 0; i < n; ++i) r = __riscv_vfmul_vv_f32m2(r, x, vl);
    }
    return __riscv_vmerge_vvm_f32m2(r, z, xle0, vl);
}

/* pow(x,y) = (x<=0) ? (y==0?1:0) : exp2(y*log2(x)) — matches sgl_math_pow_f,
 * except for exact integer exponents 0/1/2/16 which use successive multiplies so
 * the interaction kernel and SIMT VM agree bit-for-bit (and TCG is faster). */
SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_pow_m2(vfloat32m2_t x, vfloat32m2_t y, size_t vl)
{
    float y0 = __riscv_vfmv_f_s_f32m2_f32(y);
    if ((y0 == 0.f || y0 == 1.f || y0 == 2.f || y0 == 16.f) &&
        __riscv_vcpop_m_b16(__riscv_vmfne_vf_f32m2_b16(y, y0, vl), vl) == 0)
        return sgl_rvv_pow_posint_m2(x, (int)y0, vl);
    vfloat32m2_t e = sgl_rvv_exp2_m2(__riscv_vfmul_vv_f32m2(y, sgl_rvv_log2_m2(x, vl), vl), vl);
    /* guard lanes where x<=0: result is 1 when y==0, else 0 */
    vbool16_t xle0 = __riscv_vmfle_vf_f32m2_b16(x, 0.f, vl);
    vbool16_t yz   = __riscv_vmfeq_vf_f32m2_b16(y, 0.f, vl);
    vfloat32m2_t guard = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, vl),
                                                  __riscv_vfmv_v_f_f32m2(1.f, vl), yz, vl);
    return __riscv_vmerge_vvm_f32m2(e, guard, xle0, vl);
}

/* Scalar-exponent pow for hot uniforms (Doom gamma / colorScale / specular). */
SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_pow_sf_m2(vfloat32m2_t x, float y, size_t vl)
{
    if (y == 0.f || y == 1.f || y == 2.f || y == 16.f)
        return sgl_rvv_pow_posint_m2(x, (int)y, vl);
    return sgl_rvv_pow_m2(x, __riscv_vfmv_v_f_f32m2(y, vl), vl);
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_dot4_v1_m2(vfloat32m2_t a0, vfloat32m2_t a1,
                                            vfloat32m2_t a2, vfloat32m2_t a3,
                                            vfloat32m2_t b0, vfloat32m2_t b1,
                                            vfloat32m2_t b2, vfloat32m2_t b3,
                                            size_t vl)
{
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(a0, b0, vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, a1, b1, vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, a2, b2, vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, a3, b3, vl);
    return acc;
}

/* Generic dot helper: accumulates Σ a[i] * b[i], 1 <= n <= 4.
 * Kept as an explicit in-register chain to preserve bit-exact order when callers
 * rely on deterministic FMA ordering.
 */
SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_dot_v2_m2(const float a[][SGL_SIMT_LANES],
                                             const float b[][SGL_SIMT_LANES],
                                             int n, size_t vl)
{
    if (n <= 0) return __riscv_vfmv_v_f_f32m2(0.f, vl);
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(a[0], vl),
                                              __riscv_vle32_v_f32m2(b[0], vl), vl);
    if (n > 1) acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[1], vl), __riscv_vle32_v_f32m2(b[1], vl), vl);
    if (n > 2) acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[2], vl), __riscv_vle32_v_f32m2(b[2], vl), vl);
    if (n > 3) acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[3], vl), __riscv_vle32_v_f32m2(b[3], vl), vl);
    return acc;
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_dot3_v2_m2(const float a[][SGL_SIMT_LANES],
                                                      const float b[][SGL_SIMT_LANES],
                                                      size_t vl)
{
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(a[0], vl),
                                              __riscv_vle32_v_f32m2(b[0], vl), vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[1], vl), __riscv_vle32_v_f32m2(b[1], vl), vl);
    return __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[2], vl), __riscv_vle32_v_f32m2(b[2], vl), vl);
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_dot4_v2_m2(const float a[][SGL_SIMT_LANES],
                                                      const float b[][SGL_SIMT_LANES],
                                                      size_t vl)
{
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(a[0], vl),
                                              __riscv_vle32_v_f32m2(b[0], vl), vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[1], vl), __riscv_vle32_v_f32m2(b[1], vl), vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[2], vl), __riscv_vle32_v_f32m2(b[2], vl), vl);
    return __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(a[3], vl), __riscv_vle32_v_f32m2(b[3], vl), vl);
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_length3_v2(const float v[][SGL_SIMT_LANES], size_t vl)
{
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(v[0], vl),
                                              __riscv_vle32_v_f32m2(v[0], vl), vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(v[1], vl), __riscv_vle32_v_f32m2(v[1], vl), vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, __riscv_vle32_v_f32m2(v[2], vl), __riscv_vle32_v_f32m2(v[2], vl), vl);
    return __riscv_vfsqrt_v_f32m2(acc, vl);
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_distance2_v2(const float a[][SGL_SIMT_LANES],
                                                const float b[][SGL_SIMT_LANES],
                                                int n, size_t vl)
{
    if (n <= 0) return __riscv_vfmv_v_f_f32m2(0.f, vl);
    vfloat32m2_t d = __riscv_vfsub_vv_f32m2(__riscv_vle32_v_f32m2(a[0], vl),
                                            __riscv_vle32_v_f32m2(b[0], vl), vl);
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(d, d, vl);
    for (int i = 1; i < n; ++i) {
        d = __riscv_vfsub_vv_f32m2(__riscv_vle32_v_f32m2(a[i], vl), __riscv_vle32_v_f32m2(b[i], vl), vl);
        acc = __riscv_vfmacc_vv_f32m2(acc, d, d, vl);
    }
    return acc;
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_distance2_3_v2(const float a[][SGL_SIMT_LANES],
                                                          const float b[][SGL_SIMT_LANES],
                                                          size_t vl)
{
    vfloat32m2_t dx = __riscv_vfsub_vv_f32m2(__riscv_vle32_v_f32m2(a[0], vl), __riscv_vle32_v_f32m2(b[0], vl), vl);
    vfloat32m2_t dy = __riscv_vfsub_vv_f32m2(__riscv_vle32_v_f32m2(a[1], vl), __riscv_vle32_v_f32m2(b[1], vl), vl);
    vfloat32m2_t dz = __riscv_vfsub_vv_f32m2(__riscv_vle32_v_f32m2(a[2], vl), __riscv_vle32_v_f32m2(b[2], vl), vl);
    vfloat32m2_t acc = __riscv_vfmul_vv_f32m2(dx, dx, vl);
    acc = __riscv_vfmacc_vv_f32m2(acc, dy, dy, vl);
    return __riscv_vfmacc_vv_f32m2(acc, dz, dz, vl);
}

/* 3-vector cross product in SoA.
 * out[0] = ay*bz - az*by
 * out[1] = az*bx - ax*bz
 * out[2] = ax*by - ay*bx
 */
SGL_RVV_ALWAYS_INLINE void sgl_rvv_cross3_v2(const float a[][SGL_SIMT_LANES], const float b[][SGL_SIMT_LANES],
                                    float out[][SGL_SIMT_LANES], size_t vl)
{
    vfloat32m2_t ax = __riscv_vle32_v_f32m2(a[0], vl), ay = __riscv_vle32_v_f32m2(a[1], vl), az = __riscv_vle32_v_f32m2(a[2], vl);
    vfloat32m2_t bx = __riscv_vle32_v_f32m2(b[0], vl), by = __riscv_vle32_v_f32m2(b[1], vl), bz = __riscv_vle32_v_f32m2(b[2], vl);
    __riscv_vse32_v_f32m2(out[0], __riscv_vfnmsac_vv_f32m2(__riscv_vfmul_vv_f32m2(ay, bz, vl), az, by, vl), vl);
    __riscv_vse32_v_f32m2(out[1], __riscv_vfnmsac_vv_f32m2(__riscv_vfmul_vv_f32m2(az, bx, vl), ax, bz, vl), vl);
    __riscv_vse32_v_f32m2(out[2], __riscv_vfnmsac_vv_f32m2(__riscv_vfmul_vv_f32m2(ax, by, vl), ay, bx, vl), vl);
}

/* I - 2*dot(N, I)*N for vector width m (1..4).
 * `I` and `N` arrays must contain at least m rows.
 */
SGL_RVV_ALWAYS_INLINE void sgl_rvv_reflect_v2(const float I[][SGL_SIMT_LANES], const float N[][SGL_SIMT_LANES],
                                     int m, float out[][SGL_SIMT_LANES], size_t vl)
{
    if (m < 0) m = 0;
    if (m > 4) m = 4;
    vfloat32m2_t d = sgl_rvv_dot_v2_m2(I, N, m, vl);
    d = __riscv_vfadd_vv_f32m2(d, d, vl);      /* 2*d */
    for (int i = 0; i < m; ++i) {
        __riscv_vse32_v_f32m2(out[i],
                              __riscv_vfnmsac_vv_f32m2(__riscv_vle32_v_f32m2(I[i], vl), d,
                                                       __riscv_vle32_v_f32m2(N[i], vl), vl),
                              vl);
    }
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_reflect3_v2(const float I[][SGL_SIMT_LANES], const float N[][SGL_SIMT_LANES],
                                               float out[][SGL_SIMT_LANES], size_t vl)
{
    vfloat32m2_t ix = __riscv_vle32_v_f32m2(I[0], vl);
    vfloat32m2_t iy = __riscv_vle32_v_f32m2(I[1], vl);
    vfloat32m2_t iz = __riscv_vle32_v_f32m2(I[2], vl);
    vfloat32m2_t nx = __riscv_vle32_v_f32m2(N[0], vl);
    vfloat32m2_t ny = __riscv_vle32_v_f32m2(N[1], vl);
    vfloat32m2_t nz = __riscv_vle32_v_f32m2(N[2], vl);
    vfloat32m2_t d = __riscv_vfmul_vv_f32m2(ix, nx, vl);
    d = __riscv_vfmacc_vv_f32m2(d, iy, ny, vl);
    d = __riscv_vfmacc_vv_f32m2(d, iz, nz, vl);
    d = __riscv_vfadd_vv_f32m2(d, d, vl);
    __riscv_vse32_v_f32m2(out[0], __riscv_vfnmsac_vv_f32m2(ix, d, nx, vl), vl);
    __riscv_vse32_v_f32m2(out[1], __riscv_vfnmsac_vv_f32m2(iy, d, ny, vl), vl);
    __riscv_vse32_v_f32m2(out[2], __riscv_vfnmsac_vv_f32m2(iz, d, nz, vl), vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_normalize_v2(const float v[][SGL_SIMT_LANES], int n,
                                       float out[][SGL_SIMT_LANES], size_t vl)
{
    if (n < 0) n = 0;
    if (n > 4) n = 4;
    vfloat32m2_t s = sgl_rvv_dot_v2_m2(v, v, n, vl);
    vfloat32m2_t inv = sgl_rvv_rsqrt_fast_m2(s, vl);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, vl), inv,
                                   __riscv_vmfgt_vf_f32m2_b16(s, 0.f, vl), vl);
    for (int i = 0; i < n; ++i) {
        __riscv_vse32_v_f32m2(out[i], __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(v[i], vl), inv, vl), vl);
    }
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_normalize3_v2(const float v[][SGL_SIMT_LANES],
                                                 float out[][SGL_SIMT_LANES], size_t vl)
{
    vfloat32m2_t x = __riscv_vle32_v_f32m2(v[0], vl);
    vfloat32m2_t y = __riscv_vle32_v_f32m2(v[1], vl);
    vfloat32m2_t z = __riscv_vle32_v_f32m2(v[2], vl);
    vfloat32m2_t s = __riscv_vfmul_vv_f32m2(x, x, vl);
    s = __riscv_vfmacc_vv_f32m2(s, y, y, vl);
    s = __riscv_vfmacc_vv_f32m2(s, z, z, vl);
    vfloat32m2_t inv = sgl_rvv_rsqrt_fast_m2(s, vl);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, vl), inv,
                                   __riscv_vmfgt_vf_f32m2_b16(s, 0.f, vl), vl);
    __riscv_vse32_v_f32m2(out[0], __riscv_vfmul_vv_f32m2(x, inv, vl), vl);
    __riscv_vse32_v_f32m2(out[1], __riscv_vfmul_vv_f32m2(y, inv, vl), vl);
    __riscv_vse32_v_f32m2(out[2], __riscv_vfmul_vv_f32m2(z, inv, vl), vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_mat4xvec4_m2(float (*out0)[SGL_SIMT_LANES], float (*out1)[SGL_SIMT_LANES],
                                        float (*out2)[SGL_SIMT_LANES], float (*out3)[SGL_SIMT_LANES],
                                        const float M[16],
                                        vfloat32m2_t x, vfloat32m2_t y,
                                        vfloat32m2_t z, vfloat32m2_t w, size_t vl)
{
    vfloat32m2_t r0 = __riscv_vfmul_vf_f32m2(x, M[0], vl);
    r0 = __riscv_vfmacc_vf_f32m2(r0, M[4], y, vl);
    r0 = __riscv_vfmacc_vf_f32m2(r0, M[8], z, vl);
    r0 = __riscv_vfmacc_vf_f32m2(r0, M[12], w, vl);
    __riscv_vse32_v_f32m2(*out0, r0, vl);

    vfloat32m2_t r1 = __riscv_vfmul_vf_f32m2(x, M[1], vl);
    r1 = __riscv_vfmacc_vf_f32m2(r1, M[5], y, vl);
    r1 = __riscv_vfmacc_vf_f32m2(r1, M[9], z, vl);
    r1 = __riscv_vfmacc_vf_f32m2(r1, M[13], w, vl);
    __riscv_vse32_v_f32m2(*out1, r1, vl);

    vfloat32m2_t r2 = __riscv_vfmul_vf_f32m2(x, M[2], vl);
    r2 = __riscv_vfmacc_vf_f32m2(r2, M[6], y, vl);
    r2 = __riscv_vfmacc_vf_f32m2(r2, M[10], z, vl);
    r2 = __riscv_vfmacc_vf_f32m2(r2, M[14], w, vl);
    __riscv_vse32_v_f32m2(*out2, r2, vl);

    vfloat32m2_t r3 = __riscv_vfmul_vf_f32m2(x, M[3], vl);
    r3 = __riscv_vfmacc_vf_f32m2(r3, M[7], y, vl);
    r3 = __riscv_vfmacc_vf_f32m2(r3, M[11], z, vl);
    r3 = __riscv_vfmacc_vf_f32m2(r3, M[15], w, vl);
    __riscv_vse32_v_f32m2(*out3, r3, vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_mat3xvec3_m2(float (*out0)[SGL_SIMT_LANES], float (*out1)[SGL_SIMT_LANES],
                                        float (*out2)[SGL_SIMT_LANES], const float M[9],
                                        vfloat32m2_t x, vfloat32m2_t y,
                                        vfloat32m2_t z, size_t vl)
{
    vfloat32m2_t r0 = __riscv_vfmul_vf_f32m2(x, M[0], vl);
    r0 = __riscv_vfmacc_vf_f32m2(r0, M[3], y, vl);
    r0 = __riscv_vfmacc_vf_f32m2(r0, M[6], z, vl);
    __riscv_vse32_v_f32m2(*out0, r0, vl);

    vfloat32m2_t r1 = __riscv_vfmul_vf_f32m2(x, M[1], vl);
    r1 = __riscv_vfmacc_vf_f32m2(r1, M[4], y, vl);
    r1 = __riscv_vfmacc_vf_f32m2(r1, M[7], z, vl);
    __riscv_vse32_v_f32m2(*out1, r1, vl);

    vfloat32m2_t r2 = __riscv_vfmul_vf_f32m2(x, M[2], vl);
    r2 = __riscv_vfmacc_vf_f32m2(r2, M[5], y, vl);
    r2 = __riscv_vfmacc_vf_f32m2(r2, M[8], z, vl);
    __riscv_vse32_v_f32m2(*out2, r2, vl);
}

/* Coherent-access fast path (Lever 2). A screen-aligned blit interpolates u
 * linearly, so for a run of fragments on one texture row the texel x-indices
 * form an exact unit ramp (xi[k] = xi0 + k) and yi is uniform — the gather then
 * reads a CONTIGUOUS span. Detect that (cheap: 2 vmv.x.s + a vid compare) and
 * replace the indexed vluxei32 (~500 TCG cyc/64 lanes, a per-lane loop) with a
 * single unit-stride vle32 (~50 cyc, a block load). Provably bit-identical: the
 * ramp+uniform check guarantees the contiguous load addresses exactly equal the
 * gather addresses; any deviation (clamped tail, scaled/projective row) fails the
 * check and falls through to the indexed gather. Overhead on the non-coherent
 * (3D) path is ~4 ops, <1% of a real gather. Helps nearest AND bilinear (each
 * bilinear corner row is itself a unit ramp) and both the VM and kernel paths. */
/* Item 4 (RVV audit): in the shipping build this is a compile-time constant 1, so
 * the outer enable-branch (and the .sdata load of the global) folds away. The inner
 * unit-ramp check below still chooses vle32 vs the indexed gather at runtime — that
 * decision is data-dependent and must stay. overlay_validate flips it at runtime to
 * prove coherent==indexed bit-exact, so it builds with -DSGL_GATHER_RUNTIME_TOGGLE. */
#ifdef SGL_GATHER_RUNTIME_TOGGLE
extern int sgl_rvv_coherent_gather_enabled;   /* default 1; off = force indexed */
#else
#define sgl_rvv_coherent_gather_enabled 1
#endif

SGL_RVV_ALWAYS_INLINE vuint32m2_t sgl_rvv_tex_gather_m2(const sgl_tex *t, vuint32m2_t xi,
                                                vuint32m2_t yi, size_t vl)
{
    if (sgl_rvv_coherent_gather_enabled) {
        uint32_t yi0 = __riscv_vmv_x_s_u32m2_u32(yi);
        if (__riscv_vcpop_m_b16(__riscv_vmsne_vx_u32m2_b16(yi, yi0, vl), vl) == 0) {  /* one row */
            uint32_t xi0 = __riscv_vmv_x_s_u32m2_u32(xi);
            vuint32m2_t unit = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), xi0, vl);
            if (__riscv_vcpop_m_b16(__riscv_vmsne_vv_u32m2_b16(xi, unit, vl), vl) == 0)  /* unit ramp */
                return __riscv_vle32_v_u32m2(t->texels + (size_t)yi0 * (size_t)t->w + xi0, vl);
        }
    }
    vuint32m2_t idx = __riscv_vadd_vv_u32m2(__riscv_vmul_vx_u32m2(yi, (uint32_t)t->w, vl), xi, vl);
    return __riscv_vluxei32_v_u32m2(t->texels, __riscv_vsll_vx_u32m2(idx, 2, vl), vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_unpack_rgba_m2(vuint32m2_t p, size_t vl, float inv255,
                                          vfloat32m2_t *r, vfloat32m2_t *g,
                                          vfloat32m2_t *b, vfloat32m2_t *a)
{
    *r = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(p, 0xFFu, vl), vl), inv255, vl);
    *g = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(p, 8, vl), 0xFFu, vl), vl), inv255, vl);
    *b = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(p, 16, vl), 0xFFu, vl), vl), inv255, vl);
    *a = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vsrl_vx_u32m2(p, 24, vl), vl), inv255, vl);
}

SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_lerp_m2(vfloat32m2_t a, vfloat32m2_t b, vfloat32m2_t d, size_t vl)
{
    return __riscv_vfmacc_vv_f32m2(a, d, __riscv_vfsub_vv_f32m2(b, a, vl), vl);
}

/* Extract one RGBA8 channel from a packed texel word as a normalised float.
 * `shift` is the byte position (0/8/16/24); bit-identical to sgl_rvv_unpack_rgba_m2
 * for the corresponding channel (alpha is the top byte, needs no mask). Used by
 * the per-channel bilinear path so only one channel of the 4 corners is live at a
 * time. `shift` is always a compile-time constant at the call site, so the branch
 * and the no-op srl fold away. */
SGL_RVV_ALWAYS_INLINE vfloat32m2_t sgl_rvv_chan_m2(vuint32m2_t p, int shift, float inv255, size_t vl)
{
    vuint32m2_t b = shift ? __riscv_vsrl_vx_u32m2(p, (size_t)shift, vl) : p;
    if (shift != 24) b = __riscv_vand_vx_u32m2(b, 0xFFu, vl);
    return __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(b, vl), inv255, vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_wrap01_m2(vfloat32m2_t *u, vfloat32m2_t *v, size_t vl, int wrap)
{
    if (wrap) {
        vint32m2_t uf = __riscv_vfcvt_x_f_v_i32m2_rm(*u, __RISCV_FRM_RDN, vl);
        vint32m2_t vf = __riscv_vfcvt_x_f_v_i32m2_rm(*v, __RISCV_FRM_RDN, vl);
        *u = __riscv_vfsub_vv_f32m2(*u, __riscv_vfcvt_f_x_v_f32m2(uf, vl), vl);
        *v = __riscv_vfsub_vv_f32m2(*v, __riscv_vfcvt_f_x_v_f32m2(vf, vl), vl);
    }
    *u = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(*u, 0.f, vl), 1.f, vl);
    *v = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(*v, 0.f, vl), 1.f, vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample2d_nearest_m2(const sgl_tex *tex, vfloat32m2_t u, vfloat32m2_t v,
                                                       size_t vl, float inv255, int wrap,
                                                       vfloat32m2_t *tr, vfloat32m2_t *tg,
                                                       vfloat32m2_t *tb, vfloat32m2_t *ta)
{
    sgl_rvv_wrap01_m2(&u, &v, vl, wrap);
    vuint32m2_t xi = __riscv_vminu_vx_u32m2(
        __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfmul_vf_f32m2(u, (float)tex->w, vl), vl),
        (uint32_t)(tex->w - 1), vl);
    vuint32m2_t yi = __riscv_vminu_vx_u32m2(
        __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfmul_vf_f32m2(v, (float)tex->h, vl), vl),
        (uint32_t)(tex->h - 1), vl);
    vuint32m2_t word = sgl_rvv_tex_gather_m2(tex, xi, yi, vl);
    if (tex->is_depth) {
        /* Depth texel holds raw float bits: return (d,d,d,1) with d reinterpreted. */
        vfloat32m2_t d = __riscv_vreinterpret_v_u32m2_f32m2(word);
        *tr = d; *tg = d; *tb = d;
        *ta = __riscv_vfmv_v_f_f32m2(1.0f, vl);
        return;
    }
    sgl_rvv_unpack_rgba_m2(word, vl, inv255, tr, tg, tb, ta);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample2d_bilinear_m2(const sgl_tex *tex, vfloat32m2_t u, vfloat32m2_t v,
                                                        size_t vl, float inv255, int wrap,
                                                        vfloat32m2_t *tr, vfloat32m2_t *tg,
                                                        vfloat32m2_t *tb, vfloat32m2_t *ta)
{
    sgl_rvv_wrap01_m2(&u, &v, vl, wrap);
    vfloat32m2_t fx = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(u, (float)tex->w, vl), 0.5f, vl);
    vfloat32m2_t fy = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(v, (float)tex->h, vl), 0.5f, vl);
    vint32m2_t x0 = __riscv_vfcvt_x_f_v_i32m2_rm(fx, __RISCV_FRM_RDN, vl);
    vint32m2_t y0 = __riscv_vfcvt_x_f_v_i32m2_rm(fy, __RISCV_FRM_RDN, vl);
    vfloat32m2_t dx = __riscv_vfsub_vv_f32m2(fx, __riscv_vfcvt_f_x_v_f32m2(x0, vl), vl);
    vfloat32m2_t dy = __riscv_vfsub_vv_f32m2(fy, __riscv_vfcvt_f_x_v_f32m2(y0, vl), vl);
    vint32m2_t x1 = __riscv_vadd_vx_i32m2(x0, 1, vl);
    vint32m2_t y1 = __riscv_vadd_vx_i32m2(y0, 1, vl);
    x0 = __riscv_vmax_vx_i32m2(__riscv_vmin_vx_i32m2(x0, tex->w - 1, vl), 0, vl);
    x1 = __riscv_vmax_vx_i32m2(__riscv_vmin_vx_i32m2(x1, tex->w - 1, vl), 0, vl);
    y0 = __riscv_vmax_vx_i32m2(__riscv_vmin_vx_i32m2(y0, tex->h - 1, vl), 0, vl);
    y1 = __riscv_vmax_vx_i32m2(__riscv_vmin_vx_i32m2(y1, tex->h - 1, vl), 0, vl);
    vuint32m2_t x0u = __riscv_vreinterpret_v_i32m2_u32m2(x0), x1u = __riscv_vreinterpret_v_i32m2_u32m2(x1);
    vuint32m2_t y0u = __riscv_vreinterpret_v_i32m2_u32m2(y0), y1u = __riscv_vreinterpret_v_i32m2_u32m2(y1);

    /* Keep only the 4 PACKED corner words live (4x vuint32m2 = 8 vregs), then
     * resolve ONE channel at a time. Item 1 (RVV audit): unpacking all 16 corner
     * channels up front needs 32 vregs = the whole file, forcing 88 whole-register
     * (vs2r/vl2re32) spills in glsl_fs_kernel_interaction; per-channel keeps the
     * live set ~20 vregs and spills ~0. Bit-identical: same per-channel lerp order
     * (horizontal in dx, then vertical in dy) as the original unpack-then-lerp. */
    /* Degenerate-axis fast path. A 1-texel-high texture clamps both y0 and y1 to
     * 0, so p01 == p00 and p11 == p10 and the outer lerp collapses to
     * a + dy*(a - a) == a EXACTLY (texels come from vfcvt.f.xu so are never -0.0,
     * the one case where that identity would not hold). Half the gathers and half
     * the per-channel lerps are therefore dead weight. This is not a Doom-specific
     * hack: Doom 3's _specularTable is 256x1 and is sampled on every interaction
     * fragment, but any GLES2 Nx1/1xN gradient LUT hits it. Bit-exact. */
    if (tex->h == 1) {
        vuint32m2_t q0 = sgl_rvv_tex_gather_m2(tex, x0u, y0u, vl);
        vuint32m2_t q1 = sgl_rvv_tex_gather_m2(tex, x1u, y0u, vl);
#define SGL_BILERP_CH1(OUT, SH) \
        *(OUT) = sgl_rvv_lerp_m2(sgl_rvv_chan_m2(q0, (SH), inv255, vl), \
                                 sgl_rvv_chan_m2(q1, (SH), inv255, vl), dx, vl)
        SGL_BILERP_CH1(tr, 0);
        SGL_BILERP_CH1(tg, 8);
        SGL_BILERP_CH1(tb, 16);
        SGL_BILERP_CH1(ta, 24);
#undef SGL_BILERP_CH1
        return;
    }
    if (tex->w == 1) {
        vuint32m2_t q0 = sgl_rvv_tex_gather_m2(tex, x0u, y0u, vl);
        vuint32m2_t q1 = sgl_rvv_tex_gather_m2(tex, x0u, y1u, vl);
#define SGL_BILERP_CW1(OUT, SH) \
        *(OUT) = sgl_rvv_lerp_m2(sgl_rvv_chan_m2(q0, (SH), inv255, vl), \
                                 sgl_rvv_chan_m2(q1, (SH), inv255, vl), dy, vl)
        SGL_BILERP_CW1(tr, 0);
        SGL_BILERP_CW1(tg, 8);
        SGL_BILERP_CW1(tb, 16);
        SGL_BILERP_CW1(ta, 24);
#undef SGL_BILERP_CW1
        return;
    }

    vuint32m2_t p00 = sgl_rvv_tex_gather_m2(tex, x0u, y0u, vl);
    vuint32m2_t p10 = sgl_rvv_tex_gather_m2(tex, x1u, y0u, vl);
    vuint32m2_t p01 = sgl_rvv_tex_gather_m2(tex, x0u, y1u, vl);
    vuint32m2_t p11 = sgl_rvv_tex_gather_m2(tex, x1u, y1u, vl);

#define SGL_BILERP_CH(OUT, SH) do { \
        vfloat32m2_t c00 = sgl_rvv_chan_m2(p00, (SH), inv255, vl); \
        vfloat32m2_t c10 = sgl_rvv_chan_m2(p10, (SH), inv255, vl); \
        vfloat32m2_t c01 = sgl_rvv_chan_m2(p01, (SH), inv255, vl); \
        vfloat32m2_t c11 = sgl_rvv_chan_m2(p11, (SH), inv255, vl); \
        *(OUT) = sgl_rvv_lerp_m2(sgl_rvv_lerp_m2(c00, c10, dx, vl), \
                                 sgl_rvv_lerp_m2(c01, c11, dx, vl), dy, vl); \
    } while (0)
    SGL_BILERP_CH(tr, 0);
    SGL_BILERP_CH(tg, 8);
    SGL_BILERP_CH(tb, 16);
    SGL_BILERP_CH(ta, 24);
#undef SGL_BILERP_CH
}

/* Texture sample helper shared by raster+fragment SIMT paths.
 * `min_filter` is interpreted like the scalar sgl_sample convention:
 *   0: nearest
 *   1: bilinear
 * `wrap` follows sgl_sample semantics: 1 = repeat, otherwise clamp-to-edge.
 *
 * Fast-path contract: same arithmetic layout and wrapping semantics as the scalar
 * `sgl_sample()` so that all backends remain bit-compatible at sampling points.
 */
/* Lever 5: mag-downgrade. When the texture is MAGNIFIED (<1 texel per screen
 * pixel — an upscaled blit, e.g. Doom 3's POT background shown on 800x600),
 * bilinear's 4 corner gathers interpolate between texels that are several pixels
 * apart; nearest costs ONE gather and the visible difference is small. The per-
 * pixel texcoord stride is read cheaply from the batch (lanes are consecutive
 * screen pixels): texels/pixel = |du/dx|*texW. NOT bit-exact (visual) -> gated;
 * default on, sgl_rvv_mag_downgrade_enabled=0 restores true bilinear. */
extern int sgl_rvv_mag_downgrade_enabled;
/* Kill switch for the live-varying interpolation skip (sgl_drawstate.
 * vary_live_mask). On by default; set to 0 to bisect a rendering difference
 * back to a kernel whose declared live set is wrong. */
extern int sgl_vary_liveness_enabled;
/* Blend between two mip levels (true trilinear). Off: pick the nearest level,
 * which halves the texture fetches. */
extern int sgl_mip_trilinear_enabled;
/* Master switch for mip sampling. Default OFF: under QEMU TCG the guest cache
 * hierarchy is not emulated, so the locality win mips exist for is invisible
 * while their LOD cost is fully counted -- measured a large net loss. Turn ON
 * for real hardware and for image quality (it also fixes minification aliasing). */
extern int sgl_mip_enabled;
extern int sgl_heathaze_deform;   /* 0 = clean undistorted glass (path-A); 1 = normal-map deform */

/* Mip level selection.
 *
 * The batch's lanes are consecutive screen pixels, so the texel stride per pixel
 * -- exactly GL's rho -- falls out of the first/last lane the same way the
 * mag-downgrade test already reads it. That makes LOD ~6 scalar ops per batch
 * with no per-lane derivative machinery, and one level for the whole batch.
 *
 * Per-batch rather than per-lane is an approximation: a batch spanning a sharp
 * perspective gradient gets one level for all 64 lanes. Within a 64-pixel span
 * of one triangle that is a fraction of a level, which is well below what a
 * trilinear blend would smooth out anyway.
 *
 * Returns the level and leaves `frac` as the blend weight toward level+1. */
SGL_RVV_ALWAYS_INLINE int sgl_rvv_pick_lod(const sgl_tex *tex, float rho, float *frac)
{
    *frac = 0.f;
    if (tex->nmip <= 1 || rho <= 1.0f) return 0;
    /* log2 via the float exponent: rho >= 1 here, so the biased exponent is the
     * integer part and the mantissa is the fraction. Cheaper than a real log2
     * and exact at level boundaries, which is where banding would show. */
    union { float f; uint32_t u; } bits;
    bits.f = rho;
    int lod = (int)((bits.u >> 23) & 0xffu) - 127;
    if (lod < 0) lod = 0;
    if (lod >= tex->nmip - 1) return tex->nmip - 1;
    bits.u = (bits.u & 0x007fffffu) | 0x3f800000u;   /* mantissa in [1,2) */
    *frac = bits.f - 1.0f;
    return lod;
}

/* Build a level view. The existing samplers only read w/h/texels, so a stack
 * copy with those three swapped samples that level with no other changes. */
SGL_RVV_ALWAYS_INLINE sgl_tex sgl_rvv_mip_view(const sgl_tex *tex, int level)
{
    sgl_tex v = *tex;
    if (level > 0 && level < tex->nmip && tex->mips && tex->mips[level].texels) {
        v.w = tex->mips[level].w;
        v.h = tex->mips[level].h;
        v.texels = tex->mips[level].texels;
    }
    return v;
}

/* Blend a bilinear fetch at (uk,vk) between mip levels `lod` and `lod+1` (weight
 * `frac`) when trilinear is requested, else a single-level bilinear. Writes the
 * result to the four out registers. `lv`/`lv1` are prebuilt level views. */
SGL_RVV_ALWAYS_INLINE void sgl_rvv_tap_trilinear_m2(const sgl_tex *tex,
        const sgl_tex *lv, int lod, float frac,
        vfloat32m2_t uk, vfloat32m2_t vk, size_t vl, float inv255, int wrap,
        vfloat32m2_t *r, vfloat32m2_t *g, vfloat32m2_t *b, vfloat32m2_t *a)
{
    sgl_rvv_sample2d_bilinear_m2(lv, uk, vk, vl, inv255, wrap, r, g, b, a);
    if (tex->mip_filter == 2 && sgl_mip_trilinear_enabled && frac > 0.f && lod + 1 < tex->nmip) {
        vfloat32m2_t r1, g1, b1, a1;
        sgl_tex lv1 = sgl_rvv_mip_view(tex, lod + 1);
        sgl_rvv_sample2d_bilinear_m2(&lv1, uk, vk, vl, inv255, wrap, &r1, &g1, &b1, &a1);
        *r = __riscv_vfmacc_vf_f32m2(*r, frac, __riscv_vfsub_vv_f32m2(r1, *r, vl), vl);
        *g = __riscv_vfmacc_vf_f32m2(*g, frac, __riscv_vfsub_vv_f32m2(g1, *g, vl), vl);
        *b = __riscv_vfmacc_vf_f32m2(*b, frac, __riscv_vfsub_vv_f32m2(b1, *b, vl), vl);
        *a = __riscv_vfmacc_vf_f32m2(*a, frac, __riscv_vfsub_vv_f32m2(a1, *a, vl), vl);
    }
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample2d_m2(const sgl_tex *tex, vfloat32m2_t u, vfloat32m2_t v,
                                       size_t vl, float inv255, int min_filter, int wrap,
                                       const sgl_uv_deriv *duv,
                                       vfloat32m2_t *tr, vfloat32m2_t *tg,
                                       vfloat32m2_t *tb, vfloat32m2_t *ta)
{
    if (tex->is_depth) {
        /* Depth textures are point-sampled and their texels are raw float bits,
         * so bilinear/LOD/aniso blending is meaningless; force the nearest path.
         * This is the depth agent's no-mip, no-aniso, reinterpret path and MUST
         * stay first so is_depth never reaches the mip/aniso logic below. */
        sgl_rvv_sample2d_nearest_m2(tex, u, v, vl, inv255, wrap, tr, tg, tb, ta);
        return;
    }
    if (min_filter == 1) {
        if (duv) {
            /* Analytic per-batch derivatives: exact rho and (optional) anisotropy,
             * no cross-lane sampling and correct on vcompress-packed batches. */
            float dudx = duv->dudx * (float)tex->w, dvdx = duv->dvdx * (float)tex->h;
            float dudy = duv->dudy * (float)tex->w, dvdy = duv->dvdy * (float)tex->h;
            float px = __builtin_sqrtf(dudx * dudx + dvdx * dvdx);   /* texels/pixel along screen x */
            float py = __builtin_sqrtf(dudy * dudy + dvdy * dvdy);   /* texels/pixel along screen y */
            float pmax = px > py ? px : py;
            float pmin = px < py ? px : py;
            float aniso = tex->max_aniso; if (aniso < 1.f) aniso = 1.f;  /* 0-init -> isotropic */
            int   do_mip = sgl_mip_enabled && tex->mip_filter && tex->nmip > 1;

            if (aniso > 1.f && pmin > 0.f && pmax > pmin) {
                /* Anisotropic: N bilinear taps stepped along the major screen axis;
                 * base LOD from the MINOR axis so each tap's footprint is ~isotropic. */
                int N = (int)(pmax / pmin + 0.5f);
                int Nmax = (int)aniso;
                if (Nmax < 1) Nmax = 1;
                if (N < 1) N = 1;
                if (N > Nmax) N = Nmax;
                if (N > 1) {
                    float frac = 0.f;
                    int lod = do_mip ? sgl_rvv_pick_lod(tex, pmin, &frac) : 0;
                    sgl_tex lv = sgl_rvv_mip_view(tex, lod);
                    float su, sv;                         /* major-axis step, normalised uv */
                    if (px >= py) { su = duv->dudx; sv = duv->dvdx; }
                    else          { su = duv->dudy; sv = duv->dvdy; }
                    float invN = 1.f / (float)N;
                    su *= invN; sv *= invN;
                    float base = -0.5f * (float)(N - 1);  /* centre the taps on (u,v) */
                    vfloat32m2_t accr = __riscv_vfmv_v_f_f32m2(0.f, vl);
                    vfloat32m2_t accg = accr, accb = accr, acca = accr;
                    for (int k = 0; k < N; ++k) {
                        float off = base + (float)k;
                        vfloat32m2_t uk = __riscv_vfadd_vf_f32m2(u, off * su, vl);
                        vfloat32m2_t vk = __riscv_vfadd_vf_f32m2(v, off * sv, vl);
                        vfloat32m2_t r, g, b, a;
                        sgl_rvv_tap_trilinear_m2(tex, &lv, lod, frac, uk, vk, vl, inv255, wrap, &r, &g, &b, &a);
                        accr = __riscv_vfadd_vv_f32m2(accr, r, vl);
                        accg = __riscv_vfadd_vv_f32m2(accg, g, vl);
                        accb = __riscv_vfadd_vv_f32m2(accb, b, vl);
                        acca = __riscv_vfadd_vv_f32m2(acca, a, vl);
                    }
                    *tr = __riscv_vfmul_vf_f32m2(accr, invN, vl);
                    *tg = __riscv_vfmul_vf_f32m2(accg, invN, vl);
                    *tb = __riscv_vfmul_vf_f32m2(accb, invN, vl);
                    *ta = __riscv_vfmul_vf_f32m2(acca, invN, vl);
                    return;
                }
            }
            /* Isotropic (max_aniso<=1, the common case): single (tri)linear at the
             * level matching the LONGER axis -- byte-identical fetch to a plain
             * bilinear when no minification/level is selected. */
            if (do_mip) {
                float frac;
                int lod = sgl_rvv_pick_lod(tex, pmax, &frac);
                if (lod > 0) {
                    sgl_tex lv = sgl_rvv_mip_view(tex, lod);
                    sgl_rvv_tap_trilinear_m2(tex, &lv, lod, frac, u, v, vl, inv255, wrap, tr, tg, tb, ta);
                    return;
                }
            }
            sgl_rvv_sample2d_bilinear_m2(tex, u, v, vl, inv255, wrap, tr, tg, tb, ta);
            return;
        }
        /* No analytic derivatives supplied -> preserve the prior per-batch two-lane
         * estimate exactly (callers with no plane data, e.g. the GLSL VM, keep
         * their previous behaviour). */
        if ((sgl_rvv_mag_downgrade_enabled || (sgl_mip_enabled && tex->nmip > 1)) && vl >= 2) {
            float u_lo = __riscv_vfmv_f_s_f32m2_f32(u);
            float u_hi = __riscv_vfmv_f_s_f32m2_f32(__riscv_vslidedown_vx_f32m2(u, vl - 1, 1));
            float v_lo = __riscv_vfmv_f_s_f32m2_f32(v);
            float v_hi = __riscv_vfmv_f_s_f32m2_f32(__riscv_vslidedown_vx_f32m2(v, vl - 1, 1));
            float du = (u_hi - u_lo); if (du < 0.f) du = -du;
            float dv = (v_hi - v_lo); if (dv < 0.f) dv = -dv;
            float inv = 1.f / (float)(vl - 1);
            float tu = du * inv * (float)tex->w;
            float tv = dv * inv * (float)tex->h;
            if (sgl_rvv_mag_downgrade_enabled && tu <= 1.0f && tv <= 1.0f) {
                sgl_rvv_sample2d_nearest_m2(tex, u, v, vl, inv255, wrap, tr, tg, tb, ta);
                return;
            }
            if (sgl_mip_enabled && tex->mip_filter && tex->nmip > 1) {
                float rho = tu > tv ? tu : tv;
                float frac;
                int lod = sgl_rvv_pick_lod(tex, rho, &frac);
                if (lod > 0) {
                    sgl_tex lv = sgl_rvv_mip_view(tex, lod);
                    sgl_rvv_tap_trilinear_m2(tex, &lv, lod, frac, u, v, vl, inv255, wrap, tr, tg, tb, ta);
                    return;
                }
            }
        }
        sgl_rvv_sample2d_bilinear_m2(tex, u, v, vl, inv255, wrap, tr, tg, tb, ta);
        return;
    }
    sgl_rvv_sample2d_nearest_m2(tex, u, v, vl, inv255, wrap, tr, tg, tb, ta);
}

/* textureCube — bit-identical face select + UV map to sample_cube_tex() in glsl.c,
 * then the same 2D sampler as texture2D. Lanes are partitioned across the six
 * faces; each non-empty face does one RVV sample2d and the results are merged.
 * Under TCG this replaces a scalar per-lane cube walk (the old interaction
 * kernel path) with at most 6 vector samples, usually 1–3 live faces. */
SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample_cube_m2(const sgl_tex *cube,
        vfloat32m2_t dx, vfloat32m2_t dy, vfloat32m2_t dz, size_t vl, float inv255,
        vfloat32m2_t *tr, vfloat32m2_t *tg, vfloat32m2_t *tb, vfloat32m2_t *ta)
{
    vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, vl);
    vfloat32m2_t one = __riscv_vfmv_v_f_f32m2(1.f, vl);
    vfloat32m2_t half = __riscv_vfmv_v_f_f32m2(0.5f, vl);
    *tr = *tg = *tb = z;
    *ta = one;
    if (!cube) return;

    vfloat32m2_t ax = __riscv_vfabs_v_f32m2(dx, vl);
    vfloat32m2_t ay = __riscv_vfabs_v_f32m2(dy, vl);
    vfloat32m2_t az = __riscv_vfabs_v_f32m2(dz, vl);
    vbool16_t x_dom = __riscv_vmand_mm_b16(
        __riscv_vmfge_vv_f32m2_b16(ax, ay, vl),
        __riscv_vmfge_vv_f32m2_b16(ax, az, vl), vl);
    vbool16_t y_dom = __riscv_vmand_mm_b16(
        __riscv_vmnot_m_b16(x_dom, vl),
        __riscv_vmfge_vv_f32m2_b16(ay, az, vl), vl);
    vbool16_t z_dom = __riscv_vmnot_m_b16(__riscv_vmor_mm_b16(x_dom, y_dom, vl), vl);

    /* Face masks match sample_cube_tex major-axis ties. (No vbool arrays — GCC
     * RVV types are not array-able.) */
    vbool16_t m0 = __riscv_vmand_mm_b16(x_dom, __riscv_vmfge_vf_f32m2_b16(dx, 0.f, vl), vl);
    vbool16_t m1 = __riscv_vmand_mm_b16(x_dom, __riscv_vmflt_vf_f32m2_b16(dx, 0.f, vl), vl);
    vbool16_t m2 = __riscv_vmand_mm_b16(y_dom, __riscv_vmfge_vf_f32m2_b16(dy, 0.f, vl), vl);
    vbool16_t m3 = __riscv_vmand_mm_b16(y_dom, __riscv_vmflt_vf_f32m2_b16(dy, 0.f, vl), vl);
    vbool16_t m4 = __riscv_vmand_mm_b16(z_dom, __riscv_vmfge_vf_f32m2_b16(dz, 0.f, vl), vl);
    vbool16_t m5 = __riscv_vmand_mm_b16(z_dom, __riscv_vmflt_vf_f32m2_b16(dz, 0.f, vl), vl);

#define SGL_CUBE_FACE(MASK, FACE, MA, SC, TC) do { \
        if (__riscv_vcpop_m_b16((MASK), vl) == 0) break; \
        vfloat32m2_t _ma = (MA); \
        _ma = __riscv_vmerge_vvm_f32m2(_ma, one, __riscv_vmfeq_vf_f32m2_b16(_ma, 0.f, vl), vl); \
        vfloat32m2_t _u = __riscv_vfmacc_vf_f32m2(half, 0.5f, __riscv_vfdiv_vv_f32m2((SC), _ma, vl), vl); \
        vfloat32m2_t _v = __riscv_vfmacc_vf_f32m2(half, 0.5f, __riscv_vfdiv_vv_f32m2((TC), _ma, vl), vl); \
        const sgl_tex *_src = (cube->is_cube && cube->cube_faces[(FACE)]) \
            ? cube->cube_faces[(FACE)] : cube; \
        vfloat32m2_t _fr = z, _fg = z, _fb = z, _fa = one; \
        if (_src && _src->texels) \
            sgl_rvv_sample2d_m2(_src, _u, _v, vl, inv255, _src->min_filter, _src->wrap, \
                                (const sgl_uv_deriv *)0, &_fr, &_fg, &_fb, &_fa); \
        *tr = __riscv_vmerge_vvm_f32m2(*tr, _fr, (MASK), vl); \
        *tg = __riscv_vmerge_vvm_f32m2(*tg, _fg, (MASK), vl); \
        *tb = __riscv_vmerge_vvm_f32m2(*tb, _fb, (MASK), vl); \
        *ta = __riscv_vmerge_vvm_f32m2(*ta, _fa, (MASK), vl); \
    } while (0)

    SGL_CUBE_FACE(m0, 0, ax, __riscv_vfneg_v_f32m2(dz, vl), __riscv_vfneg_v_f32m2(dy, vl));
    SGL_CUBE_FACE(m1, 1, ax, dz,                            __riscv_vfneg_v_f32m2(dy, vl));
    SGL_CUBE_FACE(m2, 2, ay, dx,                            dz);
    SGL_CUBE_FACE(m3, 3, ay, dx,                            __riscv_vfneg_v_f32m2(dz, vl));
    SGL_CUBE_FACE(m4, 4, az, dx,                            __riscv_vfneg_v_f32m2(dy, vl));
    SGL_CUBE_FACE(m5, 5, az, __riscv_vfneg_v_f32m2(dx, vl), __riscv_vfneg_v_f32m2(dy, vl));
#undef SGL_CUBE_FACE
}

/* ===================================================================== *
 * fp16 (zvfh) shading path — the GeForce 6 "fp16 register sweet spot" on RVV.
 *
 * At vlen=1024, e16m2 carries 128 lanes (twice the 64 of e32m2), so a fp16
 * kernel shades twice the fragments per dispatch while halving shading
 * register pressure. Texture *addressing* stays fp32 (e32m4, also 128 lanes)
 * so texel selection is exact regardless of texcoord magnitude; only the
 * unpacked colour and the colour arithmetic run at fp16, where GLSL ES
 * `precision mediump float` already licenses the reduced precision. Texels
 * remain RGBA8 (the gather is unchanged) — the win is lane count + fp16
 * registers, not texel bandwidth.
 * ===================================================================== */
#if defined(__riscv_zvfh)

SGL_RVV_ALWAYS_INLINE vfloat16m2_t sgl_rvv_lerp_h(vfloat16m2_t a, vfloat16m2_t b,
                                                  vfloat16m2_t d, size_t vl)
{   /* a + (b-a)*d */
    return __riscv_vfmacc_vv_f16m2(a, __riscv_vfsub_vv_f16m2(b, a, vl), d, vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_wrap01_m4(vfloat32m4_t *u, vfloat32m4_t *v, size_t vl, int wrap)
{
    if (wrap) {
        vint32m4_t uf = __riscv_vfcvt_x_f_v_i32m4_rm(*u, __RISCV_FRM_RDN, vl);
        vint32m4_t vf = __riscv_vfcvt_x_f_v_i32m4_rm(*v, __RISCV_FRM_RDN, vl);
        *u = __riscv_vfsub_vv_f32m4(*u, __riscv_vfcvt_f_x_v_f32m4(uf, vl), vl);
        *v = __riscv_vfsub_vv_f32m4(*v, __riscv_vfcvt_f_x_v_f32m4(vf, vl), vl);
    }
    *u = __riscv_vfmin_vf_f32m4(__riscv_vfmax_vf_f32m4(*u, 0.f, vl), 1.f, vl);
    *v = __riscv_vfmin_vf_f32m4(__riscv_vfmax_vf_f32m4(*v, 0.f, vl), 1.f, vl);
}

/* Gather one RGBA8 texel per lane (128 lanes, e32m4) and narrow to fp16 [0,1]. */
SGL_RVV_ALWAYS_INLINE vuint32m4_t sgl_rvv_tex_gather_m4(const sgl_tex *t, vuint32m4_t xi,
                                                        vuint32m4_t yi, size_t vl)
{
    vuint32m4_t idx = __riscv_vadd_vv_u32m4(__riscv_vmul_vx_u32m4(yi, (uint32_t)t->w, vl), xi, vl);
    return __riscv_vluxei32_v_u32m4(t->texels, __riscv_vsll_vx_u32m4(idx, 2, vl), vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_unpack_rgba_hf(vuint32m4_t p, size_t vl,
        vfloat16m2_t *r, vfloat16m2_t *g, vfloat16m2_t *b, vfloat16m2_t *a)
{
    const _Float16 inv255 = (_Float16)(1.0f / 255.0f);
    /* All four byte-extracts first (stay in e32m4), THEN all four narrowing
     * converts (stay in e16m2): two vtype regions, one toggle — instead of
     * interleaving and thrashing vsetvli on every channel. The narrowing
     * vfncvt.f.xu.w folds the int->float and the 32->16 narrow into one op. */
    vuint32m4_t cr = __riscv_vand_vx_u32m4(p, 0xFFu, vl);
    vuint32m4_t cg = __riscv_vand_vx_u32m4(__riscv_vsrl_vx_u32m4(p, 8, vl), 0xFFu, vl);
    vuint32m4_t cb = __riscv_vand_vx_u32m4(__riscv_vsrl_vx_u32m4(p, 16, vl), 0xFFu, vl);
    vuint32m4_t ca = __riscv_vsrl_vx_u32m4(p, 24, vl);
    *r = __riscv_vfmul_vf_f16m2(__riscv_vfncvt_f_xu_w_f16m2(cr, vl), inv255, vl);
    *g = __riscv_vfmul_vf_f16m2(__riscv_vfncvt_f_xu_w_f16m2(cg, vl), inv255, vl);
    *b = __riscv_vfmul_vf_f16m2(__riscv_vfncvt_f_xu_w_f16m2(cb, vl), inv255, vl);
    *a = __riscv_vfmul_vf_f16m2(__riscv_vfncvt_f_xu_w_f16m2(ca, vl), inv255, vl);
}

/* 128-lane nearest sample: fp32 coords in, fp16 colour out. */
SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample2d_nearest_hf(const sgl_tex *tex,
        vfloat32m4_t u, vfloat32m4_t v, size_t vl, int wrap,
        vfloat16m2_t *tr, vfloat16m2_t *tg, vfloat16m2_t *tb, vfloat16m2_t *ta)
{
    sgl_rvv_wrap01_m4(&u, &v, vl, wrap);
    vuint32m4_t xi = __riscv_vminu_vx_u32m4(
        __riscv_vfcvt_rtz_xu_f_v_u32m4(__riscv_vfmul_vf_f32m4(u, (float)tex->w, vl), vl),
        (uint32_t)(tex->w - 1), vl);
    vuint32m4_t yi = __riscv_vminu_vx_u32m4(
        __riscv_vfcvt_rtz_xu_f_v_u32m4(__riscv_vfmul_vf_f32m4(v, (float)tex->h, vl), vl),
        (uint32_t)(tex->h - 1), vl);
    sgl_rvv_unpack_rgba_hf(sgl_rvv_tex_gather_m4(tex, xi, yi, vl), vl, tr, tg, tb, ta);
}

/* 128-lane bilinear sample: fp32 coords/weights, fp16 colour lerps. */
SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample2d_bilinear_hf(const sgl_tex *tex,
        vfloat32m4_t u, vfloat32m4_t v, size_t vl, int wrap,
        vfloat16m2_t *tr, vfloat16m2_t *tg, vfloat16m2_t *tb, vfloat16m2_t *ta)
{
    sgl_rvv_wrap01_m4(&u, &v, vl, wrap);
    vfloat32m4_t fx = __riscv_vfsub_vf_f32m4(__riscv_vfmul_vf_f32m4(u, (float)tex->w, vl), 0.5f, vl);
    vfloat32m4_t fy = __riscv_vfsub_vf_f32m4(__riscv_vfmul_vf_f32m4(v, (float)tex->h, vl), 0.5f, vl);
    vint32m4_t x0 = __riscv_vfcvt_x_f_v_i32m4_rm(fx, __RISCV_FRM_RDN, vl);
    vint32m4_t y0 = __riscv_vfcvt_x_f_v_i32m4_rm(fy, __RISCV_FRM_RDN, vl);
    vfloat16m2_t dx = __riscv_vfncvt_f_f_w_f16m2(__riscv_vfsub_vv_f32m4(fx, __riscv_vfcvt_f_x_v_f32m4(x0, vl), vl), vl);
    vfloat16m2_t dy = __riscv_vfncvt_f_f_w_f16m2(__riscv_vfsub_vv_f32m4(fy, __riscv_vfcvt_f_x_v_f32m4(y0, vl), vl), vl);
    vint32m4_t x1 = __riscv_vadd_vx_i32m4(x0, 1, vl);
    vint32m4_t y1 = __riscv_vadd_vx_i32m4(y0, 1, vl);
    x0 = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(x0, tex->w - 1, vl), 0, vl);
    x1 = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(x1, tex->w - 1, vl), 0, vl);
    y0 = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(y0, tex->h - 1, vl), 0, vl);
    y1 = __riscv_vmax_vx_i32m4(__riscv_vmin_vx_i32m4(y1, tex->h - 1, vl), 0, vl);
    vuint32m4_t x0u = __riscv_vreinterpret_v_i32m4_u32m4(x0), x1u = __riscv_vreinterpret_v_i32m4_u32m4(x1);
    vuint32m4_t y0u = __riscv_vreinterpret_v_i32m4_u32m4(y0), y1u = __riscv_vreinterpret_v_i32m4_u32m4(y1);
    vfloat16m2_t r00, g00, b00, a00, r10, g10, b10, a10, r01, g01, b01, a01, r11, g11, b11, a11;
    sgl_rvv_unpack_rgba_hf(sgl_rvv_tex_gather_m4(tex, x0u, y0u, vl), vl, &r00, &g00, &b00, &a00);
    sgl_rvv_unpack_rgba_hf(sgl_rvv_tex_gather_m4(tex, x1u, y0u, vl), vl, &r10, &g10, &b10, &a10);
    sgl_rvv_unpack_rgba_hf(sgl_rvv_tex_gather_m4(tex, x0u, y1u, vl), vl, &r01, &g01, &b01, &a01);
    sgl_rvv_unpack_rgba_hf(sgl_rvv_tex_gather_m4(tex, x1u, y1u, vl), vl, &r11, &g11, &b11, &a11);
    *tr = sgl_rvv_lerp_h(sgl_rvv_lerp_h(r00, r10, dx, vl), sgl_rvv_lerp_h(r01, r11, dx, vl), dy, vl);
    *tg = sgl_rvv_lerp_h(sgl_rvv_lerp_h(g00, g10, dx, vl), sgl_rvv_lerp_h(g01, g11, dx, vl), dy, vl);
    *tb = sgl_rvv_lerp_h(sgl_rvv_lerp_h(b00, b10, dx, vl), sgl_rvv_lerp_h(b01, b11, dx, vl), dy, vl);
    *ta = sgl_rvv_lerp_h(sgl_rvv_lerp_h(a00, a10, dx, vl), sgl_rvv_lerp_h(a01, a11, dx, vl), dy, vl);
}

SGL_RVV_ALWAYS_INLINE void sgl_rvv_sample2d_hf(const sgl_tex *tex,
        vfloat32m4_t u, vfloat32m4_t v, size_t vl, int min_filter, int wrap,
        vfloat16m2_t *tr, vfloat16m2_t *tg, vfloat16m2_t *tb, vfloat16m2_t *ta)
{
    if (min_filter == 1) sgl_rvv_sample2d_bilinear_hf(tex, u, v, vl, wrap, tr, tg, tb, ta);
    else                 sgl_rvv_sample2d_nearest_hf(tex, u, v, vl, wrap, tr, tg, tb, ta);
}

#endif /* __riscv_zvfh */

#endif /* __riscv_v_intrinsic */

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
/* ---------------- compatibility API (historical names) ---------------- */

/* m1 compatibility names used by existing tests/docs. */
static inline vfloat32m1_t sgl_rvv_rsqrt_fast(vfloat32m1_t x, size_t vl)
{ return sgl_rvv_rsqrt_fast_m1(x, vl); }

static inline vfloat32m1_t sgl_rvv_rsqrt_accurate(vfloat32m1_t x, size_t vl)
{ return sgl_rvv_rsqrt_accurate_m1(x, vl); }

static inline vfloat32m1_t sgl_rvv_dot4_v(vfloat32m1_t a0, vfloat32m1_t a1,
                                          vfloat32m1_t a2, vfloat32m1_t a3,
                                          vfloat32m1_t b0, vfloat32m1_t b1,
                                          vfloat32m1_t b2, vfloat32m1_t b3,
                                          size_t vl)
{
    return sgl_rvv_dot4_v1_m1(a0, a1, a2, a3, b0, b1, b2, b3, vl);
}

static inline vfloat32m1_t sgl_rvv_dot4(const float *a0, const float *a1,
                                        const float *a2, const float *a3,
                                        const float *b0, const float *b1,
                                        const float *b2, const float *b3,
                                        size_t vl)
{
    return sgl_rvv_dot4_m1(a0, a1, a2, a3, b0, b1, b2, b3, vl);
}

static inline void sgl_rvv_mat4xvec4(vfloat32m1_t *out0, vfloat32m1_t *out1,
                                     vfloat32m1_t *out2, vfloat32m1_t *out3,
                                     const float M[16],
                                     vfloat32m1_t x, vfloat32m1_t y,
                                     vfloat32m1_t z, vfloat32m1_t w, size_t vl)
{
    sgl_rvv_mat4xvec4_m1(out0, out1, out2, out3, M, x, y, z, w, vl);
}

static inline void sgl_rvv_sincos_f(vfloat32m1_t *sin_out, vfloat32m1_t *cos_out,
                                   vfloat32m1_t a, size_t vl)
{
    sgl_rvv_sincos_f_m1(sin_out, cos_out, a, vl);
}
#endif

#endif /* SOFTGLES2_SG_MATH_H */
