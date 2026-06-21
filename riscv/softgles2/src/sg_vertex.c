#include "sg_vertex.h"
#include "sg_diag.h"
#include "sg_layout.h"
#include "glsl_internal.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SG_VERTEX_RVV 1
#else
#  define SG_VERTEX_RVV 0
#endif

#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#endif

volatile unsigned long sgl_dbg_vs_simt = 0;
volatile unsigned long sgl_dbg_vs_scalar = 0;
volatile unsigned long sgl_dbg_vs_cache_hit = 0;
volatile unsigned long sgl_dbg_vs_cache_miss = 0;

typedef struct {
    float c[GLSL_MAX_ATTRIBS][4][SGL_SIMT_LANES];
} sg_vertex_attr_soa SG_ALIGNED_TYPE(SG_RVV_ALIGN);

static void sg_vertex_fetch_4(void *fetch_ctx, sg_vertex_fetch_fn fetch, int loc, int vi, float out[4])
{
    static const float def[4] = {0, 0, 0, 1};
    fetch(fetch_ctx, loc, vi, out, 4, def);
}

/* Runtime-tunable register-group split policy for the VS SIMT path.
 * Defaults are exported for benchmark-driven tuning and kept stable so existing
 * behavior is unchanged unless explicitly overridden via
 * sg_vertex_simt_lmul_policy_set().
 */
static sg_vertex_simt_lmul_policy g_vs_simt_policy = {
    16, 32, SGL_SIMT_LANES / 4,
    16, 32, SGL_SIMT_LANES / 2,
    SGL_SIMT_LANES
};

sg_vertex_simt_lmul_policy sg_vertex_simt_lmul_policy_default(void)
{
    sg_vertex_simt_lmul_policy p = {
        16, 32, SGL_SIMT_LANES / 4,
        16, 32, SGL_SIMT_LANES / 2,
        SGL_SIMT_LANES
    };
    return p;
}

sg_vertex_simt_lmul_policy sg_vertex_simt_lmul_policy_get(void)
{
    return g_vs_simt_policy;
}

void sg_vertex_simt_lmul_policy_set(const sg_vertex_simt_lmul_policy *policy)
{
    if (!policy) {
        g_vs_simt_policy = sg_vertex_simt_lmul_policy_default();
        return;
    }
    g_vs_simt_policy = *policy;
    if (g_vs_simt_policy.quarter_lanes < 1) g_vs_simt_policy.quarter_lanes = 1;
    if (g_vs_simt_policy.half_lanes < 1) g_vs_simt_policy.half_lanes = 1;
    if (g_vs_simt_policy.full_lanes < 1) g_vs_simt_policy.full_lanes = 1;
    if (g_vs_simt_policy.quarter_lanes > g_vs_simt_policy.half_lanes)
        g_vs_simt_policy.quarter_lanes = g_vs_simt_policy.half_lanes;
    if (g_vs_simt_policy.half_lanes > g_vs_simt_policy.full_lanes)
        g_vs_simt_policy.half_lanes = g_vs_simt_policy.full_lanes;
}

int sg_vertex_simt_pressure_score(const glsl_program *gp, int attr_slots, int used_vary)
{
    if (!gp || !glsl_vs_simt_ok(gp)) return 0;
    const ir_prog *ir = &gp->vs_ir;
    int score = attr_slots * 3 + used_vary * 2 + ir->nlocal + ir->nreg + ir->ninst / 2;

    for (int i = 0; i < ir->ninst; ++i) {
        const ir_inst *o = &ir->inst[i];
        if (o->op == IR_LOADU && o->i1 > 4) score += 8;
        if (o->op == IR_BIN && o->i0 == '*') score += 5;
        if (o->op == IR_BUILTIN) {
            switch (o->i0) {
            case BI_NORMALIZE:
            case BI_DISTANCE:
            case BI_LENGTH:
            case BI_REFLECT:
            case BI_SMOOTHSTEP:
                score += 6;
                break;
            case BI_TEXTURE2D:
            case BI_TEXTURE2DPROJ:
            case BI_TEXTURECUBE:
                score += 12;
                break;
            default:
                score += 2;
                break;
            }
        }
    }

    return score;
}

/* Explicit LMUL split policy used for VS SIMT hot-path:
 *   - high register pressure -> quarter width
 *   - medium register pressure -> half width
 *   - default -> full width.
 */
int sg_vertex_simt_chunk_lanes_for(const glsl_program *gp, int attr_slots, int used_vary)
{
    if (!gp || !glsl_vs_simt_ok(gp)) return 1;

    int pressure = sg_vertex_simt_pressure_score(gp, attr_slots, used_vary);
    if (pressure >= 256 ||
        attr_slots > g_vs_simt_policy.quarter_attr_slots ||
        used_vary > g_vs_simt_policy.quarter_vary_slots)
        return g_vs_simt_policy.quarter_lanes;
    if (pressure >= 192 ||
        attr_slots > g_vs_simt_policy.half_attr_slots ||
        used_vary > g_vs_simt_policy.half_vary_slots)
        return g_vs_simt_policy.half_lanes;
    return g_vs_simt_policy.full_lanes;
}

static sgl_pvert sg_vertex_prog_once(int vi, glsl_program *gp, float *varybuf,
                                     int mvp_set, const float *mvp, int vp_x, int vp_y, int vp_w, int vp_h,
                                     float depth_near, float depth_far,
                                     sg_vertex_fetch_fn fetch, void *fetch_ctx)
{
    static const float def[4] = {0, 0, 0, 1};
    float attribs[GLSL_MAX_ATTRIBS][4];
    float clip[4];

    for (int i = 0; i < GLSL_MAX_ATTRIBS && i < SGL_MAX_VERTEX_ATTRIBS; ++i) {
        float a[4];
        sg_vertex_fetch_4(fetch_ctx, fetch, i, vi, a);
        for (int c = 0; c < 4; ++c) attribs[i][c] = a[c];
    }

    (void)def;
    (void)mvp_set;
    (void)mvp;

    glsl_run_vertex(gp, attribs, clip, varybuf);

    float invw = (clip[3] != 0.f) ? 1.f / clip[3] : 1.f;
    float ndcx = clip[0] * invw;
    float ndcy = clip[1] * invw;
    float ndcz = clip[2] * invw;

    sgl_pvert pv;
    pv.x = (float)vp_x + (ndcx * 0.5f + 0.5f) * (float)vp_w;
    pv.y = (float)vp_y + (1.f - (ndcy * 0.5f + 0.5f)) * (float)vp_h;
    pv.z = depth_near + (ndcz * 0.5f + 0.5f) * (depth_far - depth_near);   /* glDepthRangef (C4) */
    pv.invw = invw;
    pv.vary = varybuf;
    return pv;
}

static void sg_vertex_postprocess_batch(sgl_pvert *pv, GLsizei base, int vl,
                                        const float *clip,
                                        int vp_x, int vp_y, int vp_w, int vp_h,
                                        float depth_near, float depth_far)
{
    /* glDepthRangef: window-Z = near + z01*(far-near). Threaded as params (keeps
     * sg_vertex.c decoupled from the GL context). Default [0,1] => identity. */
    const float zn = depth_near, zs = depth_far - depth_near;
#if SG_VERTEX_RVV
    for (int i = 0; i < vl; ) {
        size_t l = __riscv_vsetvl_e32m2((size_t)(vl - i));
        vfloat32m2_t w = __riscv_vle32_v_f32m2(&clip[3 * SGL_SIMT_LANES + i], l);
        vfloat32m2_t one = __riscv_vfmv_v_f_f32m2(1.f, l);
        vfloat32m2_t invw = __riscv_vfdiv_vv_f32m2(one, w, l);
        invw = __riscv_vmerge_vvm_f32m2(one, invw, __riscv_vmfne_vf_f32m2_b16(w, 0.f, l), l);

        vfloat32m2_t ndcx = __riscv_vfmul_vv_f32m2(
            __riscv_vle32_v_f32m2(&clip[0 * SGL_SIMT_LANES + i], l), invw, l);
        vfloat32m2_t ndcy = __riscv_vfmul_vv_f32m2(
            __riscv_vle32_v_f32m2(&clip[1 * SGL_SIMT_LANES + i], l), invw, l);
        vfloat32m2_t ndcz = __riscv_vfmul_vv_f32m2(
            __riscv_vle32_v_f32m2(&clip[2 * SGL_SIMT_LANES + i], l), invw, l);

        vfloat32m2_t x = __riscv_vfmul_vf_f32m2(
            __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(ndcx, 0.5f, l), 0.5f, l),
            (float)vp_w, l);
        x = __riscv_vfadd_vf_f32m2(x, (float)vp_x, l);

        vfloat32m2_t y = __riscv_vfmul_vf_f32m2(
            __riscv_vfrsub_vf_f32m2(__riscv_vfmul_vf_f32m2(ndcy, 0.5f, l), 0.5f, l),
            (float)vp_h, l);
        y = __riscv_vfadd_vf_f32m2(y, (float)vp_y, l);

        vfloat32m2_t z = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(ndcz, 0.5f, l), 0.5f, l);
        z = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(z, zs, l), zn, l);   /* depth range */

        ptrdiff_t stride = (ptrdiff_t)sizeof(sgl_pvert);
        sgl_pvert *dst = pv + base + i;
        __riscv_vsse32_v_f32m2(&dst->x, stride, x, l);
        __riscv_vsse32_v_f32m2(&dst->y, stride, y, l);
        __riscv_vsse32_v_f32m2(&dst->z, stride, z, l);
        __riscv_vsse32_v_f32m2(&dst->invw, stride, invw, l);
        i += (int)l;
    }
#else
    for (int lane = 0; lane < vl; ++lane) {
        float w = clip[3 * SGL_SIMT_LANES + lane];
        float invw = (w != 0.f) ? 1.f / w : 1.f;
        float ndcx = clip[0 * SGL_SIMT_LANES + lane] * invw;
        float ndcy = clip[1 * SGL_SIMT_LANES + lane] * invw;
        float ndcz = clip[2 * SGL_SIMT_LANES + lane] * invw;
        pv[base + lane].x = (float)vp_x + (ndcx * 0.5f + 0.5f) * (float)vp_w;
        pv[base + lane].y = (float)vp_y + (1.f - (ndcy * 0.5f + 0.5f)) * (float)vp_h;
        pv[base + lane].z = zn + (ndcz * 0.5f + 0.5f) * zs;
        pv[base + lane].invw = invw;
    }
#endif
}

static int sg_vertex_prog_simt_seq(GLsizei count, const int *idx, glsl_program *gp,
                                  int vstride, sgl_pvert *pv, float *vb,
                                  int mvp_set, const float *mvp,
                                  int vp_x, int vp_y, int vp_w, int vp_h,
                                  float depth_near, float depth_far,
                                  sg_vertex_fetch_fn fetch, void *fetch_ctx)
{
    (void)mvp_set;
    (void)mvp;
    (void)vp_x;
    (void)vp_y;
    (void)vp_w;
    (void)vp_h;
    if (count < 1 || !idx || !pv || !vb || !gp || !fetch)
        return 0;
    if (!glsl_vs_simt_ok(gp))
        return 0;

    /* Build only shader-active attributes to keep register footprint proportional
     * to actual shader use, while preserving full glsl_get_attrib address slots.
     */
    int used_vary = glsl_num_varying_floats(gp);
    if (used_vary < 0) used_vary = 0;
    if (used_vary > vstride || used_vary > SGL_MAX_VARY || vstride <= 0)
        return 0;
    int used_vary_rounded = used_vary > 0 ? used_vary : 1;

    int nattr = glsl_num_attribs(gp);
    if (nattr < 0) nattr = 0;
    if (nattr > GLSL_MAX_ATTRIBS) nattr = GLSL_MAX_ATTRIBS;
    int used_locs[GLSL_MAX_ATTRIBS];
    int loc_seen[GLSL_MAX_ATTRIBS] = {0};
    int nused_locs = 0;
    int max_loc = -1;
    for (int i = 0; i < nattr; ++i) {
        int loc = -1;
        if (!glsl_get_attrib(gp, i, 0, 0, 0, 0, &loc)) continue;
        if (loc < 0 || loc >= GLSL_MAX_ATTRIBS) continue;
        if (loc_seen[loc]) continue;
        loc_seen[loc] = 1;
        used_locs[nused_locs++] = loc;
        if (loc > max_loc) max_loc = loc;
    }

    int compact_attr = (nused_locs > 0 && (max_loc + 1) != nused_locs);
    int attr_slots = compact_attr ? nused_locs : (max_loc >= 0 ? (max_loc + 1) : 1);
    if (attr_slots > GLSL_MAX_ATTRIBS) attr_slots = GLSL_MAX_ATTRIBS;
    if (attr_slots < 1) attr_slots = 1;
    int simt_lanes = sg_vertex_simt_chunk_lanes_for(gp, attr_slots, used_vary);
    if (simt_lanes < 1) simt_lanes = SGL_SIMT_LANES;
    if (simt_lanes > SGL_SIMT_LANES) simt_lanes = SGL_SIMT_LANES;
    if (count < (GLsizei)(simt_lanes * 2))
        return 0;

    static const float def[4] = {0, 0, 0, 1};
    sg_vertex_attr_soa attrs_soa;
    float *attrs = &attrs_soa.c[0][0][0];
    (void)attrs;
    int loc_to_attr[GLSL_MAX_ATTRIBS];
    if (compact_attr) {
        for (int i = 0; i < GLSL_MAX_ATTRIBS; ++i) loc_to_attr[i] = -1;
    }
    sg_vec4_soa clip_soa;
    float *clip = &clip_soa.c[0][0];

    for (GLsizei base = 0; base < count; base += simt_lanes) {
        int vl = (count - base > simt_lanes) ? simt_lanes : (int)(count - base);
        for (int u = 0; u < nused_locs; ++u) {
            int loc = used_locs[u];
            if (loc >= GLSL_MAX_ATTRIBS) continue;
            int row = compact_attr ? u : loc;
            if (row < 0 || row >= attr_slots) continue;
            if (compact_attr) loc_to_attr[loc] = row;
            for (int lane = 0; lane < vl; ++lane) {
                float a[4];
                int vi = idx[base + lane];
                if (loc < SGL_MAX_VERTEX_ATTRIBS) sg_vertex_fetch_4(fetch_ctx, fetch, loc, vi, a);
                else for (int c = 0; c < 4; ++c) a[c] = def[c];
                for (int c = 0; c < 4; ++c)
                    attrs_soa.c[row][c][lane] = a[c];
            }
        }

        if (used_vary > 0) {
            sg_vary_soa vary_soa;
            float *vary_src = &vary_soa.c[0][0];
            (void)used_vary_rounded;
            if (compact_attr) glsl_run_vertex_simt_mapped(gp, attrs, loc_to_attr, vl, clip, vary_src);
            else glsl_run_vertex_simt(gp, attrs, vl, clip, vary_src);
            if (sgl_diag_enabled) sgl_dbg_vs_simt += (unsigned long)vl;
            int vary_copy = used_vary < vstride ? used_vary : vstride;
            sg_vertex_postprocess_batch(pv, base, vl, clip, vp_x, vp_y, vp_w, vp_h, depth_near, depth_far);
            for (int lane = 0; lane < vl; ++lane)
                pv[base + lane].vary = vb + (size_t)(base + lane) * vstride;
#if SG_VERTEX_RVV
            /* Item 7 (RVV audit): vectorise the SoA->AoS varying transpose. Per
             * component k, unit-stride load the SoA row (64 lanes) and scatter it
             * to the AoS column (byte stride vstride*4). Replaces vstride*vl scalar
             * flw+fsw with ~vstride*(vle32+vsse32). Bit-identical (same float bits). */
            {
                ptrdiff_t vstr = (ptrdiff_t)vstride * (ptrdiff_t)sizeof(float);
                float *vbase = vb + (size_t)base * vstride;
                for (int k = 0; k < vary_copy; ++k)
                    for (int i = 0; i < vl; ) { size_t l = __riscv_vsetvl_e32m2((size_t)(vl - i));
                        __riscv_vsse32_v_f32m2(vbase + (size_t)i * vstride + k, vstr,
                                               __riscv_vle32_v_f32m2(&vary_soa.c[k][i], l), l); i += (int)l; }
                if (vstride > vary_copy) {
                    vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, __riscv_vsetvlmax_e32m2());
                    for (int k = vary_copy; k < vstride; ++k)
                        for (int i = 0; i < vl; ) { size_t l = __riscv_vsetvl_e32m2((size_t)(vl - i));
                            __riscv_vsse32_v_f32m2(vbase + (size_t)i * vstride + k, vstr, z, l); i += (int)l; }
                }
            }
#else
            for (int lane = 0; lane < vl; ++lane) {
                float *vary = vb + (size_t)(base + lane) * vstride;
                for (int k = 0; k < vary_copy; ++k) vary[k] = vary_soa.c[k][lane];
                for (int k = vary_copy; k < vstride; ++k) vary[k] = 0.f;
            }
#endif
        } else {
            if (compact_attr) glsl_run_vertex_simt_mapped(gp, attrs, loc_to_attr, vl, clip, 0);
            else glsl_run_vertex_simt(gp, attrs, vl, clip, 0);
            if (sgl_diag_enabled) sgl_dbg_vs_simt += (unsigned long)vl;
            sg_vertex_postprocess_batch(pv, base, vl, clip, vp_x, vp_y, vp_w, vp_h, depth_near, depth_far);
            for (int lane = 0; lane < vl; ++lane) {
                float *vary = (float *)vb + (size_t)(base + lane) * vstride;
                pv[base + lane].vary = vary;
                for (int k = 0; k < vstride; ++k) vary[k] = 0.f;
            }
        }
    }
    return 1;
}

void sg_vertex_prog_cached(GLsizei count, const int *idx, glsl_program *gp,
                          int vstride, sgl_pvert *pv, float *vb,
                          int mvp_set, const float *mvp,
                          int vp_x, int vp_y, int vp_w, int vp_h,
                          float depth_near, float depth_far,
                          sg_vertex_fetch_fn fetch, void *fetch_ctx)
{
    if (!pv || !vb || !idx || !fetch) return;
    if (!count) return;

    (void)mvp_set;
    (void)mvp;

    int sequential = 1;
#if SG_VERTEX_RVV
    /* Item 8 (RVV audit): vectorise the per-draw sequential-index precheck.
     * Strip-mine idx[] against the identity ramp (vid + base); the first strip
     * with any mismatch breaks. Bit-identical result to the scalar idx[i]!=i scan
     * (u32 bit-compare flags exactly the same mismatches). */
    for (GLsizei i = 0; i < count; ) {
        size_t l = __riscv_vsetvl_e32m2((size_t)(count - i));
        vuint32m2_t v   = __riscv_vle32_v_u32m2((const uint32_t *)&idx[i], l);
        vuint32m2_t rmp = __riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(l), (uint32_t)i, l);
        if (__riscv_vcpop_m_b16(__riscv_vmsne_vv_u32m2_b16(v, rmp, l), l) != 0) { sequential = 0; break; }
        i += (GLsizei)l;
    }
#else
    for (GLsizei i = 0; i < count; ++i) {
        if (idx[i] != (int)i) { sequential = 0; break; }
    }
#endif

    if (sequential) {
        if (sg_vertex_prog_simt_seq(count, idx, gp, vstride, pv, vb, mvp_set, mvp,
                                   vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx))
            return;
        for (GLsizei i = 0; i < count; ++i) {
            pv[i] = sg_vertex_prog_once(idx[i], gp, vb + (size_t)i * vstride, mvp_set, mvp,
                                        vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx);
        }
        if (sgl_diag_enabled) sgl_dbg_vs_scalar += (unsigned long)count;
        return;
    }

    if (sg_vertex_prog_simt_seq(count, idx, gp, vstride, pv, vb, mvp_set, mvp,
                               vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx))
        return;

    int probe = count < 64 ? count : 64;
    int likely_reuse = 0;
    for (int i = 0; i < probe && !likely_reuse; ++i) {
        for (int j = i + 1; j < probe; ++j) {
            if (idx[i] == idx[j]) { likely_reuse = 1; break; }
        }
    }
    if (!likely_reuse && count < 1024) {
        for (GLsizei i = 0; i < count; ++i) {
            pv[i] = sg_vertex_prog_once(idx[i], gp, vb + (size_t)i * vstride, mvp_set, mvp,
                                        vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx);
        }
        if (sgl_diag_enabled) sgl_dbg_vs_scalar += (unsigned long)count;
        return;
    }

    size_t cap = 1;
    while (cap < (size_t)count * 2u) cap <<= 1;
    int *keys = (int *)malloc(sizeof(int) * cap);
    uint8_t *used = (uint8_t *)malloc(cap);
    sgl_pvert *cache_pv = (sgl_pvert *)malloc(sizeof(sgl_pvert) * cap);
    float *cache_vary = (float *)malloc(sizeof(float) * cap * (size_t)vstride);
    if (!keys || !used || !cache_pv || !cache_vary) {
        free(keys);
        free(used);
        free(cache_pv);
        free(cache_vary);
        for (GLsizei i = 0; i < count; ++i) {
            pv[i] = sg_vertex_prog_once(idx[i], gp, vb + (size_t)i * vstride, mvp_set, mvp,
                                        vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx);
        }
        if (sgl_diag_enabled) sgl_dbg_vs_scalar += (unsigned long)count;
        return;
    }

    memset(used, 0, cap);

    unsigned long cache_hit = 0, cache_miss = 0;
    for (GLsizei i = 0; i < count; ++i) {
        int key = idx[i];
        size_t h = ((unsigned)key * 2654435761u) & (cap - 1u);
        while (used[h] && keys[h] != key) h = (h + 1u) & (cap - 1u);

        float *dst_vary = vb + (size_t)i * vstride;
        if (used[h]) {
            memcpy(dst_vary, cache_vary + h * (size_t)vstride, sizeof(float) * (size_t)vstride);
            pv[i] = cache_pv[h];
            pv[i].vary = dst_vary;
            cache_hit++;
        } else {
            used[h] = 1;
            keys[h] = key;
            pv[i] = sg_vertex_prog_once(key, gp, dst_vary, mvp_set, mvp,
                                        vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx);
            cache_miss++;
            cache_pv[h] = pv[i];
            memcpy(cache_vary + h * (size_t)vstride, dst_vary, sizeof(float) * (size_t)vstride);
            cache_pv[h].vary = cache_vary + h * (size_t)vstride;
        }
    }

    if (sgl_diag_enabled) {
        sgl_dbg_vs_cache_hit += cache_hit;
        sgl_dbg_vs_cache_miss += cache_miss;
    }
    free(keys);
    free(used);
    free(cache_pv);
    free(cache_vary);
}
