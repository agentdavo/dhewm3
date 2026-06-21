/*
 * vtx_validate.c — bare-metal RVV gate for the SIMT vertex (T&L) stage.
 *
 * Analogous to simt_validate.c but for the vertex transform: for representative
 * vertex shaders it (1) VALIDATES glsl_run_vertex_simt (lanes = vertices) bit-
 * against the scalar tree-walk oracle glsl_run_vertex over a batch of VL
 * vertices, and (2) MEASURES scalar-per-vertex vs SIMT ns/vertex via rdtime.
 * The dominant op is gl_Position = MVP * pos, accelerated by the batched
 * mat4*vec4 (16 vfmacc.vf) kernel.
 *
 * Linked with boot + lean libc + glsl.c.
 */
#include "libc/libc.h"
#include "boot/runtime.h"          /* timer_ticks, TIMEBASE_HZ, sbi_shutdown */
#include "glsl.h"
#include "sg_vertex.h"
#include <riscv_vector.h>

int sgl_diag_enabled = 0;

#ifndef VTX_VALIDATE_CORRECT_ITERS
#define VTX_VALIDATE_CORRECT_ITERS 96
#endif
#ifndef VTX_VALIDATE_BENCH_BATCHES
#define VTX_VALIDATE_BENCH_BATCHES 1200
#endif

void sgl_sample(const sgl_tex *t, float u, float v, float out[4])
{ (void)t; out[0]=u; out[1]=v; out[2]=0.5f; out[3]=1.0f; }

/* gl4es-style T&L: gl_Position = MVP * Vertex; pass colour + texcoord varyings. */
static const char *VS_TL =
"#version 100\n"
"attribute vec4 pos;\nattribute vec4 col;\nattribute vec4 uv;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\n"
"uniform mat4 mvp;\n"
"void main(){ gl_Position = mvp * pos; Color = col; TexCoord0 = uv; }\n";
/* heavier VS: a normalize + a second mat*vec into a varying. */
static const char *VS_HEAVY =
"#version 100\n"
"attribute vec4 pos;\nattribute vec4 nrm;\n"
"varying vec4 Color;\nvarying vec4 NrmOut;\n"
"uniform mat4 mvp;\n"
"void main(){ gl_Position = mvp * pos; NrmOut = vec4(normalize(nrm.xyz), 0.0); Color = mvp * nrm; }\n";
static const char *VS_PRESSURE =
"#version 100\n"
"attribute vec4 a0;\nattribute vec4 a1;\nattribute vec4 a2;\nattribute vec4 a3;\n"
"attribute vec4 a4;\nattribute vec4 a5;\nattribute vec4 a6;\nattribute vec4 a7;\n"
"attribute vec4 a8;\nattribute vec4 a9;\nattribute vec4 a10;\nattribute vec4 a11;\n"
"attribute vec4 a12;\nattribute vec4 a13;\nattribute vec4 a14;\n"
"uniform mat4 mvp;\n"
"varying vec4 v0;\n"
"varying vec4 v1;\n"
"varying vec4 v2;\n"
"varying vec4 v3;\n"
"varying vec4 v4;\n"
"varying vec4 v5;\n"
"varying vec4 v6;\n"
"varying vec4 v7;\n"
"void main(){\n"
"    vec4 p0 = a0 + a1;\n"
"    gl_Position = mvp * p0;\n"
"    v0 = a2;\n"
"    v1 = a3;\n"
"    v2 = a4;\n"
"    v3 = a5;\n"
"    v4 = a6;\n"
"    v5 = a7;\n"
"    v6 = a8;\n"
"    v7 = a9;\n"
"    vec4 k0 = a10 + a11;\n"
"    vec4 k1 = a12 + a13;\n"
"    vec4 k2 = a14 + k0;\n"
"    gl_Position = gl_Position + (k0 + k1 + k2);\n"
"}\n";
/* minimal passthrough (no mvp): exercises attribute load + varying store only. */
static const char *VS_PASS =
"#version 100\n"
"attribute vec4 pos;\nattribute vec4 col;\n"
"varying vec4 Color;\n"
"void main(){ gl_Position = pos; Color = col; }\n";

/* Vertex shader without any varying outputs: exercises the v0-varying branch in the
 * SIMD path and keeps compare coverage on the scalar fallback for pure position
 * transforms.
 */
static const char *VS_NOP_VARY =
"#version 100\n"
"attribute vec4 pos;\n"
"void main(){ gl_Position = pos; }\n";

static const char *FS_MIN =
"#version 100\nprecision mediump float;\nvarying vec4 Color;\n"
"void main(){ gl_FragColor = Color; }\n";
static const char *FS_WHITE =
"#version 100\nprecision mediump float;\n"
"void main(){ gl_FragColor = vec4(1.0); }\n";

static glsl_program *build(const char *vs, const char *fs)
{
    glsl_program *p = glsl_program_create(); char err[128]; err[0]=0;
    if (!glsl_attach_source(p,0,vs,err,sizeof err)){ printf("VS fail: %s\n",err); return 0; }
    if (!glsl_attach_source(p,1,fs,err,sizeof err)){ printf("FS fail: %s\n",err); return 0; }
    if (!glsl_link(p,err,sizeof err)){ printf("link fail: %s\n",err); return 0; }
    return p;
}

static unsigned seed = 88172645u;
static float rnd(void){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (float)((int)(seed>>9))/(float)(1<<22) - 1.0f; }

static inline int vtx_count_used_attr_slots(const glsl_program *p)
{
    int nattr = glsl_num_attribs(p);
    if (nattr < 0) nattr = 0;
    if (nattr > GLSL_MAX_ATTRIBS) nattr = GLSL_MAX_ATTRIBS;

    int used_locs[GLSL_MAX_ATTRIBS] = {0};
    int nused_locs = 0;
    int max_loc = -1;

    for (int i = 0; i < nattr; ++i) {
        int loc = -1;
        if (!glsl_get_attrib(p, i, 0, 0, 0, 0, &loc)) continue;
        if (loc < 0 || loc >= GLSL_MAX_ATTRIBS) continue;
        if (used_locs[loc]) continue;
        used_locs[loc] = 1;
        if (loc > max_loc) max_loc = loc;
        ++nused_locs;
    }

    int compact_attr = (nused_locs > 0 && (max_loc + 1) != nused_locs);
    int attr_slots = compact_attr ? nused_locs : (max_loc >= 0 ? max_loc + 1 : 1);
    if (attr_slots < 1) attr_slots = 1;
    if (attr_slots > GLSL_MAX_ATTRIBS) attr_slots = GLSL_MAX_ATTRIBS;
    return attr_slots;
}

static void vtx_bench_policy_sweep(const char *name, glsl_program *p,
                                  int attr_slots, int used_vary, int BATCHES,
                                  float *attrs, float *clip, float *vary)
{
    if (!glsl_vs_simt_ok(p) || attr_slots < 1) {
        printf("[vtx] %-10s lane-sweep SKIP (SIMT unsupported / no attrs)\n", name);
        return;
    }

    sg_vertex_simt_lmul_policy base = sg_vertex_simt_lmul_policy_get();
    int pressure = sg_vertex_simt_pressure_score(p, attr_slots, used_vary);
    int auto_lanes = sg_vertex_simt_chunk_lanes_for(p, attr_slots, used_vary);
    if (auto_lanes < 1) auto_lanes = SGL_SIMT_LANES;
    printf("[vtx] %-10s pressure=%d attrs=%d vary=%d auto_lanes=%d\n",
           name, pressure, attr_slots, used_vary, auto_lanes);

    const int lanes_candidates[] = {
        SGL_SIMT_LANES / 4,
        SGL_SIMT_LANES / 2,
        SGL_SIMT_LANES
    };
    int nc = (int)(sizeof(lanes_candidates) / sizeof(lanes_candidates[0]));
    uint32_t best_ns = 0;
    int best_lanes = 0;

    for (int c = 0; c < nc; ++c) {
        int forced = lanes_candidates[c];
        if (forced < 1) forced = 1;
        sg_vertex_simt_lmul_policy pol = {
            1024, 1024, forced,
            1024, 1024, forced,
            forced
        };
        sg_vertex_simt_lmul_policy_set(&pol);
        int lanes = sg_vertex_simt_chunk_lanes_for(p, attr_slots, used_vary);
        if (lanes < 1) lanes = forced;
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; ++b) {
            glsl_run_vertex_simt(p, attrs, lanes, clip, vary);
        }
        uint64_t t1 = timer_ticks();
        uint64_t verts = (uint64_t)BATCHES * (uint64_t)lanes;
        uint64_t simt_ns = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;
        uint32_t simt_pv = (uint32_t)(simt_ns / (verts ? verts : 1ull));
        printf("[vtx] %-10s lanes=%2d ns/vert=%u\n", name, lanes, simt_pv);
        if (best_ns == 0 || simt_pv < best_ns) {
            best_ns = simt_pv;
            best_lanes = lanes;
        }
    }
    printf("[vtx] %-10s lane-best=%d auto=%d\n", name, best_lanes, auto_lanes);
    sg_vertex_simt_lmul_policy_set(&base);
}

static void test(const char *name, const char *vs, const char *fs)
{
    glsl_program *p = build(vs, fs);
    if (!p){ printf("[vtx] %-10s BUILD FAILED\n", name); return; }
    int umvp=glsl_uniform_location(p,"mvp");
    if(umvp>=0){ float m[16]={1.5f,0.2f,-0.3f,0.0f, 0.1f,0.9f,0.4f,0.0f, -0.2f,0.3f,1.1f,0.0f, 0.7f,-0.5f,0.6f,1.0f}; glsl_uniform(p,umvp,m,16); }

    int simt = glsl_vs_simt_ok(p);
    int nvf  = glsl_num_varying_floats(p);
    if (!simt){ printf("[vtx] %-10s SIMT_OK=no (scalar tree-walk fallback)\n", name); glsl_program_destroy(p); return; }

    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);

    /* SoA attribute buffer: 4 floats per location, GLSL_MAX_ATTRIBS locations. */
    static float attrs[GLSL_MAX_ATTRIBS*4*GLSL_SIMT_MAXLANE];
    static float clip[4*GLSL_SIMT_MAXLANE];
    static float vary[GLSL_MAX_VARYING_FLOATS*GLSL_SIMT_MAXLANE];

    /* ---- correctness: SIMT vs scalar tree-walk oracle over many batches ---- */
    float maxd=0; uint32_t bitmiss=0;
    for (int it=0; it<VTX_VALIDATE_CORRECT_ITERS; ++it) {
        for (int a=0;a<GLSL_MAX_ATTRIBS*4;a++) for(int L=0;L<vl;L++) attrs[a*GLSL_SIMT_MAXLANE+L]=rnd();
        glsl_run_vertex_simt(p, attrs, vl, clip, vary);
        for (int L=0;L<vl;L++){
            float at[GLSL_MAX_ATTRIBS][4];
            for(int loc=0;loc<GLSL_MAX_ATTRIBS;loc++) for(int i=0;i<4;i++) at[loc][i]=attrs[(loc*4+i)*GLSL_SIMT_MAXLANE+L];
            float rc[4], rv[GLSL_MAX_VARYING_FLOATS];
            glsl_run_vertex(p, at, rc, rv);
            for(int k=0;k<4;k++){ float sv=clip[k*GLSL_SIMT_MAXLANE+L]; float d=sv-rc[k]; if(d<0)d=-d; if(d>maxd)maxd=d;
                union{float f;uint32_t u;}A,B; A.f=sv; B.f=rc[k]; if((A.u&0x7fffffffu)||(B.u&0x7fffffffu)) if(A.u!=B.u) bitmiss++; }
            for(int f=0;f<nvf;f++){ float sv=vary[f*GLSL_SIMT_MAXLANE+L]; float d=sv-rv[f]; if(d<0)d=-d; if(d>maxd)maxd=d;
                union{float f;uint32_t u;}A,B; A.f=sv; B.f=rv[f]; if((A.u&0x7fffffffu)||(B.u&0x7fffffffu)) if(A.u!=B.u) bitmiss++; }
        }
    }

    /* ---- timing: scalar-per-vertex vs SIMT-batch, same total vertices ---- */
    const int BATCHES = VTX_VALIDATE_BENCH_BATCHES;
    for (int a=0;a<GLSL_MAX_ATTRIBS*4;a++) for(int L=0;L<vl;L++) attrs[a*GLSL_SIMT_MAXLANE+L]=rnd()*0.5f+0.25f;
    uint64_t t0=timer_ticks();
    for (int b=0;b<BATCHES;b++){ for(int L=0;L<vl;L++){
        float at[GLSL_MAX_ATTRIBS][4]; for(int loc=0;loc<GLSL_MAX_ATTRIBS;loc++) for(int i=0;i<4;i++) at[loc][i]=attrs[(loc*4+i)*GLSL_SIMT_MAXLANE+L];
        float rc[4], rv[GLSL_MAX_VARYING_FLOATS]; glsl_run_vertex(p,at,rc,rv); } }
    uint64_t t1=timer_ticks();
    for (int b=0;b<BATCHES;b++) glsl_run_vertex_simt(p, attrs, vl, clip, vary);
    uint64_t t2=timer_ticks();

    uint64_t verts=(uint64_t)BATCHES*vl;
    uint64_t scal_ns=(t1-t0)*1000000000ull/TIMEBASE_HZ, simt_ns=(t2-t1)*1000000000ull/TIMEBASE_HZ;
    uint32_t scal_pv=(uint32_t)(scal_ns/verts), simt_pv=(uint32_t)(simt_ns/verts);
    uint32_t spx100=simt_pv?(uint32_t)(scal_ns*100/(simt_ns?simt_ns:1)):0;

    printf("[vtx] %-10s vl=%d  maxdiff=%u e-6  bitmiss=%u  scalar=%u ns/vert  SIMT=%u ns/vert  speedup=%u.%02ux  %s\n",
           name, vl, (unsigned)(maxd*1000000.0f), (unsigned)bitmiss, scal_pv, simt_pv, spx100/100, spx100%100,
           (bitmiss==0)?"OK(bit-exact)":((maxd<1e-4f)?"OK(approx)":"*** MISMATCH ***"));
    vtx_bench_policy_sweep(name, p, vtx_count_used_attr_slots(p), nvf, VTX_VALIDATE_BENCH_BATCHES, attrs, clip, vary);
    glsl_program_destroy(p);
}

int kmain(void)
{
    printf("\n==== SIMT vertex (T&L) validation (rva23s64) ====\n");
    test("tl",     VS_TL,      FS_MIN);
    test("heavy",  VS_HEAVY,    FS_MIN);
    test("press",  VS_PRESSURE, FS_MIN);
    test("pass",   VS_PASS,     FS_MIN);
    test("nop_var", VS_NOP_VARY, FS_WHITE);
    printf("[vtx] done.\n");
    sbi_shutdown();
    return 0;
}
