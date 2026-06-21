/*
 * frame_2048_bench.c -- bare-metal RVV throughput bench for a synthetic
 * 2048x2048 shader frame.
 *
 * This intentionally does not validate correctness. The validation gates cover
 * bit/epsilon behavior; this file measures full-frame shader throughput:
 *   - fragment SIMT: 2048*2048 pixels, reported as ms/frame and ns/pixel.
 *   - vertex SIMT:   2048*2048 vertices, reported as ms/pass and ns/vertex.
 */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "glsl.h"
#include "sg_layout.h"
#include <riscv_vector.h>

#define FRAME_W 2048
#define FRAME_H 2048
#define FRAME_PIXELS ((uint64_t)FRAME_W * (uint64_t)FRAME_H)
#ifndef FRAME_BENCH_MODE
#define FRAME_BENCH_MODE "all"
#endif

int sgl_diag_enabled = 0;

void sgl_sample(const sgl_tex *t, float u, float v, float out[4])
{
    (void)t;
    out[0] = u - (float)(int)u;
    out[1] = v - (float)(int)v;
    out[2] = 0.5f * (out[0] + out[1]);
    out[3] = 1.0f;
}

static const char *VS_FRAME =
"#version 100\n"
"attribute vec4 pos;\nattribute vec4 col;\nattribute vec4 uv;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform mat4 mvp;\n"
"void main(){ gl_Position = mvp * pos; Color = col; TexCoord0 = uv; }\n";

static const char *FS_FRAME =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  vec3 n = normalize(vec3(Color.x, Color.y, 1.0));\n"
"  vec3 l = normalize(vec3(TexCoord0.x, TexCoord0.y, 1.0));\n"
"  float d = max(dot(n, l), 0.0);\n"
"  float s = d * d;\n"
"  gl_FragColor = clamp(Color * lightColor * (d + s), 0.0, 1.0);\n"
"}\n";

static const char *FS_TEX =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform sampler2D tex;\nuniform vec4 lightColor;\n"
"void main(){\n"
"  vec4 base = texture2D(tex, TexCoord0.xy);\n"
"  gl_FragColor = clamp(base * Color * lightColor, 0.0, 1.0);\n"
"}\n";

static const char *FS_BLEND =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  vec4 src = clamp(Color * lightColor, 0.0, 1.0);\n"
"  vec4 dst = vec4(TexCoord0.y, TexCoord0.x, 0.25, 1.0);\n"
"  float a = clamp(src.a * 0.65 + 0.15, 0.0, 1.0);\n"
"  gl_FragColor = src * a + dst * (1.0 - a);\n"
"}\n";

static const char *FS_DISCARD =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  float m = fract(TexCoord0.x * 17.0 + TexCoord0.y * 31.0);\n"
"  float keep = step(0.35, m);\n"
"  gl_FragColor = clamp(Color * lightColor * (0.5 + m) * keep, 0.0, 1.0);\n"
"}\n";

static const char *FS_DEPTH =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  float z = TexCoord0.x * 0.63 + TexCoord0.y * 0.37;\n"
"  float pass = step(0.42, z) * step(z, 0.88);\n"
"  vec4 shaded = clamp(Color * lightColor * (0.25 + z), 0.0, 1.0);\n"
"  gl_FragColor = mix(vec4(0.0), shaded, pass);\n"
"}\n";

static const char *FS_STENCIL =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  float sx = floor(TexCoord0.x * 16.0);\n"
"  float sy = floor(TexCoord0.y * 16.0);\n"
"  float bit = fract((sx + sy * 3.0) * 0.5) * 2.0;\n"
"  float pass = step(1.0, bit);\n"
"  vec4 shaded = clamp(Color * lightColor, 0.0, 1.0);\n"
"  gl_FragColor = mix(vec4(0.0), shaded, pass);\n"
"}\n";

static const char *FS_COLORMASK =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  vec4 shaded = clamp(Color * lightColor, 0.0, 1.0);\n"
"  gl_FragColor = vec4(shaded.r, 0.0, shaded.b, 1.0);\n"
"}\n";

static glsl_program *build_program(const char *fs)
{
    glsl_program *p = glsl_program_create();
    char err[160]; err[0] = 0;
    if (!glsl_attach_source(p, 0, VS_FRAME, err, sizeof err)) {
        printf("[frame2048] VS fail: %s\n", err);
        return 0;
    }
    if (!glsl_attach_source(p, 1, fs, err, sizeof err)) {
        printf("[frame2048] FS fail: %s\n", err);
        return 0;
    }
    if (!glsl_link(p, err, sizeof err)) {
        printf("[frame2048] link fail: %s\n", err);
        return 0;
    }

    int umvp = glsl_uniform_location(p, "mvp");
    if (umvp >= 0) {
        float m[16] = {
            1.5f, 0.2f, -0.3f, 0.0f,
            0.1f, 0.9f,  0.4f, 0.0f,
           -0.2f, 0.3f,  1.1f, 0.0f,
            0.7f,-0.5f,  0.6f, 1.0f
        };
        glsl_uniform(p, umvp, m, 16);
    }
    int ulc = glsl_uniform_location(p, "lightColor");
    if (ulc >= 0) {
        float c[4] = { 1.0f, 0.95f, 0.85f, 1.0f };
        glsl_uniform(p, ulc, c, 4);
    }
    int utex = glsl_uniform_location(p, "tex");
    if (utex >= 0) glsl_uniform_sampler(p, utex, 0);
    return p;
}

static void fill_fragment_varys(float *varys, int nv, int vl)
{
    for (int f = 0; f < nv; ++f) {
        for (int lane = 0; lane < vl; ++lane) {
            unsigned h = (unsigned)(f * 131u + lane * 977u + 17u);
            h ^= h >> 13;
            h *= 0x85ebca6bu;
            varys[f * GLSL_SIMT_MAXLANE + lane] =
                0.25f + (float)(h & 1023u) * (0.5f / 1023.0f);
        }
    }
}

static void fill_vertex_attrs(float *attrs, int vl)
{
    for (int loc = 0; loc < GLSL_MAX_ATTRIBS; ++loc) {
        for (int c = 0; c < 4; ++c) {
            for (int lane = 0; lane < vl; ++lane) {
                unsigned h = (unsigned)(loc * 1009u + c * 257u + lane * 33u + 5u);
                h ^= h >> 15;
                h *= 0x9e3779b9u;
                attrs[(loc * 4 + c) * GLSL_SIMT_MAXLANE + lane] =
                    -1.0f + (float)(h & 2047u) * (2.0f / 2047.0f);
            }
        }
    }
}

static uint64_t ticks_to_ns(uint64_t ticks)
{
    return ticks * 1000000000ull / TIMEBASE_HZ;
}

static int streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static void bench_vertex(glsl_program *p, int vl, const float *attrs,
                         float *clip, float *vert_vary)
{
    if (!glsl_vs_simt_ok(p)) {
        printf("[frame2048] vertex SIMT_OK=no\n");
        return;
    }

    uint64_t verts = FRAME_PIXELS;
    uint64_t batches = (verts + (uint64_t)vl - 1ull) / (uint64_t)vl;
    uint64_t t0 = timer_ticks();
    for (uint64_t b = 0; b < batches; ++b) {
        int lanes = (int)(verts - b * (uint64_t)vl);
        if (lanes > vl) lanes = vl;
        glsl_run_vertex_simt(p, attrs, lanes, clip, vert_vary);
        __asm__ volatile("" : : "r"(clip[0]) : "memory");
    }
    uint64_t ns = ticks_to_ns(timer_ticks() - t0);
    uint32_t ns_vert = (uint32_t)(ns / verts);
    printf("[frame2048] vertex   vl=%d verts=%u ms=%u ns/vertex=%u\n",
           vl, (unsigned)verts, (unsigned)(ns / 1000000ull), ns_vert);
}

static void bench_fragment_passes(const char *name, glsl_program *p, int vl,
                                  const float *varys, int nv,
                                  const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                  float *frag_out, int passes)
{
    if (!glsl_fs_simt_ok(p)) {
        printf("[frame2048] fragment %-11s SIMT_OK=no\n", name);
        return;
    }

    uint64_t batches = (FRAME_PIXELS + (uint64_t)vl - 1ull) / (uint64_t)vl;
    uint64_t t0 = timer_ticks();
    for (int pass = 0; pass < passes; ++pass) {
        for (uint64_t b = 0; b < batches; ++b) {
            int lanes = (int)(FRAME_PIXELS - b * (uint64_t)vl);
            if (lanes > vl) lanes = vl;
            glsl_run_fragment_simt(p, varys, nv, lanes, samplers, frag_out);
            __asm__ volatile("" : : "r"(frag_out[0]) : "memory");
        }
    }
    uint64_t ns = ticks_to_ns(timer_ticks() - t0);
    uint64_t samples = FRAME_PIXELS * (uint64_t)passes;
    uint32_t ns_pix = (uint32_t)(ns / samples);
    printf("[frame2048] fragment %-11s vl=%d pixels=%u passes=%u ms=%u ns/sample=%u\n",
           name, vl, (unsigned)FRAME_PIXELS, (unsigned)passes,
           (unsigned)(ns / 1000000ull), ns_pix);
}

static void bench_fragment(const char *name, glsl_program *p, int vl,
                           const float *varys, int nv,
                           const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                           float *frag_out)
{
    bench_fragment_passes(name, p, vl, varys, nv, samplers, frag_out, 1);
}

static int mode_is_overdraw(const char *mode, int *passes)
{
    if (streq(mode, "overdraw1")) { *passes = 1; return 1; }
    if (streq(mode, "overdraw2")) { *passes = 2; return 1; }
    if (streq(mode, "overdraw4")) { *passes = 4; return 1; }
    if (streq(mode, "overdraw8")) { *passes = 8; return 1; }
    return 0;
}

static int texture_mode(const char *mode, int *size, int *filter, int *wrap)
{
    if (streq(mode, "tex64_clamp"))       { *size = 64;   *filter = 0; *wrap = 0; return 1; }
    if (streq(mode, "tex64_repeat"))      { *size = 64;   *filter = 0; *wrap = 1; return 1; }
    if (streq(mode, "tex512_clamp"))      { *size = 512;  *filter = 0; *wrap = 0; return 1; }
    if (streq(mode, "tex512_repeat"))     { *size = 512;  *filter = 0; *wrap = 1; return 1; }
    if (streq(mode, "tex2048_clamp"))     { *size = 2048; *filter = 0; *wrap = 0; return 1; }
    if (streq(mode, "tex2048_repeat"))    { *size = 2048; *filter = 0; *wrap = 1; return 1; }
    if (streq(mode, "tex64_bilinear"))    { *size = 64;   *filter = 1; *wrap = 1; return 1; }
    if (streq(mode, "tex512_bilinear"))   { *size = 512;  *filter = 1; *wrap = 1; return 1; }
    if (streq(mode, "tex2048_bilinear"))  { *size = 2048; *filter = 1; *wrap = 1; return 1; }
    return 0;
}

static void init_texels(unsigned *texels)
{
    for (int i = 0; i < 64 * 64; ++i) {
        unsigned x = (unsigned)(i & 63);
        unsigned y = (unsigned)(i >> 6);
        texels[i] = 0xff000000u | ((x * 4u) << 16) | ((y * 4u) << 8) | ((x ^ y) * 4u);
    }
}

static void setup_tex(sgl_tex *tex, unsigned *texels, int size, int filter, int wrap)
{
    tex->w = size;
    tex->h = size;
    tex->texels = texels;
    tex->min_filter = filter;
    tex->wrap = wrap;
}

int kmain(void)
{
    printf("\n==== 2048x2048 shader-frame bench (rva23s64) ====\n");
    glsl_program *arith = build_program(FS_FRAME);
    glsl_program *tex_nearest = build_program(FS_TEX);
    glsl_program *tex_bilinear = build_program(FS_TEX);
    glsl_program *blend = build_program(FS_BLEND);
    glsl_program *discardp = build_program(FS_DISCARD);
    glsl_program *depth = build_program(FS_DEPTH);
    glsl_program *stencil = build_program(FS_STENCIL);
    glsl_program *colormask = build_program(FS_COLORMASK);
    if (!arith || !tex_nearest || !tex_bilinear || !blend || !discardp ||
        !depth || !stencil || !colormask) {
        sbi_shutdown();
        return 1;
    }

    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);
    int nv = glsl_num_varying_floats(arith);
    if (nv < 4) nv = 4;
    if (nv > GLSL_MAX_VARYING_FLOATS) nv = GLSL_MAX_VARYING_FLOATS;

    static sg_vary_soa varys_soa;
    static sg_fragout_soa frag_out_soa;   /* 5 rows: RGBA + discard/kill row (SIMT alpha-test) */
    static float attrs[GLSL_MAX_ATTRIBS * 4 * GLSL_SIMT_MAXLANE] SG_ALIGNED_VAR(SG_RVV_ALIGN);
    static sg_vec4_soa clip_soa;
    static sg_vary_soa vert_vary_soa;
    float *varys = &varys_soa.c[0][0];
    float *frag_out = &frag_out_soa.c[0][0];
    float *clip = &clip_soa.c[0][0];
    float *vert_vary = &vert_vary_soa.c[0][0];
    static unsigned texels[64 * 64];
    static sgl_tex nearest_tex;
    static sgl_tex bilinear_tex;
    static sgl_tex mode_tex;
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS];
    init_texels(texels);
    setup_tex(&nearest_tex, texels, 64, 0, 1);
    bilinear_tex = nearest_tex;
    bilinear_tex.min_filter = 1;

    fill_fragment_varys(varys, nv, vl);
    fill_vertex_attrs(attrs, vl);

    const char *mode = FRAME_BENCH_MODE;
    int passes = 1;
    int tex_size = 64;
    int tex_filter = 0;
    int tex_wrap = 1;

    if (streq(mode, "all") || streq(mode, "vertex"))
        bench_vertex(arith, vl, attrs, clip, vert_vary);
    if (streq(mode, "all") || streq(mode, "arith")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        bench_fragment("arith", arith, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "tex_nearest")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        samplers[0] = &nearest_tex;
        bench_fragment("tex_nearest", tex_nearest, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "tex_bilinear")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        samplers[0] = &bilinear_tex;
        bench_fragment("tex_bilinear", tex_bilinear, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || mode_is_overdraw(mode, &passes)) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        if (streq(mode, "all")) passes = 4;
        bench_fragment_passes("overdraw", arith, vl, varys, nv, samplers, frag_out, passes);
    }
    if (texture_mode(mode, &tex_size, &tex_filter, &tex_wrap)) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        setup_tex(&mode_tex, texels, tex_size, tex_filter, tex_wrap);
        samplers[0] = &mode_tex;
        bench_fragment(mode, tex_filter ? tex_bilinear : tex_nearest, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "blend")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        bench_fragment("blend", blend, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "discard")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        bench_fragment("discard", discardp, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "depth")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        bench_fragment("depth", depth, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "stencil")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        bench_fragment("stencil", stencil, vl, varys, nv, samplers, frag_out);
    }
    if (streq(mode, "all") || streq(mode, "colormask")) {
        for (int i = 0; i < GLSL_MAX_SAMPLERS; ++i) samplers[i] = 0;
        bench_fragment("colormask", colormask, vl, varys, nv, samplers, frag_out);
    }

    printf("[frame2048] done.\n");
    sbi_shutdown();
    return 0;
}
