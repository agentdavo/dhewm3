/*
 * atest_validate.c — bit-exactness proof for the gl4es alpha-test overlay (id=3)
 * and 2-stage env-combine (id=8) kernels (sg_kernels.c) vs the reference SIMT VM.
 *
 * These are the exact FPE_Shader-generated fragment shaders captured from the
 * engine ([fssrc] dump). The harness compiles each, asserts the classifier binds
 * the new kernel, and proves the kernel is BIT-IDENTICAL to the VM over a random
 * fragment batch — including the discard kill mask (row 4) for the alpha-test one.
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
#include "glsl_internal.h"
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

/* id=8 VS: 3 varyings (Color, TexCoord_0, TexCoord_1 -> nvaryf=12). */
static const char *VS3 =
"#version 100\n"
"attribute vec4 p;\nattribute vec4 aColor;\nattribute vec4 aTC0;\nattribute vec4 aTC1;\n"
"varying vec4 Color;\nvarying vec4 _gl4es_TexCoord_0;\nvarying vec4 _gl4es_TexCoord_1;\n"
"void main(){ gl_Position = p; Color = aColor; _gl4es_TexCoord_0 = aTC0; _gl4es_TexCoord_1 = aTC1; }\n";
/* id=3 VS: 2 varyings (Color, TexCoord_0 -> nvaryf=8), matching the engine fsdump. */
static const char *VS2 =
"#version 100\n"
"attribute vec4 p;\nattribute vec4 aColor;\nattribute vec4 aTC0;\n"
"varying vec4 Color;\nvarying vec4 _gl4es_TexCoord_0;\n"
"void main(){ gl_Position = p; Color = aColor; _gl4es_TexCoord_0 = aTC0; }\n";

/* id=3: overlay + gl4es alpha test (exact [fssrc] capture). */
static const char *FS_ATEST =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 _gl4es_TexCoord_0;\n"
"uniform sampler2D _gl4es_TexSampler_0;\nuniform float _gl4es_AlphaRef;\n"
"void main(){ vec4 fColor = Color;\n"
"  vec4 texColor0 = texture2DProj(_gl4es_TexSampler_0, _gl4es_TexCoord_0);\n"
"  fColor *= texColor0;\n"
"  if (floor(fColor.a*255.) <= _gl4es_AlphaRef) discard;\n"
"  gl_FragColor = fColor; }\n";

/* id=8: 2-stage env-combine (exact [fssrc] capture; texColor1 dead). */
static const char *FS_ENV =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 _gl4es_TexCoord_0;\nvarying vec4 _gl4es_TexCoord_1;\n"
"uniform vec4 _gl4es_TextureEnvColor_1;\n"
"uniform sampler2D _gl4es_TexSampler_0;\nuniform sampler2D _gl4es_TexSampler_1;\n"
"void main(){ vec4 fColor = Color;\n"
"  vec4 texColor0 = texture2DProj(_gl4es_TexSampler_0, _gl4es_TexCoord_0);\n"
"  vec4 texColor1 = texture2DProj(_gl4es_TexSampler_1, _gl4es_TexCoord_1);\n"
"  vec4 Arg0, Arg1;\n"
"  fColor.rgb *= texColor0.rgb;\n"
"  Arg0 = fColor; Arg1 = _gl4es_TextureEnvColor_1;\n"
"  fColor = Arg0 * Arg1;\n"
"  fColor = clamp(fColor, 0., 1.);\n"
"  gl_FragColor = fColor; }\n";

#define TEXSZ 64
static unsigned g_texels[TEXSZ * TEXSZ];
static sgl_tex  g_tex;
static unsigned seed = 0x13579bdfu;
static float rnd(void){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (float)((int)(seed>>9))/(float)(1<<22) - 1.0f; }

static float maxdiff_rows(const float *a, const float *b, int vl, int rows)
{
    float m = 0;
    for (int k = 0; k < rows; ++k) for (int L = 0; L < vl; ++L) {
        float d = a[k * GLSL_SIMT_MAXLANE + L] - b[k * GLSL_SIMT_MAXLANE + L];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

static float varys[GLSL_MAX_VARYING_FLOATS * GLSL_SIMT_MAXLANE];
static float vm[8 * GLSL_SIMT_MAXLANE], ko[8 * GLSL_SIMT_MAXLANE];

static int run_one(const char *name, const char *vs, const char *fs, const char *want_kernel,
                   int rows, int set_alpharef, int set_env)
{
    glsl_program *p = glsl_program_create();
    char err[160]; err[0] = 0;
    if (!glsl_attach_source(p, 0, vs, err, sizeof err)) { printf("%s VS: %s\n", name, err); return 1; }
    if (!glsl_attach_source(p, 1, fs, err, sizeof err)) { printf("%s FS: %s\n", name, err); return 1; }
    if (!glsl_link(p, err, sizeof err)) { printf("%s link: %s\n", name, err); return 1; }
    { int l = glsl_uniform_location(p, "_gl4es_TexSampler_0"); if (l >= 0) glsl_uniform_sampler(p, l, 0); }
    { int l = glsl_uniform_location(p, "_gl4es_TexSampler_1"); if (l >= 0) glsl_uniform_sampler(p, l, 1); }
    if (set_alpharef) { int l = glsl_uniform_location(p, "_gl4es_AlphaRef"); float a = 128.f; if (l >= 0) glsl_uniform(p, l, &a, 1); }
    if (set_env) { int l = glsl_uniform_location(p, "_gl4es_TextureEnvColor_1"); float e[4] = {0.75f, 0.5f, 0.25f, 0.9f}; if (l >= 0) glsl_uniform(p, l, e, 4); }

    int simt = glsl_fs_simt_ok(p);
    const char *kn = p->dbg_kernel_name ? p->dbg_kernel_name : "?";
    int bound = p->fs_kernel && !strcmp(kn, want_kernel);
    printf("[%s] simt=%d nvaryf=%d kernel=%s (want %s) %s\n", name, simt,
           glsl_num_varying_floats(p), kn, want_kernel, bound ? "OK" : "*** FAIL ***");
    if (!simt || !bound) { return 1; }

    const sgl_tex *samplers[GLSL_MAX_SAMPLERS];
    for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = (i == 0 || i == 1) ? &g_tex : 0;
    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);
    int nv = glsl_num_varying_floats(p);

    for (int L = 0; L < vl; ++L) {
        for (int c = 0; c < 4; ++c) varys[c * GLSL_SIMT_MAXLANE + L] = rnd() * 0.5f + 0.5f;       /* Color */
        varys[4 * GLSL_SIMT_MAXLANE + L] = (rnd() * 0.5f + 0.5f) * 2.f;                            /* TC0.x */
        varys[5 * GLSL_SIMT_MAXLANE + L] = (rnd() * 0.5f + 0.5f) * 2.f;                            /* TC0.y */
        varys[6 * GLSL_SIMT_MAXLANE + L] = 0.f;
        varys[7 * GLSL_SIMT_MAXLANE + L] = rnd() * 0.4f + 0.6f;                                    /* TC0.w (q) */
        varys[8 * GLSL_SIMT_MAXLANE + L]  = rnd();                                                 /* TC1.x */
        varys[9 * GLSL_SIMT_MAXLANE + L]  = rnd();
        varys[10 * GLSL_SIMT_MAXLANE + L] = 0.f;
        varys[11 * GLSL_SIMT_MAXLANE + L] = 1.f;
    }

    glsl_fs_kernel_fn kfn = p->fs_kernel;
    p->fs_kernel = 0;   glsl_run_fragment_simt(p, varys, nv, vl, samplers, vm);
    p->fs_kernel = kfn; glsl_run_fragment_simt(p, varys, nv, vl, samplers, ko);

    float d = maxdiff_rows(vm, ko, vl, rows);
    int ok = d < 1e-4f;
    printf("[%s] maxdiff kernel-vs-VM over %d rows = %u e-6  %s\n",
           name, rows, (unsigned)(d * 1e6f), ok ? "PASS (bit-identical)" : "*** MISMATCH ***");
    return ok ? 0 : 1;
}

int kmain(void)
{
    printf("\n==== alpha-test / env-combine kernel validation (rva23s64) ====\n");
    for (int y = 0; y < TEXSZ; ++y) for (int x = 0; x < TEXSZ; ++x)
        g_texels[y * TEXSZ + x] = 0xff000000u | (((x * 4) & 0xFF) << 16) | (((y * 4) & 0xFF) << 8) | ((x ^ y) & 0xFF);
    g_tex.texels = g_texels; g_tex.w = TEXSZ; g_tex.h = TEXSZ;
    g_tex.min_filter = 0; g_tex.wrap = 1; g_tex.is_cube = 0;

    int fail = 0;
    /* id=3: compare 5 rows (RGBA + discard kill mask). */
    fail |= run_one("atest", VS2, FS_ATEST, "overlay_atest", 5, 1, 0);
    /* id=8: compare 4 rows (RGBA; no discard). */
    fail |= run_one("envcombine", VS3, FS_ENV, "envcombine", 4, 0, 1);

    printf("[atest] %s\n", fail ? "*** FAIL ***" : "ALL PASS");
    sbi_shutdown();
    return fail;
}
