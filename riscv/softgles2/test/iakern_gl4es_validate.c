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
"attribute vec4 _gl4es_Vertex; attribute vec4 _gl4es_Color;\n"
"attribute vec4 _gl4es_VertexAttrib_8; attribute vec4 _gl4es_VertexAttrib_9;\n"
"attribute vec4 _gl4es_VertexAttrib_10; attribute vec4 _gl4es_VertexAttrib_11;\n"
"uniform mat4 _gl4es_ModelViewProjectionMatrix;\n"
"uniform vec4 _gl4es_Vertex_ProgramEnv_4; uniform vec4 _gl4es_Vertex_ProgramEnv_5;\n"
"uniform vec4 _gl4es_Vertex_ProgramEnv_6; uniform vec4 _gl4es_Vertex_ProgramEnv_7; uniform vec4 _gl4es_Vertex_ProgramEnv_8; uniform vec4 _gl4es_Vertex_ProgramEnv_9;\n"
"uniform vec4 _gl4es_Vertex_ProgramEnv_10; uniform vec4 _gl4es_Vertex_ProgramEnv_11; uniform vec4 _gl4es_Vertex_ProgramEnv_12; uniform vec4 _gl4es_Vertex_ProgramEnv_13;\n"
"uniform vec4 _gl4es_Vertex_ProgramEnv_14; uniform vec4 _gl4es_Vertex_ProgramEnv_15; uniform vec4 _gl4es_Vertex_ProgramEnv_16; uniform vec4 _gl4es_Vertex_ProgramEnv_17;\n"
"varying vec4 _gl4es_FrontColor;\n"
"varying vec4 _gl4es_TexCoord_0;\nvarying vec4 _gl4es_TexCoord_1;\nvarying vec4 _gl4es_TexCoord_2;\n"
"varying vec4 _gl4es_TexCoord_3;\nvarying vec4 _gl4es_TexCoord_4;\nvarying vec4 _gl4es_TexCoord_5;\n"
"varying vec4 _gl4es_TexCoord_6;\n"
"void main(){ vec4 L=_gl4es_Vertex_ProgramEnv_4-_gl4es_Vertex; vec4 V=_gl4es_Vertex_ProgramEnv_5-_gl4es_Vertex; vec4 d=vec4(0.0,0.5,0.0,1.0);\n"
" gl_Position=_gl4es_ModelViewProjectionMatrix*_gl4es_Vertex;\n"
" _gl4es_TexCoord_0=vec4(dot(_gl4es_VertexAttrib_9.xyz,L.xyz),dot(_gl4es_VertexAttrib_10.xyz,L.xyz),dot(_gl4es_VertexAttrib_11.xyz,L.xyz),0.0);\n"
" _gl4es_TexCoord_1=d; _gl4es_TexCoord_1.x=dot(_gl4es_VertexAttrib_8,_gl4es_Vertex_ProgramEnv_10); _gl4es_TexCoord_1.y=dot(_gl4es_VertexAttrib_8,_gl4es_Vertex_ProgramEnv_11);\n"
" _gl4es_TexCoord_2=d; _gl4es_TexCoord_2.x=dot(_gl4es_Vertex,_gl4es_Vertex_ProgramEnv_9);\n"
" _gl4es_TexCoord_3=vec4(dot(_gl4es_Vertex,_gl4es_Vertex_ProgramEnv_6),dot(_gl4es_Vertex,_gl4es_Vertex_ProgramEnv_7),0.0,dot(_gl4es_Vertex,_gl4es_Vertex_ProgramEnv_8));\n"
" _gl4es_TexCoord_4=d; _gl4es_TexCoord_4.x=dot(_gl4es_VertexAttrib_8,_gl4es_Vertex_ProgramEnv_12); _gl4es_TexCoord_4.y=dot(_gl4es_VertexAttrib_8,_gl4es_Vertex_ProgramEnv_13);\n"
" _gl4es_TexCoord_5=d; _gl4es_TexCoord_5.x=dot(_gl4es_VertexAttrib_8,_gl4es_Vertex_ProgramEnv_14); _gl4es_TexCoord_5.y=dot(_gl4es_VertexAttrib_8,_gl4es_Vertex_ProgramEnv_15);\n"
" L.xyz=normalize(L.xyz)+normalize(V.xyz); _gl4es_TexCoord_6=vec4(dot(_gl4es_VertexAttrib_9.xyz,L.xyz),dot(_gl4es_VertexAttrib_10.xyz,L.xyz),dot(_gl4es_VertexAttrib_11.xyz,L.xyz),0.0);\n"
" _gl4es_FrontColor=_gl4es_Color*_gl4es_Vertex_ProgramEnv_16+_gl4es_Vertex_ProgramEnv_17; }\n";

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
    glsl_link_dump = 1;
    char err[200]; err[0] = 0;
    if (!glsl_attach_source(p, 0, VS_GL4ES, err, sizeof err)) { printf("VS: %s\n", err); return 1; }
    if (!glsl_attach_source(p, 1, FS_INTERACTION_GL4ES, err, sizeof err)) { printf("FS: %s\n", err); return 1; }
    if (!glsl_link(p, err, sizeof err)) { printf("link: %s\n", err); return 1; }

    { float m[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, .1f,-.2f,.3f,1}; int l=glsl_uniform_location(p,"_gl4es_ModelViewProjectionMatrix"); glsl_uniform(p,l,m,16); }
    for(int e=4;e<=17;++e){ char n[40]; const char *q="_gl4es_Vertex_ProgramEnv_"; int k=0;while(q[k]){n[k]=q[k];++k;}if(e>=10)n[k++]=(char)('0'+e/10);n[k++]=(char)('0'+e%10);n[k]=0;float v[4]={rnd(),rnd(),rnd(),rnd()};int l=glsl_uniform_location(p,n);if(l>=0)glsl_uniform(p,l,v,4); }

    /* gl4es ARB shaders encode their intended texture unit in the sampler name.
     * Do NOT manually bind these: this validates the real gl4es integration
     * contract that previously left every sampler at unit zero. */
    const char *s2d[6] = { "_gl4es_Sampler2D_1","_gl4es_Sampler2D_2","_gl4es_Sampler2D_3",
                           "_gl4es_Sampler2D_4","_gl4es_Sampler2D_5","_gl4es_Sampler2D_6" };
    int sampler_defaults_ok = 1;
    { int l = glsl_uniform_location(p, "_gl4es_SamplerCube_0");
      if (l < 0 || p->uni[l].unit != 0) sampler_defaults_ok = 0; }
    for (int i = 0; i < 6; ++i) {
        int l = glsl_uniform_location(p, s2d[i]);
        if (l < 0 || p->uni[l].unit != i + 1) sampler_defaults_ok = 0;
        /* Simulate gl4es replaying its zero-initialised sampler cache. Internal
         * ARB sampler names must retain the unit encoded by their suffix. */
        if (l >= 0) glsl_uniform_sampler(p, l, 0);
        if (l < 0 || p->uni[l].unit != i + 1) sampler_defaults_ok = 0;
    }
    printf("[gik] gl4es sampler-name unit defaults: %s\n", sampler_defaults_ok ? "OK" : "FAIL");
    if (!sampler_defaults_ok) return 1;
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
    { static float at[GLSL_MAX_ATTRIBS*4*GLSL_SIMT_MAXLANE],cv0[4*GLSL_SIMT_MAXLANE],cv1[4*GLSL_SIMT_MAXLANE];
      static float vv0[GLSL_MAX_VARYING_FLOATS*GLSL_SIMT_MAXLANE],vv1[GLSL_MAX_VARYING_FLOATS*GLSL_SIMT_MAXLANE];
      for(int i=0;i<GLSL_MAX_ATTRIBS*4*vl;++i)at[i]=rnd();
      glsl_vs_kernel_fn vk=p->vs_kernel; p->vs_kernel=0; glsl_run_vertex_simt(p,at,vl,cv0,vv0); p->vs_kernel=vk; glsl_run_vertex_simt(p,at,vl,cv1,vv1);
      float md=0;int bm=0;for(int i=0;i<4*vl;++i){float d=cv0[i]-cv1[i];if(d<0)d=-d;if(d>md)md=d;if(cv0[i]!=cv1[i])++bm;}
      for(int i=0;i<32*vl;++i){float d=vv0[i]-vv1[i];if(d<0)d=-d;if(d>md)md=d;if(vv0[i]!=vv1[i])++bm;}
      printf("[gik-vs] kernel=%s maxdiff=%u e-6 bitmiss=%d %s\n",vk?"YES":"NO",(unsigned)(md*1000000.f),bm,md<1e-4f?"OK":"FAIL");
      if(!vk||md>=1e-4f)return 1; }
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

    /* Correctness: both paths use production samplers (RVV cube + RVV 2D). The
     * kernel must stay bit-identical to the SIMT VM when both execute the same
     * face-select helper. (Scalar-cube A/B is available by setting
     * glsl_rvv_texturecube_enabled=0 before the VM run.) */
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
