/*
 * simt_validate.c — bare-metal RVV gate for A3: SIMT-on-RVV fragment shading.
 *
 * The host can't exercise RVV, so this runs on QEMU (-cpu rva23s64). For
 * representative shaders it (1) VALIDATES the SIMT executor bit-against the
 * scalar IR oracle (glsl_run_fragment) over a batch of VL pixels, and (2)
 * MEASURES scalar-IR-per-pixel vs SIMT ns/fragment via rdtime. The SIMT path
 * ships only if it matches on every shader.
 *
 * Linked with boot + lean libc + glsl.c (the fragment executor only).
 */
#include "libc/libc.h"
#include "boot/runtime.h"          /* timer_ticks, TIMEBASE_HZ, sbi_shutdown */
#include "glsl.h"
#include <riscv_vector.h>

int sgl_diag_enabled = 0;

/* glsl.c needs the scalar sampler; deterministic stub varying with (u,v). */
void sgl_sample(const sgl_tex *t, float u, float v, float out[4])
{
    (void)t;
    out[0] = u - (float)(int)u; out[1] = v - (float)(int)v;
    out[2] = 0.5f * (out[0] + out[1]); out[3] = 1.0f;
}

static const char *VS_MIN =
"#version 100\nattribute vec4 p;\nvoid main(){ gl_Position = p; }\n";
static const char *FS_SIMPLE =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\nuniform sampler2D tex;\n"
"void main(){ vec4 t = texture2D(tex, TexCoord0.xy); gl_FragColor = Color * t; }\n";
static const char *FS_HEAVY =
"#version 100\nprecision mediump float;\n"
"varying vec4 Normal;\nvarying vec4 LightDir;\nvarying vec4 TexCoord0;\nvarying vec4 Color;\n"
"uniform sampler2D tex;\nuniform vec4 lightColor;\n"
"void main(){\n"
"  vec4 base = texture2D(tex, TexCoord0.xy);\n"
"  vec3 n = normalize(Normal.xyz);\n  vec3 l = normalize(LightDir.xyz);\n"
"  float d = max(dot(n, l), 0.0);\n  float s = pow(d, 16.0);\n"
"  vec4 c = base * Color * lightColor * (d + s);\n"
"  gl_FragColor = clamp(c, 0.0, 1.0);\n}\n";
/* mat4*vec4 in the FS: exercises the batched 16-vfmacc.vf kernel + the relaxed
 * simt_ok gate (a mat uniform consumed only by mat*vec is now SIMT-eligible). */
static const char *FS_MATVEC =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nuniform mat4 mvp;\n"
"void main(){ gl_FragColor = mvp * Color; }\n";
/* mat4*vec4 followed by a dot — proves the mat result feeds the rest of the path. */
static const char *FS_MATVEC2 =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 LightDir;\nuniform mat4 mvp;\n"
"void main(){ vec4 q = mvp * Color; float d = dot(q, LightDir); gl_FragColor = q * d; }\n";
/* reflect/refract/mix/step/smoothstep/cross/faceforward batched-builtin coverage */
static const char *FS_BUILTINS =
"#version 100\nprecision mediump float;\n"
"varying vec4 A;\nvarying vec4 B;\nvarying vec4 T;\n"
"void main(){\n"
"  vec3 r = reflect(A.xyz, normalize(B.xyz));\n"
"  vec3 cm = cross(A.xyz, B.xyz);\n"
"  vec3 m = mix(A.xyz, B.xyz, T.x);\n"
"  float st = step(0.5, T.y);\n  float ss = smoothstep(0.1, 0.9, T.z);\n"
"  gl_FragColor = vec4(r + cm + m, st*ss);\n}\n";

/* texture2DProj: perspective-divided projective sample (Doom 3 projected lights). */
static const char *FS_PROJ =
"#version 100\nprecision mediump float;\n"
"varying vec4 Color;\nvarying vec4 TexCoord0;\nuniform sampler2D tex;\n"
"void main(){ gl_FragColor = Color * texture2DProj(tex, TexCoord0); }\n";

static glsl_program *build(const char *vs, const char *fs)
{
    glsl_program *p = glsl_program_create(); char err[128]; err[0]=0;
    if (!glsl_attach_source(p,0,vs,err,sizeof err)){ printf("VS fail: %s\n",err); return 0; }
    if (!glsl_attach_source(p,1,fs,err,sizeof err)){ printf("FS fail: %s\n",err); return 0; }
    if (!glsl_link(p,err,sizeof err)){ printf("link fail: %s\n",err); return 0; }
    return p;
}

static unsigned seed = 2463534242u;
static float rnd(void){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return (float)((int)(seed>>9))/(float)(1<<22) - 1.0f; }

static void test(const char *name, const char *vs, const char *fs)
{
    glsl_program *p = build(vs,fs);
    if (!p){ printf("[simt] %-14s BUILD FAILED\n", name); return; }
    int umvp=glsl_uniform_location(p,"mvp"); if(umvp>=0){ float m[16]={1.5f,0.2f,-0.3f,0.0f, 0.1f,0.9f,0.4f,0.0f, -0.2f,0.3f,1.1f,0.0f, 0.7f,-0.5f,0.6f,1.0f}; glsl_uniform(p,umvp,m,16); }
    int ulc=glsl_uniform_location(p,"lightColor"); if(ulc>=0){ float c[4]={1,1,1,1}; glsl_uniform(p,ulc,c,4); }
    int utex=glsl_uniform_location(p,"tex"); if(utex>=0) glsl_uniform_sampler(p,utex,0);

    int simt = glsl_fs_simt_ok(p);
    int nv = glsl_num_varying_floats(p); if (nv<4) nv=4; if (nv>GLSL_MAX_VARYING_FLOATS) nv=GLSL_MAX_VARYING_FLOATS;
    /* Real texture: glsl_rvv_texture2d_enabled is on by default now, so the RVV
     * gather sampler dereferences these — a fake (sgl_tex*)1 would fault. */
    static unsigned s_tx[16*16]; static sgl_tex s_realtex;
    for (int i=0;i<256;i++) s_tx[i]=0xff000000u | (((i&15)*16u)<<16) | (((unsigned)(i>>4)*16u)<<8) | ((unsigned)(i*7)&0xFFu);
    s_realtex.texels=s_tx; s_realtex.w=16; s_realtex.h=16; s_realtex.min_filter=0; s_realtex.wrap=1;
    const sgl_tex *samplers[GLSL_MAX_SAMPLERS]; for(int i=0;i<GLSL_MAX_SAMPLERS;i++) samplers[i]=&s_realtex;

    if (!simt){ printf("[simt] %-14s SIMT_OK=no (scalar-IR fallback path)\n", name); glsl_program_destroy(p); return; }

    int vl = (int)__riscv_vsetvl_e32m2((size_t)GLSL_SIMT_MAXLANE);   /* HW vector length */

    /* ---- correctness: SIMT vs scalar-IR oracle over many random batches ---- */
    static float varys[GLSL_MAX_VARYING_FLOATS*GLSL_SIMT_MAXLANE];
    static float sout[4*GLSL_SIMT_MAXLANE];
    float maxd = 0; uint32_t bitmiss = 0;   /* bitmiss: # of channels not bit-identical */
    for (int it=0; it<512; ++it) {
        for (int f=0; f<nv; ++f) for (int L=0; L<vl; ++L) varys[f*GLSL_SIMT_MAXLANE+L] = rnd();
        glsl_run_fragment_simt(p, varys, nv, vl, samplers, sout);
        for (int L=0; L<vl; ++L) {
            float vy[GLSL_MAX_VARYING_FLOATS]; for(int f=0;f<nv;f++) vy[f]=varys[f*GLSL_SIMT_MAXLANE+L];
            float ref[4]; glsl_run_fragment(p, vy, samplers, ref);    /* scalar IR = oracle here */
            for (int k=0;k<4;k++){ float sv=sout[k*GLSL_SIMT_MAXLANE+L]; float d=sv-ref[k]; if(d<0)d=-d; if(d>maxd)maxd=d;
                /* bit-identical, treating +0 and -0 as equal (numerically identical;
                 * fma vs mul of a zero matrix entry can flip the sign of a zero). */
                union{float f;uint32_t u;}A,B; A.f=sv; B.f=ref[k];
                uint32_t au=A.u&0x7fffffffu, bu=B.u&0x7fffffffu;
                if (A.u!=B.u && !(au==0&&bu==0)) bitmiss++; }
        }
    }

    /* ---- timing: scalar-IR-per-pixel vs SIMT, same total fragments ---- */
    const int BATCHES = 4000;
    for (int f=0; f<nv; ++f) for (int L=0; L<vl; ++L) varys[f*GLSL_SIMT_MAXLANE+L] = rnd()*0.5f+0.25f;
    uint64_t t0 = timer_ticks();
    for (int b=0;b<BATCHES;b++){ for(int L=0;L<vl;L++){ float vy[GLSL_MAX_VARYING_FLOATS]; for(int f=0;f<nv;f++)vy[f]=varys[f*GLSL_SIMT_MAXLANE+L]; float r[4]; glsl_run_fragment(p,vy,samplers,r); } }
    uint64_t t1 = timer_ticks();
    for (int b=0;b<BATCHES;b++) glsl_run_fragment_simt(p, varys, nv, vl, samplers, sout);
    uint64_t t2 = timer_ticks();

    uint64_t frags = (uint64_t)BATCHES * vl;
    /* TIMEBASE_HZ ticks/s -> ns/frag = ticks * 1e9 / HZ / frags */
    uint64_t scal_ns = (t1-t0)*1000000000ull/TIMEBASE_HZ;
    uint64_t simt_ns = (t2-t1)*1000000000ull/TIMEBASE_HZ;
    uint32_t scal_pf = (uint32_t)(scal_ns/frags), simt_pf = (uint32_t)(simt_ns/frags);
    uint32_t spx100 = simt_pf? (uint32_t)(scal_ns*100/ (simt_ns?simt_ns:1)) : 0;

    /* maxd printed scaled (lean printf has no %f) */
    printf("[simt] %-14s vl=%d  maxdiff=%u e-6  bitmiss=%u  scalarIR=%u ns/frag  SIMT=%u ns/frag  speedup=%u.%02ux  %s\n",
           name, vl, (unsigned)(maxd*1000000.0f), (unsigned)bitmiss, scal_pf, simt_pf, spx100/100, spx100%100,
           (bitmiss==0)?"OK(bit-exact)":((maxd<1e-4f)?"OK(approx)":"*** MISMATCH ***"));
    glsl_program_destroy(p);
}

int kmain(void)
{
    printf("\n==== A3 SIMT-on-RVV validation (rva23s64) ====\n");
    test("simple", VS_MIN, FS_SIMPLE);
    test("heavy",  VS_MIN, FS_HEAVY);
    test("matvec", VS_MIN, FS_MATVEC);
    test("matvec2",VS_MIN, FS_MATVEC2);
    test("builtins",VS_MIN,FS_BUILTINS);
    test("proj",   VS_MIN, FS_PROJ);
    printf("[simt] done.\n");
    sbi_shutdown();
    return 0;
}
