/*
 * iakern_validate.c — interaction-archetype kernel proof-of-concept.
 *
 * Carmack-rules lever: the per-op SIMT bytecode VM (simt_run_ir) round-trips
 * every IR op through a 1 KB/register memory file (~1.8 KB/fragment of spill for
 * an interaction shader). This harness measures the VM running a representative
 * Doom 3 interaction shader (5 texture samples + bump + diffuse + specular) over
 * a real-texture batch, then will A/B a register-resident specialised kernel
 * against it (bit-exact-to-tolerance, no per-op spill).
 *
 * Step A (this file as committed first): VM baseline only — establishes the
 * "before" ns/sample. The kernel + A/B lands next.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#endif
#include "boot/runtime.h"          /* timer_ticks, TIMEBASE_HZ, sbi_shutdown */
#include "glsl.h"
#include "sg_core.h"
#if defined(__riscv_v_intrinsic)
#  include <riscv_vector.h>
#endif

/* scalar texture sampler the IR oracle links against (mirrors sg_raster.c). */
void sgl_sample(const sgl_tex *t, float u, float v, float out[4])
{
    if (t->wrap == 1) { u -= (float)((int)u - (u < 0.f && (float)(int)u != u)); v -= (float)((int)v - (v < 0.f && (float)(int)v != v)); }
    if (u < 0.f) u = 0.f; else if (u > 1.f) u = 1.f;
    if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
    int x = (int)(u * (float)t->w); if (x > t->w - 1) x = t->w - 1; if (x < 0) x = 0;
    int y = (int)(v * (float)t->h); if (y > t->h - 1) y = t->h - 1; if (y < 0) y = 0;
    uint32_t c = t->texels[y * t->w + x];
    for (int k = 0; k < 4; ++k) out[k] = (float)((c >> (k * 8)) & 0xFF) / 255.f;
}

static const char *VS_MIN =
"#version 100\n"
"attribute vec4 p;\nattribute vec4 aBump;\nattribute vec4 aDiff;\nattribute vec4 aLight;\n"
"attribute vec4 aLV;\nattribute vec4 aVV;\n"
"varying vec4 TexBump;\nvarying vec4 TexDiff;\nvarying vec4 TexLight;\n"
"varying vec4 LightVec;\nvarying vec4 ViewVec;\n"
"void main(){ gl_Position = p; TexBump=aBump; TexDiff=aDiff; TexLight=aLight; LightVec=aLV; ViewVec=aVV; }\n";

/* Representative Doom 3 interaction: tangent-space bump + diffuse + specular,
 * projective light + falloff. ~5 samples and ~50 arithmetic IR ops. */
static const char *FS_INTERACTION =
"#version 100\nprecision mediump float;\n"
"varying vec4 TexBump;\nvarying vec4 TexDiff;\nvarying vec4 TexLight;\n"
"varying vec4 LightVec;\nvarying vec4 ViewVec;\n"
"uniform sampler2D bumpMap;\nuniform sampler2D diffuseMap;\nuniform sampler2D specularMap;\n"
"uniform sampler2D lightProj;\nuniform sampler2D lightFalloff;\n"
"uniform vec4 diffuseColor;\nuniform vec4 specularColor;\n"
"void main(){\n"
"  vec3 N = normalize(texture2D(bumpMap, TexBump.xy).xyz * 2.0 - 1.0);\n"
"  vec3 L = normalize(LightVec.xyz);\n"
"  vec3 V = normalize(ViewVec.xyz);\n"
"  vec3 H = normalize(L + V);\n"
"  float NdotL = max(dot(N, L), 0.0);\n"
"  float NdotH = max(dot(N, H), 0.0);\n"
"  float spec = pow(NdotH, 16.0);\n"
"  vec3 diff = texture2D(diffuseMap, TexDiff.xy).xyz * diffuseColor.xyz;\n"
"  vec3 sc   = texture2D(specularMap, TexDiff.xy).xyz * specularColor.xyz;\n"
"  vec3 lp   = texture2DProj(lightProj, TexLight).xyz;\n"
"  vec3 lf   = texture2D(lightFalloff, TexLight.xy).xyz;\n"
"  vec3 light = lp * lf;\n"
"  gl_FragColor = vec4((diff * NdotL + sc * spec) * light, 1.0);\n"
"}\n";

#define TEXSZ 64
static unsigned g_texels[5][TEXSZ * TEXSZ];
static sgl_tex  g_tex[5];

static void init_tex(int i, unsigned seed)
{
    for (int y = 0; y < TEXSZ; ++y)
        for (int x = 0; x < TEXSZ; ++x) {
            unsigned v = (unsigned)((x * 5 + y * 3 + seed * 37) & 0xFF);
            g_texels[i][y * TEXSZ + x] = 0xff000000u | (v << 16) | ((v ^ 0x55u) << 8) | ((x ^ y) & 0xFFu);
        }
    g_tex[i].texels = g_texels[i];
    g_tex[i].w = TEXSZ; g_tex[i].h = TEXSZ;
    g_tex[i].min_filter = 0;  /* nearest */
    g_tex[i].wrap = 1;        /* repeat */
}

static unsigned seed = 12345u;
static float rnd(void){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (float)((int)(seed>>9))/(float)(1<<22) - 1.0f; }

#include "sg_math.h"        /* sgl_rvv_sample2d_m2 / rsqrt_fast_m2 / pow_m2 */
#include "glsl_internal.h"  /* p->fs_kernel toggle for the VM-vs-kernel A/B */

/* Register-resident interaction kernel: same RVV helpers as the VM, but every
 * intermediate lives in a vfloat32m2_t local instead of the 1 KB/register
 * memory file. Computes FS_INTERACTION exactly. Varying layout (declaration
 * order, 4 floats each): TexBump 0, TexDiff 4, TexLight 8, LightVec 12,
 * ViewVec 16. Colour uniforms inlined (same constants the VM reads from ustore). */
#define IAK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * GLSL_SIMT_MAXLANE], VL)
static void interaction_kernel(const float *varys, int vl,
                               const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float *out)
{
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    const float inv255 = 1.f / 255.f;
    vfloat32m2_t r0, r1, r2, r3;

    /* N = normalize(sample(bump, TexBump.xy).xyz * 2 - 1) */
    sgl_rvv_sample2d_m2(samplers[0], IAK_V(0), IAK_V(1), VL, inv255,
                        samplers[0]->min_filter, samplers[0]->wrap, &r0, &r1, &r2, &r3);
    vfloat32m2_t nx = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r0, 2.f, VL), 1.f, VL);
    vfloat32m2_t ny = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r1, 2.f, VL), 1.f, VL);
    vfloat32m2_t nz = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r2, 2.f, VL), 1.f, VL);
    vfloat32m2_t s = __riscv_vfmul_vv_f32m2(nx, nx, VL);
    s = __riscv_vfmacc_vv_f32m2(s, ny, ny, VL); s = __riscv_vfmacc_vv_f32m2(s, nz, nz, VL);
    vfloat32m2_t inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL),
                            sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    nx = __riscv_vfmul_vv_f32m2(nx, inv, VL); ny = __riscv_vfmul_vv_f32m2(ny, inv, VL); nz = __riscv_vfmul_vv_f32m2(nz, inv, VL);

    /* L = normalize(LightVec.xyz) */
    vfloat32m2_t lx = IAK_V(12), ly = IAK_V(13), lz = IAK_V(14);
    s = __riscv_vfmul_vv_f32m2(lx, lx, VL); s = __riscv_vfmacc_vv_f32m2(s, ly, ly, VL); s = __riscv_vfmacc_vv_f32m2(s, lz, lz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    lx = __riscv_vfmul_vv_f32m2(lx, inv, VL); ly = __riscv_vfmul_vv_f32m2(ly, inv, VL); lz = __riscv_vfmul_vv_f32m2(lz, inv, VL);

    /* V = normalize(ViewVec.xyz) */
    vfloat32m2_t vx = IAK_V(16), vy = IAK_V(17), vz = IAK_V(18);
    s = __riscv_vfmul_vv_f32m2(vx, vx, VL); s = __riscv_vfmacc_vv_f32m2(s, vy, vy, VL); s = __riscv_vfmacc_vv_f32m2(s, vz, vz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    vx = __riscv_vfmul_vv_f32m2(vx, inv, VL); vy = __riscv_vfmul_vv_f32m2(vy, inv, VL); vz = __riscv_vfmul_vv_f32m2(vz, inv, VL);

    /* H = normalize(L + V) */
    vfloat32m2_t hx = __riscv_vfadd_vv_f32m2(lx, vx, VL), hy = __riscv_vfadd_vv_f32m2(ly, vy, VL), hz = __riscv_vfadd_vv_f32m2(lz, vz, VL);
    s = __riscv_vfmul_vv_f32m2(hx, hx, VL); s = __riscv_vfmacc_vv_f32m2(s, hy, hy, VL); s = __riscv_vfmacc_vv_f32m2(s, hz, hz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    hx = __riscv_vfmul_vv_f32m2(hx, inv, VL); hy = __riscv_vfmul_vv_f32m2(hy, inv, VL); hz = __riscv_vfmul_vv_f32m2(hz, inv, VL);

    /* NdotL = max(dot(N,L),0); NdotH = max(dot(N,H),0); spec = pow(NdotH,16) */
    vfloat32m2_t ndl = __riscv_vfmul_vv_f32m2(nx, lx, VL); ndl = __riscv_vfmacc_vv_f32m2(ndl, ny, ly, VL); ndl = __riscv_vfmacc_vv_f32m2(ndl, nz, lz, VL);
    ndl = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), ndl, __riscv_vmfgt_vf_f32m2_b16(ndl, 0.f, VL), VL);
    vfloat32m2_t ndh = __riscv_vfmul_vv_f32m2(nx, hx, VL); ndh = __riscv_vfmacc_vv_f32m2(ndh, ny, hy, VL); ndh = __riscv_vfmacc_vv_f32m2(ndh, nz, hz, VL);
    ndh = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), ndh, __riscv_vmfgt_vf_f32m2_b16(ndh, 0.f, VL), VL);
    vfloat32m2_t spec = sgl_rvv_pow_m2(ndh, __riscv_vfmv_v_f_f32m2(16.f, VL), VL);

    /* diff = sample(diffuse, TexDiff.xy).xyz * diffuseColor.xyz (0.9,0.8,0.7) */
    vfloat32m2_t dr, dg, db, da;
    sgl_rvv_sample2d_m2(samplers[1], IAK_V(4), IAK_V(5), VL, inv255, samplers[1]->min_filter, samplers[1]->wrap, &dr, &dg, &db, &da);
    dr = __riscv_vfmul_vf_f32m2(dr, 0.9f, VL); dg = __riscv_vfmul_vf_f32m2(dg, 0.8f, VL); db = __riscv_vfmul_vf_f32m2(db, 0.7f, VL);

    /* sc = sample(specular, TexDiff.xy).xyz * specularColor.xyz (1,1,1) */
    vfloat32m2_t sr, sg, sb, sa;
    sgl_rvv_sample2d_m2(samplers[2], IAK_V(4), IAK_V(5), VL, inv255, samplers[2]->min_filter, samplers[2]->wrap, &sr, &sg, &sb, &sa);

    /* lp = texture2DProj(lightProj, TexLight).xyz */
    vfloat32m2_t q = IAK_V(11);
    vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
    q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
    vfloat32m2_t pu = __riscv_vfdiv_vv_f32m2(IAK_V(8), q, VL), pv = __riscv_vfdiv_vv_f32m2(IAK_V(9), q, VL);
    vfloat32m2_t lpr, lpg, lpb, lpa;
    sgl_rvv_sample2d_m2(samplers[3], pu, pv, VL, inv255, samplers[3]->min_filter, samplers[3]->wrap, &lpr, &lpg, &lpb, &lpa);

    /* lf = sample(lightFalloff, TexLight.xy).xyz; light = lp * lf */
    vfloat32m2_t fr, fg, fb, fa;
    sgl_rvv_sample2d_m2(samplers[4], IAK_V(8), IAK_V(9), VL, inv255, samplers[4]->min_filter, samplers[4]->wrap, &fr, &fg, &fb, &fa);
    vfloat32m2_t lr = __riscv_vfmul_vv_f32m2(lpr, fr, VL), lg = __riscv_vfmul_vv_f32m2(lpg, fg, VL), lb = __riscv_vfmul_vv_f32m2(lpb, fb, VL);

    /* out = (diff*NdotL + sc*spec) * light */
    vfloat32m2_t or0 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(dr, ndl, VL), sr, spec, VL), lr, VL);
    vfloat32m2_t or1 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(dg, ndl, VL), sg, spec, VL), lg, VL);
    vfloat32m2_t or2 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(db, ndl, VL), sb, spec, VL), lb, VL);
    __riscv_vse32_v_f32m2(&out[0 * GLSL_SIMT_MAXLANE], or0, VL);
    __riscv_vse32_v_f32m2(&out[1 * GLSL_SIMT_MAXLANE], or1, VL);
    __riscv_vse32_v_f32m2(&out[2 * GLSL_SIMT_MAXLANE], or2, VL);
    __riscv_vse32_v_f32m2(&out[3 * GLSL_SIMT_MAXLANE], __riscv_vfmv_v_f_f32m2(1.f, VL), VL);
}

int kmain(void)
{
    printf("\n==== interaction-kernel validation (rva23s64) ====\n");
    for (int i = 0; i < 5; ++i) init_tex(i, (unsigned)(i + 1));

    glsl_program *p = glsl_program_create();
    char err[160]; err[0] = 0;
    if (!glsl_attach_source(p, 0, VS_MIN, err, sizeof err)) { printf("VS: %s\n", err); return 1; }
    if (!glsl_attach_source(p, 1, FS_INTERACTION, err, sizeof err)) { printf("FS: %s\n", err); return 1; }
    if (!glsl_link(p, err, sizeof err)) { printf("link: %s\n", err); return 1; }

    /* bind sampler units 0..4 and the two colour uniforms */
    const char *snames[5] = { "bumpMap", "diffuseMap", "specularMap", "lightProj", "lightFalloff" };
    for (int i = 0; i < 5; ++i) { int l = glsl_uniform_location(p, snames[i]); if (l >= 0) glsl_uniform_sampler(p, l, i); }
    { int l = glsl_uniform_location(p, "diffuseColor");  float c[4]={0.9f,0.8f,0.7f,1.f}; if(l>=0) glsl_uniform(p,l,c,4); }
    { int l = glsl_uniform_location(p, "specularColor"); float c[4]={1.0f,1.0f,1.0f,1.f}; if(l>=0) glsl_uniform(p,l,c,4); }

    int simt = glsl_fs_simt_ok(p);
    int nv = glsl_num_varying_floats(p); if (nv < 4) nv = 4; if (nv > GLSL_MAX_VARYING_FLOATS) nv = GLSL_MAX_VARYING_FLOATS;
    printf("[iakern] simt_ok=%d nvaryfloats=%d fs_cost=%d\n", simt, nv, glsl_fs_cost(p));
    if (!simt) { printf("[iakern] FS not SIMT-eligible — abort\n"); return 1; }

    const sgl_tex *samplers[GLSL_MAX_SAMPLERS];
    for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = (i < 5) ? &g_tex[i] : 0;

    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);
    static float varys[GLSL_MAX_VARYING_FLOATS * GLSL_SIMT_MAXLANE];
    static float out[4 * GLSL_SIMT_MAXLANE];
    for (int f = 0; f < nv; ++f) for (int L = 0; L < vl; ++L) varys[f * GLSL_SIMT_MAXLANE + L] = rnd() * 0.5f + 0.5f;

    /* The classifier must have auto-bound the engine kernel for this shader. */
    glsl_fs_kernel_fn kfn = p->fs_kernel;
    printf("[iakern] classifier bound engine kernel: %s\n", kfn ? "YES" : "NO (FAIL)");
    static float kout[4 * GLSL_SIMT_MAXLANE];
    const int BATCHES = 4000;
    uint64_t frags = (uint64_t)BATCHES * vl;

    /* ---- VM baseline (force the kernel off) ---- */
    p->fs_kernel = 0;
    uint64_t t0 = timer_ticks();
    for (int b = 0; b < BATCHES; ++b) glsl_run_fragment_simt(p, varys, nv, vl, samplers, out);
    uint64_t t1 = timer_ticks();
    uint64_t vm_ns = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;

    /* ---- engine kernel via the SAME entry point (auto-dispatch) ---- */
    p->fs_kernel = kfn;
    uint64_t k0 = timer_ticks();
    for (int b = 0; b < BATCHES; ++b) glsl_run_fragment_simt(p, varys, nv, vl, samplers, kout);
    uint64_t k1 = timer_ticks();
    uint64_t k_ns = (k1 - k0) * 1000000000ull / TIMEBASE_HZ;

    /* ---- correctness: engine kernel vs VM, and vs the local reference kernel ---- */
    p->fs_kernel = 0;   glsl_run_fragment_simt(p, varys, nv, vl, samplers, out);
    p->fs_kernel = kfn; glsl_run_fragment_simt(p, varys, nv, vl, samplers, kout);
    static float lout[4 * GLSL_SIMT_MAXLANE];
    interaction_kernel(varys, vl, samplers, lout);
    float maxd = 0, maxl = 0;
    for (int k = 0; k < 4; ++k) for (int L = 0; L < vl; ++L) {
        float vv = out[k * GLSL_SIMT_MAXLANE + L], kv = kout[k * GLSL_SIMT_MAXLANE + L], lv = lout[k * GLSL_SIMT_MAXLANE + L];
        float d = vv - kv; if (d < 0) d = -d; if (d > maxd) maxd = d;
        float e = kv - lv; if (e < 0) e = -e; if (e > maxl) maxl = e;
    }

    uint32_t spx = (uint32_t)(vm_ns * 100 / (k_ns ? k_ns : 1));
    printf("[iakern] VM     vl=%d frags=%llu  %llu ns/frag\n", vl, (unsigned long long)frags, (unsigned long long)(vm_ns / frags));
    printf("[iakern] kernel vl=%d frags=%llu  %llu ns/frag  speedup=%u.%02ux  maxdiff(vs VM)=%u e-6  maxdiff(vs ref)=%u e-6  %s\n",
           vl, (unsigned long long)frags, (unsigned long long)(k_ns / frags),
           spx / 100, spx % 100, (unsigned)(maxd * 1000000.0f), (unsigned)(maxl * 1000000.0f),
           (maxd < 1e-4f) ? "OK" : "*** MISMATCH ***");
    printf("[iakern] done.\n");
    sbi_shutdown();
    return 0;
}
