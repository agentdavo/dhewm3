#include "sg_vertex.h"
#include "sg_diag.h"
#include "sg_prof.h"
#include "sg_layout.h"
#include "sg_mt_sched.h"
#include "glsl_internal.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Post-transform vertex dedupe (see sg_vertex_prog_simt_dedup).
 * MIN_VERTS: below this the hash build outweighs the transforms saved.
 * MAX_PCT:   give up when uniques exceed this percentage of corners, i.e. when
 *            there is too little reuse for the expansion copies to pay off.
 *            A closed indexed mesh sits near 33%; 90% only rejects the
 *            pathological all-distinct case. */
#ifndef SGL_VS_DEDUP_MIN_VERTS
#define SGL_VS_DEDUP_MIN_VERTS 64
#endif

/* Runtime-settable so a test can render the SAME scene with the optimisation on
 * and off IN ONE PROCESS and compare exactly -- the only way to check a claim of
 * "bit-exact by construction", which is an argument, not a measurement. Read
 * once per DRAW, never per vertex, so this costs nothing measurable. Raise it
 * above any real draw's vertex count to disable the path. */
int sgl_vs_dedup_min_verts = SGL_VS_DEDUP_MIN_VERTS;

/* Defined below; declared here so the dedupe path can route its (already
 * shrunk) unique-vertex list through the hart pool too. */
static int sg_vertex_prog_simt_mt(GLsizei count, const int *idx, glsl_program *gp,
                                  int vstride, sgl_pvert *pv, float *vb,
                                  int mvp_set, const float *mvp,
                                  int vp_x, int vp_y, int vp_w, int vp_h,
                                  float depth_near, float depth_far,
                                  sg_vertex_fetch_fn fetch, void *fetch_ctx);

/* Parallel when the draw is big enough to amortise a dispatch, serial
 * otherwise. One place so every caller gets the same policy. */
#define SG_VS_SIMT(count, idx, gp, vstride, pv, vb, ms, mvp, x, y, w, h, dn, df, f, fc) \
    (sg_vertex_prog_simt_mt((count), (idx), (gp), (vstride), (pv), (vb), (ms), (mvp), \
                            (x), (y), (w), (h), (dn), (df), (f), (fc)) ||             \
     sg_vertex_prog_simt_seq((count), (idx), (gp), (vstride), (pv), (vb), (ms), (mvp), \
                             (x), (y), (w), (h), (dn), (df), (f), (fc)))
#ifndef SGL_VS_DEDUP_MAX_PCT
#define SGL_VS_DEDUP_MAX_PCT   90u
#endif

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
volatile unsigned long sgl_prof_vs_simt_draws = 0, sgl_prof_vs_simt_verts = 0;

/* Draw-size distribution for the vertex stage.
 *
 * The vertex transform is the last SERIAL stage in the frame: FrameBin moved
 * raster onto all harts, but pv[] is still built on the submitting hart while
 * the other five idle (~800ms serial against ~2489ms parallel raster).
 *
 * How to fix it depends entirely on the SHAPE of the workload, which the mean
 * cannot tell you: 91761 verts over 994 draws averages 92, comfortably under
 * any per-draw MT threshold -- but if a handful of large draws carry most of
 * the vertices, dispatching just those to the hart pool is a few lines, whereas
 * a uniform distribution forces the full deferred-vertex refactor (copy
 * attributes at record time, transform in parallel at flush; it cannot simply
 * defer the FETCH because Doom 3 uses client-side arrays -- vbo:0 every frame --
 * and refills them between draws).
 *
 * So: count draws and verts per size bucket, and report what fraction of all
 * vertices live in draws big enough to be worth dispatching. Buckets are
 * powers of two from <64 up to >=4096. */
#define SGL_VS_HIST_BUCKETS 8
volatile unsigned long sgl_prof_vs_hist_draws[SGL_VS_HIST_BUCKETS];
volatile unsigned long sgl_prof_vs_hist_verts[SGL_VS_HIST_BUCKETS];

static void sg_vs_hist_add(GLsizei count)
{
    unsigned b = 0;
    unsigned long c = (unsigned long)(count > 0 ? count : 0);
    unsigned long n = c >> 6;            /* bucket 0 = <64 */
    while (n && b < SGL_VS_HIST_BUCKETS - 1) { n >>= 1; ++b; }
    sgl_prof_vs_hist_draws[b]++;
    sgl_prof_vs_hist_verts[b] += c;
}
volatile unsigned long sgl_prof_vs_scalar_draws = 0, sgl_prof_vs_scalar_verts = 0;

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
    /* Register-resident kernels have no generic IR register-file setup and win
     * even on a single triangle. Keep the measured two-chunk crossover only for
     * the generic vertex VM. */
    if (!gp->vs_kernel && count < (GLsizei)(simt_lanes * 2))
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
        /* Attribute gather, timed separately from the transform below. It is
         * scalar by construction: one INDIRECT call per lane per attribute, each
         * doing a bounds check and a memcpy per float, so no compiler can
         * vectorise through it. Vertex data here is client-side AoS idDrawVert at
         * 60-byte stride (the engine reports vbo:0 every frame), which is exactly
         * the shape a strided load handles in one instruction per component.
         * Measure before rewriting -- gather and transform were previously summed
         * into one bucket, so neither could be attributed. */
        uint64_t pf_g0 = sgl_prof_now();
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
        if (sgl_prof_enabled) sgl_prof_vs_gather += sgl_prof_now() - pf_g0;

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

/*
 * Transform each DISTINCT index once through the SIMT path, then expand the
 * results back out to one entry per triangle corner.
 *
 * Returns 1 if it handled the draw, 0 to fall through unchanged (too small, too
 * little reuse to pay for the hash, allocation failure, or the SIMT path itself
 * declining the program).
 *
 * The expansion writes each corner's varyings into the caller's vb exactly where
 * the non-deduped path would have, and fixes up pv[i].vary to point at it, so
 * nothing downstream can tell the difference.
 */
static int sg_vertex_prog_simt_dedup(GLsizei count, const int *idx, glsl_program *gp,
                                     int vstride, sgl_pvert *pv, float *vb,
                                     int mvp_set, const float *mvp,
                                     int vp_x, int vp_y, int vp_w, int vp_h,
                                     float depth_near, float depth_far,
                                     sg_vertex_fetch_fn fetch, void *fetch_ctx)
{
    /* Below this the hash build costs more than the transforms it saves. */
    if (count < sgl_vs_dedup_min_verts || vstride < 0)
        return 0;
    if (!glsl_vs_simt_ok(gp))
        return 0;

    size_t cap = 1;
    while (cap < (size_t)count * 2u) cap <<= 1;

    int *slot = (int *)malloc(sizeof(int) * cap);          /* hash -> unique idx */
    int *uidx = (int *)malloc(sizeof(int) * (size_t)count); /* unique index list */
    int *map  = (int *)malloc(sizeof(int) * (size_t)count); /* corner -> unique   */
    if (!slot || !uidx || !map) {
        free(slot); free(uidx); free(map);
        return 0;
    }
    for (size_t i = 0; i < cap; ++i) slot[i] = -1;

    GLsizei nuniq = 0;
    for (GLsizei i = 0; i < count; ++i) {
        int key = idx[i];
        size_t h = ((unsigned)key * 2654435761u) & (cap - 1u);
        while (slot[h] >= 0 && uidx[slot[h]] != key) h = (h + 1u) & (cap - 1u);
        if (slot[h] < 0) {
            slot[h] = nuniq;
            uidx[nuniq] = key;
            ++nuniq;
        }
        map[i] = slot[h];
    }
    free(slot);

    /* Not enough reuse to be worth the copies. A closed mesh sits near 3x; this
     * bails out on the degenerate "every corner distinct" case, where deduping
     * would be pure overhead. */
    if ((size_t)nuniq * 100u > (size_t)count * SGL_VS_DEDUP_MAX_PCT) {
        free(uidx); free(map);
        return 0;
    }

    sgl_pvert *upv = (sgl_pvert *)malloc(sizeof(sgl_pvert) * (size_t)nuniq);
    float *uvb = (float *)malloc(sizeof(float) * (size_t)nuniq *
                                 (size_t)(vstride > 0 ? vstride : 1));
    if (!upv || !uvb) {
        free(upv); free(uvb); free(uidx); free(map);
        return 0;
    }

    if (!SG_VS_SIMT(nuniq, uidx, gp, vstride, upv, uvb, mvp_set, mvp,
                    vp_x, vp_y, vp_w, vp_h, depth_near, depth_far,
                    fetch, fetch_ctx)) {
        free(upv); free(uvb); free(uidx); free(map);
        return 0;
    }

    for (GLsizei i = 0; i < count; ++i) {
        int u = map[i];
        float *dst = vb + (size_t)i * (size_t)vstride;
        pv[i] = upv[u];
        if (vstride > 0)
            memcpy(dst, uvb + (size_t)u * (size_t)vstride,
                   sizeof(float) * (size_t)vstride);
        pv[i].vary = dst;
    }

    if (sgl_diag_enabled) {
        sgl_dbg_vs_cache_hit  += (unsigned long)(count - nuniq);
        sgl_dbg_vs_cache_miss += (unsigned long)nuniq;
    }

    free(upv); free(uvb); free(uidx); free(map);
    return 1;
}

/*
 * Spread ONE large draw's vertex transform across the hart pool.
 *
 * The vertex stage is the last serial part of a frame: FrameBin put raster on
 * every hart, but pv[] is still built on the submitting hart (~800ms serial
 * against ~2489ms parallel raster), so by Amdahl it now caps the frame.
 *
 * Whether this cheap fix is enough is a question about the WORKLOAD SHAPE, and
 * the mean actively misleads: 27012 verts over ~105 draws averages 257, which
 * looks far too small to dispatch. The [rprof-vshist] distribution shows the
 * truth -- FOUR draws of >=4096 verts carry 93% of all vertices. So a per-draw
 * threshold reaches almost all of the work, and the deferred-transform refactor
 * (copy attributes at record time, transform at flush) is not needed.
 *
 * Bit-exactness: each vertex's result depends only on its own attributes and the
 * uniforms, never on which batch or hart it lands in, so splitting the range is
 * exact rather than merely close. Ranges are disjoint in both pv[] and vb[], so
 * there is no sharing to synchronise.
 */
#ifndef SGL_VS_MT_MIN_VERTS
#define SGL_VS_MT_MIN_VERTS 2048
#endif

/* Runtime-settable, same reasoning as sgl_vs_dedup_min_verts above. */
int sgl_vs_mt_min_verts = SGL_VS_MT_MIN_VERTS;

typedef struct {
    GLsizei count; const int *idx; glsl_program *gp; int vstride;
    sgl_pvert *pv; float *vb; int mvp_set; const float *mvp;
    int vp_x, vp_y, vp_w, vp_h; float depth_near, depth_far;
    sg_vertex_fetch_fn fetch; void *fetch_ctx;
    volatile int failed;
} sg_vs_mt_ctx;

/* sg_vs_mt_range() lives in src/sg_vs_range.c: dependency-free integer
 * arithmetic so a HOST test can exercise the real function (not a copy of it).
 * See test/vsmt_ranges.c. */
int sg_vs_mt_range(int count, int worker, int nworkers, int *lo, int *hi);

static void sg_vs_mt_work(void *vctx, int worker, int nworkers)
{
    sg_vs_mt_ctx *c = (sg_vs_mt_ctx *)vctx;
    int lo, hi;

    if (!sg_vs_mt_range((int)c->count, worker, nworkers, &lo, &hi))
        return;

    if (!sg_vertex_prog_simt_seq(hi - lo, c->idx + lo, c->gp, c->vstride,
                                 c->pv + lo, c->vb + (size_t)lo * (size_t)c->vstride,
                                 c->mvp_set, c->mvp, c->vp_x, c->vp_y, c->vp_w, c->vp_h,
                                 c->depth_near, c->depth_far, c->fetch, c->fetch_ctx))
        __atomic_store_n(&c->failed, 1, __ATOMIC_RELAXED);
}

static int sg_vertex_prog_simt_mt(GLsizei count, const int *idx, glsl_program *gp,
                                  int vstride, sgl_pvert *pv, float *vb,
                                  int mvp_set, const float *mvp,
                                  int vp_x, int vp_y, int vp_w, int vp_h,
                                  float depth_near, float depth_far,
                                  sg_vertex_fetch_fn fetch, void *fetch_ctx)
{
#if defined(SGL_BAREMETAL)
    sg_vs_mt_ctx c;

    if (count < sgl_vs_mt_min_verts)
        return 0;
    /* Re-entering sg_mt_sched_run() from inside a parallel section deadlocks on
     * its single gen/done handshake. */
    if (sg_mt_sched_busy() || sg_mt_sched_nharts() < 2)
        return 0;
    /* Every worker must agree to take the SIMT path, or some ranges would be
     * transformed and others left untouched. Ask once, up front. */
    if (!glsl_vs_simt_ok(gp))
        return 0;

    c.count = count; c.idx = idx; c.gp = gp; c.vstride = vstride;
    c.pv = pv; c.vb = vb; c.mvp_set = mvp_set; c.mvp = mvp;
    c.vp_x = vp_x; c.vp_y = vp_y; c.vp_w = vp_w; c.vp_h = vp_h;
    c.depth_near = depth_near; c.depth_far = depth_far;
    c.fetch = fetch; c.fetch_ctx = fetch_ctx;
    c.failed = 0;

    sg_mt_sched_run(sg_vs_mt_work, &c);
    return !c.failed;
#else
    (void)count; (void)idx; (void)gp; (void)vstride; (void)pv; (void)vb;
    (void)mvp_set; (void)mvp; (void)vp_x; (void)vp_y; (void)vp_w; (void)vp_h;
    (void)depth_near; (void)depth_far; (void)fetch; (void)fetch_ctx;
    return 0;
#endif
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
    if (sgl_prof_enabled) sg_vs_hist_add(count);

    (void)mvp_set;
    (void)mvp;

    uint64_t phase0 = sgl_prof_now();
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
    if (sgl_prof_enabled) sgl_prof_vs_index += sgl_prof_now() - phase0;

    if (sequential) {
        phase0 = sgl_prof_now();
        if (SG_VS_SIMT(count, idx, gp, vstride, pv, vb, mvp_set, mvp,
                       vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx)) {
            if (sgl_prof_enabled) sgl_prof_vs_simt += sgl_prof_now() - phase0;
            if (sgl_prof_enabled) { ++sgl_prof_vs_simt_draws; sgl_prof_vs_simt_verts += (unsigned long)count; }
            return;
        }
        for (GLsizei i = 0; i < count; ++i) {
            pv[i] = sg_vertex_prog_once(idx[i], gp, vb + (size_t)i * vstride, mvp_set, mvp,
                                        vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx);
        }
        if (sgl_prof_enabled) sgl_prof_vs_scalar += sgl_prof_now() - phase0;
        if (sgl_diag_enabled) sgl_dbg_vs_scalar += (unsigned long)count;
        if (sgl_prof_enabled) { ++sgl_prof_vs_scalar_draws; sgl_prof_vs_scalar_verts += (unsigned long)count; }
        return;
    }

    /* Indexed draws arrive as one entry per TRIANGLE CORNER, so a closed mesh
     * presents each vertex once per triangle that touches it -- measured here at
     * exactly 3.0x (91761 corners for 30599 triangles), i.e. no reuse was being
     * exploited at all. A post-transform cache existed below, but the SIMT path
     * returns before it can ever run, so it was dead code on every indexed draw.
     *
     * Deduping in front of the SIMT path gets both: each distinct vertex is
     * transformed once, and it is still transformed in wide SIMT batches. The
     * expansion afterwards is a copy, which is far cheaper than re-running the
     * vertex shader. Bit-exact by construction -- the transform is a pure
     * function of (index, uniforms), so computing it once and copying gives the
     * same bits as computing it three times. */
    phase0 = sgl_prof_now();
    if (sg_vertex_prog_simt_dedup(count, idx, gp, vstride, pv, vb, mvp_set, mvp,
                                  vp_x, vp_y, vp_w, vp_h, depth_near, depth_far,
                                  fetch, fetch_ctx)) {
        if (sgl_prof_enabled) sgl_prof_vs_simt += sgl_prof_now() - phase0;
        if (sgl_prof_enabled) { ++sgl_prof_vs_simt_draws; sgl_prof_vs_simt_verts += (unsigned long)count; }
        return;
    }

    if (SG_VS_SIMT(count, idx, gp, vstride, pv, vb, mvp_set, mvp,
                   vp_x, vp_y, vp_w, vp_h, depth_near, depth_far, fetch, fetch_ctx)) {
        if (sgl_prof_enabled) sgl_prof_vs_simt += sgl_prof_now() - phase0;
        if (sgl_prof_enabled) { ++sgl_prof_vs_simt_draws; sgl_prof_vs_simt_verts += (unsigned long)count; }
        return;
    }

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
        if (sgl_prof_enabled) sgl_prof_vs_scalar += sgl_prof_now() - phase0;
        if (sgl_diag_enabled) sgl_dbg_vs_scalar += (unsigned long)count;
        if (sgl_prof_enabled) { ++sgl_prof_vs_scalar_draws; sgl_prof_vs_scalar_verts += (unsigned long)count; }
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
        if (sgl_prof_enabled) sgl_prof_vs_scalar += sgl_prof_now() - phase0;
        if (sgl_diag_enabled) sgl_dbg_vs_scalar += (unsigned long)count;
        if (sgl_prof_enabled) { ++sgl_prof_vs_scalar_draws; sgl_prof_vs_scalar_verts += (unsigned long)count; }
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
    if (sgl_prof_enabled) { ++sgl_prof_vs_scalar_draws; sgl_prof_vs_scalar_verts += cache_miss; }
    if (sgl_prof_enabled) sgl_prof_vs_cache += sgl_prof_now() - phase0;
    free(keys);
    free(used);
    free(cache_pv);
    free(cache_vary);
}
