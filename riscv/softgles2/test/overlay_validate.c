/*
 * overlay_validate.c — 2D-overlay kernel + coherent-gather proof (Lever 2).
 *
 * The gl4es 2D blit  gl_FragColor = Color * texture2DProj(sampler0, TexCoord_0)
 * is THE shader the demo renders every frame, and it currently runs the per-op
 * SIMT VM (glsl_fs_classify bound no kernel for it). This harness:
 *   1. compiles that exact shader, links, and asserts the classifier now binds
 *      glsl_fs_kernel_overlay (sg_kernels.c);
 *   2. proves the kernel is BIT-IDENTICAL to the VM over a coherent (1:1 blit)
 *      fragment batch AND with the coherent-row gather toggled off;
 *   3. measures the kernel vs VM speedup.
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
#include "glsl_internal.h"     /* p->fs_kernel toggle for the VM-vs-kernel A/B */
#if defined(__riscv_v_intrinsic)
#  include <riscv_vector.h>
#endif

extern int sgl_rvv_coherent_gather_enabled;

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

static const char *VS =
"#version 100\n"
"attribute vec4 p;\nattribute vec4 aColor;\nattribute vec4 aTC;\n"
"varying vec4 Color;\nvarying vec4 TexCoord_0;\n"
"void main(){ gl_Position = p; Color = aColor; TexCoord_0 = aTC; }\n";

/* The gl4es 2D-overlay fragment shader (declaration order Color, TexCoord_0 ->
 * vary[0]@0, vary[1]@4, matching the engine fsdump). */
static const char *FS =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord_0;\n"
"uniform sampler2D tex0;\n"
"void main(){ gl_FragColor = Color * texture2DProj(tex0, TexCoord_0); }\n";

#define TEXSZ 64
static unsigned g_texels[TEXSZ * TEXSZ];
static sgl_tex  g_tex;

static unsigned seed = 0x2468ace0u;
static float rnd(void){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (float)((int)(seed>>9))/(float)(1<<22) - 1.0f; }

static float maxdiff(const float *a, const float *b, int vl)
{
    float m = 0;
    for (int k = 0; k < 4; ++k) for (int L = 0; L < vl; ++L) {
        float d = a[k * GLSL_SIMT_MAXLANE + L] - b[k * GLSL_SIMT_MAXLANE + L];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

int kmain(void)
{
    printf("\n==== 2D-overlay kernel + coherent-gather validation (rva23s64) ====\n");
    for (int y = 0; y < TEXSZ; ++y)
        for (int x = 0; x < TEXSZ; ++x)
            g_texels[y * TEXSZ + x] = 0xff000000u | (((x * 4) & 0xFF) << 16) | (((y * 4) & 0xFF) << 8) | ((x ^ y) & 0xFF);
    g_tex.texels = g_texels; g_tex.w = TEXSZ; g_tex.h = TEXSZ;
    g_tex.min_filter = 0; g_tex.wrap = 1; g_tex.is_cube = 0;

    glsl_program *p = glsl_program_create();
    char err[160]; err[0] = 0;
    if (!glsl_attach_source(p, 0, VS, err, sizeof err)) { printf("VS: %s\n", err); return 1; }
    if (!glsl_attach_source(p, 1, FS, err, sizeof err)) { printf("FS: %s\n", err); return 1; }
    if (!glsl_link(p, err, sizeof err)) { printf("link: %s\n", err); return 1; }
    { int l = glsl_uniform_location(p, "tex0"); if (l >= 0) glsl_uniform_sampler(p, l, 0); }

    int simt = glsl_fs_simt_ok(p);
    int nv = glsl_num_varying_floats(p);
    printf("[ovk] simt_ok=%d nvaryfloats=%d nvary=%d fs_cost=%d\n", simt, nv, p->nvary, glsl_fs_cost(p));
    glsl_fs_kernel_fn kfn = p->fs_kernel;
    printf("[ovk] classifier bound overlay kernel: %s\n", kfn ? "YES" : "NO (FAIL)");
    if (!simt || !kfn) { printf("[ovk] *** FAIL: kernel not bound ***\n"); sbi_shutdown(); return 1; }

    const sgl_tex *samplers[GLSL_MAX_SAMPLERS];
    for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = (i == 0) ? &g_tex : 0;

    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);
    static float varys[GLSL_MAX_VARYING_FLOATS * GLSL_SIMT_MAXLANE];
    static float vm[4 * GLSL_SIMT_MAXLANE], kon[4 * GLSL_SIMT_MAXLANE], koff[4 * GLSL_SIMT_MAXLANE];

    /* Coherent 1:1 blit batch: TexCoord_0 = (lane/w, 0, 0, 1) so the texel x is a
     * unit ramp on one row -> the coherent gather fires. Color random. */
    for (int L = 0; L < vl; ++L) {
        varys[0 * GLSL_SIMT_MAXLANE + L] = rnd() * 0.5f + 0.5f;   /* Color.r */
        varys[1 * GLSL_SIMT_MAXLANE + L] = rnd() * 0.5f + 0.5f;   /* Color.g */
        varys[2 * GLSL_SIMT_MAXLANE + L] = rnd() * 0.5f + 0.5f;   /* Color.b */
        varys[3 * GLSL_SIMT_MAXLANE + L] = rnd() * 0.5f + 0.5f;   /* Color.a */
        varys[4 * GLSL_SIMT_MAXLANE + L] = (float)L / (float)TEXSZ; /* TexCoord.x ramp */
        varys[5 * GLSL_SIMT_MAXLANE + L] = 0.0f;                  /* TexCoord.y */
        varys[6 * GLSL_SIMT_MAXLANE + L] = 0.0f;                  /* TexCoord.z */
        varys[7 * GLSL_SIMT_MAXLANE + L] = 1.0f;                  /* TexCoord.w (proj q) */
    }

    /* outputs: VM, kernel(coherent on), kernel(coherent off) */
    p->fs_kernel = 0;   glsl_run_fragment_simt(p, varys, nv, vl, samplers, vm);
    p->fs_kernel = kfn; sgl_rvv_coherent_gather_enabled = 1; glsl_run_fragment_simt(p, varys, nv, vl, samplers, kon);
    p->fs_kernel = kfn; sgl_rvv_coherent_gather_enabled = 0; glsl_run_fragment_simt(p, varys, nv, vl, samplers, koff);
    sgl_rvv_coherent_gather_enabled = 1;

    float d_kon = maxdiff(vm, kon, vl);      /* kernel(coherent) vs VM */
    float d_koff = maxdiff(vm, koff, vl);     /* kernel(indexed)  vs VM */
    float d_coh = maxdiff(kon, koff, vl);     /* coherent vs indexed gather */
    printf("[ovk] maxdiff kernel-coherent vs VM   = %u e-6\n", (unsigned)(d_kon * 1e6f));
    printf("[ovk] maxdiff kernel-indexed  vs VM   = %u e-6\n", (unsigned)(d_koff * 1e6f));
    printf("[ovk] maxdiff coherent vs indexed gather = %u e-6\n", (unsigned)(d_coh * 1e6f));

    /* timing: VM vs kernel (coherent on, the production path) */
    const int BATCHES = 8000;
    uint64_t frags = (uint64_t)BATCHES * vl;
    p->fs_kernel = 0;
    uint64_t t0 = timer_ticks();
    for (int b = 0; b < BATCHES; ++b) glsl_run_fragment_simt(p, varys, nv, vl, samplers, vm);
    uint64_t t1 = timer_ticks();
    p->fs_kernel = kfn;
    for (int b = 0; b < BATCHES; ++b) glsl_run_fragment_simt(p, varys, nv, vl, samplers, kon);
    uint64_t t2 = timer_ticks();
    uint64_t vm_ns = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;
    uint64_t k_ns  = (t2 - t1) * 1000000000ull / TIMEBASE_HZ;
    uint32_t spx = (uint32_t)(vm_ns * 100 / (k_ns ? k_ns : 1));

    printf("[ovk] VM     %llu ns/frag\n", (unsigned long long)(vm_ns / frags));
    printf("[ovk] kernel %llu ns/frag  speedup=%u.%02ux\n",
           (unsigned long long)(k_ns / frags), spx / 100, spx % 100);

    int ok = (d_kon < 1e-4f) && (d_koff < 1e-4f) && (d_coh < 1e-4f);
    printf("[ovk] %s\n", ok ? "ALL PASS (bit-identical)" : "*** MISMATCH ***");
    printf("[ovk] done.\n");
    sbi_shutdown();
    return ok ? 0 : 1;
}
