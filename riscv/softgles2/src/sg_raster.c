/*
 * softgles2 rasteriser.
 *
 * Edge-function triangle rasteriser with perspective-correct interpolation of
 * colour and texture coordinates, nearest-texel sampling, depth test, and
 * alpha/additive blending. Two implementations behind one entry point:
 *   - a scalar reference (also the host/x86 path), always correct;
 *   - an RVV (V extension, v1.0 intrinsics) inner loop shading a width-agnostic
 *     run of pixels per vsetvli — SoA, mask-predicated, texture fetch via
 *     indexed gather (vluxei32), masked colour/depth stores.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#endif

#include "sgl.h"

#include "sg_diag.h"
#include "sg_prof.h"
#include "sg_pareto.h"
#include "sg_layout.h"
#include "sg_math.h"
#include "sg_blend.h"
#include "sg_raster.h"
#include "sg_frag.h"

/* compact one-line clamps below are intentional */
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SGL_RVV 1
#else
#  define SGL_RVV 0
#endif

/* Diagnostic A/B: omit stencil operations that cannot change stored bits. */
int sgl_stencil_noop_prune = 1;
int sgl_alpha_only_store = 1;
int sgl_stencil_always_fast = 1;
int sgl_depth_only_setup_prune = 1;

/* ---- scalar helpers (no libm) ---- */
static inline float fminf_(float a, float b) { return a < b ? a : b; }
static inline float fmaxf_(float a, float b) { return a > b ? a : b; }
static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline int   ifloor(float x) { int i = (int)x; return (x < 0.f && (float)i != x) ? i - 1 : i; }
static inline int   iceil(float x)  { return -ifloor(-x); }

/* Ordered (Bayer 4x4) dithering of the final 8-bit colour store. OFF by default so
 * the host conformance tests (which link libbm_sgl and compare exact framebuffer
 * pixels) are unaffected; the bare-metal engine turns it on (sys_dos.cpp). It
 * replaces the round-to-nearest +0.5 quantisation bias with a per-pixel threshold
 * whose MEAN is 0.5 (so overall brightness is unchanged), breaking up 8-bit
 * gradient banding in the light-falloff-lit world. Deterministic in (x,y): [fbhash]
 * stays reproducible, and additive light passes get a FIXED per-pixel bias rather
 * than accumulating random noise. Only the RVV store path consults it; the scalar
 * (#else, host-test) path keeps exact round-to-nearest. */
int sgl_dither_enabled = 0;
static const uint32_t sgl_bayer4[16] = { 0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5 };
int sgl_dither_table_fast = 1;
/* Exact binary fractions, repeated so x modulo four selects a contiguous
 * window. Avoid a gather, integer conversion and arithmetic for every store.
 * Larger future vector lengths fall back to the general expression below. */
#define BIAS4(a,b,c,d) ((2*(a)+1)/32.f),((2*(b)+1)/32.f),((2*(c)+1)/32.f),((2*(d)+1)/32.f)
#define BIAS16(a,b,c,d) BIAS4(a,b,c,d),BIAS4(a,b,c,d),BIAS4(a,b,c,d),BIAS4(a,b,c,d)
#define BIAS68(a,b,c,d) BIAS16(a,b,c,d),BIAS16(a,b,c,d),BIAS16(a,b,c,d),BIAS16(a,b,c,d),BIAS4(a,b,c,d)
static const float sgl_bayer_bias[4][68] = {
    {BIAS68(0,8,2,10)}, {BIAS68(12,4,14,6)},
    {BIAS68(3,11,1,9)}, {BIAS68(15,7,13,5)}
};
#undef BIAS68
#undef BIAS16
#undef BIAS4

/* ---- RVV stencil helpers (only instantiated when SGL_RVV) ---- */
/*
 * Vectorised stencil operation.  `mask` selects which bytes to modify;
 * `cur` holds the current (per-pixel) stencil byte, `sf` the stencil face,
 * and `op` one of SGL_STENCIL_{KEEP,REPLACE,INCR,...}.  Returns the updated
 * values after write_mask is applied.
 *
 * Two LMUL versions are provided so the mask type matches the calling loop:
 *   rvv_stencil_op_f16 — for use with e32m2/vbool16_t (SIMT programmable path)
 *   rvv_stencil_op_f16 — for use with e32m2/vbool16_t (fixed-function path)
 */
#if SGL_RVV
static inline vuint8mf2_t rvv_stencil_op_f16(vbool16_t mask, int op, vuint8mf2_t cur,
                                              const sgl_stencil_face *sf, size_t vl)
{
    vuint8mf2_t wm = __riscv_vmv_v_x_u8mf2(sf->write_mask, vl);
    vuint8mf2_t opval;
    switch (op) {
    case SGL_STENCIL_KEEP:      opval = cur; break;
    case SGL_STENCIL_REPLACE:   opval = __riscv_vmv_v_x_u8mf2(sf->ref, vl); break;
    case SGL_STENCIL_INCR:      opval = __riscv_vsaddu_vx_u8mf2(cur, 1, vl); break;
    case SGL_STENCIL_DECR:      opval = __riscv_vssubu_vx_u8mf2(cur, 1, vl); break;
    case SGL_STENCIL_INCR_WRAP: opval = __riscv_vand_vx_u8mf2(__riscv_vadd_vx_u8mf2(cur, 1, vl), 255, vl); break;
    case SGL_STENCIL_DECR_WRAP: opval = __riscv_vand_vx_u8mf2(__riscv_vsub_vx_u8mf2(cur, 1, vl), 255, vl); break;
    case SGL_STENCIL_INVERT:    opval = __riscv_vand_vx_u8mf2(__riscv_vnot_v_u8mf2(cur, vl), 255, vl); break;
    default:                    opval = cur; break;
    }
    vuint8mf2_t keep = __riscv_vand_vv_u8mf2(cur, __riscv_vnot_v_u8mf2(wm, vl), vl);
    vuint8mf2_t write = __riscv_vand_vv_u8mf2(opval, wm, vl);
    vuint8mf2_t merged = __riscv_vor_vv_u8mf2(keep, write, vl);
    return __riscv_vmerge_vvm_u8mf2(cur, merged, mask, vl);
}

/* Per-lane quantisation bias for the RGBA8 store at (x..x+vl-1, y). Returns the
 * ordered-dither threshold (mean 0.5) when sgl_dither_enabled, else a 0.5 splat
 * (bit-identical to the previous round-to-nearest). See sgl_dither_enabled. */
static inline vfloat32m2_t sgl_store_bias_m2(int x, int y, size_t vl)
{
    if (!sgl_dither_enabled)
        return __riscv_vfmv_v_f_f32m2(0.5f, vl);
    if (sgl_dither_table_fast && vl <= 64)
        return __riscv_vle32_v_f32m2(&sgl_bayer_bias[y & 3][x & 3], vl);
    vuint32m2_t idx = __riscv_vand_vx_u32m2(
        __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), (uint32_t)x, vl), 3u, vl);
    idx = __riscv_vadd_vx_u32m2(idx, ((uint32_t)(y & 3)) << 2, vl);
    vuint32m2_t bv = __riscv_vluxei32_v_u32m2(sgl_bayer4, __riscv_vsll_vx_u32m2(idx, 2, vl), vl);
    return __riscv_vfmul_vf_f32m2(
        __riscv_vfadd_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(bv, vl), 0.5f, vl), 1.0f / 16.0f, vl);
}

#endif

/* Pack a fragment to RGBA8 (0xAABBGGRR). Alpha is the real (clamped) src alpha
 * (C5): exact for opaque BLEND_NONE writes; for blended writes it is the src
 * alpha (alpha-blend factors are deferred to C2). cmask's alpha bit (0x8) gates
 * whether this alpha actually reaches the framebuffer at the store sites. */
static inline uint32_t pack_rgba(float r, float g, float b, float a)
{
    uint32_t R = (uint32_t)(clamp01(r) * 255.f + 0.5f);
    uint32_t G = (uint32_t)(clamp01(g) * 255.f + 0.5f);
    uint32_t B = (uint32_t)(clamp01(b) * 255.f + 0.5f);
    uint32_t A = (uint32_t)(clamp01(a) * 255.f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}

/* Shared scalar sampler (nearest or bilinear). */
void sgl_sample(const sgl_tex *t, float u, float v, float out[4])
{
    if (t->wrap == 1) { u -= (float)ifloor(u); v -= (float)ifloor(v); }  /* repeat */
    if (u < 0.f) u = 0.f; else if (u > 1.f) u = 1.f;
    if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;

    if (t->min_filter == 1 && !t->is_depth) {       /* bilinear */
        float fx = u * (float)t->w - 0.5f, fy = v * (float)t->h - 0.5f;
        int x0 = ifloor(fx), y0 = ifloor(fy);
        float dx = fx - (float)x0, dy = fy - (float)y0;
        int x1 = x0 + 1, y1 = y0 + 1;
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > t->w - 1) x1 = t->w - 1; if (y1 > t->h - 1) y1 = t->h - 1;
        if (x0 > t->w - 1) x0 = t->w - 1; if (y0 > t->h - 1) y0 = t->h - 1;
        uint32_t c00 = t->texels[y0 * t->w + x0], c10 = t->texels[y0 * t->w + x1];
        uint32_t c01 = t->texels[y1 * t->w + x0], c11 = t->texels[y1 * t->w + x1];
        for (int k = 0; k < 4; ++k) {
            float a = (float)((c00 >> (k * 8)) & 0xFF), b = (float)((c10 >> (k * 8)) & 0xFF);
            float c = (float)((c01 >> (k * 8)) & 0xFF), d = (float)((c11 >> (k * 8)) & 0xFF);
            float top = a + (b - a) * dx, bot = c + (d - c) * dx;
            out[k] = (top + (bot - top) * dy) / 255.f;
        }
    } else {                                        /* nearest */
        int x = (int)(u * (float)t->w); if (x > t->w - 1) x = t->w - 1; if (x < 0) x = 0;
        int y = (int)(v * (float)t->h); if (y > t->h - 1) y = t->h - 1; if (y < 0) y = 0;
        uint32_t c = t->texels[y * t->w + x];
        if (t->is_depth) {
            /* Depth texel holds raw float bits: return (d,d,d,1). Bit-for-bit the
             * same value the RVV nearest path yields. */
            float d;
            memcpy(&d, &c, sizeof d);
            out[0] = out[1] = out[2] = d;
            out[3] = 1.0f;
            return;
        }
        for (int k = 0; k < 4; ++k) out[k] = (float)((c >> (k * 8)) & 0xFF) / 255.f;
    }
}

/* DIAG: triangles handed to the rasteriser since last reset (both fixed +
 * programmable paths), and the subset whose clamped bounding box is non-empty
 * (i.e. actually land in the viewport after projection). dos_gl_present reads +
 * clears these per frame. onscreen≈0 with tris large => geometry projected
 * off-screen (transposed projection / bad viewport); onscreen large with
 * nonblack==0 => depth-reject or shaded-black. */
volatile unsigned long sgl_dbg_tris = 0;
volatile unsigned long sgl_dbg_tris_onscreen = 0;
volatile unsigned long sgl_dbg_tris_hiz_culled = 0;
volatile unsigned long sgl_dbg_frag_zpass = 0;
volatile unsigned long sgl_dbg_frag_cmask0 = 0;
volatile unsigned long sgl_dbg_frag_shaded = 0;
volatile unsigned long sgl_dbg_frag_shader_nonzero = 0;
volatile unsigned long sgl_dbg_frag_store = 0;
volatile unsigned long sgl_dbg_frag_store_nonzero = 0;
/* DIAG: opaque-overwrite stores (blend==NONE) and the subset that write BLACK.
 * If ow_black is large (≈ screen-fulls) on a lit frame, a late opaque pass is
 * painting black over the computed scene (store_nz high yet nonblack==0). */
volatile unsigned long sgl_dbg_frag_ow = 0;
volatile unsigned long sgl_dbg_frag_ow_black = 0;

/* glPolygonOffset window-depth bias for one polygon (GL_POLYGON_OFFSET_FILL):
 *   offset = factor * maxDepthSlope + units * r
 * maxDepthSlope = max(|dz/dx|, |dz/dy|). Window depth is screen-space linear, so
 * the slope is constant per triangle; callers add the returned offset to each
 * vertex z (cheaper than per-pixel, and applies to both depth test and write).
 * Depth is float in [0,1]; r = 2^-23 is ~one mantissa step near z=1 (the
 * smallest resolvable difference for this buffer; tunable). Returns 0 when
 * disabled, so the non-offset path stays bit-exact (z + 0.0). */
#define SGL_POLY_OFFSET_R (1.0f / 8388608.0f)   /* 2^-23 */

static inline float sgl_poly_offset_z(const sgl_drawstate *st, float invArea,
    float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
{
    if (!st->poly_off_on) return 0.f;
    float dzdx = ((bz - az) * (cy - ay) - (cz - az) * (by - ay)) * invArea;
    float dzdy = ((bx - ax) * (cz - az) - (cx - ax) * (bz - az)) * invArea;
    float adx = dzdx < 0.f ? -dzdx : dzdx;
    float ady = dzdy < 0.f ? -dzdy : dzdy;
    float slope = adx > ady ? adx : ady;
    return st->poly_off_factor * slope + st->poly_off_units * SGL_POLY_OFFSET_R;
}

void sgl_raster_tri(sgl_framebuffer *fb,
                    sgl_scrvert a, sgl_scrvert b, sgl_scrvert c,
                    const sgl_drawstate *st)
{
    SGL_DIAG_INC(sgl_dbg_tris);
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.f) return;
    if (area < 0.f) { sgl_scrvert t = b; b = c; c = t; area = -area; }
    float invArea = 1.f / area;

    {   /* glPolygonOffset: fold the per-triangle depth bias into each vertex z */
        float zoff = sgl_poly_offset_z(st, invArea, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z);
        a.z += zoff; b.z += zoff; c.z += zoff;
    }

    int minx = ifloor(fminf_(a.x, fminf_(b.x, c.x)));
    int maxx = ifloor(fmaxf_(a.x, fmaxf_(b.x, c.x)));
    int miny = ifloor(fminf_(a.y, fminf_(b.y, c.y)));
    int maxy = ifloor(fmaxf_(a.y, fmaxf_(b.y, c.y)));
    /* Clamp the bounding box to the framebuffer AND (if active) the scissor rect
     * before deriving the incremental edge starts — the edge functions are seeded
     * from (minx,miny), so this must happen first or the walk desyncs. */
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fb->w - 1) maxx = fb->w - 1;
    if (maxy > fb->h - 1) maxy = fb->h - 1;
    if (st->scissor_on) {
        if (minx < st->sx0) minx = st->sx0;
        if (miny < st->sy0) miny = st->sy0;
        if (maxx > st->sx1) maxx = st->sx1;
        if (maxy > st->sy1) maxy = st->sy1;
    }
    if (st->clip_x1 > 0) {                       /* parallel tile clip */
        if (minx < st->clip_x0) minx = st->clip_x0;
        if (maxx > st->clip_x1 - 1) maxx = st->clip_x1 - 1;
    }
    if (st->clip_y1 > 0) {
        if (miny < st->clip_y0) miny = st->clip_y0;
        if (maxy > st->clip_y1 - 1) maxy = st->clip_y1 - 1;
    }
    if (minx > maxx || miny > maxy) return;
    SGL_DIAG_INC(sgl_dbg_tris_onscreen);

    const float Ax0 = -(c.y - b.y), Ay0 = (c.x - b.x);
    const float Ax1 = -(a.y - c.y), Ay1 = (a.x - c.x);
    const float Ax2 = -(b.y - a.y), Ay2 = (b.x - a.x);

    float px0 = (float)minx + 0.5f, py0 = (float)miny + 0.5f;
    float row_e0 = (c.x - b.x) * (py0 - b.y) - (c.y - b.y) * (px0 - b.x);
    float row_e1 = (a.x - c.x) * (py0 - c.y) - (a.y - c.y) * (px0 - c.x);
    float row_e2 = (b.x - a.x) * (py0 - a.y) - (b.y - a.y) * (px0 - a.x);

    const sgl_tex *tex = st->tex;
    const int      blend = st->blend;
    const int      depth_test = st->depth_test;
    const int      depth_func = st->depth_func;
    const int      depth_write = st->depth_write;
    const int      cmask = st->color_mask;
    const int      stencil_test = st->stencil_test && fb->stencil;
    const sgl_stencil_face *stencil =
        st->stencil_frontface ? &st->stencil_front : &st->stencil_back;

    for (int y = miny; y <= maxy; ++y) {
        uint32_t *crow = &fb->color[y * fb->w];
        float    *zrow = &fb->depth[y * fb->w];
        uint8_t  *srow = fb->stencil ? &fb->stencil[y * fb->w] : 0;

#if SGL_RVV
        {
            const float inv255 = 1.f / 255.f;
            float e0s = row_e0, e1s = row_e1, e2s = row_e2;
            int x = minx;
            while (x <= maxx) {
            size_t vl = __riscv_vsetvl_e32m2((size_t)(maxx - x + 1));
            vfloat32m2_t lane = __riscv_vfcvt_f_xu_v_f32m2(__riscv_vid_v_u32m2(vl), vl);

            vfloat32m2_t e0 = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(lane, Ax0, vl), e0s, vl);
            vfloat32m2_t e1 = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(lane, Ax1, vl), e1s, vl);
            vfloat32m2_t e2 = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(lane, Ax2, vl), e2s, vl);

            vbool16_t in = __riscv_vmand_mm_b16(
                               __riscv_vmand_mm_b16(
                                   __riscv_vmfge_vf_f32m2_b16(e0, 0.f, vl),
                                   __riscv_vmfge_vf_f32m2_b16(e1, 0.f, vl), vl),
                               __riscv_vmfge_vf_f32m2_b16(e2, 0.f, vl), vl);
            if (__riscv_vcpop_m_b16(in, vl) == 0) {
                float fvl = (float)vl;
                e0s += Ax0 * fvl; e1s += Ax1 * fvl; e2s += Ax2 * fvl;
                x += (int)vl;
                continue;
            }

            vfloat32m2_t w0 = __riscv_vfmul_vf_f32m2(e0, invArea, vl);
            vfloat32m2_t w1 = __riscv_vfmul_vf_f32m2(e1, invArea, vl);
            vfloat32m2_t w2 = __riscv_vfmul_vf_f32m2(e2, invArea, vl);

            vfloat32m2_t z = __riscv_vfmul_vf_f32m2(w0, a.z, vl);
            z = __riscv_vfmacc_vf_f32m2(z, b.z, w1, vl);
            z = __riscv_vfmacc_vf_f32m2(z, c.z, w2, vl);

            /* ---- stencil test (before early-Z) ---- */
            vbool16_t cov = in;
            if (stencil_test) {
                size_t vl8 = __riscv_vsetvl_e8mf2(vl);
                vuint8mf2_t sv = __riscv_vle8_v_u8mf2(&srow[x], vl8);
                vuint32m2_t sv32 = __riscv_vzext_vf4_u32m2(sv, vl);
                uint32_t reff = (uint32_t)(stencil->ref & stencil->read_mask);
                uint32_t rmask = (uint32_t)stencil->read_mask;
                vuint32m2_t masked = __riscv_vand_vx_u32m2(sv32, rmask, vl);
                vbool16_t sp;
                switch (stencil->func) {
                case SGL_DEPTH_NEVER:    sp = __riscv_vmseq_vv_u32m2_b16(masked, __riscv_vadd_vx_u32m2(masked, 1, vl), vl); break;
                case SGL_DEPTH_LESS:     sp = __riscv_vmsgtu_vx_u32m2_b16(masked, reff, vl); break;
                case SGL_DEPTH_EQUAL:    sp = __riscv_vmseq_vx_u32m2_b16(masked, reff, vl); break;
                case SGL_DEPTH_LEQUAL:   sp = __riscv_vmsgeu_vx_u32m2_b16(masked, reff, vl); break;
                case SGL_DEPTH_GREATER:  sp = __riscv_vmsltu_vx_u32m2_b16(masked, reff, vl); break;
                case SGL_DEPTH_NOTEQUAL: sp = __riscv_vmsne_vx_u32m2_b16(masked, reff, vl); break;
                case SGL_DEPTH_GEQUAL:   sp = __riscv_vmsleu_vx_u32m2_b16(masked, reff, vl); break;
                case SGL_DEPTH_ALWAYS:
                default:                 sp = cov; break;
                }
                vbool16_t sfail = __riscv_vmand_mm_b16(cov, __riscv_vmnot_m_b16(sp, vl), vl);
                if (__riscv_vcpop_m_b16(sfail, vl)) {
                    sv = rvv_stencil_op_f16(sfail, stencil->sfail, sv, stencil, vl8);
                    __riscv_vse8_v_u8mf2_m(sfail, &srow[x], sv, vl8);
                }
                cov = __riscv_vmand_mm_b16(cov, sp, vl);
                if (__riscv_vcpop_m_b16(cov, vl) == 0) {
                    float fvl = (float)vl;
                    e0s += Ax0 * fvl; e1s += Ax1 * fvl; e2s += Ax2 * fvl;
                    x += (int)vl;
                    continue;
                }
            }

            vbool16_t wr = cov;
            if (depth_test && depth_func != SGL_DEPTH_ALWAYS) {
                vfloat32m2_t zb = __riscv_vle32_v_f32m2(&zrow[x], vl);
                vbool16_t dp;
                switch (depth_func) {
                case SGL_DEPTH_NEVER:    dp = __riscv_vmxor_mm_b16(cov, cov, vl); break; /* all false */
                case SGL_DEPTH_EQUAL: {  /* |z-zb| <= eps */
                    vfloat32m2_t d = __riscv_vfabs_v_f32m2(__riscv_vfsub_vv_f32m2(z, zb, vl), vl);
                    dp = __riscv_vmfle_vf_f32m2_b16(d, SGL_DEPTH_EPS, vl); break;
                }
                case SGL_DEPTH_LEQUAL:   dp = __riscv_vmfle_vv_f32m2_b16(z, zb, vl); break;
                case SGL_DEPTH_GREATER:  dp = __riscv_vmfgt_vv_f32m2_b16(z, zb, vl); break;
                case SGL_DEPTH_NOTEQUAL: {
                    vfloat32m2_t d = __riscv_vfabs_v_f32m2(__riscv_vfsub_vv_f32m2(z, zb, vl), vl);
                    dp = __riscv_vmfgt_vf_f32m2_b16(d, SGL_DEPTH_EPS, vl); break;
                }
                case SGL_DEPTH_GEQUAL:   dp = __riscv_vmfge_vv_f32m2_b16(z, zb, vl); break;
                case SGL_DEPTH_LESS:
                default:                 dp = __riscv_vmflt_vv_f32m2_b16(z, zb, vl); break;
                }
                wr = __riscv_vmand_mm_b16(cov, dp, vl);
            }

            /* ---- stencil zfail/zpass (after depth test) ---- */
            if (stencil_test) {
                vbool16_t zfail = __riscv_vmand_mm_b16(cov, __riscv_vmnot_m_b16(wr, vl), vl);
                if (__riscv_vcpop_m_b16(zfail, vl)) {
                    size_t vl8z = __riscv_vsetvl_e8mf2(vl);
                    vuint8mf2_t svz = __riscv_vle8_v_u8mf2(&srow[x], vl8z);
                    svz = rvv_stencil_op_f16(zfail, stencil->zfail, svz, stencil, vl8z);
                    __riscv_vse8_v_u8mf2_m(zfail, &srow[x], svz, vl8z);
                }
                if (__riscv_vcpop_m_b16(wr, vl)) {
                    size_t vl8p = __riscv_vsetvl_e8mf2(vl);
                    vuint8mf2_t svp = __riscv_vle8_v_u8mf2(&srow[x], vl8p);
                    svp = rvv_stencil_op_f16(wr, stencil->zpass, svp, stencil, vl8p);
                    __riscv_vse8_v_u8mf2_m(wr, &srow[x], svp, vl8p);
                }
            }
            if (__riscv_vcpop_m_b16(wr, vl) == 0) {
                float fvl = (float)vl;
                e0s += Ax0 * fvl; e1s += Ax1 * fvl; e2s += Ax2 * fvl;
                x += (int)vl;
                continue;
            }

            if (cmask == 0) {
                if (depth_test && depth_write)
                    __riscv_vse32_v_f32m2_m(wr, &zrow[x], z, vl);
                float fvl = (float)vl;
                e0s += Ax0 * fvl; e1s += Ax1 * fvl; e2s += Ax2 * fvl;
                x += (int)vl;
                continue;
            }

            /* perspective-correct: divide interpolated (attr*invw) by interp invw */
            vfloat32m2_t iw = __riscv_vfmul_vf_f32m2(w0, a.invw, vl);
            iw = __riscv_vfmacc_vf_f32m2(iw, b.invw, w1, vl);
            iw = __riscv_vfmacc_vf_f32m2(iw, c.invw, w2, vl);
            vfloat32m2_t s = __riscv_vfrec7_v_f32m2(iw, vl);
            { vfloat32m2_t t = __riscv_vfmul_vv_f32m2(s, iw, vl);
              t = __riscv_vfrsub_vf_f32m2(t, 2.f, vl);
              s = __riscv_vfmul_vv_f32m2(s, t, vl); }

            vfloat32m2_t cr = __riscv_vfmul_vf_f32m2(w0, a.r, vl);
            cr = __riscv_vfmacc_vf_f32m2(cr, b.r, w1, vl); cr = __riscv_vfmacc_vf_f32m2(cr, c.r, w2, vl);
            vfloat32m2_t cg = __riscv_vfmul_vf_f32m2(w0, a.g, vl);
            cg = __riscv_vfmacc_vf_f32m2(cg, b.g, w1, vl); cg = __riscv_vfmacc_vf_f32m2(cg, c.g, w2, vl);
            vfloat32m2_t cb = __riscv_vfmul_vf_f32m2(w0, a.b, vl);
            cb = __riscv_vfmacc_vf_f32m2(cb, b.b, w1, vl); cb = __riscv_vfmacc_vf_f32m2(cb, c.b, w2, vl);
            vfloat32m2_t ca = __riscv_vfmul_vf_f32m2(w0, a.a, vl);
            ca = __riscv_vfmacc_vf_f32m2(ca, b.a, w1, vl); ca = __riscv_vfmacc_vf_f32m2(ca, c.a, w2, vl);
            cr = __riscv_vfmul_vv_f32m2(cr, s, vl);
            cg = __riscv_vfmul_vv_f32m2(cg, s, vl);
            cb = __riscv_vfmul_vv_f32m2(cb, s, vl);
            ca = __riscv_vfmul_vv_f32m2(ca, s, vl);

            if (tex) {
                vfloat32m2_t u = __riscv_vfmul_vf_f32m2(w0, a.u, vl);
                u = __riscv_vfmacc_vf_f32m2(u, b.u, w1, vl); u = __riscv_vfmacc_vf_f32m2(u, c.u, w2, vl);
                vfloat32m2_t v = __riscv_vfmul_vf_f32m2(w0, a.v, vl);
                v = __riscv_vfmacc_vf_f32m2(v, b.v, w1, vl); v = __riscv_vfmacc_vf_f32m2(v, c.v, w2, vl);
                u = __riscv_vfmul_vv_f32m2(u, s, vl);
                v = __riscv_vfmul_vv_f32m2(v, s, vl);
                /* Analytic screen-space UV derivatives for this batch (mip-LOD +
                 * aniso). Evaluated at the batch's first pixel (edge values e?s,
                 * scanline y). Gated so the mip/aniso-off fast path is unchanged. */
                const sgl_uv_deriv *pderiv = 0;
                sgl_uv_deriv duv;
                if (sgl_lod_deriv_enabled && tex->min_filter == 1 &&
                    (tex->max_aniso > 1.f || (sgl_mip_enabled && tex->nmip > 1))) {
                    float ev[3]  = { e0s, e1s, e2s };
                    float Axv[3] = { Ax0, Ax1, Ax2 };
                    float Ayv[3] = { Ay0, Ay1, Ay2 };
                    float iwv[3] = { a.invw, b.invw, c.invw };
                    float pu[3]  = { a.u, b.u, c.u };
                    float pvv[3] = { a.v, b.v, c.v };
                    sgl_vary_screen_deriv(ev, Axv, Ayv, invArea, iwv, pu,  &duv.dudx, &duv.dudy);
                    sgl_vary_screen_deriv(ev, Axv, Ayv, invArea, iwv, pvv, &duv.dvdx, &duv.dvdy);
                    pderiv = &duv;
                }
                vfloat32m2_t tr, tg, tb, ta;
                sgl_rvv_sample2d_m2(tex, u, v, vl, inv255, tex->min_filter, tex->wrap, pderiv, &tr, &tg, &tb, &ta);
                cr = __riscv_vfmul_vv_f32m2(cr, tr, vl);
                cg = __riscv_vfmul_vv_f32m2(cg, tg, vl);
                cb = __riscv_vfmul_vv_f32m2(cb, tb, vl);
                ca = __riscv_vfmul_vv_f32m2(ca, ta, vl);
            }

            if (blend != SGL_BLEND_NONE) {
                vuint32m2_t dst = __riscv_vle32_v_u32m2(&crow[x], vl);
                vfloat32m2_t dr = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(dst, 0xFFu, vl), vl), inv255, vl);
                vfloat32m2_t dg = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(dst, 8, vl), 0xFFu, vl), vl), inv255, vl);
                vfloat32m2_t db = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(dst, 16, vl), 0xFFu, vl), vl), inv255, vl);
                vfloat32m2_t da = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vsrl_vx_u32m2(dst, 24, vl), vl), inv255, vl);
                if (!sgl_blend_fast_m2(&cr, &cg, &cb, &ca, dr, dg, db, da, &st->blendf, vl))
                    sgl_blend_generic_m2(&cr, &cg, &cb, &ca, dr, dg, db, da, &st->blendf, vl);
            }

            cr = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cr, 0.f, vl), 1.f, vl);
            cg = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cg, 0.f, vl), 1.f, vl);
            cb = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cb, 0.f, vl), 1.f, vl);

            /* round-half-up quantisation + ordered dither (mean 0.5 -> no brightness shift) */
            vfloat32m2_t dth = sgl_store_bias_m2(x, y, vl);
            vuint32m2_t R = __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(cr, 255.f, vl), dth, vl), vl);
            vuint32m2_t G = __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(cg, 255.f, vl), dth, vl), vl);
            vuint32m2_t B = __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(cb, 255.f, vl), dth, vl), vl);
            vuint32m2_t col = R;
            col = __riscv_vor_vv_u32m2(col, __riscv_vsll_vx_u32m2(G, 8, vl), vl);
            col = __riscv_vor_vv_u32m2(col, __riscv_vsll_vx_u32m2(B, 16, vl), vl);
            {   /* C5: real src alpha into bits 31:24 (clamped, round-half-up) */
                vuint32m2_t A = __riscv_vfcvt_rtz_xu_f_v_u32m2(
                    __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(
                        __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(ca, 0.f, vl), 1.f, vl),
                        255.f, vl), 0.5f, vl), vl);
                col = __riscv_vor_vv_u32m2(col, __riscv_vsll_vx_u32m2(A, 24, vl), vl);
            }

            if (cmask == 0xF) {
                __riscv_vse32_v_u32m2_m(wr, &crow[x], col, vl);
            } else if (cmask) {                 /* per-channel write enable */
                uint32_t keep = 0;
                if (!(cmask & 1)) keep |= 0x000000FFu;
                if (!(cmask & 2)) keep |= 0x0000FF00u;
                if (!(cmask & 4)) keep |= 0x00FF0000u;
                if (!(cmask & 8)) keep |= 0xFF000000u;
                vuint32m2_t dst = __riscv_vle32_v_u32m2(&crow[x], vl);
                vuint32m2_t merged = __riscv_vor_vv_u32m2(__riscv_vand_vx_u32m2(col, ~keep, vl),
                                                          __riscv_vand_vx_u32m2(dst, keep, vl), vl);
                __riscv_vse32_v_u32m2_m(wr, &crow[x], merged, vl);
            }
            if (depth_test && depth_write)
                __riscv_vse32_v_f32m2_m(wr, &zrow[x], z, vl);

            float fvl = (float)vl;
            e0s += Ax0 * fvl; e1s += Ax1 * fvl; e2s += Ax2 * fvl;
            x += (int)vl;
            }
        }
#else
        {
            float e0 = row_e0, e1 = row_e1, e2 = row_e2;
            for (int x = minx; x <= maxx; ++x) {
                if (e0 >= 0.f && e1 >= 0.f && e2 >= 0.f) {
                    float w0 = e0 * invArea, w1 = e1 * invArea, w2 = e2 * invArea;
                    float z = w0 * a.z + w1 * b.z + w2 * c.z;
                    if (stencil_test && !sgl_stencil_pass(stencil, srow[x])) {
                        srow[x] = sgl_stencil_apply_op(stencil, stencil->sfail, srow[x]);
                    } else {
                        int zpass = !depth_test || sgl_depth_pass(depth_func, z, zrow[x]);
                        if (stencil_test && !zpass)
                            srow[x] = sgl_stencil_apply_op(stencil, stencil->zfail, srow[x]);
                        if (zpass) {
                            if (stencil_test)
                                srow[x] = sgl_stencil_apply_op(stencil, stencil->zpass, srow[x]);
                            if (cmask == 0) {
                                if (depth_test && depth_write) zrow[x] = z;
                                e0 += Ax0; e1 += Ax1; e2 += Ax2;
                                continue;
                            }
                            float s = 1.f / (w0 * a.invw + w1 * b.invw + w2 * c.invw);
                            float cr = (w0 * a.r + w1 * b.r + w2 * c.r) * s;
                            float cg = (w0 * a.g + w1 * b.g + w2 * c.g) * s;
                            float cb = (w0 * a.b + w1 * b.b + w2 * c.b) * s;
                            float ca = (w0 * a.a + w1 * b.a + w2 * c.a) * s;
                            if (tex) {
                                float u = (w0 * a.u + w1 * b.u + w2 * c.u) * s;
                                float v = (w0 * a.v + w1 * b.v + w2 * c.v) * s;
                                float ts[4]; sgl_sample(tex, u, v, ts);
                                cr *= ts[0]; cg *= ts[1]; cb *= ts[2]; ca *= ts[3];
                            }
                            if (blend != SGL_BLEND_NONE) {
                                uint32_t d = crow[x];
                                float cc[4] = { cr, cg, cb, ca };
                                float dd[4] = { (d & 0xFF) / 255.f, ((d >> 8) & 0xFF) / 255.f,
                                                ((d >> 16) & 0xFF) / 255.f, ((d >> 24) & 0xFF) / 255.f };
                                sgl_blend_generic_f(cc, dd, &st->blendf);
                                cr = cc[0]; cg = cc[1]; cb = cc[2]; ca = cc[3];
                            }
                            if (cmask == 0xF) {
                                crow[x] = pack_rgba(cr, cg, cb, ca);
                            } else if (cmask) {           /* per-channel write enable */
                                uint32_t pc = pack_rgba(cr, cg, cb, ca), d = crow[x];
                                uint32_t keep = 0;
                                if (!(cmask & 1)) keep |= 0x000000FFu;
                                if (!(cmask & 2)) keep |= 0x0000FF00u;
                                if (!(cmask & 4)) keep |= 0x00FF0000u;
                                if (!(cmask & 8)) keep |= 0xFF000000u;
                                crow[x] = (pc & ~keep) | (d & keep);
                            }
                            if (depth_test && depth_write) zrow[x] = z;
                        }
                    }
                }
                e0 += Ax0; e1 += Ax1; e2 += Ax2;
            }
        }
#endif
        row_e0 += Ay0; row_e1 += Ay1; row_e2 += Ay2;
    }
}

/* Programmable triangle: interpolate arbitrary varyings (perspective-correct)
 * and call the fragment shader per pixel. Scalar (the RVV fragment VM is next). */
/* ===================================================================== *
 * Hierarchical-Z: one coarse depth-max per HIZ_TILE square. Lets a whole
 * triangle be rejected before any pixel work when every covered tile is
 * already fully in front of it (LESS/LEQUAL only). Conservative by
 * construction: cull uses the triangle's global zmin against each tile's
 * stored max, so a kept tile can never wrong-cull a visible fragment.
 *
 * Refreshed exactly from the depth buffer once per draw (after the tile
 * fan-out joins) — no incremental staleness, no MT write race. The cull is
 * a pure read of g_hiz, safe from every hart during raster.
 *
 * Carmack-style: three tiny helpers, one static buffer, sized lazily to the
 * live framebuffer. Helps front-to-back overdraw (depth prepass, occluders
 * first); a no-op for low-overdraw 2D where it just costs one cheap scan.
 * ===================================================================== */
/* Coarser tiles under TCG: 32x32 halves the tile count vs 16x16 and cuts
 * refresh work ~4x while remaining conservative (cull uses tile max vs tri
 * zmin). Real-HW still benefits; slightly fewer culls on small tris. */
#define HIZ_TILE 32
int sgl_hiz_enabled = 1;
static float      *g_hiz;
static int         g_hiz_tw, g_hiz_th;
/* Which framebuffer g_hiz currently describes. The buffer is global and keyed
 * on dimensions, so two same-size FBOs would otherwise alias. The cull bails
 * (returns 0) whenever the live fb isn't the owner, so a stale buffer can only
 * cost a missed cull, never a wrong one. */
static const void *g_hiz_fb;
/* Deferred dirty rect: many depth-writing draws land per frame; coalesce their
 * rescan into one refresh before the next cull that needs a fresh buffer. */
static int g_hiz_dirty;
static int g_hiz_dx0, g_hiz_dy0, g_hiz_dx1, g_hiz_dy1;

static void hiz_ensure(int w, int h)
{
    int tw = (w + HIZ_TILE - 1) / HIZ_TILE;
    int th = (h + HIZ_TILE - 1) / HIZ_TILE;
    if (tw != g_hiz_tw || th != g_hiz_th) {
        free(g_hiz);
        g_hiz = (float *)malloc((size_t)tw * th * sizeof(float));
        g_hiz_tw = g_hiz ? tw : 0;
        g_hiz_th = g_hiz ? th : 0;
        g_hiz_dirty = 0;
    }
}

void sgl_hiz_clear(const sgl_framebuffer *fb, float depth)
{
    g_hiz_fb = 0;
    g_hiz_dirty = 0;
    if (!sgl_hiz_enabled || !fb) { g_hiz_tw = g_hiz_th = 0; return; }
    hiz_ensure(fb->w, fb->h);
#if SGL_RVV
    {
        size_t n = (size_t)g_hiz_tw * (size_t)g_hiz_th;
        for (size_t i = 0; i < n; ) {
            size_t vl = __riscv_vsetvl_e32m8(n - i);
            __riscv_vse32_v_f32m8(&g_hiz[i], __riscv_vfmv_v_f_f32m8(depth, vl), vl);
            i += vl;
        }
    }
#else
    for (int i = 0, n = g_hiz_tw * g_hiz_th; i < n; i++) g_hiz[i] = depth;
#endif
    g_hiz_fb = fb;
}

/* Recompute one tile's depth-max from the depth buffer. RVV: e32m2 redmax over
 * each tile row (up to HIZ_TILE floats) — wider LMUL than m1 for TCG throughput. */
static float hiz_tile_max(const float *depth, int w, int h, int tx, int ty)
{
    int x0 = tx * HIZ_TILE, x1 = x0 + HIZ_TILE; if (x1 > w) x1 = w;
    int y0 = ty * HIZ_TILE, y1 = y0 + HIZ_TILE; if (y1 > h) y1 = h;
    float m = 0.f;
#if SGL_RVV
    size_t rw = (size_t)(x1 - x0);
    /* Scalar identity for empty rect. */
    if (rw == 0 || y0 >= y1) return 0.f;
    size_t vl0 = __riscv_vsetvl_e32m2(rw);
    vfloat32m2_t vacc = __riscv_vfmv_v_f_f32m2(0.f, vl0);
    for (int y = y0; y < y1; y++) {
        const float *row = &depth[(size_t)y * w + x0];
        size_t off = 0;
        while (off < rw) {
            size_t vl = __riscv_vsetvl_e32m2(rw - off);
            vfloat32m2_t v = __riscv_vle32_v_f32m2(row + off, vl);
            vacc = __riscv_vfmax_vv_f32m2(vacc, v, vl);
            off += vl;
        }
    }
    {
        size_t vl = __riscv_vsetvl_e32m2(rw);
        vfloat32m1_t red = __riscv_vfredmax_vs_f32m2_f32m1(vacc,
            __riscv_vfmv_v_f_f32m1(0.f, __riscv_vsetvlmax_e32m1()), vl);
        m = __riscv_vfmv_f_s_f32m1_f32(red);
    }
#else
    for (int y = y0; y < y1; y++) {
        const float *row = &depth[(size_t)y * w + x0];
        for (int x = x0; x < x1; x++) { float d = *row++; if (d > m) m = d; }
    }
#endif
    return m;
}

/* Bring hierarchical-Z back in sync with the depth buffer over the screen
 * rect [rx0,rx1]x[ry0,ry1] (inclusive, the draw's bounding box). Tiles outside
 * the rect weren't touched by the draw, so their stored max is still exact and
 * we leave them be — this is the whole point: a 5%-of-screen draw rescans 5%
 * of the tiles, not the entire buffer. A full rescan (rect = whole screen) is
 * forced whenever g_hiz doesn't already describe this fb, since then the
 * untouched tiles can't be trusted. */
void sgl_hiz_refresh_rect(const sgl_framebuffer *fb, int rx0, int ry0, int rx1, int ry1)
{
    if (!sgl_hiz_enabled || !fb || !fb->depth) { g_hiz_fb = 0; g_hiz_tw = g_hiz_th = 0; g_hiz_dirty = 0; return; }
    int w = fb->w, h = fb->h;
    hiz_ensure(w, h);
    if (!g_hiz) { g_hiz_fb = 0; return; }
    if (g_hiz_fb != fb) { rx0 = 0; ry0 = 0; rx1 = w - 1; ry1 = h - 1; }  /* untouched tiles unsafe -> full */
    if (rx0 < 0) rx0 = 0; if (ry0 < 0) ry0 = 0;
    if (rx1 > w - 1) rx1 = w - 1; if (ry1 > h - 1) ry1 = h - 1;
    if (rx0 > rx1 || ry0 > ry1) { g_hiz_fb = fb; g_hiz_dirty = 0; return; }
    int tx0 = rx0 / HIZ_TILE, tx1 = rx1 / HIZ_TILE;
    int ty0 = ry0 / HIZ_TILE, ty1 = ry1 / HIZ_TILE;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++)
            g_hiz[ty * g_hiz_tw + tx] = hiz_tile_max(fb->depth, w, h, tx, ty);
    g_hiz_fb = fb;
    g_hiz_dirty = 0;
}

/* Mark a screen rect dirty instead of rescanning immediately. Multiple depth
 * writes coalesce into one refresh on the next cull. Always conservative: a
 * stale (pre-write) tile max can only miss a cull, never wrong-cull. */
void sgl_hiz_mark_dirty_rect(const sgl_framebuffer *fb, int rx0, int ry0, int rx1, int ry1)
{
    if (!sgl_hiz_enabled || !fb || !fb->depth) return;
    if (g_hiz_fb != fb) {
        /* Different fb or never cleared: force full ownership transfer later. */
        sgl_hiz_refresh_rect(fb, 0, 0, fb->w - 1, fb->h - 1);
        return;
    }
    if (rx0 < 0) rx0 = 0; if (ry0 < 0) ry0 = 0;
    if (rx1 > fb->w - 1) rx1 = fb->w - 1; if (ry1 > fb->h - 1) ry1 = fb->h - 1;
    if (rx0 > rx1 || ry0 > ry1) return;
    if (!g_hiz_dirty) {
        g_hiz_dx0 = rx0; g_hiz_dy0 = ry0; g_hiz_dx1 = rx1; g_hiz_dy1 = ry1;
        g_hiz_dirty = 1;
    } else {
        if (rx0 < g_hiz_dx0) g_hiz_dx0 = rx0;
        if (ry0 < g_hiz_dy0) g_hiz_dy0 = ry0;
        if (rx1 > g_hiz_dx1) g_hiz_dx1 = rx1;
        if (ry1 > g_hiz_dy1) g_hiz_dy1 = ry1;
    }
}

static void hiz_flush_dirty(const sgl_framebuffer *fb)
{
    if (!g_hiz_dirty || !fb) return;
    sgl_hiz_refresh_rect(fb, g_hiz_dx0, g_hiz_dy0, g_hiz_dx1, g_hiz_dy1);
}

/* Full-screen refresh (rescans every tile). */
void sgl_hiz_refresh(const sgl_framebuffer *fb)
{
    g_hiz_fb = 0;   /* force the rect path to treat this as a full rescan */
    if (fb) sgl_hiz_refresh_rect(fb, 0, 0, fb->w - 1, fb->h - 1);
}

/* 1 = every covered tile is fully in front of the triangle -> reject it.
 * Bails unless g_hiz currently describes this very framebuffer. Flushes any
 * deferred dirty rect first so cull sees post-write tile maxes. */
static int hiz_cull(const sgl_framebuffer *fb, int minx, int miny, int maxx, int maxy, float zmin)
{
    hiz_flush_dirty(fb);
    if (!g_hiz_tw || fb != g_hiz_fb) return 0;
    int tx0 = minx / HIZ_TILE, tx1 = maxx / HIZ_TILE;
    int ty0 = miny / HIZ_TILE, ty1 = maxy / HIZ_TILE;
    if (tx1 >= g_hiz_tw) tx1 = g_hiz_tw - 1;
    if (ty1 >= g_hiz_th) ty1 = g_hiz_th - 1;
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    for (int ty = ty0; ty <= ty1; ty++) {
        const float *trow = &g_hiz[ty * g_hiz_tw];
        for (int tx = tx0; tx <= tx1; tx++)
            if (zmin < trow[tx]) return 0;   /* this tile may still be visible */
    }
    return 1;
}

void sgl_raster_tri_prog(sgl_framebuffer *fb,
                         const sgl_pvert *A, const sgl_pvert *B, const sgl_pvert *C,
                         int nvary, const sgl_drawstate *st,
                         sgl_frag_fn frag, sgl_frag_simt_fn frag_simt, void *ctx)
{
    SGL_DIAG_INC(sgl_dbg_tris);
    sgl_pvert a = *A, b = *B, c = *C;
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.f) return;
    if (area < 0.f) { sgl_pvert t = b; b = c; c = t; area = -area; }
    float invArea = 1.f / area;

    {   /* glPolygonOffset: fold the per-triangle depth bias into each vertex z */
        float zoff = sgl_poly_offset_z(st, invArea, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z);
        a.z += zoff; b.z += zoff; c.z += zoff;
    }

    int minx = ifloor(fminf_(a.x, fminf_(b.x, c.x))), maxx = ifloor(fmaxf_(a.x, fmaxf_(b.x, c.x)));
    int miny = ifloor(fminf_(a.y, fminf_(b.y, c.y))), maxy = ifloor(fmaxf_(a.y, fmaxf_(b.y, c.y)));
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
    if (maxx > fb->w - 1) maxx = fb->w - 1; if (maxy > fb->h - 1) maxy = fb->h - 1;
    if (st->scissor_on) {                       /* clamp before seeding edges */
        if (minx < st->sx0) minx = st->sx0;
        if (miny < st->sy0) miny = st->sy0;
        if (maxx > st->sx1) maxx = st->sx1;
        if (maxy > st->sy1) maxy = st->sy1;
    }
    if (st->clip_x1 > 0) {                       /* parallel tile clip */
        if (minx < st->clip_x0) minx = st->clip_x0;
        if (maxx > st->clip_x1 - 1) maxx = st->clip_x1 - 1;
    }
    if (st->clip_y1 > 0) {
        if (miny < st->clip_y0) miny = st->clip_y0;
        if (maxy > st->clip_y1 - 1) maxy = st->clip_y1 - 1;
    }
    if (minx > maxx || miny > maxy || nvary > SGL_MAX_VARY) return;
    SGL_DIAG_INC(sgl_dbg_tris_onscreen);

    const int depth_func  = st->depth_func;
    const int depth_write = st->depth_write;
    const int cmask       = st->color_mask;
    const int stencil_test = st->stencil_test && fb->stencil;
    const sgl_stencil_face *stencil =
        st->stencil_frontface ? &st->stencil_front : &st->stencil_back;

    /* Hierarchical-Z whole-triangle reject (front-to-back occlusion). Only for
     * depth_test + LESS/LEQUAL, where a depth-failed fragment has NO side effect
     * (no colour, no depth, no blend src). A stencil pass whose sfail/zfail op
     * writes — e.g. z-fail shadow volumes increment/decrement on depth fail — DOES
     * have a side effect on exactly the fragments this cull drops, so skip the cull
     * there or we'd lose stencil writes and corrupt shadows. zmin is the triangle's
     * nearest vertex depth -> conservative against every tile. */
    int hiz_safe = !stencil_test ||
        (stencil->sfail == SGL_STENCIL_KEEP && stencil->zfail == SGL_STENCIL_KEEP);
    if (sgl_hiz_enabled && hiz_safe && st->depth_test &&
        (depth_func == SGL_DEPTH_LESS || depth_func == SGL_DEPTH_LEQUAL)) {
        float zmin = fminf_(a.z, fminf_(b.z, c.z));
        if (hiz_cull(fb, minx, miny, maxx, maxy, zmin)) {
            SGL_DIAG_INC(sgl_dbg_tris_hiz_culled);
            return;
        }
    }

    const float Ax0 = -(c.y - b.y), Ax1 = -(a.y - c.y), Ax2 = -(b.y - a.y);
    float px0 = (float)minx + 0.5f, py0 = (float)miny + 0.5f;
    float row_e0 = (c.x - b.x) * (py0 - b.y) - (c.y - b.y) * (px0 - b.x);
    float row_e1 = (a.x - c.x) * (py0 - c.y) - (a.y - c.y) * (px0 - c.x);
    float row_e2 = (b.x - a.x) * (py0 - a.y) - (b.y - a.y) * (px0 - a.x);
    const float Ay0 = (c.x - b.x), Ay1 = (a.x - c.x), Ay2 = (b.x - a.x);

#if SGL_RVV
    /* ===== batched SIMT path: vectorised coverage + early-Z, SoA varying gather,
     * one fragment-VM dispatch per lane-batch. Gated on a batched shader hook;
     * NULL (shader not SIMT-vectorisable, e.g. uses discard/indexing) falls back
     * to the scalar oracle below. Lanes = an LMUL=2 run of horizontal pixels, the
     * same lane model the VM uses, so vl matches one vsetvli on both sides. ===== */
    if (frag_simt && nvary <= SGL_MAX_VARY) {
        const float inv255 = 1.f / 255.f;
        const int   depth_test = st->depth_test;
        const int   blend      = st->blend;
        /* ALWAYS cannot fail: no stored-value read or sfail operation needed.
         * Depth-dependent stencil writes below remain unchanged. */
        const int stencil_compare = stencil_test &&
            (!sgl_stencil_always_fast || stencil->func != SGL_DEPTH_ALWAYS);
        const int stencil_sfail_write = !sgl_stencil_noop_prune ||
            (stencil->write_mask && stencil->sfail != SGL_STENCIL_KEEP);
        const int stencil_depth_write = !sgl_stencil_noop_prune ||
            (stencil->write_mask && (stencil->zfail != SGL_STENCIL_KEEP ||
                                     stencil->zpass != SGL_STENCIL_KEEP));
        /* GL_EXT_depth_bounds_test: cull lanes whose existing (destination) depth is
         * outside [min,max]. Doom 3 sets it per light to the light's depth extent so
         * geometry outside the light volume in Z isn't shaded/stencil-filled. */
        const int   db_on  = st->depth_bounds_on && fb->depth;
        const float db_min = st->depth_bounds_min, db_max = st->depth_bounds_max;
        /* Profiling accumulates into call-local counters and flushes ONCE at the
         * end of the triangle (below) instead of an atomic per 64-lane batch.
         * The per-batch atomics bounced the sgl_prof_* cache lines across harts,
         * inflating the very numbers they measured (Lever-2 critique). */
        uint64_t pf_loc_frags = 0, pf_loc_lanes = 0, pf_loc_shade = 0;
        /* Premultiply each varying by its vertex's invw ONCE per triangle. The
         * perspective-correct numerator is then a pure w0*vpa+w1*vpb+w2*vpc
         * vfmacc.vf chain, and the rational divide collapses to a SINGLE vector
         * reciprocal (1/denominator) per lane-batch, reused across every varying
         * via vfmul — the divide is amortised over nvary*vl interpolations. */
        float vpa[SGL_MAX_VARY], vpb[SGL_MAX_VARY], vpc[SGL_MAX_VARY];
#define SGL_PREMUL_VARY(K) do { vpa[(K)]=a.vary[(K)]*a.invw; vpb[(K)]=b.vary[(K)]*b.invw; vpc[(K)]=c.vary[(K)]*c.invw; } while (0)
        /* Constant trip counts cover every hot gl4es layout and let GCC remove
         * the dynamic loop compare/address scaling from this per-tile setup. */
        /* Color-masked, non-discard passes finish before interpolation. Do not
         * prepare shader inputs for these depth/shadow-volume triangles. */
        if (!sgl_depth_only_setup_prune || cmask || st->has_discard) {
        if (nvary==32) { for (int k=0;k<32;++k) SGL_PREMUL_VARY(k); }
        else if (nvary==12) { for (int k=0;k<12;++k) SGL_PREMUL_VARY(k); }
        else if (nvary==8) { for (int k=0;k<8;++k) SGL_PREMUL_VARY(k); }
        else { for (int k=0;k<nvary;++k) SGL_PREMUL_VARY(k); }
        }
#undef SGL_PREMUL_VARY
        sg_vary_soa varys;
        sg_fragout_soa out;   /* RGBA rows 0-3 + discard/kill row 4 (SIMT alpha-test) */
        for (int y = miny; y <= maxy; ++y) {
            uint32_t *crow = &fb->color[y * fb->w];
            float    *zrow = &fb->depth[y * fb->w];
            uint8_t  *srow = fb->stencil ? &fb->stencil[y * fb->w] : 0;
            /* Per-scanline active span: clip [minx,maxx] to the columns where all
             * three edge functions are >= 0 on THIS row, so we never iterate (and pay
             * edge-eval + coverage vcpop for) the empty batches left/right of the
             * triangle inside its bounding box. Edge i at column x (x>=minx) is
             * row_ei + Axi*(x-minx); Axi>0 gives a left bound, Axi<0 a right bound,
             * Axi==0 an all-or-nothing row. The per-lane cov mask below still does
             * exact coverage, so a conservative superset suffices — bounds are floored
             * out / ceiled up by a pixel for FP safety. Row-edge advance at loop end
             * is unaffected: an empty row just runs the x-loop zero times. */
            int sxL = minx, sxR = maxx, sempty = 0;
            if (Ax0 > 0.f)      { int t = minx + ifloor(-row_e0 / Ax0); if (t > sxL) sxL = t; }
            else if (Ax0 < 0.f) { int t = minx + iceil (-row_e0 / Ax0); if (t < sxR) sxR = t; }
            else if (row_e0 < 0.f) sempty = 1;
            if (Ax1 > 0.f)      { int t = minx + ifloor(-row_e1 / Ax1); if (t > sxL) sxL = t; }
            else if (Ax1 < 0.f) { int t = minx + iceil (-row_e1 / Ax1); if (t < sxR) sxR = t; }
            else if (row_e1 < 0.f) sempty = 1;
            if (Ax2 > 0.f)      { int t = minx + ifloor(-row_e2 / Ax2); if (t > sxL) sxL = t; }
            else if (Ax2 < 0.f) { int t = minx + iceil (-row_e2 / Ax2); if (t < sxR) sxR = t; }
            else if (row_e2 < 0.f) sempty = 1;
            sxL -= 1; sxR += 1;                         /* FP-safety widen; cov mask is exact */
            if (sxL < minx) sxL = minx;
            if (sxR > maxx) sxR = maxx;
            if (sempty) sxR = sxL - 1;                  /* run the x-loop zero times */
            float e0s = row_e0 + Ax0 * (float)(sxL - minx);
            float e1s = row_e1 + Ax1 * (float)(sxL - minx);
            float e2s = row_e2 + Ax2 * (float)(sxL - minx);
            for (int x = sxL; x <= sxR; ) {
                PARETO_BEGIN();
                size_t span = (size_t)(sxR - x + 1);
                size_t vl = __riscv_vsetvl_e32m2(span < SGL_SIMT_LANES ? span : SGL_SIMT_LANES);
                vfloat32m2_t lane = __riscv_vfcvt_f_xu_v_f32m2(__riscv_vid_v_u32m2(vl), vl);
                vfloat32m2_t e0 = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(lane, Ax0, vl), e0s, vl);
                vfloat32m2_t e1 = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(lane, Ax1, vl), e1s, vl);
                vfloat32m2_t e2 = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(lane, Ax2, vl), e2s, vl);
                vbool16_t cov = __riscv_vmand_mm_b16(
                                    __riscv_vmand_mm_b16(__riscv_vmfge_vf_f32m2_b16(e0, 0.f, vl),
                                                         __riscv_vmfge_vf_f32m2_b16(e1, 0.f, vl), vl),
                                    __riscv_vmfge_vf_f32m2_b16(e2, 0.f, vl), vl);
                /* Batch-start edge values (lane 0's pixel), captured before the
                 * edges advance -- the representative point for this batch's
                 * analytic UV derivatives (see the dispatch site below). */
                float ex0 = e0s, ex1 = e1s, ex2 = e2s;
                /* advance the incremental edges for the next batch up front, so the
                 * coverage/depth early-outs below can just continue. */
                float fvl = (float)vl;
                e0s += Ax0 * fvl; e1s += Ax1 * fvl; e2s += Ax2 * fvl;

                if (__riscv_vcpop_m_b16(cov, vl) == 0) { x += (int)vl; PARETO_END(); continue; }

                /* depth-bounds reject (before stencil/depth, on the destination
                 * depth) — culls stencil writes too, matching the EXT semantics. */
                if (db_on) {
                    vfloat32m2_t zb = __riscv_vle32_v_f32m2(&zrow[x], vl);
                    cov = __riscv_vmand_mm_b16(cov, __riscv_vmand_mm_b16(
                              __riscv_vmfge_vf_f32m2_b16(zb, db_min, vl),
                              __riscv_vmfle_vf_f32m2_b16(zb, db_max, vl), vl), vl);
                    if (__riscv_vcpop_m_b16(cov, vl) == 0) { x += (int)vl; PARETO_END(); continue; }
                }

                vfloat32m2_t w0 = __riscv_vfmul_vf_f32m2(e0, invArea, vl);
                vfloat32m2_t w1 = __riscv_vfmul_vf_f32m2(e1, invArea, vl);
                vfloat32m2_t w2 = __riscv_vfmul_vf_f32m2(e2, invArea, vl);
                /* #5: interpolate depth only when it can be read (early-Z or
                 * depth-write). The 2D overlay runs depth_test off, so this
                 * skips 3 FMA/batch there. (Both z uses below are depth-gated.) */
                vfloat32m2_t z = __riscv_vundefined_f32m2();
                if (depth_test) {
                    z = __riscv_vfmul_vf_f32m2(w0, a.z, vl);
                    z = __riscv_vfmacc_vf_f32m2(z, b.z, w1, vl);
                    z = __riscv_vfmacc_vf_f32m2(z, c.z, w2, vl);
                }

                /* ---- stencil test (before early-Z) ---- */
                if (stencil_compare) {
                    size_t vl8 = __riscv_vsetvl_e8mf2(vl);
                    vuint8mf2_t sv = __riscv_vle8_v_u8mf2(&srow[x], vl8);
                    vuint32m2_t sv32 = __riscv_vzext_vf4_u32m2(sv, vl);
                    uint32_t reff = (uint32_t)(stencil->ref & stencil->read_mask);
                    uint32_t rmask = (uint32_t)stencil->read_mask;
                    vuint32m2_t masked = __riscv_vand_vx_u32m2(sv32, rmask, vl);
                    vbool16_t sp;
                    switch (stencil->func) {
                    case SGL_DEPTH_NEVER:    sp = __riscv_vmseq_vv_u32m2_b16(masked, __riscv_vadd_vx_u32m2(masked, 1, vl), vl); break;
                    case SGL_DEPTH_LESS:     sp = __riscv_vmsgtu_vx_u32m2_b16(masked, reff, vl); break;
                    case SGL_DEPTH_EQUAL:    sp = __riscv_vmseq_vx_u32m2_b16(masked, reff, vl); break;
                    case SGL_DEPTH_LEQUAL:   sp = __riscv_vmsgeu_vx_u32m2_b16(masked, reff, vl); break;
                    case SGL_DEPTH_GREATER:  sp = __riscv_vmsltu_vx_u32m2_b16(masked, reff, vl); break;
                    case SGL_DEPTH_NOTEQUAL: sp = __riscv_vmsne_vx_u32m2_b16(masked, reff, vl); break;
                    case SGL_DEPTH_GEQUAL:   sp = __riscv_vmsleu_vx_u32m2_b16(masked, reff, vl); break;
                    case SGL_DEPTH_ALWAYS:
                    default:                 sp = cov; break;
                    }
                    if (stencil_sfail_write) {
                        vbool16_t sfail = __riscv_vmand_mm_b16(cov, __riscv_vmnot_m_b16(sp, vl), vl);
                        if (__riscv_vcpop_m_b16(sfail, vl)) {
                            sv = rvv_stencil_op_f16(sfail, stencil->sfail, sv, stencil, vl8);
                            __riscv_vse8_v_u8mf2_m(sfail, &srow[x], sv, vl8);
                        }
                    }
                    cov = __riscv_vmand_mm_b16(cov, sp, vl);
                    if (__riscv_vcpop_m_b16(cov, vl) == 0) { x += (int)vl; PARETO_END(); continue; }
                }

                /* vectorised early-Z: cull occluded lanes BEFORE the shader runs. */
                vbool16_t wr = cov;
                if (depth_test && depth_func != SGL_DEPTH_ALWAYS) {
                    vfloat32m2_t zb = __riscv_vle32_v_f32m2(&zrow[x], vl);
                    vbool16_t dp;
                    switch (depth_func) {
                    case SGL_DEPTH_NEVER:    dp = __riscv_vmxor_mm_b16(cov, cov, vl); break;
                    case SGL_DEPTH_EQUAL: {
                        vfloat32m2_t d = __riscv_vfabs_v_f32m2(__riscv_vfsub_vv_f32m2(z, zb, vl), vl);
                        dp = __riscv_vmfle_vf_f32m2_b16(d, SGL_DEPTH_EPS, vl); break; }
                    case SGL_DEPTH_LEQUAL:   dp = __riscv_vmfle_vv_f32m2_b16(z, zb, vl); break;
                    case SGL_DEPTH_GREATER:  dp = __riscv_vmfgt_vv_f32m2_b16(z, zb, vl); break;
                    case SGL_DEPTH_NOTEQUAL: {
                        vfloat32m2_t d = __riscv_vfabs_v_f32m2(__riscv_vfsub_vv_f32m2(z, zb, vl), vl);
                        dp = __riscv_vmfgt_vf_f32m2_b16(d, SGL_DEPTH_EPS, vl); break; }
                    case SGL_DEPTH_GEQUAL:   dp = __riscv_vmfge_vv_f32m2_b16(z, zb, vl); break;
                    case SGL_DEPTH_LESS:
                    default:                 dp = __riscv_vmflt_vv_f32m2_b16(z, zb, vl); break;
                    }
                    wr = __riscv_vmand_mm_b16(cov, dp, vl);
                    if (__riscv_vcpop_m_b16(wr, vl) == 0) { x += (int)vl; PARETO_END(); continue; }
                }

                /* ---- stencil zfail/zpass (after depth test) ---- */
                /* Item 6 (RVV audit): zfail and wr(=zpass) are disjoint sub-masks
                 * of cov, and rvv_stencil_op_f16 preserves non-masked lanes, so
                 * load srow + set the e8 vl ONCE, apply both ops to one register,
                 * and do a single masked store (union mask). Bit-identical to the
                 * prior load/op/store x2; removes 1 vle8 + 1 vse8 + 1 e8 SEW-toggle
                 * vsetvl per batch on shadow-volume (stencil) passes. */
                if (stencil_test && stencil_depth_write) {
                    vbool16_t zfail = __riscv_vmand_mm_b16(cov, __riscv_vmnot_m_b16(wr, vl), vl);
                    int any_zf = __riscv_vcpop_m_b16(zfail, vl);
                    int any_zp = __riscv_vcpop_m_b16(wr, vl);
                    if (any_zf || any_zp) {
                        /* build the union store-mask while still at e32m2 vtype, so
                         * the only SEW transition is the single e32m2->e8mf2 below. */
                        vbool16_t store_mask = __riscv_vmor_mm_b16(zfail, wr, vl);
                        size_t vl8 = __riscv_vsetvl_e8mf2(vl);
                        vuint8mf2_t s = __riscv_vle8_v_u8mf2(&srow[x], vl8);
                        if (any_zf) s = rvv_stencil_op_f16(zfail, stencil->zfail, s, stencil, vl8);
                        if (any_zp) s = rvv_stencil_op_f16(wr, stencil->zpass, s, stencil, vl8);
                        __riscv_vse8_v_u8mf2_m(store_mask, &srow[x], s, vl8);
                    }
                }

                /* depth-write bookkeeping BEFORE the cmask gate, so a depth-only
                 * pre-pass (cmask==0, depth_write==1) still lays down Z correctly. */
                /* zpass count is diagnostic-only; the vcpop is dead work in
                 * production (diag off), so don't pay it in the hot loop. */
                unsigned long zpass = 0;
                if (sgl_diag_enabled) {
                    zpass = (unsigned long)__riscv_vcpop_m_b16(wr, vl);
                    sgl_dbg_frag_zpass += zpass;
                }
                /* Lever 1: shaded-fragment denominator. Accumulated call-local
                 * (flushed once below) so it doesn't bounce a shared cache line. */
                int live_lanes = (int)__riscv_vcpop_m_b16(wr, vl);
                if (sgl_prof_enabled) {
                    pf_loc_frags += (uint64_t)live_lanes;
                    pf_loc_lanes += (uint64_t)vl;
                }
                /* Non-discard fast path writes depth here (before shading). Discard
                 * shaders defer the depth-write to AFTER the shader (a discarded
                 * fragment writes neither colour nor depth) — see below. */
                if (depth_test && depth_write && !st->has_discard)
                    __riscv_vse32_v_f32m2_m(wr, &zrow[x], z, vl);

                /* cmask gate: colour fully masked off (Doom 3 stencil / shadow-volume
                 * passes use glColorMask(0,0,0,0)) means the fragment output is
                 * discarded — skip the varying gather AND the VM dispatch entirely.
                 * Coverage + early-Z are already resolved above; this reclaims the
                 * full per-pixel shading cost of screen-filling shadow geometry. */
                if (cmask == 0 && !st->has_discard) { SGL_DIAG_ADD(sgl_dbg_frag_cmask0, zpass); x += (int)vl; PARETO_END(); continue; }
                /* No surviving lanes after early-Z (and any prior mask): do not
                 * build 32 varyings or call the interaction kernel. */
                if (live_lanes == 0) { x += (int)vl; PARETO_END(); continue; }

                PARETO_STAGE(1);
                /* perspective-correct varying gather into SoA: one reciprocal,
                 * then an FMA chain per varying (same op order as sgl_raster_tri). */
                /* Sparse batches waste most of the 8/12/32-vary interpolation
                 * and shader kernel on dead lanes. Pack at <=50% occupancy,
                 * shade a dense prefix, then scatter output rows back. */
                size_t raster_vl = vl, shade_vl = vl;
                /* Pack when occupancy ≤75% (was 50%). Lit Doom frames measure
                 * occ≈55–80% under early-Z, so the old threshold almost never
                 * fired; 75% still amortises compress/scatter vs full shade. */
                int packed = (live_lanes >= 4 && live_lanes * 4 <= (int)vl * 3);
                uint32_t packed_lane[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
                vfloat32m2_t sw0=w0, sw1=w1, sw2=w2;
                if (packed) {
                    vuint32m2_t li=__riscv_vid_v_u32m2(vl);
                    vuint32m2_t pi=__riscv_vcompress_vm_u32m2(li,wr,vl);
                    sw0=__riscv_vcompress_vm_f32m2(w0,wr,vl);
                    sw1=__riscv_vcompress_vm_f32m2(w1,wr,vl);
                    sw2=__riscv_vcompress_vm_f32m2(w2,wr,vl);
                    shade_vl=__riscv_vsetvl_e32m2((size_t)live_lanes);
                    __riscv_vse32_v_u32m2(packed_lane,pi,shade_vl);
                }
                vfloat32m2_t iw = __riscv_vfmul_vf_f32m2(sw0, a.invw, shade_vl);
                iw = __riscv_vfmacc_vf_f32m2(iw, b.invw, sw1, shade_vl);
                iw = __riscv_vfmacc_vf_f32m2(iw, c.invw, sw2, shade_vl);
                /* Floor the perspective denominator before reciprocal. On grazing
                 * overhead slivers the float32 edge-function accumulation can admit
                 * lanes whose true barycentric is marginally negative, driving iw to
                 * ~0 or slightly negative; vfrec7 (7-bit seed + 1 Newton step) then
                 * returns a huge/negative s that blows the varyings up -> shader
                 * clamps NaN->0 -> black triangular shards near ceilings. The true
                 * in-triangle iw is always >= the smallest vertex invw, so this floor
                 * never perturbs well-formed coverage. */
                iw = __riscv_vfmax_vf_f32m2(iw, 1e-7f, shade_vl);
                vfloat32m2_t s = __riscv_vfrec7_v_f32m2(iw, shade_vl);
                { vfloat32m2_t t = __riscv_vfmul_vv_f32m2(s, iw, shade_vl);
                  t = __riscv_vfrsub_vf_f32m2(t, 2.f, shade_vl);
                  s = __riscv_vfmul_vv_f32m2(s, t, shade_vl); }
#define SGL_INTERP_VARY(K) do { \
                    vfloat32m2_t num=__riscv_vfmul_vf_f32m2(sw0,vpa[(K)],shade_vl); \
                    num=__riscv_vfmacc_vf_f32m2(num,vpb[(K)],sw1,shade_vl); \
                    num=__riscv_vfmacc_vf_f32m2(num,vpc[(K)],sw2,shade_vl); \
                    __riscv_vse32_v_f32m2(varys.c[(K)],__riscv_vfmul_vv_f32m2(num,s,shade_vl),shade_vl); \
                } while (0)
                /* Skip varyings the bound kernel never reads. The gl4es
                 * interaction shader declares 32 floats and reads 18, so this
                 * removes 14 x (2 FMA + 2 mul + store) per batch on the shader
                 * that dominates lit-world shading. */
                unsigned vlive = sgl_vary_liveness_enabled ? st->vary_live_mask : 0u;
                if (vlive) {
                    unsigned m = vlive;
                    while (m) { int k = __builtin_ctz(m); m &= m - 1u; SGL_INTERP_VARY(k); }
                } else if (nvary==32) {
#pragma GCC unroll 4
                    for (int k=0;k<32;++k) SGL_INTERP_VARY(k);
                } else if (nvary==12) {
#pragma GCC unroll 4
                    for (int k=0;k<12;++k) SGL_INTERP_VARY(k);
                } else if (nvary==8) {
#pragma GCC unroll 4
                    for (int k=0;k<8;++k) SGL_INTERP_VARY(k);
                } else for (int k=0;k<nvary;++k) {
                    vfloat32m2_t num = __riscv_vfmul_vf_f32m2(sw0, vpa[k], shade_vl);
                    num = __riscv_vfmacc_vf_f32m2(num, vpb[k], sw1, shade_vl);
                    num = __riscv_vfmacc_vf_f32m2(num, vpc[k], sw2, shade_vl);
                    __riscv_vse32_v_f32m2(varys.c[k], __riscv_vfmul_vv_f32m2(num, s, shade_vl), shade_vl);
                }
#undef SGL_INTERP_VARY
                PARETO_STAGE(2);

                /* one VM dispatch shades the whole batch (predicated, dead lanes
                 * computed but never stored — cheaper than vcompress repacking). */
                SGL_DIAG_ADD(sgl_dbg_frag_shaded, zpass);
                /* Per-batch analytic UV derivatives (mip-LOD + anisotropy),
                 * threaded to the interaction kernels via the same global-array
                 * pattern as gl_FragCoord. Computed once per batch from the plane
                 * data at the batch-start pixel, so it is exact and independent of
                 * the vcompress packing done above (which broke the old two-lane
                 * estimate). Gated: when derivatives are disabled (bm_mip 0) the
                 * kernels see deriv_valid==0 and pass NULL, and this whole block is
                 * skipped -- the mip-OFF fast path is unchanged. */
                if (sgl_lod_deriv_enabled && !st->skip_derivatives) {
                    float ev[3]  = { ex0, ex1, ex2 };
                    float Axv[3] = { Ax0, Ax1, Ax2 };
                    float Ayv[3] = { Ay0, Ay1, Ay2 };
                    float iwv[3] = { a.invw, b.invw, c.invw };
                    unsigned deriv_mask = st->derivative_mask ? st->derivative_mask : vlive;
                    if (deriv_mask) {
                        unsigned m = deriv_mask;
                        while (m) {
                            int k = __builtin_ctz(m); m &= m - 1u;
                            float p3[3] = { vpa[k], vpb[k], vpc[k] };
                            sgl_vary_screen_deriv(ev, Axv, Ayv, invArea, iwv, p3,
                                                  &sgl_simt_vary_ddx[k], &sgl_simt_vary_ddy[k]);
                        }
                    } else {
                        for (int k = 0; k < nvary; ++k) {
                            float p3[3] = { vpa[k], vpb[k], vpc[k] };
                            sgl_vary_screen_deriv(ev, Axv, Ayv, invArea, iwv, p3,
                                                  &sgl_simt_vary_ddx[k], &sgl_simt_vary_ddy[k]);
                        }
                    }
                    sgl_simt_deriv_valid = 1;
                } else {
                    sgl_simt_deriv_valid = 0;
                }
                /* gl_FragCoord for heatHaze / post-process: pixel centres of the
                 * (optionally packed) shade lanes. */
                if (!st->skip_fragcoord) {
                    if (packed)
                        sgl_simt_set_fragcoord_packed(x, packed_lane, y, (int)shade_vl);
                    else
                        sgl_simt_set_fragcoord_run(x, y, (int)shade_vl);
                }
                /* split mainbusy: time the shader call vs the raster wrapper. */
                uint64_t sh0 = sgl_prof_enabled ? sgl_prof_now() : 0;
                sg_fragout_soa packed_out;
                float *shader_out = packed ? &packed_out.c[0][0] : &out.c[0][0];
                PARETO_STAGE(3);
                frag_simt(ctx, &varys.c[0][0], (int)shade_vl, shader_out);
                PARETO_STAGE(4);
                if (sgl_prof_enabled) pf_loc_shade += sgl_prof_now() - sh0;
                if (packed) {
                    vuint32m2_t pi=__riscv_vle32_v_u32m2(packed_lane,shade_vl);
                    vuint32m2_t off=__riscv_vsll_vx_u32m2(pi,2,shade_vl);
                    /* Non-discard shaders never write kill-mask row 4 — scatter 4
                     * channels only (saves one indexed store per packed batch). */
                    int nrows = st->has_discard ? 5 : 4;
                    for (int r=0;r<nrows;++r)
                        __riscv_vsuxei32_v_f32m2(out.c[r],off,
                            __riscv_vle32_v_f32m2(packed_out.c[r],shade_vl),shade_vl);
                    vl=__riscv_vsetvl_e32m2(raster_vl);
                }

                /* SIMT discard/alpha-test: the shader wrote a per-lane kill mask in
                 * row 4. Remove killed lanes from the write coverage, THEN do the
                 * deferred depth-write — a discarded fragment writes neither colour
                 * nor depth (GLES2 order: shader/discard precedes the depth/colour
                 * write). Non-discard draws already wrote depth early, above. */
                if (st->has_discard) {
                    vbool16_t kill = __riscv_vmfne_vf_f32m2_b16(__riscv_vle32_v_f32m2(out.c[4], vl), 0.f, vl);
                    wr = __riscv_vmand_mm_b16(wr, __riscv_vmnot_m_b16(kill, vl), vl);
                    if (depth_test && depth_write)
                        __riscv_vse32_v_f32m2_m(wr, &zrow[x], z, vl);
                    if (cmask == 0) { x += (int)vl; PARETO_END(); continue; }   /* depth-only alpha-test pass: done */
                }

                /* gl4es attenuation passes often write alpha only. Preserve
                 * the exact blend dispatch, but let dead-code elimination omit
                 * its unused RGB math, unpacking, dithering and quantization. */
                if (sgl_alpha_only_store && cmask == 8 && !sgl_diag_enabled) {
                    vuint32m2_t dst = __riscv_vle32_v_u32m2(&crow[x], vl);
                    vfloat32m2_t alpha = __riscv_vle32_v_f32m2(out.c[3], vl);
                    if (blend != SGL_BLEND_NONE) {
                        vfloat32m2_t da = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(
                            __riscv_vsrl_vx_u32m2(dst, 24, vl), vl), inv255, vl);
                        vfloat32m2_t unused_r=alpha, unused_g=alpha, unused_b=alpha;
                        if (!sgl_blend_fast_m2(&unused_r,&unused_g,&unused_b,&alpha,da,da,da,da,&st->blendf,vl))
                            sgl_blend_generic_m2(&unused_r,&unused_g,&unused_b,&alpha,da,da,da,da,&st->blendf,vl);
                    }
                    vuint32m2_t A = __riscv_vfcvt_rtz_xu_f_v_u32m2(
                        __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(
                            __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(alpha,0.f,vl),1.f,vl),255.f,vl),.5f,vl),vl);
                    vuint32m2_t col = __riscv_vor_vv_u32m2(__riscv_vand_vx_u32m2(dst,0x00ffffffu,vl),
                        __riscv_vsll_vx_u32m2(A,24,vl),vl);
                    __riscv_vse32_v_u32m2_m(wr,&crow[x],col,vl);
                    x += (int)vl;
                    PARETO_END(); continue;
                }

                vfloat32m2_t cr = __riscv_vle32_v_f32m2(out.c[0], vl);
                vfloat32m2_t cg = __riscv_vle32_v_f32m2(out.c[1], vl);
                vfloat32m2_t cb = __riscv_vle32_v_f32m2(out.c[2], vl);
                vfloat32m2_t ca = __riscv_vle32_v_f32m2(out.c[3], vl);
                /* "shader emitted non-black" is a diag metric only -> don't build
                 * the mask or reduce it unless diagnostics are armed. */
                if (sgl_diag_enabled) {
                    vbool16_t nzout = __riscv_vmor_mm_b16(
                                          __riscv_vmor_mm_b16(__riscv_vmfne_vf_f32m2_b16(cr, 0.f, vl),
                                                              __riscv_vmfne_vf_f32m2_b16(cg, 0.f, vl), vl),
                                          __riscv_vmfne_vf_f32m2_b16(cb, 0.f, vl), vl);
                    sgl_dbg_frag_shader_nonzero += (unsigned long)__riscv_vcpop_m_b16(__riscv_vmand_mm_b16(wr, nzout, vl), vl);
                }

                if (blend != SGL_BLEND_NONE) {
                    vuint32m2_t dst = __riscv_vle32_v_u32m2(&crow[x], vl);
                    vfloat32m2_t dr = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(dst, 0xFFu, vl), vl), inv255, vl);
                    vfloat32m2_t dg = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(dst, 8, vl), 0xFFu, vl), vl), inv255, vl);
                    vfloat32m2_t db = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vand_vx_u32m2(__riscv_vsrl_vx_u32m2(dst, 16, vl), 0xFFu, vl), vl), inv255, vl);
                    vfloat32m2_t da = __riscv_vfmul_vf_f32m2(__riscv_vfcvt_f_xu_v_f32m2(__riscv_vsrl_vx_u32m2(dst, 24, vl), vl), inv255, vl);
                    if (!sgl_blend_fast_m2(&cr, &cg, &cb, &ca, dr, dg, db, da, &st->blendf, vl))
                        sgl_blend_generic_m2(&cr, &cg, &cb, &ca, dr, dg, db, da, &st->blendf, vl);
                }

                /* Quantise RGBA8. Opaque overwrite (BLEND_NONE + full cmask) is
                 * the depth-prepass/ambient hot path: clamp RGB once, force A=255
                 * (Doom materials write opaque alpha on this path; matches prior
                 * clamp of ca in [0,1] then round when ca was already 1). */
                cr = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cr, 0.f, vl), 1.f, vl);
                cg = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cg, 0.f, vl), 1.f, vl);
                cb = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cb, 0.f, vl), 1.f, vl);
                /* round-half-up quantisation + ordered dither (mean 0.5 -> no brightness shift) */
                vfloat32m2_t dth = sgl_store_bias_m2(x, y, vl);
                vuint32m2_t R = __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(cr, 255.f, vl), dth, vl), vl);
                vuint32m2_t G = __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(cg, 255.f, vl), dth, vl), vl);
                vuint32m2_t B = __riscv_vfcvt_rtz_xu_f_v_u32m2(__riscv_vfadd_vv_f32m2(__riscv_vfmul_vf_f32m2(cb, 255.f, vl), dth, vl), vl);
                vuint32m2_t col = R;
                col = __riscv_vor_vv_u32m2(col, __riscv_vsll_vx_u32m2(G, 8, vl), vl);
                col = __riscv_vor_vv_u32m2(col, __riscv_vsll_vx_u32m2(B, 16, vl), vl);
                {   /* C5: real src alpha into bits 31:24 (clamped, round-half-up) */
                    vuint32m2_t A = __riscv_vfcvt_rtz_xu_f_v_u32m2(
                        __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(
                            __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(ca, 0.f, vl), 1.f, vl),
                            255.f, vl), 0.5f, vl), vl);
                    col = __riscv_vor_vv_u32m2(col, __riscv_vsll_vx_u32m2(A, 24, vl), vl);
                }
                /* store-coverage / black-store metrics are diag-only: the mask
                 * build + four vcpop reductions are skipped in production. */
                if (sgl_diag_enabled) {
                    vbool16_t nzstore = __riscv_vmsne_vx_u32m2_b16(__riscv_vand_vx_u32m2(col, 0x00FFFFFFu, vl), 0, vl);
                    sgl_dbg_frag_store += zpass;
                    sgl_dbg_frag_store_nonzero += (unsigned long)__riscv_vcpop_m_b16(__riscv_vmand_mm_b16(wr, nzstore, vl), vl);
                    if (blend == SGL_BLEND_NONE) {                /* opaque-overwrite lanes, and the black subset */
                        sgl_dbg_frag_ow += (unsigned long)__riscv_vcpop_m_b16(wr, vl);
                        sgl_dbg_frag_ow_black += (unsigned long)__riscv_vcpop_m_b16(__riscv_vmandn_mm_b16(wr, nzstore, vl), vl);
                    }
                }

                if (cmask == 0xF) {
                    __riscv_vse32_v_u32m2_m(wr, &crow[x], col, vl);
                } else if (cmask) {                 /* per-channel write enable */
                    uint32_t keep = 0;
                    if (!(cmask & 1)) keep |= 0x000000FFu;
                    if (!(cmask & 2)) keep |= 0x0000FF00u;
                    if (!(cmask & 4)) keep |= 0x00FF0000u;
                    if (!(cmask & 8)) keep |= 0xFF000000u;
                    vuint32m2_t dst = __riscv_vle32_v_u32m2(&crow[x], vl);
                    vuint32m2_t merged = __riscv_vor_vv_u32m2(__riscv_vand_vx_u32m2(col, ~keep, vl),
                                                              __riscv_vand_vx_u32m2(dst, keep, vl), vl);
                    __riscv_vse32_v_u32m2_m(wr, &crow[x], merged, vl);
                }
                /* depth store already handled above (before the cmask gate). */

                PARETO_END();
                x += (int)vl;
            }
            row_e0 += Ay0; row_e1 += Ay1; row_e2 += Ay2;
        }
        /* one atomic flush per triangle instead of one per batch (#2) */
        if (sgl_prof_enabled) {
            __atomic_fetch_add(&sgl_prof_frags, pf_loc_frags, __ATOMIC_RELAXED);
            __atomic_fetch_add(&sgl_prof_frag_lanes, pf_loc_lanes, __ATOMIC_RELAXED);
            __atomic_fetch_add(&sgl_prof_shade, pf_loc_shade, __ATOMIC_RELAXED);
        }
        return;
    }
#else
    (void)frag_simt;
#endif

    float vary[SGL_MAX_VARY], rgba[4];
    for (int y = miny; y <= maxy; ++y) {
        uint32_t *crow = &fb->color[y * fb->w];
        float    *zrow = &fb->depth[y * fb->w];
        uint8_t  *srow = fb->stencil ? &fb->stencil[y * fb->w] : 0;
        float e0 = row_e0, e1 = row_e1, e2 = row_e2;
        for (int x = minx; x <= maxx; ++x) {
            if (e0 >= 0.f && e1 >= 0.f && e2 >= 0.f) {
                float w0 = e0 * invArea, w1 = e1 * invArea, w2 = e2 * invArea;
                float z = w0 * a.z + w1 * b.z + w2 * c.z;
                if (stencil_test && !sgl_stencil_pass(stencil, srow[x])) {
                    srow[x] = sgl_stencil_apply_op(stencil, stencil->sfail, srow[x]);
                } else {
                    int zpass = !st->depth_test || sgl_depth_pass(depth_func, z, zrow[x]);
                    if (stencil_test && !zpass)
                        srow[x] = sgl_stencil_apply_op(stencil, stencil->zfail, srow[x]);
                    if (zpass) {
                        if (stencil_test)
                            srow[x] = sgl_stencil_apply_op(stencil, stencil->zpass, srow[x]);
                        SGL_DIAG_INC(sgl_dbg_frag_zpass);
                        if (cmask == 0) {
                            SGL_DIAG_INC(sgl_dbg_frag_cmask0);
                            if (st->depth_test && depth_write) zrow[x] = z;
                            e0 += Ax0; e1 += Ax1; e2 += Ax2;
                            continue;
                        }
                        float iw = w0 * a.invw + w1 * b.invw + w2 * c.invw;
                        /* Floor before reciprocal: grazing slivers can round iw to
                         * ~0/negative and blow up the varyings (black shards). True
                         * in-triangle iw >= smallest vertex invw, so this is a no-op
                         * for well-formed coverage. Mirrors the SIMT path above. */
                        float s = 1.f / (iw > 1e-7f ? iw : 1e-7f);
                        for (int k = 0; k < nvary; ++k)
                            vary[k] = (w0 * a.vary[k] * a.invw + w1 * b.vary[k] * b.invw + w2 * c.vary[k] * c.invw) * s;
                        SGL_DIAG_INC(sgl_dbg_frag_shaded);
                        if (!st->skip_fragcoord) sgl_scalar_set_fragcoord(x, y);
                        if (frag(ctx, vary, rgba)) {
                            if (rgba[0] != 0.f || rgba[1] != 0.f || rgba[2] != 0.f) SGL_DIAG_INC(sgl_dbg_frag_shader_nonzero);
                            if (st->blend != SGL_BLEND_NONE) {
                                uint32_t d = crow[x];
                                float dd[4] = { (d & 0xFF) / 255.f, ((d >> 8) & 0xFF) / 255.f,
                                                ((d >> 16) & 0xFF) / 255.f, ((d >> 24) & 0xFF) / 255.f };
                                sgl_blend_generic_f(rgba, dd, &st->blendf);
                            }
                            if (cmask == 0xF) {
                                uint32_t pc = pack_rgba(rgba[0], rgba[1], rgba[2], rgba[3]);
                                SGL_DIAG_INC(sgl_dbg_frag_store);
                                if (pc & 0x00FFFFFFu) SGL_DIAG_INC(sgl_dbg_frag_store_nonzero);
                                if (st->blend == SGL_BLEND_NONE) { SGL_DIAG_INC(sgl_dbg_frag_ow); if (!(pc & 0x00FFFFFFu)) SGL_DIAG_INC(sgl_dbg_frag_ow_black); }
                                crow[x] = pc;
                            } else if (cmask) {
                                uint32_t pc = pack_rgba(rgba[0], rgba[1], rgba[2], rgba[3]), d = crow[x];
                                SGL_DIAG_INC(sgl_dbg_frag_store);
                                if (pc & 0x00FFFFFFu) SGL_DIAG_INC(sgl_dbg_frag_store_nonzero);
                                uint32_t keep = 0;
                                if (!(cmask & 1)) keep |= 0x000000FFu;
                                if (!(cmask & 2)) keep |= 0x0000FF00u;
                                if (!(cmask & 4)) keep |= 0x00FF0000u;
                                if (!(cmask & 8)) keep |= 0xFF000000u;
                                crow[x] = (pc & ~keep) | (d & keep);
                            }
                            if (st->depth_test && depth_write) zrow[x] = z;
                        }
                    }
                }
            }
            e0 += Ax0; e1 += Ax1; e2 += Ax2;
        }
        row_e0 += Ay0; row_e1 += Ay1; row_e2 += Ay2;
    }
}

/* -------------------------------------------------------------------------
 * Public sgl.h API (simple, colour-only) layered on the rasteriser core.
 * ---------------------------------------------------------------------- */

void sgl_clear(sgl_framebuffer *fb, uint32_t rgba, float depth)
{
    size_t n = (size_t)fb->w * fb->h;
    fb->clear_color = rgba;
    fb->clear_depth = depth;
    fb->clear_stencil = 0;
    fb->clear_valid = 1;
#if SGL_RVV
    uint32_t *cp = fb->color;
    float    *zp = fb->depth;
    if (cp && zp) {
        for (size_t i = 0; i < n; ) {
            size_t vl = __riscv_vsetvl_e32m8(n - i);
            __riscv_vse32_v_u32m8(&cp[i], __riscv_vmv_v_x_u32m8(rgba, vl), vl);
            __riscv_vse32_v_f32m8(&zp[i], __riscv_vfmv_v_f_f32m8(depth, vl), vl);
            i += vl;
        }
    } else if (cp) {
        for (size_t i = 0; i < n; ) {
            size_t vl = __riscv_vsetvl_e32m8(n - i);
            __riscv_vse32_v_u32m8(&cp[i], __riscv_vmv_v_x_u32m8(rgba, vl), vl);
            i += vl;
        }
    } else if (zp) {
        for (size_t i = 0; i < n; ) {
            size_t vl = __riscv_vsetvl_e32m8(n - i);
            __riscv_vse32_v_f32m8(&zp[i], __riscv_vfmv_v_f_f32m8(depth, vl), vl);
            i += vl;
        }
    }
#else
    for (size_t i = 0; i < n; ++i) {
        if (fb->color) fb->color[i] = rgba;
        if (fb->depth) fb->depth[i] = depth;
    }
#endif
    if (fb->stencil) {
#if SGL_RVV
        for (size_t i = 0; i < n; ) {
            size_t vl = __riscv_vsetvl_e8m8(n - i);
            __riscv_vse8_v_u8m8(&fb->stencil[i], __riscv_vmv_v_x_u8m8(0, vl), vl);
            i += vl;
        }
#else
        memset(fb->stencil, 0, n);
#endif
    }
}

static sgl_scrvert project(const sgl_framebuffer *fb, const sgl_vertex *v)
{
    float invw = 1.f / v->w;
    sgl_scrvert s;
    s.x = (v->x * invw * 0.5f + 0.5f) * (float)fb->w;
    s.y = (1.f - (v->y * invw * 0.5f + 0.5f)) * (float)fb->h;
    s.z = v->z * invw * 0.5f + 0.5f;
    s.invw = invw;
    s.r = v->r * invw; s.g = v->g * invw; s.b = v->b * invw; s.a = v->a * invw;
    s.u = 0.f; s.v = 0.f;
    return s;
}

void sgl_draw_triangles(sgl_framebuffer *fb, const sgl_vertex *verts, int count)
{
    sgl_drawstate st;
    memset(&st, 0, sizeof(st));
    st.depth_test = 1;
    st.blend = SGL_BLEND_NONE;
    st.depth_func = SGL_DEPTH_LESS;
    st.depth_write = 1;
    st.color_mask = 0xF;
    st.stencil_front.func = st.stencil_back.func = SGL_DEPTH_ALWAYS;
    st.stencil_front.read_mask = st.stencil_back.read_mask = 0xFFu;
    st.stencil_front.write_mask = st.stencil_back.write_mask = 0xFFu;
    st.stencil_front.sfail = st.stencil_back.sfail = SGL_STENCIL_KEEP;
    st.stencil_front.zfail = st.stencil_back.zfail = SGL_STENCIL_KEEP;
    st.stencil_front.zpass = st.stencil_back.zpass = SGL_STENCIL_KEEP;
    st.stencil_frontface = 1;
    for (int i = 0; i + 2 < count; i += 3) {
        sgl_raster_tri(fb, project(fb, &verts[i]), project(fb, &verts[i + 1]),
                       project(fb, &verts[i + 2]), &st);
    }
}

int sgl_rvv_enabled(void) { return SGL_RVV; }
