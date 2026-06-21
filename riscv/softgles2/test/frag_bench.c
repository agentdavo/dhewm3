/*
 * frag_bench.c — host fragment-shading microbenchmark + IR-vs-oracle validator.
 *
 * The MENU is not fragment-bound, so it's the wrong benchmark for the GLSL
 * interpreter. This times glsl_run_fragment() over N pixels with representative
 * gl4es-style shaders, on the host, so the scalar interpreter optimizations (A1
 * memset, A2 bytecode IR) are measurable in seconds. It also validates the IR
 * path against the tree-walk oracle (glsl_run_fragment_ref) bit-for-bit-ish.
 *
 *   cc -O2 -I../src -I../include frag_bench.c ../src/glsl.c -lm -o /tmp/fragbench
 *   /tmp/fragbench [N] [corpus_fs.glsl ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "glsl.h"

/* Deterministic sampler stub that varies with (u,v) so nothing folds to const. */
void sgl_sample(const sgl_tex *t, float u, float v, float out[4])
{
    (void)t;
    out[0] = u - (float)(int)u;        /* fract(u) */
    out[1] = v - (float)(int)v;
    out[2] = 0.5f * (out[0] + out[1]);
    out[3] = 1.0f;
}

/* tree-walk oracle (added in glsl.c alongside the IR path) */
int glsl_run_fragment_ref(glsl_program *p, const float *varying,
                          const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float rgba[4]);

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- representative inline shaders ---- */
static const char *VS_LIGHT =
"#version 100\n"
"attribute vec4 _gl4es_Vertex;\n"
"varying vec4 Color;\n"
"varying vec4 TexCoord0;\n"
"uniform mat4 mvp;\n"
"void main(){ gl_Position = mvp * _gl4es_Vertex; Color = vec4(1.0); TexCoord0 = vec4(0.0); }\n";
static const char *FS_SIMPLE =
"#version 100\n"
"precision mediump float;\n"
"varying vec4 Color;\n"
"varying vec4 TexCoord0;\n"
"uniform sampler2D tex;\n"
"void main(){ vec4 t = texture2D(tex, TexCoord0.xy); gl_FragColor = Color * t; }\n";

static const char *VS_HEAVY =
"#version 100\n"
"attribute vec4 _gl4es_Vertex;\n"
"varying vec4 Normal;\n"
"varying vec4 LightDir;\n"
"varying vec4 TexCoord0;\n"
"varying vec4 Color;\n"
"uniform mat4 mvp;\n"
"void main(){ gl_Position = mvp * _gl4es_Vertex; Normal=vec4(0.0); LightDir=vec4(0.0); TexCoord0=vec4(0.0); Color=vec4(1.0); }\n";
static const char *FS_HEAVY =
"#version 100\n"
"precision mediump float;\n"
"varying vec4 Normal;\n"
"varying vec4 LightDir;\n"
"varying vec4 TexCoord0;\n"
"varying vec4 Color;\n"
"uniform sampler2D tex;\n"
"uniform vec4 lightColor;\n"
"void main(){\n"
"  vec4 base = texture2D(tex, TexCoord0.xy);\n"
"  vec3 n = normalize(Normal.xyz);\n"
"  vec3 l = normalize(LightDir.xyz);\n"
"  float d = max(dot(n, l), 0.0);\n"
"  float s = pow(d, 16.0);\n"
"  vec4 c = base * Color * lightColor * (d + s);\n"
"  gl_FragColor = clamp(c, 0.0, 1.0);\n"
"}\n";

static glsl_program *build(const char *vs, const char *fs)
{
    glsl_program *p = glsl_program_create();
    char err[256]; err[0] = 0;
    if (!glsl_attach_source(p, 0, vs, err, sizeof err)) { printf("VS fail: %s\n", err); return 0; }
    if (!glsl_attach_source(p, 1, fs, err, sizeof err)) { printf("FS fail: %s\n", err); return 0; }
    if (!glsl_link(p, err, sizeof err)) { printf("link fail: %s\n", err); return 0; }
    return p;
}

/* run N fragments, varying the inputs slightly each iter (defeats const-fold) */
static double bench(glsl_program *p, long N, int use_ir)
{
    int nv = glsl_num_varying_floats(p); if (nv < 4) nv = 4;
    float vary[GLSL_MAX_VARYING_FLOATS];
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS]; for (int i=0;i<GLSL_MAX_SAMPLERS;i++) samplers[i]=(const sgl_tex*)1;
    float rgba[4]; volatile float sink = 0;
    double t0 = now_s();
    for (long i = 0; i < N; ++i) {
        float f = (float)(i & 1023) * (1.0f/1024.0f) + 0.001f;
        for (int k = 0; k < nv; ++k) vary[k] = f * (float)(k+1) - 0.3f;
        if (use_ir) glsl_run_fragment(p, vary, samplers, rgba);
        else        glsl_run_fragment_ref(p, vary, samplers, rgba);
        sink += rgba[0] + rgba[1] + rgba[2] + rgba[3];
    }
    double dt = now_s() - t0;
    (void)sink;
    return dt;
}

/* validate IR == oracle over random-ish inputs; return max abs diff */
static float validate(glsl_program *p, long iters)
{
    int nv = glsl_num_varying_floats(p); if (nv < 4) nv = 4;
    float vary[GLSL_MAX_VARYING_FLOATS];
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS]; for (int i=0;i<GLSL_MAX_SAMPLERS;i++) samplers[i]=(const sgl_tex*)1;
    float maxd = 0; unsigned seed = 12345;
    for (long i = 0; i < iters; ++i) {
        for (int k = 0; k < nv; ++k) { seed = seed*1664525u+1013904223u; vary[k] = ((int)(seed>>9)/(float)(1<<22)) - 1.0f; }
        float a[4], b[4];
        glsl_run_fragment(p, vary, samplers, a);          /* IR (or fallback) */
        glsl_run_fragment_ref(p, vary, samplers, b);      /* oracle */
        float idmax=0; for (int k = 0; k < 4; ++k) { float d = a[k]-b[k]; if (d<0) d=-d; if (d>maxd) maxd=d; if(d>idmax)idmax=d; }
        if (idmax > 1e-4f && i < 3) {
            printf("   MISMATCH @%ld vary[%d]={%.3f %.3f %.3f %.3f ...}\n     IR ={%.4f %.4f %.4f %.4f}\n     REF={%.4f %.4f %.4f %.4f}\n",
                   i, nv, vary[0],vary[1],vary[2],vary[3], a[0],a[1],a[2],a[3], b[0],b[1],b[2],b[3]);
        }
    }
    return maxd;
}

int glsl_fs_has_ir(const glsl_program *p);

static char *readfile(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    char *s=(char*)malloc(n+1); size_t rd=fread(s,1,n,f); s[rd]=0; fclose(f); return s;
}

/* Validate the IR against the oracle on a corpus FS (paired with a minimal VS):
 * randomize uniforms + varyings each iter, compare both paths. */
static void corpus_check(const char *path)
{
    static const char *MINVS = "#version 100\nattribute vec4 p;\nvoid main(){ gl_Position = p; }\n";
    char *fs = readfile(path); if (!fs) { printf("  %-30s CANNOT OPEN\n", path); return; }
    glsl_program *pr = build(MINVS, fs); free(fs);
    if (!pr) { printf("  %-30s (FS did not link)\n", path); return; }
    int hasir = glsl_fs_has_ir(pr);
    int nuni = glsl_num_uniforms(pr);
    float vary[GLSL_MAX_VARYING_FLOATS];
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS]; for (int i=0;i<GLSL_MAX_SAMPLERS;i++) samplers[i]=(const sgl_tex*)1;
    float maxd=0; unsigned seed=99991;
    for (int it=0; it<2000; ++it) {
        for (int k=0;k<GLSL_MAX_VARYING_FLOATS;k++){ seed=seed*1664525u+1013904223u; vary[k]=((int)(seed>>9)/(float)(1<<22))-1.f; }
        for (int u=0;u<nuni;u++){ float d[16]; for(int j=0;j<16;j++){seed=seed*1664525u+1013904223u; d[j]=((int)(seed>>9)/(float)(1<<22))-1.f;} glsl_uniform(pr,u,d,16); }
        float a[4],b[4];
        glsl_run_fragment(pr,vary,samplers,a);
        glsl_run_fragment_ref(pr,vary,samplers,b);
        for(int k=0;k<4;k++){float d=a[k]-b[k]; if(d<0)d=-d; if(d>maxd)maxd=d;}
    }
    printf("  %-30s IR=%s  maxdiff %.2e  %s\n", path, hasir?"yes":"FALLBACK",
           maxd, (maxd<1e-4f)?"OK":"*** MISMATCH ***");
    glsl_program_destroy(pr);
}

int main(int argc, char **argv)
{
    long N = 2000000;
    int firstfile = 1;
    if (argc > 1 && atol(argv[1]) > 0) { N = atol(argv[1]); firstfile = 2; }

    struct { const char *name, *vs, *fs; } shaders[] = {
        { "simple(textured)", VS_LIGHT, FS_SIMPLE },
        { "heavy(lit+pow)",   VS_HEAVY, FS_HEAVY },
    };
    /* set a uniform mvp + lightColor where present (offsets resolved at link) */
    for (int s = 0; s < 2; ++s) {
        glsl_program *p = build(shaders[s].vs, shaders[s].fs);
        if (!p) { printf("%s: build failed\n", shaders[s].name); continue; }
        int umvp = glsl_uniform_location(p, "mvp");
        if (umvp >= 0) { float m[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; glsl_uniform(p, umvp, m, 16); }
        int ulc = glsl_uniform_location(p, "lightColor");
        if (ulc >= 0) { float c[4]={1,1,1,1}; glsl_uniform(p, ulc, c, 4); }
        int utex = glsl_uniform_location(p, "tex"); if (utex>=0) glsl_uniform_sampler(p, utex, 0);

        float md = validate(p, 4000);
        double t_ir  = bench(p, N, 1);
        double t_ref = bench(p, N, 0);
        printf("%-18s  oracle %6.2f ns/frag   path %6.2f ns/frag   speedup %4.2fx   maxdiff %.2e %s\n",
               shaders[s].name,
               t_ref*1e9/N, t_ir*1e9/N, t_ref/t_ir, md,
               md < 1e-4f ? "OK" : "*** MISMATCH ***");
        glsl_program_destroy(p);
    }

    if (argc > firstfile) {
        printf("--- corpus IR-vs-oracle validation ---\n");
        for (int i = firstfile; i < argc; ++i) corpus_check(argv[i]);
    }
    return 0;
}
