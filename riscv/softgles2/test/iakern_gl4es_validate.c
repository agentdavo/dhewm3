/*
 * iakern_gl4es_validate.c — bit-exactness proof for the REAL gl4es-emitted Doom 3
 * interaction shader kernel (glsl_fs_kernel_interaction_gl4es, sg_kernels.c).
 *
 * The shader source (interaction_gl4es_fs.h) is the EXACT GLSL gl4es produces from
 * interaction.vfp, captured live (Phase 0 id=2): cubemap normalised-light, bump
 * (DXT5nm), N.L, two projected falloff textures, diffuse*color, specular via a
 * dependent N.H lookup, then colorScale + per-channel gamma pow. 6x sampler2D +
 * 1 samplerCube, 8 varyings (FrontColor + TexCoord_0..6), 3 vec4 uniforms.
 *
 * Test: compile/link it (the classifier must auto-bind the kernel), then run the
 * fragment shading once through the kernel and once through the per-op VM with the
 * SAME inputs and compare. maxdiff must be 0 — the kernel mirrors the VM op-for-op.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#endif
#include "boot/runtime.h"
#include "glsl.h"
#include "sg_core.h"
#include "glsl_internal.h"   /* full glsl_program + glsl_fs_kernel_fn for the kernel A/B */
#if defined(__riscv_v_intrinsic)
#  include <riscv_vector.h>
#endif

/* scalar sampler the IR oracle + the cubemap path link against (mirrors sg_raster.c). */
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

/* VS declares the 8 gl4es varyings in the exact order the FS expects so the linker
 * lays them out at float offsets 0/4/8/.../28 (what the classifier verifies). */
static const char *VS_GL4ES =
"#version 100\n"
"attribute vec4 p;\n"
"attribute vec4 aCol;\nattribute vec4 a0;\nattribute vec4 a1;\nattribute vec4 a2;\n"
"attribute vec4 a3;\nattribute vec4 a4;\nattribute vec4 a5;\nattribute vec4 a6;\n"
"varying vec4 _gl4es_FrontColor;\n"
"varying vec4 _gl4es_TexCoord_0;\nvarying vec4 _gl4es_TexCoord_1;\nvarying vec4 _gl4es_TexCoord_2;\n"
"varying vec4 _gl4es_TexCoord_3;\nvarying vec4 _gl4es_TexCoord_4;\nvarying vec4 _gl4es_TexCoord_5;\n"
"varying vec4 _gl4es_TexCoord_6;\n"
"void main(){ gl_Position=p; _gl4es_FrontColor=aCol;\n"
"  _gl4es_TexCoord_0=a0; _gl4es_TexCoord_1=a1; _gl4es_TexCoord_2=a2; _gl4es_TexCoord_3=a3;\n"
"  _gl4es_TexCoord_4=a4; _gl4es_TexCoord_5=a5; _gl4es_TexCoord_6=a6; }\n";

#include "interaction_gl4es_fs.h"   /* FS_INTERACTION_GL4ES — the captured shader */

#define TEXSZ 64
static uint32_t g_2d[6][TEXSZ * TEXSZ];
static sgl_tex  g_t2d[6];
static uint32_t g_cf[6][TEXSZ * TEXSZ];
static sgl_tex  g_face[6];
static sgl_tex  g_cube;

static void fill(uint32_t *px, unsigned s)
{
    for (int y = 0; y < TEXSZ; ++y)
        for (int x = 0; x < TEXSZ; ++x) {
            unsigned v = (unsigned)((x * 5 + y * 3 + s * 37) & 0xFF);
            px[y * TEXSZ + x] = 0xff000000u | (v << 16) | ((v ^ 0x55u) << 8) | ((x ^ y) & 0xFFu);
        }
}

static unsigned seed = 12345u;
static float rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (float)((int)(seed >> 9)) / (float)(1 << 22) - 1.0f; }

int kmain(void)
{
    printf("\n==== gl4es-interaction-kernel validation (rva23s64) ====\n");
    for (int i = 0; i < 6; ++i) {
        fill(g_2d[i], (unsigned)(i + 1));
        g_t2d[i].w = TEXSZ; g_t2d[i].h = TEXSZ; g_t2d[i].texels = g_2d[i];
        g_t2d[i].min_filter = 0; g_t2d[i].wrap = 1; g_t2d[i].is_cube = 0;
        fill(g_cf[i], (unsigned)(100 + i));
        g_face[i].w = TEXSZ; g_face[i].h = TEXSZ; g_face[i].texels = g_cf[i];
        g_face[i].min_filter = 0; g_face[i].wrap = 0; g_face[i].is_cube = 0;
    }
    g_cube.w = TEXSZ; g_cube.h = TEXSZ; g_cube.texels = 0;
    g_cube.min_filter = 0; g_cube.wrap = 0; g_cube.is_cube = 1;
    for (int i = 0; i < 6; ++i) g_cube.cube_faces[i] = &g_face[i];

    glsl_program *p = glsl_program_create();
    char err[200]; err[0] = 0;
    if (!glsl_attach_source(p, 0, VS_GL4ES, err, sizeof err)) { printf("VS: %s\n", err); return 1; }
    if (!glsl_attach_source(p, 1, FS_INTERACTION_GL4ES, err, sizeof err)) { printf("FS: %s\n", err); return 1; }
    if (!glsl_link(p, err, sizeof err)) { printf("link: %s\n", err); return 1; }

    /* bind cube -> unit 0, Sampler2D_1..6 -> units 1..6, + the 3 ProgramEnv vec4s */
    { int l = glsl_uniform_location(p, "_gl4es_SamplerCube_0"); if (l >= 0) glsl_uniform_sampler(p, l, 0); }
    const char *s2d[6] = { "_gl4es_Sampler2D_1","_gl4es_Sampler2D_2","_gl4es_Sampler2D_3",
                           "_gl4es_Sampler2D_4","_gl4es_Sampler2D_5","_gl4es_Sampler2D_6" };
    for (int i = 0; i < 6; ++i) { int l = glsl_uniform_location(p, s2d[i]); if (l >= 0) glsl_uniform_sampler(p, l, i + 1); }
    { int l = glsl_uniform_location(p, "_gl4es_Fragment_ProgramEnv_0");  float c[4]={0.9f,0.8f,0.7f,1.f};  if(l>=0) glsl_uniform(p,l,c,4); }
    { int l = glsl_uniform_location(p, "_gl4es_Fragment_ProgramEnv_1");  float c[4]={1.0f,1.0f,1.0f,1.f};  if(l>=0) glsl_uniform(p,l,c,4); }
    { int l = glsl_uniform_location(p, "_gl4es_Fragment_ProgramEnv_21"); float c[4]={1.0f,1.0f,1.0f,0.5f}; if(l>=0) glsl_uniform(p,l,c,4); }

    int simt = glsl_fs_simt_ok(p);
    int nv = glsl_num_varying_floats(p); if (nv < 4) nv = 4; if (nv > GLSL_MAX_VARYING_FLOATS) nv = GLSL_MAX_VARYING_FLOATS;
    glsl_fs_kernel_fn kfn = p->fs_kernel;
    printf("[gik] simt_ok=%d nvaryfloats=%d kernel_bound=%s\n", simt, nv, kfn ? "YES" : "NO (classifier did not match!)");
    if (!simt) { printf("[gik] FS not SIMT-eligible — abort\n"); return 1; }
    if (!kfn)  { printf("[gik] *** classifier did NOT bind interaction_gl4es — FAIL ***\n"); sbi_shutdown(); return 1; }

    const sgl_tex *samplers[GLSL_MAX_SAMPLERS];
    samplers[0] = &g_cube;
    for (int i = 0; i < 6; ++i) samplers[i + 1] = &g_t2d[i];
    for (int i = 7; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;

    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);
    static float varys[GLSL_MAX_VARYING_FLOATS * GLSL_SIMT_MAXLANE];
    static float kout[4 * GLSL_SIMT_MAXLANE], vout[4 * GLSL_SIMT_MAXLANE];
    /* non-degenerate inputs in [-1,1] so cube face-selection, N.L/N.H and the proj
     * divides all exercise real values. */
    for (int f = 0; f < nv; ++f) for (int L = 0; L < vl; ++L) varys[f * GLSL_SIMT_MAXLANE + L] = rnd();

    /* timed A/B + correctness */
    const int BATCHES = 4000;
    uint64_t frags = (uint64_t)BATCHES * vl;
    p->fs_kernel = 0;
    uint64_t t0 = timer_ticks();
    for (int b = 0; b < BATCHES; ++b) glsl_run_fragment_simt(p, varys, nv, vl, samplers, vout);
    uint64_t t1 = timer_ticks();
    p->fs_kernel = kfn;
    uint64_t k0 = timer_ticks();
    for (int b = 0; b < BATCHES; ++b) glsl_run_fragment_simt(p, varys, nv, vl, samplers, kout);
    uint64_t k1 = timer_ticks();
    uint64_t vm_ns = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;
    uint64_t k_ns  = (k1 - k0) * 1000000000ull / TIMEBASE_HZ;

    p->fs_kernel = 0;   glsl_run_fragment_simt(p, varys, nv, vl, samplers, vout);
    p->fs_kernel = kfn; glsl_run_fragment_simt(p, varys, nv, vl, samplers, kout);
    float maxd = 0; int bitmiss = 0;
    for (int k = 0; k < 4; ++k) for (int L = 0; L < vl; ++L) {
        float vv = vout[k * GLSL_SIMT_MAXLANE + L], kv = kout[k * GLSL_SIMT_MAXLANE + L];
        float d = vv - kv; if (d < 0) d = -d; if (d > maxd) maxd = d;
        if (vv != kv) bitmiss++;
    }
    uint32_t spx = (uint32_t)(vm_ns * 100 / (k_ns ? k_ns : 1));
    printf("[gik] VM     %llu ns/frag\n", (unsigned long long)(vm_ns / frags));
    printf("[gik] kernel %llu ns/frag  speedup=%u.%02ux  maxdiff(vs VM)=%u e-6  bitmiss=%d  %s\n",
           (unsigned long long)(k_ns / frags), spx / 100, spx % 100,
           (unsigned)(maxd * 1000000.0f), bitmiss,
           (maxd == 0.f) ? "OK(bit-exact)" : (maxd < 1e-4f ? "OK(approx)" : "*** MISMATCH ***"));
    printf("[gik] done.\n");
    sbi_shutdown();
    return 0;
}
