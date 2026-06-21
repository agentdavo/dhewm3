/*
 * fp16_bench — the GeForce 6 "fp16 register sweet spot" lesson, on RVV.
 *
 * Shades the gl4es 2D-overlay archetype  out = Color * texture2DProj(s, tc)
 * — the one shader that actually renders in the engine — two ways over an
 * identical fragment set:
 *
 *   fp32 path:  e32m2, 64 lanes/dispatch, colour math in fp32 (today's path)
 *   fp16 path:  e16m2, 128 lanes/dispatch, fp32 texcoord addressing but fp16
 *               colour unpack + arithmetic (new; sg_math.h sgl_rvv_*_hf)
 *
 * Reports per-fragment ns for each (-> speedup) and the max absolute colour
 * difference. Output is quantised to 8-bit, so any error below 1/256 ~= 0.0039
 * is invisible on screen; GLSL ES `precision mediump float` licenses fp16 here.
 *
 * Build/run:  make -C riscv/softgles2 fp16run
 */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sg_core.h"
#include "sg_math.h"

#if !defined(__riscv_zvfh)
int kmain(void){ printf("[fp16] zvfh not available in this build\n"); return 0; }
#else

#define NFRAG  (128 * 64)      /* 8192 fragments = 64 fp16 batches / 128 fp32 batches */
#define ITERS  64              /* repeat for a stable timer reading */
#define TW     64
#define TH     64

static uint32_t g_texels[TW * TH];
static sgl_tex  g_tex;

/* fragment inputs (proj texcoord u,v,q + vertex colour rgba), SoA fp32 */
static float U[NFRAG], V[NFRAG], Q[NFRAG];
static float CR[NFRAG], CG[NFRAG], CB[NFRAG], CA[NFRAG];
/* outputs */
static float R32[NFRAG], G32[NFRAG], B32[NFRAG], A32[NFRAG];
static float R16[NFRAG], G16[NFRAG], B16[NFRAG], A16[NFRAG];

static void build_inputs(void)
{
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            unsigned char r = (unsigned char)(x * 4), g = (unsigned char)(y * 4);
            unsigned char b = (unsigned char)((x ^ y) * 3), a = 255;
            g_texels[y * TW + x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    g_tex.texels = g_texels; g_tex.w = TW; g_tex.h = TH;
    g_tex.min_filter = 0; g_tex.wrap = 1; g_tex.is_cube = 0;

    /* deterministic pseudo-random-ish coords/colours spread across the texture,
     * with a projective q != 1 so the divide is exercised. */
    uint32_t s = 0x1234567u;
    for (int i = 0; i < NFRAG; ++i) {
        s = s * 1664525u + 1013904223u;
        float fu = (float)((s >> 8) & 0xFFFF) / 65535.f;
        s = s * 1664525u + 1013904223u;
        float fv = (float)((s >> 8) & 0xFFFF) / 65535.f;
        s = s * 1664525u + 1013904223u;
        float fq = 0.5f + (float)((s >> 8) & 0xFF) / 255.f;   /* 0.5..1.5 */
        U[i] = fu * fq; V[i] = fv * fq; Q[i] = fq;            /* pre-multiplied like a proj coord */
        CR[i] = 0.3f + 0.7f * fu; CG[i] = 0.2f + 0.6f * fv;
        CB[i] = 0.4f + 0.5f * fu * fv; CA[i] = 1.0f;
    }
}

/* ---- fp32 reference path: 64 lanes (e32m2), colour math in fp32 ---- */
static void shade_fp32(void)
{
    const float inv255 = 1.f / 255.f;
    for (int base = 0; base < NFRAG; ) {
        size_t vl = __riscv_vsetvl_e32m2((size_t)(NFRAG - base));
        vfloat32m2_t u = __riscv_vle32_v_f32m2(&U[base], vl);
        vfloat32m2_t v = __riscv_vle32_v_f32m2(&V[base], vl);
        vfloat32m2_t q = __riscv_vle32_v_f32m2(&Q[base], vl);
        u = __riscv_vfdiv_vv_f32m2(u, q, vl);
        v = __riscv_vfdiv_vv_f32m2(v, q, vl);
        vfloat32m2_t tr, tg, tb, ta;
        sgl_rvv_sample2d_nearest_m2(&g_tex, u, v, vl, inv255, g_tex.wrap, &tr, &tg, &tb, &ta);
        vfloat32m2_t o0 = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&CR[base], vl), tr, vl);
        vfloat32m2_t o1 = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&CG[base], vl), tg, vl);
        vfloat32m2_t o2 = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&CB[base], vl), tb, vl);
        vfloat32m2_t o3 = __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&CA[base], vl), ta, vl);
        __riscv_vse32_v_f32m2(&R32[base], o0, vl);
        __riscv_vse32_v_f32m2(&G32[base], o1, vl);
        __riscv_vse32_v_f32m2(&B32[base], o2, vl);
        __riscv_vse32_v_f32m2(&A32[base], o3, vl);
        base += (int)vl;
    }
}

/* ---- fp16 path: 128 lanes (e16m2), fp32 addressing + fp16 colour ---- */
static void shade_fp16(void)
{
    for (int base = 0; base < NFRAG; ) {
        size_t vl = __riscv_vsetvl_e16m2((size_t)(NFRAG - base));   /* up to 128 */
        vfloat32m4_t u = __riscv_vle32_v_f32m4(&U[base], vl);
        vfloat32m4_t v = __riscv_vle32_v_f32m4(&V[base], vl);
        vfloat32m4_t q = __riscv_vle32_v_f32m4(&Q[base], vl);
        u = __riscv_vfdiv_vv_f32m4(u, q, vl);
        v = __riscv_vfdiv_vv_f32m4(v, q, vl);
        vfloat16m2_t tr, tg, tb, ta;
        /* call the nearest helper directly (no runtime filter dispatch) so the
         * disassembly reflects exactly the executed path — the raster knows the
         * filter at draw time and would bind a filter-specialised kernel. */
        sgl_rvv_sample2d_nearest_hf(&g_tex, u, v, vl, g_tex.wrap, &tr, &tg, &tb, &ta);
        vfloat16m2_t cr = __riscv_vfncvt_f_f_w_f16m2(__riscv_vle32_v_f32m4(&CR[base], vl), vl);
        vfloat16m2_t cg = __riscv_vfncvt_f_f_w_f16m2(__riscv_vle32_v_f32m4(&CG[base], vl), vl);
        vfloat16m2_t cb = __riscv_vfncvt_f_f_w_f16m2(__riscv_vle32_v_f32m4(&CB[base], vl), vl);
        vfloat16m2_t ca = __riscv_vfncvt_f_f_w_f16m2(__riscv_vle32_v_f32m4(&CA[base], vl), vl);
        vfloat16m2_t o0 = __riscv_vfmul_vv_f16m2(cr, tr, vl);
        vfloat16m2_t o1 = __riscv_vfmul_vv_f16m2(cg, tg, vl);
        vfloat16m2_t o2 = __riscv_vfmul_vv_f16m2(cb, tb, vl);
        vfloat16m2_t o3 = __riscv_vfmul_vv_f16m2(ca, ta, vl);
        __riscv_vse32_v_f32m4(&R16[base], __riscv_vfwcvt_f_f_v_f32m4(o0, vl), vl);
        __riscv_vse32_v_f32m4(&G16[base], __riscv_vfwcvt_f_f_v_f32m4(o1, vl), vl);
        __riscv_vse32_v_f32m4(&B16[base], __riscv_vfwcvt_f_f_v_f32m4(o2, vl), vl);
        __riscv_vse32_v_f32m4(&A16[base], __riscv_vfwcvt_f_f_v_f32m4(o3, vl), vl);
        base += (int)vl;
    }
}

static float fabsf_(float x) { return x < 0.f ? -x : x; }

int kmain(void)
{
    printf("\n== softgles2 fp16 (zvfh) shading bench — 2D overlay archetype ==\n");
    build_inputs();

    /* warm + correctness */
    shade_fp32();
    shade_fp16();
    float maxerr = 0.f; int worst = 0;
    for (int i = 0; i < NFRAG; ++i) {
        float e;
        e = fabsf_(R16[i] - R32[i]); if (e > maxerr) { maxerr = e; worst = i; }
        e = fabsf_(G16[i] - G32[i]); if (e > maxerr) { maxerr = e; worst = i; }
        e = fabsf_(B16[i] - B32[i]); if (e > maxerr) { maxerr = e; worst = i; }
        e = fabsf_(A16[i] - A32[i]); if (e > maxerr) { maxerr = e; worst = i; }
    }

    uint64_t t0 = timer_ticks();
    for (int it = 0; it < ITERS; ++it) shade_fp32();
    uint64_t t1 = timer_ticks();
    for (int it = 0; it < ITERS; ++it) shade_fp16();
    uint64_t t2 = timer_ticks();

    uint64_t ns32 = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;
    uint64_t ns16 = (t2 - t1) * 1000000000ull / TIMEBASE_HZ;
    uint64_t frags = (uint64_t)NFRAG * ITERS;

    printf("[fp16] %d fragments x %d iters, %dx%d %s texture, vlen=1024\n",
           NFRAG, ITERS, TW, TH, g_tex.min_filter ? "bilinear" : "nearest");
    printf("[fp16] fp32 (64-lane e32m2): %lu ns total, %lu ps/frag\n",
           (unsigned long)ns32, (unsigned long)((t1 - t0) * 1000000000000ull / TIMEBASE_HZ / frags));
    printf("[fp16] fp16 (128-lane e16m2): %lu ns total, %lu ps/frag\n",
           (unsigned long)ns16, (unsigned long)((t2 - t1) * 1000000000000ull / TIMEBASE_HZ / frags));
    if (ns16) printf("[fp16] speedup = %lu.%02lux\n",
                     (unsigned long)(ns32 / ns16), (unsigned long)((ns32 * 100 / ns16) % 100));
    /* maxerr * 255 = error in 8-bit levels; < 0.5 means quantises identically */
    printf("[fp16] max abs colour error = %d / 100000  (= %d/255 levels, worst frag %d)\n",
           (int)(maxerr * 100000.f + 0.5f), (int)(maxerr * 255.f + 0.5f), worst);
    printf("[fp16] correctness: %s (mediump tolerance = 1/255 = 0.0039)\n",
           maxerr < (1.f / 255.f) ? "PASS (sub-quantum)" : (maxerr < 2.f / 255.f ? "OK (<=1 level)" : "CHECK"));

    printf("[ok] fp16 bench complete; powering off.\n");
    return 0;
}
#endif
