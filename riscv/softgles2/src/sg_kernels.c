/*
 * softgles2 — register-resident fragment kernels (the "Carmack lever").
 *
 * These are hand-written specialisations of whole shader archetypes that keep
 * every intermediate in vector registers, calling the same RVV helpers as the
 * per-op SIMT VM (simt_run_ir in glsl_simt.c) but WITHOUT spilling each IR op
 * through the ~1KB-per-register memory file. glsl_fs_classify() binds one only
 * when the linked fragment shader matches the kernel's exact verified layout;
 * any mismatch leaves fs_kernel==0 and the VM runs (zero behavioural risk).
 *
 * Split out of glsl_simt.c so the VM and its specialised replacements are
 * separate concerns. Two kernels today:
 *   - interaction: bump + diffuse + specular + projective light (5 textures)
 *   - overlay:     gl4es 2D blit  gl_FragColor = Color * texture2DProj(s, tc)
 *                  — THE shader that actually renders the demo every frame.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#endif

#include "glsl_internal.h"
#include "sg_math.h"

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define GLSL_SIMT_RVV 1
#else
#  define GLSL_SIMT_RVV 0
#endif
#define SIMT_MAXLANE GLSL_SIMT_MAXLANE

#if GLSL_SIMT_RVV

static int iak_uoff(const glsl_program *p, const char *name)
{
    for (int i = 0; i < p->nuni; ++i)
        if (!p->uni[i].is_sampler && !strcmp(p->uni[i].name, name)) return p->uni[i].offset;
    return -1;
}

/* Interaction: bump + diffuse + specular + projective light/falloff.
 * Layout (verified by the classifier): 5 vec4 varyings TexBump/TexDiff/TexLight/
 * LightVec/ViewVec at float offsets 0/4/8/12/16; samplers 0..4; vec4 uniforms
 * diffuseColor + specularColor. */
#define IAK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
static int glsl_fs_kernel_interaction(glsl_program *p, const float *varys, int vl,
                                      const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                      const float *ustore, float *out)
{
    if (!samplers[0] || !samplers[1] || !samplers[2] || !samplers[3] || !samplers[4])
        return 0;   /* missing texture -> let the VM handle it */
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    const float inv255 = 1.f / 255.f;
    int doff = iak_uoff(p, "diffuseColor"), soff = iak_uoff(p, "specularColor");
    float dcx = doff >= 0 ? ustore[doff] : 1.f, dcy = doff >= 0 ? ustore[doff + 1] : 1.f, dcz = doff >= 0 ? ustore[doff + 2] : 1.f;
    float scx = soff >= 0 ? ustore[soff] : 1.f, scy = soff >= 0 ? ustore[soff + 1] : 1.f, scz = soff >= 0 ? ustore[soff + 2] : 1.f;
    vfloat32m2_t r0, r1, r2, r3;

    sgl_rvv_sample2d_m2(samplers[0], IAK_V(0), IAK_V(1), VL, inv255, samplers[0]->min_filter, samplers[0]->wrap, &r0, &r1, &r2, &r3);
    vfloat32m2_t nx = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r0, 2.f, VL), 1.f, VL);
    vfloat32m2_t ny = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r1, 2.f, VL), 1.f, VL);
    vfloat32m2_t nz = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r2, 2.f, VL), 1.f, VL);
    vfloat32m2_t s = __riscv_vfmul_vv_f32m2(nx, nx, VL); s = __riscv_vfmacc_vv_f32m2(s, ny, ny, VL); s = __riscv_vfmacc_vv_f32m2(s, nz, nz, VL);
    vfloat32m2_t inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    nx = __riscv_vfmul_vv_f32m2(nx, inv, VL); ny = __riscv_vfmul_vv_f32m2(ny, inv, VL); nz = __riscv_vfmul_vv_f32m2(nz, inv, VL);

    vfloat32m2_t lx = IAK_V(12), ly = IAK_V(13), lz = IAK_V(14);
    s = __riscv_vfmul_vv_f32m2(lx, lx, VL); s = __riscv_vfmacc_vv_f32m2(s, ly, ly, VL); s = __riscv_vfmacc_vv_f32m2(s, lz, lz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    lx = __riscv_vfmul_vv_f32m2(lx, inv, VL); ly = __riscv_vfmul_vv_f32m2(ly, inv, VL); lz = __riscv_vfmul_vv_f32m2(lz, inv, VL);

    vfloat32m2_t vx = IAK_V(16), vy = IAK_V(17), vz = IAK_V(18);
    s = __riscv_vfmul_vv_f32m2(vx, vx, VL); s = __riscv_vfmacc_vv_f32m2(s, vy, vy, VL); s = __riscv_vfmacc_vv_f32m2(s, vz, vz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    vx = __riscv_vfmul_vv_f32m2(vx, inv, VL); vy = __riscv_vfmul_vv_f32m2(vy, inv, VL); vz = __riscv_vfmul_vv_f32m2(vz, inv, VL);

    vfloat32m2_t hx = __riscv_vfadd_vv_f32m2(lx, vx, VL), hy = __riscv_vfadd_vv_f32m2(ly, vy, VL), hz = __riscv_vfadd_vv_f32m2(lz, vz, VL);
    s = __riscv_vfmul_vv_f32m2(hx, hx, VL); s = __riscv_vfmacc_vv_f32m2(s, hy, hy, VL); s = __riscv_vfmacc_vv_f32m2(s, hz, hz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    hx = __riscv_vfmul_vv_f32m2(hx, inv, VL); hy = __riscv_vfmul_vv_f32m2(hy, inv, VL); hz = __riscv_vfmul_vv_f32m2(hz, inv, VL);

    vfloat32m2_t ndl = __riscv_vfmul_vv_f32m2(nx, lx, VL); ndl = __riscv_vfmacc_vv_f32m2(ndl, ny, ly, VL); ndl = __riscv_vfmacc_vv_f32m2(ndl, nz, lz, VL);
    ndl = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), ndl, __riscv_vmfgt_vf_f32m2_b16(ndl, 0.f, VL), VL);
    vfloat32m2_t ndh = __riscv_vfmul_vv_f32m2(nx, hx, VL); ndh = __riscv_vfmacc_vv_f32m2(ndh, ny, hy, VL); ndh = __riscv_vfmacc_vv_f32m2(ndh, nz, hz, VL);
    ndh = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), ndh, __riscv_vmfgt_vf_f32m2_b16(ndh, 0.f, VL), VL);
    vfloat32m2_t spec = sgl_rvv_pow_m2(ndh, __riscv_vfmv_v_f_f32m2(16.f, VL), VL);

    vfloat32m2_t dr, dg, db, da;
    sgl_rvv_sample2d_m2(samplers[1], IAK_V(4), IAK_V(5), VL, inv255, samplers[1]->min_filter, samplers[1]->wrap, &dr, &dg, &db, &da);
    dr = __riscv_vfmul_vf_f32m2(dr, dcx, VL); dg = __riscv_vfmul_vf_f32m2(dg, dcy, VL); db = __riscv_vfmul_vf_f32m2(db, dcz, VL);

    vfloat32m2_t sr, sg, sb, sa;
    sgl_rvv_sample2d_m2(samplers[2], IAK_V(4), IAK_V(5), VL, inv255, samplers[2]->min_filter, samplers[2]->wrap, &sr, &sg, &sb, &sa);
    sr = __riscv_vfmul_vf_f32m2(sr, scx, VL); sg = __riscv_vfmul_vf_f32m2(sg, scy, VL); sb = __riscv_vfmul_vf_f32m2(sb, scz, VL);

    vfloat32m2_t q = IAK_V(11);
    vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
    q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
    vfloat32m2_t pu = __riscv_vfdiv_vv_f32m2(IAK_V(8), q, VL), pv = __riscv_vfdiv_vv_f32m2(IAK_V(9), q, VL);
    vfloat32m2_t lpr, lpg, lpb, lpa;
    sgl_rvv_sample2d_m2(samplers[3], pu, pv, VL, inv255, samplers[3]->min_filter, samplers[3]->wrap, &lpr, &lpg, &lpb, &lpa);

    vfloat32m2_t fr, fg, fb, fa;
    sgl_rvv_sample2d_m2(samplers[4], IAK_V(8), IAK_V(9), VL, inv255, samplers[4]->min_filter, samplers[4]->wrap, &fr, &fg, &fb, &fa);
    vfloat32m2_t lr = __riscv_vfmul_vv_f32m2(lpr, fr, VL), lg = __riscv_vfmul_vv_f32m2(lpg, fg, VL), lb = __riscv_vfmul_vv_f32m2(lpb, fb, VL);

    vfloat32m2_t o0 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(dr, ndl, VL), sr, spec, VL), lr, VL);
    vfloat32m2_t o1 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(dg, ndl, VL), sg, spec, VL), lg, VL);
    vfloat32m2_t o2 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(db, ndl, VL), sb, spec, VL), lb, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], o0, VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], o1, VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], o2, VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], __riscv_vfmv_v_f_f32m2(1.f, VL), VL);
    return 1;
}
#undef IAK_V

static int iak_sunit(const glsl_program *p, const char *name)
{
    for (int i = 0; i < p->nuni; ++i)
        if (p->uni[i].is_sampler && !strcmp(p->uni[i].name, name)) return p->uni[i].unit;
    return -1;
}

/* The REAL gl4es-emitted Doom 3 interaction shader (interaction.vfp -> GLSL ES):
 * cubemap normalised-light lookup, bump (DXT5nm: normal.x in .w), N.L, two
 * projected light/falloff textures, diffuse*color, specular via a dependent N.H
 * lookup table * specColor * (specmap*2), then colorScale + per-channel gamma pow.
 * 8 varyings (FrontColor + TexCoord_0..6 = 32 floats), 6x sampler2D + 1 samplerCube,
 * 3 vec4 uniforms (Fragment_ProgramEnv_0/1/21). This is what the menu/intro AND
 * in-game lighting actually run; the classifier's old "interaction" archetype
 * (5 varyings, normalize(), no cube) never matches it, so without this kernel core
 * lighting runs the per-pixel VM at 133 ops/fragment.
 *
 * Bit-exact vs the VM: every op mirrors the VM's IR execution — separate
 * vfmul/vfadd (NOT fused vfmacc) for the shader's * and + (the VM runs each IR op
 * with its own rounding, -ffp-contract=off), vfmul+vfmacc dot order matching
 * sgl_rvv_dot3_v2_m2, sgl_rvv_rsqrt_fast_m2 for inversesqrt, sgl_rvv_pow_m2 for pow,
 * sgl_rvv_sample2d_m2 for 2D/proj, and the scalar sample_cube_tex per lane for the
 * cube (the VM has no RVV cube path; it falls back to the same scalar sampler). */
#define GIK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
/* Sample a 2D texture, or return the VM's missing-texture default {0,0,0,1} when the
 * sampler is unbound. Bit-exact vs the SIMT VM null path (glsl_simt.c:330-333 writes
 * literal 0/0/0/1; the proj null path returns the same via the scalar fallback). Lets
 * the interaction kernel handle materials lacking spec/bump/etc. instead of bailing to
 * the slow VM. Generic: optional-sampler handling is standard GLES2, not Doom3-specific. */
static inline void iak_sample2d_or_default(const sgl_tex *s, vfloat32m2_t u, vfloat32m2_t v,
                                           size_t VL, float inv255,
                                           vfloat32m2_t *r, vfloat32m2_t *g, vfloat32m2_t *b, vfloat32m2_t *a)
{
    if (s) {
        sgl_rvv_sample2d_m2(s, u, v, VL, inv255, s->min_filter, s->wrap, r, g, b, a);
    } else {
        vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, VL);
        *r = z; *g = z; *b = z; *a = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }
}

/* Diagnostic toggle: force the interaction light term to unity so the kernel outputs
 * diffuse*diffuseColor + spec with NO light modulation. Isolates whether the flat world
 * look comes from the light term (e.g. a null light-projection/falloff sampler ->
 * iak_sample2d_or_default returns {0,0,0,1} -> light multiplies to 0) versus the diffuse
 * sample itself. Set from the engine via the bm_iakLightUnity cvar (sys_dos.cpp). 0=off. */
int sgl_dbg_iak_light_unity = 0;

static int glsl_fs_kernel_interaction_gl4es(glsl_program *p, const float *varys, int vl,
                                            const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                            const float *ustore, float *out)
{
    int uc = iak_sunit(p, "_gl4es_SamplerCube_0");
    int u1 = iak_sunit(p, "_gl4es_Sampler2D_1"), u2 = iak_sunit(p, "_gl4es_Sampler2D_2");
    int u3 = iak_sunit(p, "_gl4es_Sampler2D_3"), u4 = iak_sunit(p, "_gl4es_Sampler2D_4");
    int u5 = iak_sunit(p, "_gl4es_Sampler2D_5"), u6 = iak_sunit(p, "_gl4es_Sampler2D_6");
    if (uc < 0 || u1 < 0 || u2 < 0 || u3 < 0 || u4 < 0 || u5 < 0 || u6 < 0) return 0;
    const sgl_tex *sc = (uc < GLSL_MAX_SAMPLERS) ? samplers[uc] : 0;   /* cube may be null */
    const sgl_tex *s1 = samplers[u1], *s2 = samplers[u2], *s3 = samplers[u3];
    const sgl_tex *s4 = samplers[u4], *s5 = samplers[u5], *s6 = samplers[u6];
    /* null 2D samplers are handled per-sample (iak_sample2d_or_default -> {0,0,0,1},
     * matching the VM) instead of bailing, so materials missing spec/bump/etc. stay on
     * the fast kernel. uniform-location check (uc/u1..u6 >= 0) above still gates structure. */

    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    const float inv255 = 1.f / 255.f;
    int o0 = iak_uoff(p, "_gl4es_Fragment_ProgramEnv_0");
    int o1 = iak_uoff(p, "_gl4es_Fragment_ProgramEnv_1");
    int o21 = iak_uoff(p, "_gl4es_Fragment_ProgramEnv_21");
    float d0=o0>=0?ustore[o0]:1.f, d1=o0>=0?ustore[o0+1]:1.f, d2=o0>=0?ustore[o0+2]:1.f, d3=o0>=0?ustore[o0+3]:1.f;
    float p0=o1>=0?ustore[o1]:1.f, p1=o1>=0?ustore[o1+1]:1.f, p2c=o1>=0?ustore[o1+2]:1.f, p3c=o1>=0?ustore[o1+3]:1.f;
    float g0=o21>=0?ustore[o21]:1.f, g1=o21>=0?ustore[o21+1]:1.f, g2=o21>=0?ustore[o21+2]:1.f, gw=o21>=0?ustore[o21+3]:1.f;

    /* specular half-angle: rinv = inversesqrt(dot(t6,t6)); h = rinv * t6 */
    vfloat32m2_t t6x=GIK_V(28), t6y=GIK_V(29), t6z=GIK_V(30);
    vfloat32m2_t sd=__riscv_vfmul_vv_f32m2(t6x,t6x,VL);
    sd=__riscv_vfmacc_vv_f32m2(sd,t6y,t6y,VL); sd=__riscv_vfmacc_vv_f32m2(sd,t6z,t6z,VL);
    vfloat32m2_t rinv=__riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f,VL), sgl_rvv_rsqrt_fast_m2(sd,VL), __riscv_vmfgt_vf_f32m2_b16(sd,0.f,VL), VL);
    vfloat32m2_t hx=__riscv_vfmul_vv_f32m2(rinv,t6x,VL), hy=__riscv_vfmul_vv_f32m2(rinv,t6y,VL), hz=__riscv_vfmul_vv_f32m2(rinv,t6z,VL);

    /* light = textureCube(cube, t0.xyz) per lane (matches VM scalar fallback), then *2-1 */
    float ax[SGL_SIMT_LANES], ay[SGL_SIMT_LANES], az[SGL_SIMT_LANES];
    float cr[SGL_SIMT_LANES], cg[SGL_SIMT_LANES], cb[SGL_SIMT_LANES];
    __riscv_vse32_v_f32m2(ax, GIK_V(4), VL); __riscv_vse32_v_f32m2(ay, GIK_V(5), VL); __riscv_vse32_v_f32m2(az, GIK_V(6), VL);
    for (int L=0; L<vl; ++L) { float o[4]={0.f,0.f,0.f,1.f}; if (sc) sample_cube_tex(sc, ax[L], ay[L], az[L], o); cr[L]=o[0]; cg[L]=o[1]; cb[L]=o[2]; }
    vfloat32m2_t lx=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(__riscv_vle32_v_f32m2(cr,VL),2.f,VL),-1.f,VL);
    vfloat32m2_t ly=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(__riscv_vle32_v_f32m2(cg,VL),2.f,VL),-1.f,VL);
    vfloat32m2_t lz=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(__riscv_vle32_v_f32m2(cb,VL),2.f,VL),-1.f,VL);

    /* localNormal = texture2D(bump); .x = .w (DXT5nm); then *2-1 */
    vfloat32m2_t br,bg,bb,ba;
    iak_sample2d_or_default(s1, GIK_V(8), GIK_V(9), VL, inv255, &br,&bg,&bb,&ba);
    vfloat32m2_t nx=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(ba,2.f,VL),-1.f,VL);   /* .x <- .w */
    vfloat32m2_t ny=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(bg,2.f,VL),-1.f,VL);
    vfloat32m2_t nz=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(bb,2.f,VL),-1.f,VL);

    /* N.L (light vec . normal), broadcast scalar */
    vfloat32m2_t ndl=__riscv_vfmul_vv_f32m2(lx,nx,VL);
    ndl=__riscv_vfmacc_vv_f32m2(ndl,ly,ny,VL); ndl=__riscv_vfmacc_vv_f32m2(ndl,lz,nz,VL);

    /* light *= texture2DProj(s3, t3); light *= texture2DProj(s2, t2)  (q-guarded) */
    vfloat32m2_t q=GIK_V(19); q=__riscv_vmerge_vvm_f32m2(q,__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vmfeq_vf_f32m2_b16(q,0.f,VL),VL);
    vfloat32m2_t a3r,a3g,a3b,a3a;
    iak_sample2d_or_default(s3, __riscv_vfdiv_vv_f32m2(GIK_V(16),q,VL), __riscv_vfdiv_vv_f32m2(GIK_V(17),q,VL), VL, inv255, &a3r,&a3g,&a3b,&a3a);
    vfloat32m2_t lr=__riscv_vfmul_vv_f32m2(ndl,a3r,VL), lg=__riscv_vfmul_vv_f32m2(ndl,a3g,VL), lb=__riscv_vfmul_vv_f32m2(ndl,a3b,VL), la=__riscv_vfmul_vv_f32m2(ndl,a3a,VL);
    q=GIK_V(15); q=__riscv_vmerge_vvm_f32m2(q,__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vmfeq_vf_f32m2_b16(q,0.f,VL),VL);
    vfloat32m2_t a2r,a2g,a2b,a2a;
    iak_sample2d_or_default(s2, __riscv_vfdiv_vv_f32m2(GIK_V(12),q,VL), __riscv_vfdiv_vv_f32m2(GIK_V(13),q,VL), VL, inv255, &a2r,&a2g,&a2b,&a2a);
    lr=__riscv_vfmul_vv_f32m2(lr,a2r,VL); lg=__riscv_vfmul_vv_f32m2(lg,a2g,VL); lb=__riscv_vfmul_vv_f32m2(lb,a2b,VL); la=__riscv_vfmul_vv_f32m2(la,a2a,VL);

    /* color = diffuse * diffuseColor */
    vfloat32m2_t dr,dg,db,da;
    iak_sample2d_or_default(s4, GIK_V(20), GIK_V(21), VL, inv255, &dr,&dg,&db,&da);
    vfloat32m2_t cr2=__riscv_vfmul_vf_f32m2(dr,d0,VL), cg2=__riscv_vfmul_vf_f32m2(dg,d1,VL), cb2=__riscv_vfmul_vf_f32m2(db,d2,VL), ca2=__riscv_vfmul_vf_f32m2(da,d3,VL);

    /* N.H -> spec LUT(s6) * specColor; R2 = specmap(s5)*2; color = R1*R2 + color */
    vfloat32m2_t ndh=__riscv_vfmul_vv_f32m2(hx,nx,VL);
    ndh=__riscv_vfmacc_vv_f32m2(ndh,hy,ny,VL); ndh=__riscv_vfmacc_vv_f32m2(ndh,hz,nz,VL);
    vfloat32m2_t er,eg,eb,ea;
    iak_sample2d_or_default(s6, ndh, ndh, VL, inv255, &er,&eg,&eb,&ea);
    er=__riscv_vfmul_vf_f32m2(er,p0,VL); eg=__riscv_vfmul_vf_f32m2(eg,p1,VL); eb=__riscv_vfmul_vf_f32m2(eb,p2c,VL); ea=__riscv_vfmul_vf_f32m2(ea,p3c,VL);
    vfloat32m2_t mr,mg,mb,ma;
    iak_sample2d_or_default(s5, GIK_V(24), GIK_V(25), VL, inv255, &mr,&mg,&mb,&ma);
    mr=__riscv_vfadd_vv_f32m2(mr,mr,VL); mg=__riscv_vfadd_vv_f32m2(mg,mg,VL); mb=__riscv_vfadd_vv_f32m2(mb,mb,VL); ma=__riscv_vfadd_vv_f32m2(ma,ma,VL);
    /* color = R1*R2 + color  (separate mul then add: two roundings, matches the VM) */
    cr2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(er,mr,VL),cr2,VL);
    cg2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(eg,mg,VL),cg2,VL);
    cb2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(eb,mb,VL),cb2,VL);
    ca2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(ea,ma,VL),ca2,VL);

    /* color = light * color; tmp = color * FrontColor */
    if (sgl_dbg_iak_light_unity) { lr=lg=lb=la=__riscv_vfmv_v_f_f32m2(1.f,VL); }   /* diag: diffuse-only */
    cr2=__riscv_vfmul_vv_f32m2(lr,cr2,VL); cg2=__riscv_vfmul_vv_f32m2(lg,cg2,VL); cb2=__riscv_vfmul_vv_f32m2(lb,cb2,VL); ca2=__riscv_vfmul_vv_f32m2(la,ca2,VL);
    vfloat32m2_t tr=__riscv_vfmul_vv_f32m2(cr2,GIK_V(0),VL), tg=__riscv_vfmul_vv_f32m2(cg2,GIK_V(1),VL), tb=__riscv_vfmul_vv_f32m2(cb2,GIK_V(2),VL), ta=__riscv_vfmul_vv_f32m2(ca2,GIK_V(3),VL);

    /* rgb = pow(clamp(colorScale.xyz * tmp.xyz, 0, 1), gamma); a = tmp.w (raw) */
    vfloat32m2_t one=__riscv_vfmv_v_f_f32m2(1.f,VL), zero=__riscv_vfmv_v_f_f32m2(0.f,VL), gv=__riscv_vfmv_v_f_f32m2(gw,VL);
    tr=__riscv_vfmul_vf_f32m2(tr,g0,VL); tg=__riscv_vfmul_vf_f32m2(tg,g1,VL); tb=__riscv_vfmul_vf_f32m2(tb,g2,VL);
    tr=__riscv_vmerge_vvm_f32m2(tr,one,__riscv_vmfgt_vf_f32m2_b16(tr,1.f,VL),VL); tr=__riscv_vmerge_vvm_f32m2(tr,zero,__riscv_vmflt_vf_f32m2_b16(tr,0.f,VL),VL);
    tg=__riscv_vmerge_vvm_f32m2(tg,one,__riscv_vmfgt_vf_f32m2_b16(tg,1.f,VL),VL); tg=__riscv_vmerge_vvm_f32m2(tg,zero,__riscv_vmflt_vf_f32m2_b16(tg,0.f,VL),VL);
    tb=__riscv_vmerge_vvm_f32m2(tb,one,__riscv_vmfgt_vf_f32m2_b16(tb,1.f,VL),VL); tb=__riscv_vmerge_vvm_f32m2(tb,zero,__riscv_vmflt_vf_f32m2_b16(tb,0.f,VL),VL);
    __riscv_vse32_v_f32m2(&out[0*SIMT_MAXLANE], sgl_rvv_pow_m2(tr,gv,VL), VL);
    __riscv_vse32_v_f32m2(&out[1*SIMT_MAXLANE], sgl_rvv_pow_m2(tg,gv,VL), VL);
    __riscv_vse32_v_f32m2(&out[2*SIMT_MAXLANE], sgl_rvv_pow_m2(tb,gv,VL), VL);
    __riscv_vse32_v_f32m2(&out[3*SIMT_MAXLANE], ta, VL);
    return 1;
}
#undef GIK_V

/* 2D overlay (gl4es blit): gl_FragColor = Color * texture2DProj(sampler0, TexCoord_0).
 * Verified layout: vary[0]=Color @0 (vec4), vary[1]=TexCoord_0 @4 (vec4); 1 sampler.
 * Ops MATCH the VM's BI_TEXTURE2DPROJ exactly (q=tc.w, q==0 guard, u=tc.x/q,
 * v=tc.y/q, sample) then the component multiply -> bit-identical to the per-op
 * VM, but register-resident: no IR-op memory spill per fragment. This is THE
 * shader the demo renders every frame, so this kernel always fires in-engine. */
#define OVK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
static int glsl_fs_kernel_overlay(glsl_program *p, const float *varys, int vl,
                                  const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                  const float *ustore, float *out)
{
    (void)p; (void)ustore;
    const sgl_tex *t = samplers[0];
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);

    vfloat32m2_t tr, tg, tb, ta;
    if (t) {
        vfloat32m2_t q = OVK_V(7);
        vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(OVK_V(4), q, VL);
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(OVK_V(5), q, VL);
        sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, &tr, &tg, &tb, &ta);
    } else {
        /* Unbound sampler: substitute the VM's missing-texture default {0,0,0,1}
         * (glsl_simt.c BI_TEXTURE2DPROJ null path) instead of bailing to the slow
         * VM, so Color*texProj layers with no bound texture stay on the kernel.
         * out becomes Color*{0,0,0,1} = {0,0,0,Color.a} — bit-identical to the VM. */
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
        ta = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }

    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(0), tr, VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(1), tg, VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(2), tb, VL), VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(3), ta, VL), VL);
    return 1;
}

/* Overlay + gl4es alpha test (the FPE_Shader emits, fsdump proj=1 nvaryf=8 + discard):
 *   fColor = Color * texture2DProj(s0, tc0);
 *   if (floor(fColor.a*255.) <= _gl4es_AlphaRef) discard;
 * Colour is identical to the plain overlay; additionally emits the SIMT kill mask
 * into out row 4 (1 = discard). Matches the VM exactly: BI_FLOOR = vfcvt round-down
 * round-trip, IR_CMP '<=' = vmfle, IR_DISCARD reads the condition register. */
static int glsl_fs_kernel_overlay_atest(glsl_program *p, const float *varys, int vl,
                                        const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                        const float *ustore, float *out)
{
    int su = iak_sunit(p, "_gl4es_TexSampler_0");
    const sgl_tex *t = (su >= 0 && su < GLSL_MAX_SAMPLERS) ? samplers[su] : 0;
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);

    vfloat32m2_t tr, tg, tb, ta;
    if (t) {
        vfloat32m2_t q = OVK_V(7);
        vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(OVK_V(4), q, VL);
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(OVK_V(5), q, VL);
        sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, &tr, &tg, &tb, &ta);
    } else {
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
        ta = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }
    vfloat32m2_t ca = __riscv_vfmul_vv_f32m2(OVK_V(3), ta, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(0), tr, VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(1), tg, VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(2), tb, VL), VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], ca, VL);

    /* discard mask: floor(ca*255.) <= AlphaRef  ->  out[4] = 1 (kill) else 0 */
    int aoff = iak_uoff(p, "_gl4es_AlphaRef");
    float aref = aoff >= 0 ? ustore[aoff] : 0.f;
    vfloat32m2_t a255 = __riscv_vfmul_vf_f32m2(ca, 255.f, VL);
    vfloat32m2_t afl = __riscv_vfcvt_f_x_v_f32m2(
        __riscv_vfcvt_x_f_v_i32m2_rm(a255, __RISCV_FRM_RDN, VL), VL);
    vbool16_t kill = __riscv_vmfle_vf_f32m2_b16(afl, aref, VL);
    __riscv_vse32_v_f32m2(&out[4 * SIMT_MAXLANE],
        __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL),
                                 __riscv_vfmv_v_f_f32m2(1.f, VL), kill, VL), VL);
    return 1;
}

/* gl4es 2-stage texture-env combine (fsdump proj=2 nvaryf=12, no discard):
 *   texColor0 = texture2DProj(s0, tc0);   // texColor1 (s1) is sampled-but-unused
 *   fColor.rgb = Color.rgb * texColor0.rgb;  fColor.a = Color.a;
 *   fColor *= _gl4es_TextureEnvColor_1;   // vec4 uniform, componentwise
 *   gl_FragColor = clamp(fColor, 0., 1.);
 * texColor1 is dead in the gl4es output, so it's omitted (bit-identical result). */
#define I8K_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
static int glsl_fs_kernel_envcombine(glsl_program *p, const float *varys, int vl,
                                     const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                     const float *ustore, float *out)
{
    int su = iak_sunit(p, "_gl4es_TexSampler_0");
    const sgl_tex *t = (su >= 0 && su < GLSL_MAX_SAMPLERS) ? samplers[su] : 0;
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    int eoff = iak_uoff(p, "_gl4es_TextureEnvColor_1");
    float er = eoff >= 0 ? ustore[eoff]     : 1.f, eg = eoff >= 0 ? ustore[eoff + 1] : 1.f;
    float eb = eoff >= 0 ? ustore[eoff + 2] : 1.f, ea = eoff >= 0 ? ustore[eoff + 3] : 1.f;

    vfloat32m2_t tr, tg, tb;
    if (t) {
        vfloat32m2_t q = I8K_V(7);
        vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(I8K_V(4), q, VL);
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(I8K_V(5), q, VL);
        vfloat32m2_t ta;
        sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, &tr, &tg, &tb, &ta);
        (void)ta;
    } else {
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
    }
    /* fColor.rgb = (Color.rgb * tc0.rgb) * Env.rgb;  fColor.a = Color.a * Env.a (two
     * separate muls each -> matches the VM's BIN op rounding), then clamp [0,1]. */
    vfloat32m2_t cr = __riscv_vfmul_vf_f32m2(__riscv_vfmul_vv_f32m2(I8K_V(0), tr, VL), er, VL);
    vfloat32m2_t cg = __riscv_vfmul_vf_f32m2(__riscv_vfmul_vv_f32m2(I8K_V(1), tg, VL), eg, VL);
    vfloat32m2_t cb = __riscv_vfmul_vf_f32m2(__riscv_vfmul_vv_f32m2(I8K_V(2), tb, VL), eb, VL);
    vfloat32m2_t ca = __riscv_vfmul_vf_f32m2(I8K_V(3), ea, VL);
    cr = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cr, 0.f, VL), 1.f, VL);
    cg = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cg, 0.f, VL), 1.f, VL);
    cb = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cb, 0.f, VL), 1.f, VL);
    ca = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(ca, 0.f, VL), 1.f, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], cr, VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], cg, VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], cb, VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], ca, VL);
    return 1;
}
#undef I8K_V
#undef OVK_V

void glsl_fs_classify(glsl_program *p)
{
    p->fs_kernel = 0;
    p->dbg_kernel_name = "VM";
    p->fs_alpha_zero_if_color_zero = 0;
    const ir_prog *ir = &p->fs_ir;
    if (!ir->ok || !ir->simt_ok) return;
    int t2 = 0, tp = 0, tc = 0, nm = 0, pw = 0;
    for (int i = 0; i < ir->ninst; ++i) {
        const ir_inst *o = &ir->inst[i];
        if (o->op != IR_BUILTIN) continue;
        switch (o->i0) { case BI_TEXTURE2D: t2++; break; case BI_TEXTURE2DPROJ: tp++; break;
                         case BI_TEXTURECUBE: tc++; break;
                         case BI_NORMALIZE: nm++; break; case BI_POW: pw++; break; default: break; }
    }
    /* interaction archetype: 5 vec4 varyings @0/4/8/12/16, >=4 textures, >=3 normalize, >=1 pow */
    if ((t2 + tp) >= 4 && nm >= 3 && pw >= 1 && p->nvary == 5 && p->nvaryfloats == 20 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 && p->vary[4].offset == 16) {
        p->fs_kernel = glsl_fs_kernel_interaction;
        p->dbg_kernel_name = "interaction";
        return;
    }
    /* gl4es interaction archetype (the REAL Doom 3 lighting shader as gl4es emits
     * it): 8 varyings (FrontColor + TexCoord_0..6 @0..28), 4 tex2D + 2 proj + 1
     * cube + 3 pow — cube normalised-light, dual projected falloff, bump (DXT5nm),
     * diffuse, spec LUT, gamma. Uses inversesqrt(dot) so normalize-count is 0. */
    if (t2 == 4 && tp == 2 && tc == 1 && pw == 3 &&
        p->nvary == 8 && p->nvaryfloats == 32 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 && p->vary[7].offset == 28) {
        p->fs_kernel = glsl_fs_kernel_interaction_gl4es;
        p->dbg_kernel_name = "interaction_gl4es";
        return;
    }
    /* overlay archetype: exactly one texture2DProj, no other builtins; 2 vec4
     * varyings Color@0 + TexCoord_0@4. (gl4es 2D blit, fsdump: proj=1 nvaryf=8.)
     * The alpha-test variant (same fingerprint + discard) must NOT match here — it
     * needs the discard kernel below, and binding the plain overlay would drop the
     * alpha test AND leave the kill-mask row uninitialised. */
    if (tp == 1 && t2 == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvary == 2 && p->nvaryfloats == 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4) {
        p->fs_kernel = glsl_fs_kernel_overlay;
        p->dbg_kernel_name = "overlay";
        /* gl_FragColor = Color * texture2DProj(...): out.a = Color.a * tex.a, so
         * out.a == 0 wherever Color.a (vary[3]) == 0 -> fully-transparent-layer skip. */
        p->fs_alpha_zero_if_color_zero = 1;
        return;
    }
    /* overlay + gl4es alpha test: same fingerprint as overlay but WITH discard. */
    if (tp == 1 && t2 == 0 && nm == 0 && pw == 0 && p->fs_has_discard &&
        p->nvary == 2 && p->nvaryfloats == 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4) {
        p->fs_kernel = glsl_fs_kernel_overlay_atest;
        p->dbg_kernel_name = "overlay_atest";
        return;
    }
    /* gl4es 2-stage env-combine: 2 texture2DProj (one dead), no other builtins, no
     * discard; 3 vec4 varyings Color@0 + TexCoord_0@4 + TexCoord_1@8 (proj=2 nvaryf=12). */
    if (tp == 2 && t2 == 0 && tc == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvary == 3 && p->nvaryfloats == 12 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4 &&
        p->vary[2].comp == 4 && p->vary[2].offset == 8) {
        p->fs_kernel = glsl_fs_kernel_envcombine;
        p->dbg_kernel_name = "envcombine";
        return;
    }
}

#else  /* host / non-RVV: no register-resident kernels, the reference VM runs */
void glsl_fs_classify(glsl_program *p) { p->fs_kernel = 0; p->dbg_kernel_name = "VM"; p->fs_alpha_zero_if_color_zero = 0; }
#endif
