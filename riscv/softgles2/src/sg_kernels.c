/*
 * softgles2 — register-resident fragment kernels (the "Carmack lever").
 *
 * These are hand-written specialisations of whole shader archetypes that keep
 * every intermediate in vector registers, calling the same RVV helpers as the
 * per-op SIMT VM (simt_run_ir in glsl_simt.c) but WITHOUT spilling each IR op
 * through the ~1KB-per-register memory file. glsl_fs_classify() binds one only
 * when the linked fragment shader matches the kernel's exact verified layout;
 * any mismatch leaves fs_kernel==0 and the VM runs (zero behavioural risk).
 *
 * Split out of glsl_simt.c so the VM and its specialised replacements are
 * separate concerns. Two kernels today:
 *   - interaction: bump + diffuse + specular + projective light (5 textures)
 *   - overlay:     gl4es 2D blit  gl_FragColor = Color * texture2DProj(s, tc)
 *                  — THE shader that actually renders the demo every frame.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "glsl_internal.h"
#include "sg_math.h"
#include "sg_frag.h"   /* sgl_simt_frag_x/y for heatHaze */

/* Per-frame interaction-light pipeline probe.  Counters are atomic because
 * fragment batches execute on several worker harts.  This deliberately lives
 * beside the kernel so it observes the values actually consumed by RVV, not a
 * reconstructed approximation in the draw front end. */
int sgl_light_probe_enabled = 0;
enum { LP_BATCH, LP_LANE, LP_ALLTEX, LP_SC, LP_S1, LP_S2, LP_S3, LP_S4, LP_S5, LP_S6,
       LP_CUBE, LP_BUMP, LP_NDLP, LP_NDLN, LP_PROJ, LP_FALL, LP_DIFF, LP_COLOR,
       LP_LIGHT, LP_FRONT, LP_PREOUT, LP_DUNI, LP_GUNI, LP_COUNT };
static uint64_t sgl_lp[LP_COUNT];

void sgl_light_probe_report_reset(void)
{
    uint64_t v[LP_COUNT];
    for (int i = 0; i < LP_COUNT; ++i)
        v[i] = __atomic_exchange_n(&sgl_lp[i], 0, __ATOMIC_RELAXED);
    if (!v[LP_BATCH]) return;
    printf("[lprobe] batches=%lu lanes=%lu alltex=%lu bind(c,1..6)=%lu,%lu,%lu,%lu,%lu,%lu,%lu cube=%lu bump=%lu ndl+=%lu ndl-=%lu proj=%lu fall=%lu diff=%lu color=%lu light=%lu front=%lu preout=%lu uni(d,g)=%lu,%lu\n",
           (unsigned long)v[LP_BATCH], (unsigned long)v[LP_LANE],
           (unsigned long)v[LP_ALLTEX], (unsigned long)v[LP_SC], (unsigned long)v[LP_S1],
           (unsigned long)v[LP_S2], (unsigned long)v[LP_S3], (unsigned long)v[LP_S4],
           (unsigned long)v[LP_S5], (unsigned long)v[LP_S6], (unsigned long)v[LP_CUBE],
           (unsigned long)v[LP_BUMP], (unsigned long)v[LP_NDLP],
           (unsigned long)v[LP_NDLN], (unsigned long)v[LP_PROJ],
           (unsigned long)v[LP_FALL], (unsigned long)v[LP_DIFF], (unsigned long)v[LP_COLOR],
           (unsigned long)v[LP_LIGHT], (unsigned long)v[LP_FRONT],
           (unsigned long)v[LP_PREOUT], (unsigned long)v[LP_DUNI], (unsigned long)v[LP_GUNI]);
}

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define GLSL_SIMT_RVV 1
#else
#  define GLSL_SIMT_RVV 0
#endif
#define SIMT_MAXLANE GLSL_SIMT_MAXLANE

/* Diagnostic toggle: force the interaction light term to unity so the kernel outputs
 * diffuse*diffuseColor + spec with NO light modulation. Isolates whether the flat world
 * look comes from the light term (e.g. a null light-projection/falloff sampler ->
 * iak_sample2d_or_default returns {0,0,0,1} -> light multiplies to 0) versus the diffuse
 * sample itself. Set from the engine via the bm_iakLightUnity cvar (sys_dos.cpp). 0=off. */
int sgl_dbg_iak_light_unity = 0;

/* ^ written by the engine (BM_SyncRendererCvars, sys_dos.cpp) on every build,
 * so it must be defined outside the GLSL_SIMT_RVV block below. */

#if GLSL_SIMT_RVV

/* Resolve immutable metadata once at link/classification time. Workers only
 * read this table: no per-batch strings, capacity misses, or lazy-cache writes.
 * Sampler entries hold uniform indices, so glUniform1i remains dynamic. */
static void iak_bind_metadata(glsl_program *p)
{
    static const struct { const char *name; int kind; } specs[] = {
        [KB_ModelViewProjectionMatrix] = {"_gl4es_ModelViewProjectionMatrix", 0},
        [KB_Vertex] = {"_gl4es_Vertex", 2},
        [KB_Color] = {"_gl4es_Color", 2},
        [KB_MultiTexCoord0] = {"_gl4es_MultiTexCoord0", 2},
        [KB_TextureMatrix_0] = {"_gl4es_TextureMatrix_0", 0},
        [KB_diffuseColor] = {"diffuseColor", 0},
        [KB_specularColor] = {"specularColor", 0},
        [KB_TexSampler_0] = {"_gl4es_TexSampler_0", 1},
        [KB_SamplerCube_0] = {"_gl4es_SamplerCube_0", 1},
        [KB_Sampler2D_0] = {"_gl4es_Sampler2D_0", 1},
        [KB_Sampler2D_1] = {"_gl4es_Sampler2D_1", 1},
        [KB_Sampler2D_2] = {"_gl4es_Sampler2D_2", 1},
        [KB_Fragment_ProgramEnv_0] = {"_gl4es_Fragment_ProgramEnv_0", 0},
        [KB_Fragment_ProgramEnv_1] = {"_gl4es_Fragment_ProgramEnv_1", 0},
        [KB_Fragment_ProgramEnv_21] = {"_gl4es_Fragment_ProgramEnv_21", 0},
        [KB_TexSampler_1] = {"_gl4es_TexSampler_1", 1},
        [KB_Sampler2D_3] = {"_gl4es_Sampler2D_3", 1},
        [KB_Sampler2D_4] = {"_gl4es_Sampler2D_4", 1},
        [KB_Sampler2D_5] = {"_gl4es_Sampler2D_5", 1},
        [KB_Sampler2D_6] = {"_gl4es_Sampler2D_6", 1},
        [KB_AlphaRef] = {"_gl4es_AlphaRef", 0},
        [KB_TextureEnvColor_1] = {"_gl4es_TextureEnvColor_1", 0},
        [KB_VertexAttrib_8] = {"_gl4es_VertexAttrib_8", 2},
        [KB_VertexAttrib_9] = {"_gl4es_VertexAttrib_9", 2},
        [KB_VertexAttrib_10] = {"_gl4es_VertexAttrib_10", 2},
        [KB_VertexAttrib_11] = {"_gl4es_VertexAttrib_11", 2},
        [KB_MultiTexCoord1] = {"_gl4es_MultiTexCoord1", 2},
        [KB_MultiTexCoord2] = {"_gl4es_MultiTexCoord2", 2},
        [KB_MultiTexCoord3] = {"_gl4es_MultiTexCoord3", 2},
        [KB_Vertex_ProgramEnv_4] = {"_gl4es_Vertex_ProgramEnv_4", 0},
        [KB_Vertex_ProgramEnv_5] = {"_gl4es_Vertex_ProgramEnv_5", 0},
        [KB_Vertex_ProgramEnv_6] = {"_gl4es_Vertex_ProgramEnv_6", 0},
        [KB_Vertex_ProgramEnv_7] = {"_gl4es_Vertex_ProgramEnv_7", 0},
        [KB_Vertex_ProgramEnv_8] = {"_gl4es_Vertex_ProgramEnv_8", 0},
        [KB_Vertex_ProgramEnv_9] = {"_gl4es_Vertex_ProgramEnv_9", 0},
        [KB_Vertex_ProgramEnv_10] = {"_gl4es_Vertex_ProgramEnv_10", 0},
        [KB_Vertex_ProgramEnv_11] = {"_gl4es_Vertex_ProgramEnv_11", 0},
        [KB_Vertex_ProgramEnv_12] = {"_gl4es_Vertex_ProgramEnv_12", 0},
        [KB_Vertex_ProgramEnv_13] = {"_gl4es_Vertex_ProgramEnv_13", 0},
        [KB_Vertex_ProgramEnv_14] = {"_gl4es_Vertex_ProgramEnv_14", 0},
        [KB_Vertex_ProgramEnv_15] = {"_gl4es_Vertex_ProgramEnv_15", 0},
        [KB_Vertex_ProgramEnv_16] = {"_gl4es_Vertex_ProgramEnv_16", 0},
        [KB_Vertex_ProgramEnv_17] = {"_gl4es_Vertex_ProgramEnv_17", 0},
    };
    for (int id = 0; id < SGL_KERNEL_BINDING_COUNT; ++id) {
        p->kernel_binding[id] = -1;
        if (specs[id].kind == 2) {
            for (int i = 0; i < p->vs.nglob; ++i)
                if (p->vs.globals[i].qual == Q_ATTRIB &&
                    !strcmp(p->vs.globals[i].name, specs[id].name)) {
                    p->kernel_binding[id] = p->vs.globals[i].location; break;
                }
        } else {
            for (int i = 0; i < p->nuni; ++i)
                if (!!p->uni[i].is_sampler == specs[id].kind &&
                    !strcmp(p->uni[i].name, specs[id].name)) {
                    p->kernel_binding[id] = i; break;
                }
        }
    }
}

static int iak_uoff(const glsl_program *p, int id)
{
    int i = p->kernel_binding[id];
    return i >= 0 ? p->uni[i].offset : -1;
}

/* True if the program derives any texcoord from glTexGen (OBJECT_LINEAR /
 * EYE_LINEAR object/eye planes). The overlay/mvp fast-path VS kernels read the
 * MultiTexCoord attribute and TextureMatrix only -- they do NOT evaluate texgen
 * -- so binding one to a texgen program (e.g. Doom 3's fog pass, whose distance
 * texcoord comes from glTexGen GL_OBJECT_PLANE) samples the wrong coordinate and
 * renders fog uniformly opaque. Such programs must fall to the VM VS. */
static int iak_uses_texgen(const glsl_program *p)
{
    for (int k = 0; k < p->nuni; ++k) {
        const char *n = p->uni[k].name;
        if (!p->uni[k].is_sampler &&
            (!strncmp(n, "_gl4es_ObjectPlane", 18) ||
             !strncmp(n, "_gl4es_EyePlane", 15)))
            return 1;
    }
    return 0;
}

static int iak_aloc(const glsl_program *p, int id)
{
    return p->kernel_binding[id];
}

/* Register-resident form of interaction.vfp's vertex program.  This removes a
 * ~96-register generic IR frame and 70+ interpreted ops from every interaction
 * draw while preserving the exact gl4es attribute/uniform/varying contract. */
static int __attribute__((unused)) glsl_vs_kernel_interaction_gl4es(glsl_program *p, const float *attrs,
                                             const int *amap, int vl,
                                             float *clip, float *vary)
{
    if (!vary) return 0;
    /* gl4es' live ARB converter preserves generic attribute numbers 8..11;
     * older synthetic tests used MultiTexCoord aliases. Prefer the real ABI and
     * retain aliases so both forms exercise the same kernel. */
    const int an[6]={KB_Vertex,KB_Color,KB_VertexAttrib_8,
                       KB_VertexAttrib_9,KB_VertexAttrib_10,KB_VertexAttrib_11};
    const int alias[6]={-1,-1,KB_MultiTexCoord0,KB_MultiTexCoord1,
                           KB_MultiTexCoord2,KB_MultiTexCoord3};
    int al[6];
    for(int i=0;i<6;++i){ al[i]=iak_aloc(p,an[i]); if(al[i]<0&&alias[i]>=0)al[i]=iak_aloc(p,alias[i]); if(al[i]<0)return 0; if(amap){al[i]=amap[al[i]];if(al[i]<0)return 0;} }
    int mo=iak_uoff(p, KB_ModelViewProjectionMatrix);
    /* Static literals rather than assembling each name char-by-char on every
     * vertex batch: this ran ~3600 times per lit frame, and the scratch buffer it
     * used was also what broke the lookup memo (see iak_memo_find). */
    static const int venv[18] = { 0, 0, 0, 0,
        KB_Vertex_ProgramEnv_4,  KB_Vertex_ProgramEnv_5,
        KB_Vertex_ProgramEnv_6,  KB_Vertex_ProgramEnv_7,
        KB_Vertex_ProgramEnv_8,  KB_Vertex_ProgramEnv_9,
        KB_Vertex_ProgramEnv_10, KB_Vertex_ProgramEnv_11,
        KB_Vertex_ProgramEnv_12, KB_Vertex_ProgramEnv_13,
        KB_Vertex_ProgramEnv_14, KB_Vertex_ProgramEnv_15,
        KB_Vertex_ProgramEnv_16, KB_Vertex_ProgramEnv_17 };
    int eo[18]; for(int i=4;i<=17;++i){ eo[i]=iak_uoff(p,venv[i]); if(eo[i]<0)return 0; }
    if(mo<0)return 0;
    size_t VL=__riscv_vsetvl_e32m2((size_t)vl);
#define AV(A,C) __riscv_vle32_v_f32m2(&attrs[((al[(A)]*4+(C))*SIMT_MAXLANE)],VL)
#define STO(O,C,V) __riscv_vse32_v_f32m2(&vary[((O)+(C))*SIMT_MAXLANE],(V),VL)
    vfloat32m2_t px=AV(0,0),py=AV(0,1),pz=AV(0,2),pw=AV(0,3);
    sgl_rvv_mat4xvec4_m2((float(*)[SGL_SIMT_LANES])&clip[0*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[1*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[2*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[3*SIMT_MAXLANE],
                         &p->ustore[mo],px,py,pz,pw,VL);
    vfloat32m2_t z=__riscv_vfmv_v_f_f32m2(0.f,VL), half=__riscv_vfmv_v_f_f32m2(.5f,VL), one=__riscv_vfmv_v_f_f32m2(1.f,VL);
    for(int f=0;f<32;++f)__riscv_vse32_v_f32m2(&vary[f*SIMT_MAXLANE],z,VL);
    vfloat32m2_t lx=__riscv_vfrsub_vf_f32m2(px,p->ustore[eo[4]],VL);
    vfloat32m2_t ly=__riscv_vfrsub_vf_f32m2(py,p->ustore[eo[4]+1],VL);
    vfloat32m2_t lz=__riscv_vfrsub_vf_f32m2(pz,p->ustore[eo[4]+2],VL);
#define DOT3ATTR(A,X,Y,Z) __riscv_vfmacc_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(AV((A),0),(X),VL),AV((A),1),(Y),VL),(Z),AV((A),2),VL)
#define DP4U(O) __riscv_vfmacc_vf_f32m2(__riscv_vfmacc_vf_f32m2(__riscv_vfmacc_vf_f32m2(__riscv_vfmul_vf_f32m2(px,p->ustore[(O)],VL),p->ustore[(O)+1],py,VL),p->ustore[(O)+2],pz,VL),p->ustore[(O)+3],pw,VL)
#define DP4TC(O) __riscv_vfmacc_vf_f32m2(__riscv_vfmacc_vf_f32m2(__riscv_vfmacc_vf_f32m2(__riscv_vfmul_vf_f32m2(AV(2,0),p->ustore[(O)],VL),p->ustore[(O)+1],AV(2,1),VL),p->ustore[(O)+2],AV(2,2),VL),p->ustore[(O)+3],AV(2,3),VL)
    STO(4,0,DOT3ATTR(3,lx,ly,lz)); STO(4,1,DOT3ATTR(4,lx,ly,lz)); STO(4,2,DOT3ATTR(5,lx,ly,lz));
    STO(8,0,DP4TC(eo[10])); STO(8,1,DP4TC(eo[11])); STO(8,2,z); STO(8,3,one);
    STO(12,0,DP4U(eo[9])); STO(12,1,half); STO(12,2,z); STO(12,3,one);
    STO(16,0,DP4U(eo[6])); STO(16,1,DP4U(eo[7])); STO(16,3,DP4U(eo[8]));
    STO(20,0,DP4TC(eo[12])); STO(20,1,DP4TC(eo[13])); STO(20,2,z); STO(20,3,one);
    STO(24,0,DP4TC(eo[14])); STO(24,1,DP4TC(eo[15])); STO(24,2,z); STO(24,3,one);
    vfloat32m2_t s=__riscv_vfmacc_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(lx,lx,VL),ly,ly,VL),lz,lz,VL);
    vfloat32m2_t ri=__riscv_vmerge_vvm_f32m2(z,sgl_rvv_rsqrt_fast_m2(s,VL),__riscv_vmfgt_vf_f32m2_b16(s,0.f,VL),VL);
    lx=__riscv_vfmul_vv_f32m2(lx,ri,VL);ly=__riscv_vfmul_vv_f32m2(ly,ri,VL);lz=__riscv_vfmul_vv_f32m2(lz,ri,VL);
    vfloat32m2_t vx=__riscv_vfrsub_vf_f32m2(px,p->ustore[eo[5]],VL),vy=__riscv_vfrsub_vf_f32m2(py,p->ustore[eo[5]+1],VL),vz=__riscv_vfrsub_vf_f32m2(pz,p->ustore[eo[5]+2],VL);
    s=__riscv_vfmacc_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(vx,vx,VL),vy,vy,VL),vz,vz,VL);
    ri=__riscv_vmerge_vvm_f32m2(z,sgl_rvv_rsqrt_fast_m2(s,VL),__riscv_vmfgt_vf_f32m2_b16(s,0.f,VL),VL);
    lx=__riscv_vfadd_vv_f32m2(lx,__riscv_vfmul_vv_f32m2(vx,ri,VL),VL);ly=__riscv_vfadd_vv_f32m2(ly,__riscv_vfmul_vv_f32m2(vy,ri,VL),VL);lz=__riscv_vfadd_vv_f32m2(lz,__riscv_vfmul_vv_f32m2(vz,ri,VL),VL);
    STO(28,0,DOT3ATTR(3,lx,ly,lz)); STO(28,1,DOT3ATTR(4,lx,ly,lz)); STO(28,2,DOT3ATTR(5,lx,ly,lz));
    for(int c=0;c<4;++c){vfloat32m2_t q=__riscv_vfmul_vf_f32m2(AV(1,c),p->ustore[eo[16]+c],VL);STO(0,c,__riscv_vfadd_vf_f32m2(q,p->ustore[eo[17]+c],VL));}
#undef DP4TC
#undef DP4U
#undef DOT3ATTR
#undef STO
#undef AV
    return 1;
}

/* Depth / shadow / z-fill vertex path: clip = MVP * Vertex. Optionally pass
 * Color as varying0 when present; otherwise leave varyings untouched/zeroed by
 * the caller. This rescues depth pre-pass and many non-textured draws from the
 * scalar VS hash-cache path without risking the interaction ABI. */
static int glsl_vs_kernel_mvp_gl4es(glsl_program *p, const float *attrs,
                                   const int *amap, int vl, float *clip, float *vary)
{
    int av = iak_aloc(p, KB_Vertex);
    int mo = iak_uoff(p, KB_ModelViewProjectionMatrix);
    if (av < 0 || mo < 0) return 0;
    if (amap) { av = amap[av]; if (av < 0) return 0; }
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
#define MAV(L,C) __riscv_vle32_v_f32m2(&attrs[((L)*4+(C))*SIMT_MAXLANE], VL)
    sgl_rvv_mat4xvec4_m2((float(*)[SGL_SIMT_LANES])&clip[0*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[1*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[2*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[3*SIMT_MAXLANE],
                         &p->ustore[mo], MAV(av,0), MAV(av,1), MAV(av,2), MAV(av,3), VL);
    if (vary) {
        int ac = iak_aloc(p, KB_Color);
        if (ac >= 0) {
            if (amap) { ac = amap[ac]; if (ac < 0) goto no_color; }
            for (int c = 0; c < 4; ++c)
                __riscv_vse32_v_f32m2(&vary[c * SIMT_MAXLANE], MAV(ac, c), VL);
        } else {
no_color:
            { vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, VL);
              for (int c = 0; c < 4; ++c) __riscv_vse32_v_f32m2(&vary[c * SIMT_MAXLANE], z, VL); }
        }
    }
#undef MAV
    return 1;
}

/* Common gl4es fixed-function/FPE vertex path used by overlays and GUI layers:
 * MVP*Vertex, pass Color, TextureMatrix0*MultiTexCoord0. */
static int glsl_vs_kernel_overlay_gl4es(glsl_program *p, const float *attrs,
                                        const int *amap, int vl,float *clip,float *vary)
{
    if(!vary)return 0;
    int av=iak_aloc(p, KB_Vertex),ac=iak_aloc(p, KB_Color),at=iak_aloc(p, KB_MultiTexCoord0);
    if(av<0||ac<0||at<0)return 0;
    if(amap){av=amap[av];ac=amap[ac];at=amap[at];if(av<0||ac<0||at<0)return 0;}
    int mo=iak_uoff(p, KB_ModelViewProjectionMatrix),to=iak_uoff(p, KB_TextureMatrix_0);
    if(mo<0||to<0)return 0;
    size_t VL=__riscv_vsetvl_e32m2((size_t)vl);
#define OAV(L,C) __riscv_vle32_v_f32m2(&attrs[((L)*4+(C))*SIMT_MAXLANE],VL)
    sgl_rvv_mat4xvec4_m2((float(*)[SGL_SIMT_LANES])&clip[0*SIMT_MAXLANE],(float(*)[SGL_SIMT_LANES])&clip[1*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&clip[2*SIMT_MAXLANE],(float(*)[SGL_SIMT_LANES])&clip[3*SIMT_MAXLANE],
                         &p->ustore[mo],OAV(av,0),OAV(av,1),OAV(av,2),OAV(av,3),VL);
    for(int c=0;c<4;++c)__riscv_vse32_v_f32m2(&vary[c*SIMT_MAXLANE],OAV(ac,c),VL);
    sgl_rvv_mat4xvec4_m2((float(*)[SGL_SIMT_LANES])&vary[4*SIMT_MAXLANE],(float(*)[SGL_SIMT_LANES])&vary[5*SIMT_MAXLANE],
                         (float(*)[SGL_SIMT_LANES])&vary[6*SIMT_MAXLANE],(float(*)[SGL_SIMT_LANES])&vary[7*SIMT_MAXLANE],
                         &p->ustore[to],OAV(at,0),OAV(at,1),OAV(at,2),OAV(at,3),VL);
#undef OAV
    return 1;
}

/* gl_FragColor = Color (first varying). Depth pre-pass with cmask often never
 * calls this (cmask==0 skips shade), but lit solid / debug / simple materials
 * hit it every frame when gl4es emits a colour-only FPE shader. */
static int glsl_fs_kernel_color(glsl_program *p, const float *varys, int vl,
                                const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                const float *ustore, float *out)
{
    (void)p; (void)samplers; (void)ustore;
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], __riscv_vle32_v_f32m2(&varys[0 * SIMT_MAXLANE], VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], __riscv_vle32_v_f32m2(&varys[1 * SIMT_MAXLANE], VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], __riscv_vle32_v_f32m2(&varys[2 * SIMT_MAXLANE], VL), VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], __riscv_vle32_v_f32m2(&varys[3 * SIMT_MAXLANE], VL), VL);
    return 1;
}

/* Interaction: bump + diffuse + specular + projective light/falloff.
 * Layout (verified by the classifier): 5 vec4 varyings TexBump/TexDiff/TexLight/
 * LightVec/ViewVec at float offsets 0/4/8/12/16; samplers 0..4; vec4 uniforms
 * diffuseColor + specularColor. */
/* Build the (dudx,dvdx,dudy,dvdy) derivative for a sampler whose texcoord is the
 * varying pair (iu,iv), from the rasteriser's per-batch analytic derivatives
 * (sgl_simt_vary_ddx/ddy). Returns NULL when derivatives are unavailable this
 * batch (mip/aniso disabled, or the immediate/VM paths that do not fill them) so
 * the sampler keeps its prior behaviour; iu<0 also returns NULL, for a dependent
 * or projective/scaled coordinate that has no direct varying derivative. */
static inline const sgl_uv_deriv *iak_uv_deriv(int iu, int iv, sgl_uv_deriv *out)
{
    if (!sgl_simt_deriv_valid || iu < 0 || iv < 0) return 0;
    out->dudx = sgl_simt_vary_ddx[iu]; out->dudy = sgl_simt_vary_ddy[iu];
    out->dvdx = sgl_simt_vary_ddx[iv]; out->dvdy = sgl_simt_vary_ddy[iv];
    return out;
}

#define IAK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
static int glsl_fs_kernel_interaction(glsl_program *p, const float *varys, int vl,
                                      const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                      const float *ustore, float *out)
{
    if (!samplers[0] || !samplers[1] || !samplers[2] || !samplers[3] || !samplers[4])
        return 0;   /* missing texture -> let the VM handle it */
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    const float inv255 = 1.f / 255.f;
    int doff = iak_uoff(p, KB_diffuseColor), soff = iak_uoff(p, KB_specularColor);
    float dcx = doff >= 0 ? ustore[doff] : 1.f, dcy = doff >= 0 ? ustore[doff + 1] : 1.f, dcz = doff >= 0 ? ustore[doff + 2] : 1.f;
    float scx = soff >= 0 ? ustore[soff] : 1.f, scy = soff >= 0 ? ustore[soff + 1] : 1.f, scz = soff >= 0 ? ustore[soff + 2] : 1.f;
    vfloat32m2_t r0, r1, r2, r3;
    sgl_uv_deriv dvtmp;

    sgl_rvv_sample2d_m2(samplers[0], IAK_V(0), IAK_V(1), VL, inv255, samplers[0]->min_filter, samplers[0]->wrap, iak_uv_deriv(0, 1, &dvtmp), &r0, &r1, &r2, &r3);
    vfloat32m2_t nx = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r0, 2.f, VL), 1.f, VL);
    vfloat32m2_t ny = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r1, 2.f, VL), 1.f, VL);
    vfloat32m2_t nz = __riscv_vfsub_vf_f32m2(__riscv_vfmul_vf_f32m2(r2, 2.f, VL), 1.f, VL);
    vfloat32m2_t s = __riscv_vfmul_vv_f32m2(nx, nx, VL); s = __riscv_vfmacc_vv_f32m2(s, ny, ny, VL); s = __riscv_vfmacc_vv_f32m2(s, nz, nz, VL);
    vfloat32m2_t inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    nx = __riscv_vfmul_vv_f32m2(nx, inv, VL); ny = __riscv_vfmul_vv_f32m2(ny, inv, VL); nz = __riscv_vfmul_vv_f32m2(nz, inv, VL);

    vfloat32m2_t lx = IAK_V(12), ly = IAK_V(13), lz = IAK_V(14);
    s = __riscv_vfmul_vv_f32m2(lx, lx, VL); s = __riscv_vfmacc_vv_f32m2(s, ly, ly, VL); s = __riscv_vfmacc_vv_f32m2(s, lz, lz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    lx = __riscv_vfmul_vv_f32m2(lx, inv, VL); ly = __riscv_vfmul_vv_f32m2(ly, inv, VL); lz = __riscv_vfmul_vv_f32m2(lz, inv, VL);

    vfloat32m2_t vx = IAK_V(16), vy = IAK_V(17), vz = IAK_V(18);
    s = __riscv_vfmul_vv_f32m2(vx, vx, VL); s = __riscv_vfmacc_vv_f32m2(s, vy, vy, VL); s = __riscv_vfmacc_vv_f32m2(s, vz, vz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    vx = __riscv_vfmul_vv_f32m2(vx, inv, VL); vy = __riscv_vfmul_vv_f32m2(vy, inv, VL); vz = __riscv_vfmul_vv_f32m2(vz, inv, VL);

    vfloat32m2_t hx = __riscv_vfadd_vv_f32m2(lx, vx, VL), hy = __riscv_vfadd_vv_f32m2(ly, vy, VL), hz = __riscv_vfadd_vv_f32m2(lz, vz, VL);
    s = __riscv_vfmul_vv_f32m2(hx, hx, VL); s = __riscv_vfmacc_vv_f32m2(s, hy, hy, VL); s = __riscv_vfmacc_vv_f32m2(s, hz, hz, VL);
    inv = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), sgl_rvv_rsqrt_fast_m2(s, VL), __riscv_vmfgt_vf_f32m2_b16(s, 0.f, VL), VL);
    hx = __riscv_vfmul_vv_f32m2(hx, inv, VL); hy = __riscv_vfmul_vv_f32m2(hy, inv, VL); hz = __riscv_vfmul_vv_f32m2(hz, inv, VL);

    vfloat32m2_t ndl = __riscv_vfmul_vv_f32m2(nx, lx, VL); ndl = __riscv_vfmacc_vv_f32m2(ndl, ny, ly, VL); ndl = __riscv_vfmacc_vv_f32m2(ndl, nz, lz, VL);
    ndl = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), ndl, __riscv_vmfgt_vf_f32m2_b16(ndl, 0.f, VL), VL);
    vfloat32m2_t ndh = __riscv_vfmul_vv_f32m2(nx, hx, VL); ndh = __riscv_vfmacc_vv_f32m2(ndh, ny, hy, VL); ndh = __riscv_vfmacc_vv_f32m2(ndh, nz, hz, VL);
    ndh = __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), ndh, __riscv_vmfgt_vf_f32m2_b16(ndh, 0.f, VL), VL);
    vfloat32m2_t spec = sgl_rvv_pow_sf_m2(ndh, 16.f, VL);

    vfloat32m2_t dr, dg, db, da;
    sgl_rvv_sample2d_m2(samplers[1], IAK_V(4), IAK_V(5), VL, inv255, samplers[1]->min_filter, samplers[1]->wrap, iak_uv_deriv(4, 5, &dvtmp), &dr, &dg, &db, &da);
    dr = __riscv_vfmul_vf_f32m2(dr, dcx, VL); dg = __riscv_vfmul_vf_f32m2(dg, dcy, VL); db = __riscv_vfmul_vf_f32m2(db, dcz, VL);

    vfloat32m2_t sr, sg, sb, sa;
    sgl_rvv_sample2d_m2(samplers[2], IAK_V(4), IAK_V(5), VL, inv255, samplers[2]->min_filter, samplers[2]->wrap, iak_uv_deriv(4, 5, &dvtmp), &sr, &sg, &sb, &sa);
    sr = __riscv_vfmul_vf_f32m2(sr, scx, VL); sg = __riscv_vfmul_vf_f32m2(sg, scy, VL); sb = __riscv_vfmul_vf_f32m2(sb, scz, VL);

    vfloat32m2_t q = IAK_V(11);
    vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
    q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
    vfloat32m2_t pu = __riscv_vfdiv_vv_f32m2(IAK_V(8), q, VL), pv = __riscv_vfdiv_vv_f32m2(IAK_V(9), q, VL);
    vfloat32m2_t lpr, lpg, lpb, lpa;
    /* Projective light coord (pu,pv = t8,t9 / q): no direct varying derivative,
     * pass NULL so the sampler falls back (LOD 0 / prior behaviour). */
    sgl_rvv_sample2d_m2(samplers[3], pu, pv, VL, inv255, samplers[3]->min_filter, samplers[3]->wrap, (const sgl_uv_deriv *)0, &lpr, &lpg, &lpb, &lpa);

    vfloat32m2_t fr, fg, fb, fa;
    sgl_rvv_sample2d_m2(samplers[4], IAK_V(8), IAK_V(9), VL, inv255, samplers[4]->min_filter, samplers[4]->wrap, iak_uv_deriv(8, 9, &dvtmp), &fr, &fg, &fb, &fa);
    vfloat32m2_t lr = __riscv_vfmul_vv_f32m2(lpr, fr, VL), lg = __riscv_vfmul_vv_f32m2(lpg, fg, VL), lb = __riscv_vfmul_vv_f32m2(lpb, fb, VL);

    vfloat32m2_t o0 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(dr, ndl, VL), sr, spec, VL), lr, VL);
    vfloat32m2_t o1 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(dg, ndl, VL), sg, spec, VL), lg, VL);
    vfloat32m2_t o2 = __riscv_vfmul_vv_f32m2(__riscv_vfmacc_vv_f32m2(__riscv_vfmul_vv_f32m2(db, ndl, VL), sb, spec, VL), lb, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], o0, VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], o1, VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], o2, VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], __riscv_vfmv_v_f_f32m2(1.f, VL), VL);
    return 1;
}
#undef IAK_V

static int iak_sunit(const glsl_program *p, int id)
{
    int i = p->kernel_binding[id];
    return i >= 0 ? p->uni[i].unit : -1;
}

/* gl_FragColor = Color * texture2D(s0, TexCoord.xy) — non-projective modulate.
 * Layout: Color@0, TexCoord@4 (vec4, uses .xy). Common ambient/GUI path. */
static int glsl_fs_kernel_modulate2d(glsl_program *p, const float *varys, int vl,
                                     const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                     const float *ustore, float *out)
{
    (void)ustore;
    int su = iak_sunit(p, KB_TexSampler_0);
    const sgl_tex *t = (su >= 0 && su < GLSL_MAX_SAMPLERS) ? samplers[su] : samplers[0];
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    vfloat32m2_t tr, tg, tb, ta;
    sgl_uv_deriv dvm;
    if (t) {
        sgl_rvv_sample2d_m2(t,
            __riscv_vle32_v_f32m2(&varys[4 * SIMT_MAXLANE], VL),
            __riscv_vle32_v_f32m2(&varys[5 * SIMT_MAXLANE], VL),
            VL, 1.f / 255.f, t->min_filter, t->wrap, iak_uv_deriv(4, 5, &dvm), &tr, &tg, &tb, &ta);
    } else {
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
        ta = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[0 * SIMT_MAXLANE], VL), tr, VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[1 * SIMT_MAXLANE], VL), tg, VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[2 * SIMT_MAXLANE], VL), tb, VL), VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[3 * SIMT_MAXLANE], VL), ta, VL), VL);
    return 1;
}

/* FPE: fColor.rgb *= textureCube(s0, tc0.xyz); alpha unchanged from Color. */
static int glsl_fs_kernel_modulate_cube(glsl_program *p, const float *varys, int vl,
                                        const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                        const float *ustore, float *out)
{
    (void)ustore;
    int su = iak_sunit(p, KB_TexSampler_0);
    if (su < 0) su = iak_sunit(p, KB_SamplerCube_0);
    const sgl_tex *t = (su >= 0 && su < GLSL_MAX_SAMPLERS) ? samplers[su] : samplers[0];
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    vfloat32m2_t tr, tg, tb, ta;
    sgl_rvv_sample_cube_m2(t,
        __riscv_vle32_v_f32m2(&varys[4 * SIMT_MAXLANE], VL),
        __riscv_vle32_v_f32m2(&varys[5 * SIMT_MAXLANE], VL),
        __riscv_vle32_v_f32m2(&varys[6 * SIMT_MAXLANE], VL),
        VL, 1.f / 255.f, &tr, &tg, &tb, &ta);
    (void)ta;
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[0 * SIMT_MAXLANE], VL), tr, VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[1 * SIMT_MAXLANE], VL), tg, VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE],
        __riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(&varys[2 * SIMT_MAXLANE], VL), tb, VL), VL);
    /* alpha = Color.a (FPE does fColor.rgb *= cube; not full * texture) */
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE],
        __riscv_vle32_v_f32m2(&varys[3 * SIMT_MAXLANE], VL), VL);
    return 1;
}

/* Shared heatHaze body. vary layout:
 *   plain:     TexCoord_1@0, TexCoord_2@4  (nvaryf=8)
 *   with mask: TexCoord_0@0, TexCoord_1@4, TexCoord_2@8  (nvaryf=12)
 * with_mask=1 samples Sampler2D_2 as mask, KIL if any(mask.xy - 0.01)<0, then
 * multiplies the normal by mask; kill written to out row 4. */
static int heathaze_core(glsl_program *p, const float *varys, int vl,
                         const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                         const float *ustore, float *out, int with_mask)
{
    int s0 = iak_sunit(p, KB_Sampler2D_0);
    int s1 = iak_sunit(p, KB_Sampler2D_1);
    int s2 = with_mask ? iak_sunit(p, KB_Sampler2D_2) : -1;
    const sgl_tex *t0 = (s0 >= 0 && s0 < GLSL_MAX_SAMPLERS) ? samplers[s0] : samplers[0];
    const sgl_tex *t1 = (s1 >= 0 && s1 < GLSL_MAX_SAMPLERS) ? samplers[s1] : samplers[1];
    const sgl_tex *t2 = (s2 >= 0 && s2 < GLSL_MAX_SAMPLERS) ? samplers[s2] : 0;
    int e0 = iak_uoff(p, KB_Fragment_ProgramEnv_0);
    int e1 = iak_uoff(p, KB_Fragment_ProgramEnv_1);
    int e21 = iak_uoff(p, KB_Fragment_ProgramEnv_21);
    if (e0 < 0 || e1 < 0) return 0;
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    const float inv255 = 1.f / 255.f;
    vfloat32m2_t zero = __riscv_vfmv_v_f_f32m2(0.f, VL);
    vfloat32m2_t one = __riscv_vfmv_v_f_f32m2(1.f, VL);

    /* Mask path: sample mask @ TexCoord_0, kill if (mask - 0.01) has any < 0. */
    vfloat32m2_t mx = one, my = one, mz = one, mw = one;
    if (with_mask) {
        int tc0 = 0; /* TexCoord_0 @ float 0 */
        if (t2)
            sgl_rvv_sample2d_m2(t2,
                __riscv_vle32_v_f32m2(&varys[(tc0 + 0) * SIMT_MAXLANE], VL),
                __riscv_vle32_v_f32m2(&varys[(tc0 + 1) * SIMT_MAXLANE], VL),
                VL, inv255, t2->min_filter, t2->wrap, (const sgl_uv_deriv *)0, &mx, &my, &mz, &mw);
        vfloat32m2_t kx = __riscv_vfsub_vf_f32m2(mx, 0.01f, VL);
        vfloat32m2_t ky = __riscv_vfsub_vf_f32m2(my, 0.01f, VL);
        vfloat32m2_t kz = __riscv_vfsub_vf_f32m2(mz, 0.01f, VL);
        vfloat32m2_t kw = __riscv_vfsub_vf_f32m2(mw, 0.01f, VL);
        vbool16_t kill = __riscv_vmor_mm_b16(
            __riscv_vmor_mm_b16(__riscv_vmflt_vf_f32m2_b16(kx, 0.f, VL),
                                __riscv_vmflt_vf_f32m2_b16(ky, 0.f, VL), VL),
            __riscv_vmor_mm_b16(__riscv_vmflt_vf_f32m2_b16(kz, 0.f, VL),
                                __riscv_vmflt_vf_f32m2_b16(kw, 0.f, VL), VL), VL);
        __riscv_vse32_v_f32m2(&out[4 * SIMT_MAXLANE],
            __riscv_vmerge_vvm_f32m2(zero, one, kill, VL), VL);
    }

    /* Normal map @ TexCoord_1: plain off=0, with_mask off=4 */
    int nbase = with_mask ? 4 : 0;
    int t2base = with_mask ? 8 : 4;
    vfloat32m2_t nr, ng, nb, na;
    if (t1)
        sgl_rvv_sample2d_m2(t1,
            __riscv_vle32_v_f32m2(&varys[(nbase + 0) * SIMT_MAXLANE], VL),
            __riscv_vle32_v_f32m2(&varys[(nbase + 1) * SIMT_MAXLANE], VL),
            VL, inv255, t1->min_filter, t1->wrap, (const sgl_uv_deriv *)0, &nr, &ng, &nb, &na);
    else
        nr = ng = nb = na = __riscv_vfmv_v_f_f32m2(0.5f, VL);
    /* .x <- .w then *2-1 */
    vfloat32m2_t nx = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(na, 2.f, VL), -1.f, VL);
    vfloat32m2_t ny = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(ng, 2.f, VL), -1.f, VL);
    vfloat32m2_t nz = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(nb, 2.f, VL), -1.f, VL);
    vfloat32m2_t nw = __riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(na, 2.f, VL), -1.f, VL);
    if (with_mask) {
        nx = __riscv_vfmul_vv_f32m2(nx, mx, VL);
        ny = __riscv_vfmul_vv_f32m2(ny, my, VL);
        nz = __riscv_vfmul_vv_f32m2(nz, mz, VL);
        nw = __riscv_vfmul_vv_f32m2(nw, mw, VL);
    }

    /* Path-A clean-glass fallback: zero the deform normal so R0 collapses to
     * fragCoord*env1 -> _currentRender is sampled UNDISTORTED (clean glass showing
     * the room behind, no shimmer). Diagnostic A/B only; normal refraction is
     * enabled by default after the array-pointer and shared-coordinate fixes. */
    if (!sgl_heathaze_deform) { nx = zero; ny = zero; nz = zero; nw = zero; }

    /* R0 = gl_FragCoord * env1 */
    vfloat32m2_t r0x = __riscv_vfmul_vf_f32m2(__riscv_vle32_v_f32m2(sgl_simt_frag_x, VL), ustore[e1 + 0], VL);
    vfloat32m2_t r0y = __riscv_vfmul_vf_f32m2(__riscv_vle32_v_f32m2(sgl_simt_frag_y, VL), ustore[e1 + 1], VL);
    vfloat32m2_t r0z = __riscv_vfmul_vf_f32m2(zero, ustore[e1 + 2], VL);
    vfloat32m2_t r0w = __riscv_vfmul_vf_f32m2(one, ustore[e1 + 3], VL);

    /* R0 = clamp(localNormal * TexCoord_2 + R0, 0, 1) */
    vfloat32m2_t t2x = __riscv_vle32_v_f32m2(&varys[(t2base + 0) * SIMT_MAXLANE], VL);
    vfloat32m2_t t2y = __riscv_vle32_v_f32m2(&varys[(t2base + 1) * SIMT_MAXLANE], VL);
    vfloat32m2_t t2z = __riscv_vle32_v_f32m2(&varys[(t2base + 2) * SIMT_MAXLANE], VL);
    vfloat32m2_t t2w = __riscv_vle32_v_f32m2(&varys[(t2base + 3) * SIMT_MAXLANE], VL);
    /* separate mul+add to match VM rounding (not fused vfmacc) */
    r0x = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(nx, t2x, VL), r0x, VL);
    r0y = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(ny, t2y, VL), r0y, VL);
    r0z = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(nz, t2z, VL), r0z, VL);
    r0w = __riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(nw, t2w, VL), r0w, VL);
    r0x = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(r0x, 0.f, VL), 1.f, VL);
    r0y = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(r0y, 0.f, VL), 1.f, VL);
    r0z = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(r0z, 0.f, VL), 1.f, VL);
    r0w = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(r0w, 0.f, VL), 1.f, VL);

    r0x = __riscv_vfmul_vf_f32m2(r0x, ustore[e0 + 0], VL);
    r0y = __riscv_vfmul_vf_f32m2(r0y, ustore[e0 + 1], VL);

    vfloat32m2_t cr, cg, cb, ca;
    if (t0)
        sgl_rvv_sample2d_m2(t0, r0x, r0y, VL, inv255, t0->min_filter, t0->wrap, (const sgl_uv_deriv *)0, &cr, &cg, &cb, &ca);
    else {
        cr = cg = cb = zero; ca = one;
    }
    float g0 = e21 >= 0 ? ustore[e21] : 1.f, g1 = e21 >= 0 ? ustore[e21 + 1] : 1.f;
    float g2 = e21 >= 0 ? ustore[e21 + 2] : 1.f, gw = e21 >= 0 ? ustore[e21 + 3] : 1.f;
    cr = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(__riscv_vfmul_vf_f32m2(cr, g0, VL), 0.f, VL), 1.f, VL);
    cg = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(__riscv_vfmul_vf_f32m2(cg, g1, VL), 0.f, VL), 1.f, VL);
    cb = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(__riscv_vfmul_vf_f32m2(cb, g2, VL), 0.f, VL), 1.f, VL);
    if (gw == 1.f) {
        __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], cr, VL);
        __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], cg, VL);
        __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], cb, VL);
    } else {
        __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], sgl_rvv_pow_sf_m2(cr, gw, VL), VL);
        __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], sgl_rvv_pow_sf_m2(cg, gw, VL), VL);
        __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], sgl_rvv_pow_sf_m2(cb, gw, VL), VL);
    }
    (void)ca;
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], one, VL);
    return 1;
}

static int glsl_fs_kernel_heathaze(glsl_program *p, const float *varys, int vl,
                                   const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                   const float *ustore, float *out)
{
    return heathaze_core(p, varys, vl, samplers, ustore, out, 0);
}

static int glsl_fs_kernel_heathaze_mask(glsl_program *p, const float *varys, int vl,
                                        const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                        const float *ustore, float *out)
{
    return heathaze_core(p, varys, vl, samplers, ustore, out, 1);
}

/* FPE: fColor *= tex2DProj0; fColor *= tex2DProj1. */
static int glsl_fs_kernel_dual_proj_mul(glsl_program *p, const float *varys, int vl,
                                        const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                        const float *ustore, float *out)
{
    (void)ustore;
    int s0 = iak_sunit(p, KB_TexSampler_0);
    int s1 = iak_sunit(p, KB_TexSampler_1);
    const sgl_tex *t0 = (s0 >= 0 && s0 < GLSL_MAX_SAMPLERS) ? samplers[s0] : samplers[0];
    const sgl_tex *t1 = (s1 >= 0 && s1 < GLSL_MAX_SAMPLERS) ? samplers[s1] : samplers[1];
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    vfloat32m2_t cr = __riscv_vle32_v_f32m2(&varys[0 * SIMT_MAXLANE], VL);
    vfloat32m2_t cg = __riscv_vle32_v_f32m2(&varys[1 * SIMT_MAXLANE], VL);
    vfloat32m2_t cb = __riscv_vle32_v_f32m2(&varys[2 * SIMT_MAXLANE], VL);
    vfloat32m2_t ca = __riscv_vle32_v_f32m2(&varys[3 * SIMT_MAXLANE], VL);
#define PROJ_SAMPLE(T, UOFF, R, G, B, A) do { \
        vfloat32m2_t q = __riscv_vle32_v_f32m2(&varys[((UOFF)+3) * SIMT_MAXLANE], VL); \
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), \
            __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL), VL); \
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(__riscv_vle32_v_f32m2(&varys[(UOFF) * SIMT_MAXLANE], VL), q, VL); \
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(__riscv_vle32_v_f32m2(&varys[((UOFF)+1) * SIMT_MAXLANE], VL), q, VL); \
        if (T) sgl_rvv_sample2d_m2((T), u, v, VL, 1.f/255.f, (T)->min_filter, (T)->wrap, (const sgl_uv_deriv *)0, &(R),&(G),&(B),&(A)); \
        else { R=G=B=__riscv_vfmv_v_f_f32m2(0.f,VL); A=__riscv_vfmv_v_f_f32m2(1.f,VL); } \
    } while (0)
    vfloat32m2_t r0,g0,b0,a0, r1,g1,b1,a1;
    PROJ_SAMPLE(t0, 4, r0,g0,b0,a0);
    PROJ_SAMPLE(t1, 8, r1,g1,b1,a1);
#undef PROJ_SAMPLE
    cr = __riscv_vfmul_vv_f32m2(__riscv_vfmul_vv_f32m2(cr, r0, VL), r1, VL);
    cg = __riscv_vfmul_vv_f32m2(__riscv_vfmul_vv_f32m2(cg, g0, VL), g1, VL);
    cb = __riscv_vfmul_vv_f32m2(__riscv_vfmul_vv_f32m2(cb, b0, VL), b1, VL);
    ca = __riscv_vfmul_vv_f32m2(__riscv_vfmul_vv_f32m2(ca, a0, VL), a1, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], cr, VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], cg, VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], cb, VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], ca, VL);
    return 1;
}

/* The REAL gl4es-emitted Doom 3 interaction shader (interaction.vfp -> GLSL ES):
 * cubemap normalised-light lookup, bump (DXT5nm: normal.x in .w), N.L, two
 * projected light/falloff textures, diffuse*color, specular via a dependent N.H
 * lookup table * specColor * (specmap*2), then colorScale + per-channel gamma pow.
 * 8 varyings (FrontColor + TexCoord_0..6 = 32 floats), 6x sampler2D + 1 samplerCube,
 * 3 vec4 uniforms (Fragment_ProgramEnv_0/1/21). This is what the menu/intro AND
 * in-game lighting actually run; the classifier's old "interaction" archetype
 * (5 varyings, normalize(), no cube) never matches it, so without this kernel core
 * lighting runs the per-pixel VM at 133 ops/fragment.
 *
 * Bit-exact vs the VM: every op mirrors the VM's IR execution — separate
 * vfmul/vfadd (NOT fused vfmacc) for the shader's * and + (the VM runs each IR op
 * with its own rounding, -ffp-contract=off), vfmul+vfmacc dot order matching
 * sgl_rvv_dot3_v2_m2, sgl_rvv_rsqrt_fast_m2 for inversesqrt, sgl_rvv_pow_m2 for pow,
 * sgl_rvv_sample2d_m2 for 2D/proj, and sgl_rvv_sample_cube_m2 for the cubemap
 * (same face-select as sample_cube_tex; SIMT VM also uses the RVV cube helper). */
#define GIK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
/* Sample a 2D texture, or return the VM's missing-texture default {0,0,0,1} when the
 * sampler is unbound. Bit-exact vs the SIMT VM null path (glsl_simt.c:330-333 writes
 * literal 0/0/0/1; the proj null path returns the same via the scalar fallback). Lets
 * the interaction kernel handle materials lacking spec/bump/etc. instead of bailing to
 * the slow VM. Generic: optional-sampler handling is standard GLES2, not Doom3-specific. */
/* iu/iv name the texcoord's varying floats so the sampler gets this batch's
 * analytic derivatives (mip-LOD + aniso); pass iu=iv=-1 for a dependent or
 * projective/scaled coordinate, which falls back (NULL -> prior behaviour). */
static inline void iak_sample2d_or_default(const sgl_tex *s, vfloat32m2_t u, vfloat32m2_t v,
                                           size_t VL, float inv255, int iu, int iv,
                                           vfloat32m2_t *r, vfloat32m2_t *g, vfloat32m2_t *b, vfloat32m2_t *a)
{
    if (s) {
        sgl_uv_deriv d;
        sgl_rvv_sample2d_m2(s, u, v, VL, inv255, s->min_filter, s->wrap,
                            iak_uv_deriv(iu, iv, &d), r, g, b, a);
    } else {
        vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, VL);
        *r = z; *g = z; *b = z; *a = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }
}

/* When set, glsl_fs_classify refuses to bind the gl4es interaction FS+VS kernels,
 * forcing the reference per-op VM (glsl_simt.c) for Doom3 lighting. Kept as a
 * bisection switch for future interaction-shading differences. 0 = use the kernel. */
int sgl_dbg_disable_iak_kernel = 0;

static int glsl_fs_kernel_interaction_gl4es(glsl_program *p, const float *varys, int vl,
                                            const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                            const float *ustore, float *out)
{
    int uc = iak_sunit(p, KB_SamplerCube_0);
    int u1 = iak_sunit(p, KB_Sampler2D_1), u2 = iak_sunit(p, KB_Sampler2D_2);
    int u3 = iak_sunit(p, KB_Sampler2D_3), u4 = iak_sunit(p, KB_Sampler2D_4);
    int u5 = iak_sunit(p, KB_Sampler2D_5), u6 = iak_sunit(p, KB_Sampler2D_6);
    if (uc < 0 || u1 < 0 || u2 < 0 || u3 < 0 || u4 < 0 || u5 < 0 || u6 < 0) return 0;
    const sgl_tex *sc = (uc < GLSL_MAX_SAMPLERS) ? samplers[uc] : 0;   /* cube may be null */
    const sgl_tex *s1 = samplers[u1], *s2 = samplers[u2], *s3 = samplers[u3];
    const sgl_tex *s4 = samplers[u4], *s5 = samplers[u5], *s6 = samplers[u6];
    /* null 2D samplers are handled per-sample (iak_sample2d_or_default -> {0,0,0,1},
     * matching the VM) instead of bailing, so materials missing spec/bump/etc. stay on
     * the fast kernel. uniform-location check (uc/u1..u6 >= 0) above still gates structure. */

    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);

    if (sgl_light_probe_enabled) {
        __atomic_fetch_add(&sgl_lp[LP_BATCH], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sgl_lp[LP_LANE], (uint64_t)vl, __ATOMIC_RELAXED);
        if (sc && s1 && s2 && s3 && s4 && s5 && s6)
            __atomic_fetch_add(&sgl_lp[LP_ALLTEX], (uint64_t)vl, __ATOMIC_RELAXED);
        const sgl_tex *bt[7]={sc,s1,s2,s3,s4,s5,s6};
        for (int i=0;i<7;++i) if (bt[i]) __atomic_fetch_add(&sgl_lp[LP_SC+i],(uint64_t)vl,__ATOMIC_RELAXED);
    }
    const float inv255 = 1.f / 255.f;
    int o0 = iak_uoff(p, KB_Fragment_ProgramEnv_0);
    int o1 = iak_uoff(p, KB_Fragment_ProgramEnv_1);
    int o21 = iak_uoff(p, KB_Fragment_ProgramEnv_21);
    float d0=o0>=0?ustore[o0]:1.f, d1=o0>=0?ustore[o0+1]:1.f, d2=o0>=0?ustore[o0+2]:1.f, d3=o0>=0?ustore[o0+3]:1.f;
    float p0=o1>=0?ustore[o1]:1.f, p1=o1>=0?ustore[o1+1]:1.f, p2c=o1>=0?ustore[o1+2]:1.f, p3c=o1>=0?ustore[o1+3]:1.f;
    float g0=o21>=0?ustore[o21]:1.f, g1=o21>=0?ustore[o21+1]:1.f, g2=o21>=0?ustore[o21+2]:1.f, gw=o21>=0?ustore[o21+3]:1.f;
    if (sgl_light_probe_enabled) {
        if (d0!=0.f || d1!=0.f || d2!=0.f) __atomic_fetch_add(&sgl_lp[LP_DUNI],(uint64_t)vl,__ATOMIC_RELAXED);
        if (g0!=0.f || g1!=0.f || g2!=0.f) __atomic_fetch_add(&sgl_lp[LP_GUNI],(uint64_t)vl,__ATOMIC_RELAXED);
    }

    /* specular half-angle: rinv = inversesqrt(dot(t6,t6)); h = rinv * t6 */
    vfloat32m2_t t6x=GIK_V(28), t6y=GIK_V(29), t6z=GIK_V(30);
    vfloat32m2_t sd=__riscv_vfmul_vv_f32m2(t6x,t6x,VL);
    sd=__riscv_vfmacc_vv_f32m2(sd,t6y,t6y,VL); sd=__riscv_vfmacc_vv_f32m2(sd,t6z,t6z,VL);
    vfloat32m2_t rinv=__riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f,VL), sgl_rvv_rsqrt_fast_m2(sd,VL), __riscv_vmfgt_vf_f32m2_b16(sd,0.f,VL), VL);
    vfloat32m2_t hx=__riscv_vfmul_vv_f32m2(rinv,t6x,VL), hy=__riscv_vfmul_vv_f32m2(rinv,t6y,VL), hz=__riscv_vfmul_vv_f32m2(rinv,t6z,VL);

    /* light = textureCube(cube, t0.xyz) then *2-1. RVV face-select + sample2d
     * (sgl_rvv_sample_cube_m2) matches sample_cube_tex + sgl_sample bit-for-bit. */
    vfloat32m2_t cr, cg, cb, ca_cube;
    sgl_rvv_sample_cube_m2(sc, GIK_V(4), GIK_V(5), GIK_V(6), VL, inv255, &cr, &cg, &cb, &ca_cube);
    (void)ca_cube;
    vfloat32m2_t lx=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(cr,2.f,VL),-1.f,VL);
    vfloat32m2_t ly=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(cg,2.f,VL),-1.f,VL);
    vfloat32m2_t lz=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(cb,2.f,VL),-1.f,VL);

    /* localNormal = texture2D(bump); .x = .w (DXT5nm); then *2-1 */
    vfloat32m2_t br,bg,bb,ba;
    iak_sample2d_or_default(s1, GIK_V(8), GIK_V(9), VL, inv255, 8, 9, &br,&bg,&bb,&ba);
    vfloat32m2_t nx=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(ba,2.f,VL),-1.f,VL);   /* .x <- .w */
    vfloat32m2_t ny=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(bg,2.f,VL),-1.f,VL);
    vfloat32m2_t nz=__riscv_vfadd_vf_f32m2(__riscv_vfmul_vf_f32m2(bb,2.f,VL),-1.f,VL);

    /* N.L (light vec . normal), broadcast scalar */
    vfloat32m2_t ndl=__riscv_vfmul_vv_f32m2(lx,nx,VL);
    ndl=__riscv_vfmacc_vv_f32m2(ndl,ly,ny,VL); ndl=__riscv_vfmacc_vv_f32m2(ndl,lz,nz,VL);

    /* light *= texture2DProj(s3, t3); light *= texture2DProj(s2, t2)  (q-guarded) */
    vfloat32m2_t q=GIK_V(19); q=__riscv_vmerge_vvm_f32m2(q,__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vmfeq_vf_f32m2_b16(q,0.f,VL),VL);
    /* Skip the projective divide when every lane already has q == 1. x / 1.0f is
     * exact in IEEE-754, so this is bit-identical, and it replaces two vfdiv at
     * LMUL=2 -- among the most expensive non-gather vector ops both on TCG and on
     * hardware -- with a compare and a popcount. The paired vertex kernel writes
     * TexCoord_2.w = 1.0 unconditionally so it fires on every interaction
     * fragment, but keeping the test data-driven means it stays correct if that
     * ever stops being true. */
#define IAK_PROJ_UV(QV, IU, IV, OU, OV) \
    vfloat32m2_t OU, OV; \
    if (__riscv_vcpop_m_b16(__riscv_vmfne_vf_f32m2_b16((QV), 1.f, VL), VL) == 0) { \
        OU = GIK_V(IU); OV = GIK_V(IV); \
    } else { \
        OU = __riscv_vfdiv_vv_f32m2(GIK_V(IU), (QV), VL); \
        OV = __riscv_vfdiv_vv_f32m2(GIK_V(IV), (QV), VL); \
    }
    vfloat32m2_t a3r,a3g,a3b,a3a;
    IAK_PROJ_UV(q, 16, 17, u3_, v3_)
    iak_sample2d_or_default(s3, u3_, v3_, VL, inv255, -1, -1, &a3r,&a3g,&a3b,&a3a);
    vfloat32m2_t lr=__riscv_vfmul_vv_f32m2(ndl,a3r,VL), lg=__riscv_vfmul_vv_f32m2(ndl,a3g,VL), lb=__riscv_vfmul_vv_f32m2(ndl,a3b,VL), la=__riscv_vfmul_vv_f32m2(ndl,a3a,VL);
    q=GIK_V(15); q=__riscv_vmerge_vvm_f32m2(q,__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vmfeq_vf_f32m2_b16(q,0.f,VL),VL);
    vfloat32m2_t a2r,a2g,a2b,a2a;
    IAK_PROJ_UV(q, 12, 13, u2_, v2_)
    iak_sample2d_or_default(s2, u2_, v2_, VL, inv255, -1, -1, &a2r,&a2g,&a2b,&a2a);
#undef IAK_PROJ_UV
    lr=__riscv_vfmul_vv_f32m2(lr,a2r,VL); lg=__riscv_vfmul_vv_f32m2(lg,a2g,VL); lb=__riscv_vfmul_vv_f32m2(lb,a2b,VL); la=__riscv_vfmul_vv_f32m2(la,a2a,VL);

    /* color = diffuse * diffuseColor */
    vfloat32m2_t dr,dg,db,da;
    iak_sample2d_or_default(s4, GIK_V(20), GIK_V(21), VL, inv255, 20, 21, &dr,&dg,&db,&da);
    vfloat32m2_t cr2=__riscv_vfmul_vf_f32m2(dr,d0,VL), cg2=__riscv_vfmul_vf_f32m2(dg,d1,VL), cb2=__riscv_vfmul_vf_f32m2(db,d2,VL), ca2=__riscv_vfmul_vf_f32m2(da,d3,VL);

    /* N.H -> spec LUT(s6) * specColor; R2 = specmap(s5)*2; color = R1*R2 + color.
     * specularColor (Fragment_ProgramEnv_1 = p0..p3c) is a per-DRAW uniform, so when
     * it is zero the entire specular term adds nothing: skip the N.H dot, the spec
     * LUT sample (s6) and the specmap sample (s5) for every fragment of the draw.
     * The branch is draw-invariant -> perfectly predicted; output is bit-identical
     * (adding 0). Saves 2 texture samples + ~12 vector ops/fragment on matte draws. */
    if (p0 != 0.f || p1 != 0.f || p2c != 0.f || p3c != 0.f) {
        vfloat32m2_t ndh=__riscv_vfmul_vv_f32m2(hx,nx,VL);
        ndh=__riscv_vfmacc_vv_f32m2(ndh,hy,ny,VL); ndh=__riscv_vfmacc_vv_f32m2(ndh,hz,nz,VL);
        vfloat32m2_t er,eg,eb,ea;
        iak_sample2d_or_default(s6, ndh, ndh, VL, inv255, -1, -1, &er,&eg,&eb,&ea);
        er=__riscv_vfmul_vf_f32m2(er,p0,VL); eg=__riscv_vfmul_vf_f32m2(eg,p1,VL); eb=__riscv_vfmul_vf_f32m2(eb,p2c,VL); ea=__riscv_vfmul_vf_f32m2(ea,p3c,VL);
        vfloat32m2_t mr,mg,mb,ma;
        iak_sample2d_or_default(s5, GIK_V(24), GIK_V(25), VL, inv255, 24, 25, &mr,&mg,&mb,&ma);
        mr=__riscv_vfadd_vv_f32m2(mr,mr,VL); mg=__riscv_vfadd_vv_f32m2(mg,mg,VL); mb=__riscv_vfadd_vv_f32m2(mb,mb,VL); ma=__riscv_vfadd_vv_f32m2(ma,ma,VL);
        /* color = R1*R2 + color  (separate mul then add: two roundings, matches the VM) */
        cr2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(er,mr,VL),cr2,VL);
        cg2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(eg,mg,VL),cg2,VL);
        cb2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(eb,mb,VL),cb2,VL);
        ca2=__riscv_vfadd_vv_f32m2(__riscv_vfmul_vv_f32m2(ea,ma,VL),ca2,VL);
    }

    if (sgl_light_probe_enabled) {
        float X[SGL_SIMT_LANES], Y[SGL_SIMT_LANES], Z[SGL_SIMT_LANES];
#define LP3(slot, a, b, c) do { \
        __riscv_vse32_v_f32m2(X,(a),VL); __riscv_vse32_v_f32m2(Y,(b),VL); __riscv_vse32_v_f32m2(Z,(c),VL); \
        uint64_t n=0; for (int L=0; L<vl; ++L) if (X[L]!=0.f || Y[L]!=0.f || Z[L]!=0.f) ++n; \
        __atomic_fetch_add(&sgl_lp[(slot)],n,__ATOMIC_RELAXED); \
    } while (0)
        LP3(LP_CUBE,lx,ly,lz); LP3(LP_BUMP,nx,ny,nz);
        __riscv_vse32_v_f32m2(X,ndl,VL);
        uint64_t np=0, nn=0; for (int L=0; L<vl; ++L) { np += X[L]>0.f; nn += X[L]<0.f; }
        __atomic_fetch_add(&sgl_lp[LP_NDLP],np,__ATOMIC_RELAXED);
        __atomic_fetch_add(&sgl_lp[LP_NDLN],nn,__ATOMIC_RELAXED);
        LP3(LP_PROJ,a3r,a3g,a3b); LP3(LP_FALL,a2r,a2g,a2b);
        LP3(LP_DIFF,dr,dg,db); LP3(LP_COLOR,cr2,cg2,cb2); LP3(LP_LIGHT,lr,lg,lb);
        LP3(LP_FRONT,GIK_V(0),GIK_V(1),GIK_V(2));
#undef LP3
    }

    /* color = light * color; tmp = color * FrontColor */
    if (sgl_dbg_iak_light_unity) { lr=lg=lb=la=__riscv_vfmv_v_f_f32m2(1.f,VL); }   /* diag: diffuse-only */
    cr2=__riscv_vfmul_vv_f32m2(lr,cr2,VL); cg2=__riscv_vfmul_vv_f32m2(lg,cg2,VL); cb2=__riscv_vfmul_vv_f32m2(lb,cb2,VL); ca2=__riscv_vfmul_vv_f32m2(la,ca2,VL);
    vfloat32m2_t tr=__riscv_vfmul_vv_f32m2(cr2,GIK_V(0),VL), tg=__riscv_vfmul_vv_f32m2(cg2,GIK_V(1),VL), tb=__riscv_vfmul_vv_f32m2(cb2,GIK_V(2),VL), ta=__riscv_vfmul_vv_f32m2(ca2,GIK_V(3),VL);

    if (sgl_light_probe_enabled) {
        float X[SGL_SIMT_LANES],Y[SGL_SIMT_LANES],Z[SGL_SIMT_LANES];
        __riscv_vse32_v_f32m2(X,tr,VL); __riscv_vse32_v_f32m2(Y,tg,VL); __riscv_vse32_v_f32m2(Z,tb,VL);
        uint64_t n=0; for (int L=0; L<vl; ++L) if (X[L]!=0.f || Y[L]!=0.f || Z[L]!=0.f) ++n;
        __atomic_fetch_add(&sgl_lp[LP_PREOUT],n,__ATOMIC_RELAXED);
    }

    /* rgb = pow(clamp(colorScale.xyz * tmp.xyz, 0, 1), gamma); a = tmp.w (raw).
     * Gamma is a scalar uniform — use sgl_rvv_pow_sf_m2 so r_gamma=2 / identity
     * skip the full log2/exp2 chain (big under TCG). When gw==1 the pow is a
     * no-op after clamp (bit-identical to pow_sf with y=1). */
    vfloat32m2_t one=__riscv_vfmv_v_f_f32m2(1.f,VL), zero=__riscv_vfmv_v_f_f32m2(0.f,VL);
    tr=__riscv_vfmul_vf_f32m2(tr,g0,VL); tg=__riscv_vfmul_vf_f32m2(tg,g1,VL); tb=__riscv_vfmul_vf_f32m2(tb,g2,VL);
    tr=__riscv_vmerge_vvm_f32m2(tr,one,__riscv_vmfgt_vf_f32m2_b16(tr,1.f,VL),VL); tr=__riscv_vmerge_vvm_f32m2(tr,zero,__riscv_vmflt_vf_f32m2_b16(tr,0.f,VL),VL);
    tg=__riscv_vmerge_vvm_f32m2(tg,one,__riscv_vmfgt_vf_f32m2_b16(tg,1.f,VL),VL); tg=__riscv_vmerge_vvm_f32m2(tg,zero,__riscv_vmflt_vf_f32m2_b16(tg,0.f,VL),VL);
    tb=__riscv_vmerge_vvm_f32m2(tb,one,__riscv_vmfgt_vf_f32m2_b16(tb,1.f,VL),VL); tb=__riscv_vmerge_vvm_f32m2(tb,zero,__riscv_vmflt_vf_f32m2_b16(tb,0.f,VL),VL);
    if (gw == 1.f) {
        __riscv_vse32_v_f32m2(&out[0*SIMT_MAXLANE], tr, VL);
        __riscv_vse32_v_f32m2(&out[1*SIMT_MAXLANE], tg, VL);
        __riscv_vse32_v_f32m2(&out[2*SIMT_MAXLANE], tb, VL);
    } else {
        __riscv_vse32_v_f32m2(&out[0*SIMT_MAXLANE], sgl_rvv_pow_sf_m2(tr,gw,VL), VL);
        __riscv_vse32_v_f32m2(&out[1*SIMT_MAXLANE], sgl_rvv_pow_sf_m2(tg,gw,VL), VL);
        __riscv_vse32_v_f32m2(&out[2*SIMT_MAXLANE], sgl_rvv_pow_sf_m2(tb,gw,VL), VL);
    }
    __riscv_vse32_v_f32m2(&out[3*SIMT_MAXLANE], ta, VL);
    return 1;
}
#undef GIK_V

/* 2D overlay (gl4es blit): gl_FragColor = Color * texture2DProj(sampler0, TexCoord_0).
 * Verified layout: vary[0]=Color @0 (vec4), vary[1]=TexCoord_0 @4 (vec4); 1 sampler.
 * Ops MATCH the VM's BI_TEXTURE2DPROJ exactly (q=tc.w, q==0 guard, u=tc.x/q,
 * v=tc.y/q, sample) then the component multiply -> bit-identical to the per-op
 * VM, but register-resident: no IR-op memory spill per fragment. This is THE
 * shader the demo renders every frame, so this kernel always fires in-engine. */
#define OVK_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
static int glsl_fs_kernel_overlay(glsl_program *p, const float *varys, int vl,
                                  const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                  const float *ustore, float *out)
{
    (void)p; (void)ustore;
    const sgl_tex *t = samplers[0];
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);

    vfloat32m2_t tr, tg, tb, ta;
    if (t) {
        vfloat32m2_t q = OVK_V(7);
        vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(OVK_V(4), q, VL);
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(OVK_V(5), q, VL);
        sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, (const sgl_uv_deriv *)0, &tr, &tg, &tb, &ta);
    } else {
        /* Unbound sampler: substitute the VM's missing-texture default {0,0,0,1}
         * (glsl_simt.c BI_TEXTURE2DPROJ null path) instead of bailing to the slow
         * VM, so Color*texProj layers with no bound texture stay on the kernel.
         * out becomes Color*{0,0,0,1} = {0,0,0,Color.a} — bit-identical to the VM. */
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
        ta = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }

    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(0), tr, VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(1), tg, VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(2), tb, VL), VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(3), ta, VL), VL);
    return 1;
}

/* Overlay + gl4es alpha test (the FPE_Shader emits, fsdump proj=1 nvaryf=8 + discard):
 *   fColor = Color * texture2DProj(s0, tc0);
 *   if (floor(fColor.a*255.) <= _gl4es_AlphaRef) discard;
 * Colour is identical to the plain overlay; additionally emits the SIMT kill mask
 * into out row 4 (1 = discard). Matches the VM exactly: BI_FLOOR = vfcvt round-down
 * round-trip, IR_CMP '<=' = vmfle, IR_DISCARD reads the condition register. */
static int glsl_fs_kernel_overlay_atest(glsl_program *p, const float *varys, int vl,
                                        const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                        const float *ustore, float *out)
{
    int su = iak_sunit(p, KB_TexSampler_0);
    const sgl_tex *t = (su >= 0 && su < GLSL_MAX_SAMPLERS) ? samplers[su] : 0;
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);

    vfloat32m2_t tr, tg, tb, ta;
    if (t) {
        vfloat32m2_t q = OVK_V(7);
        vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(OVK_V(4), q, VL);
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(OVK_V(5), q, VL);
        sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, (const sgl_uv_deriv *)0, &tr, &tg, &tb, &ta);
    } else {
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
        ta = __riscv_vfmv_v_f_f32m2(1.f, VL);
    }
    vfloat32m2_t ca = __riscv_vfmul_vv_f32m2(OVK_V(3), ta, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(0), tr, VL), VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(1), tg, VL), VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], __riscv_vfmul_vv_f32m2(OVK_V(2), tb, VL), VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], ca, VL);

    /* discard mask: floor(ca*255.) <= AlphaRef  ->  out[4] = 1 (kill) else 0 */
    int aoff = iak_uoff(p, KB_AlphaRef);
    float aref = aoff >= 0 ? ustore[aoff] : 0.f;
    vfloat32m2_t a255 = __riscv_vfmul_vf_f32m2(ca, 255.f, VL);
    vfloat32m2_t afl = __riscv_vfcvt_f_x_v_f32m2(
        __riscv_vfcvt_x_f_v_i32m2_rm(a255, __RISCV_FRM_RDN, VL), VL);
    vbool16_t kill = __riscv_vmfle_vf_f32m2_b16(afl, aref, VL);
    __riscv_vse32_v_f32m2(&out[4 * SIMT_MAXLANE],
        __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL),
                                 __riscv_vfmv_v_f_f32m2(1.f, VL), kill, VL), VL);
    return 1;
}

/* gl4es 2-stage texture-env combine (fsdump proj=2 nvaryf=12, no discard):
 *   texColor0 = texture2DProj(s0, tc0);   // texColor1 (s1) is sampled-but-unused
 *   fColor.rgb = Color.rgb * texColor0.rgb;  fColor.a = Color.a;
 *   fColor *= _gl4es_TextureEnvColor_1;   // vec4 uniform, componentwise
 *   gl_FragColor = clamp(fColor, 0., 1.);
 * texColor1 is dead in the gl4es output, so it's omitted (bit-identical result). */
#define I8K_V(f) __riscv_vle32_v_f32m2(&varys[(f) * SIMT_MAXLANE], VL)
static int glsl_fs_kernel_envcombine(glsl_program *p, const float *varys, int vl,
                                     const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                                     const float *ustore, float *out)
{
    int su = iak_sunit(p, KB_TexSampler_0);
    const sgl_tex *t = (su >= 0 && su < GLSL_MAX_SAMPLERS) ? samplers[su] : 0;
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    int eoff = iak_uoff(p, KB_TextureEnvColor_1);
    float er = eoff >= 0 ? ustore[eoff]     : 1.f, eg = eoff >= 0 ? ustore[eoff + 1] : 1.f;
    float eb = eoff >= 0 ? ustore[eoff + 2] : 1.f, ea = eoff >= 0 ? ustore[eoff + 3] : 1.f;

    vfloat32m2_t tr, tg, tb;
    if (t) {
        vfloat32m2_t q = I8K_V(7);
        vbool16_t qz = __riscv_vmfeq_vf_f32m2_b16(q, 0.f, VL);
        q = __riscv_vmerge_vvm_f32m2(q, __riscv_vfmv_v_f_f32m2(1.f, VL), qz, VL);
        vfloat32m2_t u = __riscv_vfdiv_vv_f32m2(I8K_V(4), q, VL);
        vfloat32m2_t v = __riscv_vfdiv_vv_f32m2(I8K_V(5), q, VL);
        vfloat32m2_t ta;
        sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, (const sgl_uv_deriv *)0, &tr, &tg, &tb, &ta);
        (void)ta;
    } else {
        tr = tg = tb = __riscv_vfmv_v_f_f32m2(0.f, VL);
    }
    /* fColor.rgb = (Color.rgb * tc0.rgb) * Env.rgb;  fColor.a = Color.a * Env.a (two
     * separate muls each -> matches the VM's BIN op rounding), then clamp [0,1]. */
    vfloat32m2_t cr = __riscv_vfmul_vf_f32m2(__riscv_vfmul_vv_f32m2(I8K_V(0), tr, VL), er, VL);
    vfloat32m2_t cg = __riscv_vfmul_vf_f32m2(__riscv_vfmul_vv_f32m2(I8K_V(1), tg, VL), eg, VL);
    vfloat32m2_t cb = __riscv_vfmul_vf_f32m2(__riscv_vfmul_vv_f32m2(I8K_V(2), tb, VL), eb, VL);
    vfloat32m2_t ca = __riscv_vfmul_vf_f32m2(I8K_V(3), ea, VL);
    cr = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cr, 0.f, VL), 1.f, VL);
    cg = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cg, 0.f, VL), 1.f, VL);
    cb = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(cb, 0.f, VL), 1.f, VL);
    ca = __riscv_vfmin_vf_f32m2(__riscv_vfmax_vf_f32m2(ca, 0.f, VL), 1.f, VL);
    __riscv_vse32_v_f32m2(&out[0 * SIMT_MAXLANE], cr, VL);
    __riscv_vse32_v_f32m2(&out[1 * SIMT_MAXLANE], cg, VL);
    __riscv_vse32_v_f32m2(&out[2 * SIMT_MAXLANE], cb, VL);
    __riscv_vse32_v_f32m2(&out[3 * SIMT_MAXLANE], ca, VL);
    return 1;
}
#undef I8K_V
#undef OVK_V

/* Live varying-float slots read by glsl_fs_kernel_interaction_gl4es, spelled out
 * rather than as a hex constant so it can be diffed against the kernel's
 * GIK_V(n) uses by eye. Keep sorted. */
#define GIK_LIVE_SLOTS \
    L(0) L(1) L(2) L(3) L(4) L(5) L(6) L(8) L(9) L(12) L(13) L(15) \
    L(16) L(17) L(19) L(20) L(21) L(24) L(25) L(28) L(29) L(30)
#define L(n) | (1u << (n))
#define GIK_LIVE_MASK (0u GIK_LIVE_SLOTS)

void glsl_fs_classify(glsl_program *p)
{
    iak_bind_metadata(p);
    p->fs_kernel = 0;
    p->vs_kernel = 0;
    p->vary_live_mask = 0;
    p->dbg_kernel_name = "VM";
    p->fs_alpha_zero_if_color_zero = 0;

    /* Classify the vertex half independently. Previously this happened only
     * inside fragment archetype matches, so a non-SIMT fragment shader could
     * prevent its common gl4es MVP/colour/texcoord vertex shader from binding a
     * VS kernel. Those indexed draws then spent ~98% of vertex time in the
     * scalar hash-cache path. Names + uniform/varying layout form the gl4es ABI;
     * each kernel still validates every required location when invoked. */
    /* Do not classify the 32-float interaction VS from the vertex ABI alone.
     * Doom's ARB2 paths share that broad attribute/env layout with depth and
     * non-interaction programs whose varying semantics differ.  Binding the
     * interaction kernel to one of those programs corrupts the depth pre-pass,
     * so the real interaction draw subsequently produces no surviving
     * fragments.  The exact fragment fingerprint below remains the safe place
     * to bind the interaction VS+FS pair. */
    if (iak_uses_texgen(p)) {
        /* texgen -> VM VS only (see iak_uses_texgen). Leaves vs_kernel = 0. */
    } else if (p->nvaryfloats == 8 &&
             iak_aloc(p, KB_Vertex) >= 0 &&
             iak_aloc(p, KB_Color) >= 0 &&
             iak_aloc(p, KB_MultiTexCoord0) >= 0 &&
             iak_uoff(p, KB_ModelViewProjectionMatrix) >= 0 &&
             iak_uoff(p, KB_TextureMatrix_0) >= 0)
        p->vs_kernel = glsl_vs_kernel_overlay_gl4es;
    else if (iak_aloc(p, KB_Vertex) >= 0 &&
             iak_uoff(p, KB_ModelViewProjectionMatrix) >= 0 &&
             /* Depth / solid: no multi-texcoord ABI (those need overlay VS). */
             iak_aloc(p, KB_MultiTexCoord0) < 0 &&
             iak_aloc(p, KB_VertexAttrib_8) < 0)
        p->vs_kernel = glsl_vs_kernel_mvp_gl4es;

    const ir_prog *ir = &p->fs_ir;
    if (!ir->ok || !ir->simt_ok) return;
    int t2 = 0, tp = 0, tc = 0, nm = 0, pw = 0;
    for (int i = 0; i < ir->ninst; ++i) {
        const ir_inst *o = &ir->inst[i];
        if (o->op != IR_BUILTIN) continue;
        switch (o->i0) { case BI_TEXTURE2D: t2++; break; case BI_TEXTURE2DPROJ: tp++; break;
                         case BI_TEXTURECUBE: tc++; break;
                         case BI_NORMALIZE: nm++; break; case BI_POW: pw++; break; default: break; }
    }
    /* interaction archetype: 5 vec4 varyings @0/4/8/12/16, >=4 textures, >=3 normalize, >=1 pow */
    if ((t2 + tp) >= 4 && nm >= 3 && pw >= 1 && p->nvary == 5 && p->nvaryfloats == 20 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 && p->vary[4].offset == 16) {
        p->fs_kernel = glsl_fs_kernel_interaction;
        p->dbg_kernel_name = "interaction";
        return;
    }
    /* gl4es interaction archetype (the REAL Doom 3 lighting shader as gl4es emits
     * it): 8 varyings (FrontColor + TexCoord_0..6 @0..28), 4 tex2D + 2 proj + 1
     * cube + 3 pow — cube normalised-light, dual projected falloff, bump (DXT5nm),
     * diffuse, spec LUT, gamma. Uses inversesqrt(dot) so normalize-count is 0. */
    if (t2 == 4 && tp == 2 && tc == 1 && pw == 3 &&
        p->nvary == 8 && p->nvaryfloats == 32 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 && p->vary[7].offset == 28) {
        if (sgl_dbg_disable_iak_kernel) {           /* TEMP DIAGNOSTIC: force VM path */
            p->fs_kernel = 0; p->vs_kernel = 0; p->vary_live_mask = 0;
            p->dbg_kernel_name = "VM(iak-forced)";
            return;
        }
        p->fs_kernel = glsl_fs_kernel_interaction_gl4es;
        /* Exact FS fingerprint plus the kernel's live attribute-ABI checks make
         * this a safe paired specialisation. */
        p->vs_kernel = glsl_vs_kernel_interaction_gl4es;
        p->dbg_kernel_name = "interaction_gl4es";
        /* WARNING: this must list exactly the GIK_V(n) slots
         * glsl_fs_kernel_interaction_gl4es reads. It declares 32 varying floats
         * and reads these 18; the other 14 were being perspective-interpolated
         * every batch and thrown away. If you add a GIK_V() read to that kernel
         * you MUST add its slot here, or it will silently sample a stale lane
         * from the previous batch. sgl_vary_liveness_enabled=0 disables the
         * whole optimisation if you need to bisect a rendering difference. */
        p->vary_live_mask = GIK_LIVE_MASK;
        return;
    }
    /* overlay archetype: exactly one texture2DProj, no other builtins; 2 vec4
     * varyings Color@0 + TexCoord_0@4. (gl4es 2D blit, fsdump: proj=1 nvaryf=8.)
     * The alpha-test variant (same fingerprint + discard) must NOT match here — it
     * needs the discard kernel below, and binding the plain overlay would drop the
     * alpha test AND leave the kill-mask row uninitialised. */
    if (tp == 1 && t2 == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvary == 2 && p->nvaryfloats == 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4) {
        p->fs_kernel = glsl_fs_kernel_overlay;
        if (!iak_uses_texgen(p)) p->vs_kernel = glsl_vs_kernel_overlay_gl4es;
        p->dbg_kernel_name = "overlay";
        /* gl_FragColor = Color * texture2DProj(...): out.a = Color.a * tex.a, so
         * out.a == 0 wherever Color.a (vary[3]) == 0 -> fully-transparent-layer skip. */
        p->fs_alpha_zero_if_color_zero = 1;
        return;
    }
    /* overlay + gl4es alpha test: same fingerprint as overlay but WITH discard. */
    if (tp == 1 && t2 == 0 && nm == 0 && pw == 0 && p->fs_has_discard &&
        p->nvary == 2 && p->nvaryfloats == 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4) {
        p->fs_kernel = glsl_fs_kernel_overlay_atest;
        if (!iak_uses_texgen(p)) p->vs_kernel = glsl_vs_kernel_overlay_gl4es;
        p->dbg_kernel_name = "overlay_atest";
        return;
    }
    /* gl4es 2-stage env-combine: 2 texture2DProj (one dead), no other builtins, no
     * discard; 3 vec4 varyings Color@0 + TexCoord_0@4 + TexCoord_1@8 (proj=2 nvaryf=12).
     * REQUIRE _gl4es_TextureEnvColor_1: this kernel implements fColor = Color*tex0.rgb *
     * EnvColor_1 with alpha = Color.a*EnvColor_1.a (it drops tex0.a and never samples
     * tex1). The fog shader shares the same coarse fingerprint but computes
     * fColor = Color*tex0*tex1 (full vec4) with NO EnvColor_1 -- its distance gradient
     * lives in tex0.a, which this kernel would discard, hardcoding fog opacity to 1.0
     * (opaque amber everywhere). Gating on EnvColor_1 keeps fog on the correct VM path. */
    if (tp == 2 && t2 == 0 && tc == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        iak_uoff(p, KB_TextureEnvColor_1) >= 0 &&
        p->nvary == 3 && p->nvaryfloats == 12 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4 &&
        p->vary[2].comp == 4 && p->vary[2].offset == 8) {
        p->fs_kernel = glsl_fs_kernel_envcombine;
        /* TexCoord_0 still comes from TextureMatrix0*MultiTexCoord0; TexCoord_1
         * is unused by the kernel (dead sample), so overlay VS is correct --
         * unless texgen supplies the coord, in which case only the VM VS can. */
        if (!iak_uses_texgen(p) &&
            iak_aloc(p, KB_MultiTexCoord0) >= 0 &&
            iak_uoff(p, KB_TextureMatrix_0) >= 0)
            p->vs_kernel = glsl_vs_kernel_overlay_gl4es;
        p->dbg_kernel_name = "envcombine";
        return;
    }
    /* Colour * texture2D (non-proj modulate): one tex2D, Color@0 + TexCoord@4. */
    if (t2 == 1 && tp == 0 && tc == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvary == 2 && p->nvaryfloats == 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4) {
        p->fs_kernel = glsl_fs_kernel_modulate2d;
        if (!iak_uses_texgen(p) &&
            iak_aloc(p, KB_MultiTexCoord0) >= 0 &&
            iak_uoff(p, KB_TextureMatrix_0) >= 0)
            p->vs_kernel = glsl_vs_kernel_overlay_gl4es;
        p->dbg_kernel_name = "modulate2d";
        p->fs_alpha_zero_if_color_zero = 1;
        return;
    }
    /* FPE: Color.rgb *= textureCube(s0, tc0); alpha from Color. */
    if (tc == 1 && t2 == 0 && tp == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvaryfloats >= 7 && p->nvaryfloats <= 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0) {
        p->fs_kernel = glsl_fs_kernel_modulate_cube;
        if (iak_aloc(p, KB_Vertex) >= 0 &&
            iak_uoff(p, KB_ModelViewProjectionMatrix) >= 0)
            p->vs_kernel = glsl_vs_kernel_mvp_gl4es;
        p->dbg_kernel_name = "modulate_cube";
        return;
    }
    /* heatHaze: 2 tex2D, 3 pow (gamma), TexCoord_1@0 + TexCoord_2@4, env0/1/21. */
    if (t2 == 2 && tp == 0 && tc == 0 && nm == 0 && pw == 3 && !p->fs_has_discard &&
        p->nvaryfloats == 8 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4 &&
        iak_uoff(p, KB_Fragment_ProgramEnv_0) >= 0 &&
        iak_uoff(p, KB_Fragment_ProgramEnv_1) >= 0 &&
        iak_sunit(p, KB_Sampler2D_0) >= 0 &&
        iak_sunit(p, KB_Sampler2D_1) >= 0) {
        p->fs_kernel = glsl_fs_kernel_heathaze;
        p->dbg_kernel_name = "heathaze";
        return;
    }
    /* heatHazeWithMask: +mask sample + discard, 3 tex2D, nvaryf=12. */
    if (t2 == 3 && tp == 0 && tc == 0 && nm == 0 && pw == 3 && p->fs_has_discard &&
        p->nvaryfloats == 12 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4 &&
        p->vary[2].comp == 4 && p->vary[2].offset == 8 &&
        iak_uoff(p, KB_Fragment_ProgramEnv_0) >= 0 &&
        iak_uoff(p, KB_Fragment_ProgramEnv_1) >= 0 &&
        iak_sunit(p, KB_Sampler2D_0) >= 0 &&
        iak_sunit(p, KB_Sampler2D_1) >= 0 &&
        iak_sunit(p, KB_Sampler2D_2) >= 0) {
        p->fs_kernel = glsl_fs_kernel_heathaze_mask;
        p->dbg_kernel_name = "heathaze_mask";
        return;
    }
    /* FPE: fColor *= tex2DProj0; fColor *= tex2DProj1 (full multiply both).
     * Distinct from envcombine (which multiplies EnvColor after one live sample). */
    if (tp == 2 && t2 == 0 && tc == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvary == 3 && p->nvaryfloats == 12 &&
        p->vary[0].comp == 4 && p->vary[0].offset == 0 &&
        p->vary[1].comp == 4 && p->vary[1].offset == 4 &&
        p->vary[2].comp == 4 && p->vary[2].offset == 8 &&
        iak_uoff(p, KB_TextureEnvColor_1) < 0) {
        p->fs_kernel = glsl_fs_kernel_dual_proj_mul;
        /* Fog derives TexCoord_0/1 from glTexGen OBJECT_PLANE -> VM VS only. */
        if (!iak_uses_texgen(p) &&
            iak_aloc(p, KB_MultiTexCoord0) >= 0 &&
            iak_uoff(p, KB_TextureMatrix_0) >= 0)
            p->vs_kernel = glsl_vs_kernel_overlay_gl4es;
        p->dbg_kernel_name = "dual_proj_mul";
        p->fs_alpha_zero_if_color_zero = 1;
        return;
    }
    /* Colour-only FS: no textures / normalize / pow / discard. gl_FragColor =
     * Color (or a pure move of the first varying). Safe when IR has no BUILTIN
     * ops and at least one vec4 colour varying. */
    if (t2 == 0 && tp == 0 && tc == 0 && nm == 0 && pw == 0 && !p->fs_has_discard &&
        p->nvaryfloats >= 4 && p->vary[0].comp == 4 && p->vary[0].offset == 0) {
        int only_simple = 1;
        for (int i = 0; i < ir->ninst; ++i) {
            int op = ir->inst[i].op;
            if (op == IR_BUILTIN || op == IR_BIN || op == IR_CMP || op == IR_TERN ||
                op == IR_NEG || op == IR_NOT || op == IR_INDEX || op == IR_CONSTRUCT) {
                only_simple = 0; break;
            }
        }
        if (only_simple) {
            p->fs_kernel = glsl_fs_kernel_color;
            if (!p->vs_kernel && iak_aloc(p, KB_Vertex) >= 0 &&
                iak_uoff(p, KB_ModelViewProjectionMatrix) >= 0)
                p->vs_kernel = glsl_vs_kernel_mvp_gl4es;
            p->dbg_kernel_name = "color";
            return;
        }
    }
}

/* Only these kernels consume analytic varying derivatives. The VM and other
 * kernels pass NULL to the sampler for dependent/projected coordinates. */
unsigned int glsl_fs_derivative_mask(const glsl_program *p)
{
    if (p->fs_kernel == glsl_fs_kernel_interaction_gl4es)
        return (3u << 8) | (3u << 20) | (3u << 24);
    if (p->fs_kernel == glsl_fs_kernel_interaction) return 3u | (3u << 4);
    if (p->fs_kernel == glsl_fs_kernel_modulate2d) return 3u << 4;
    return 0;
}

#else  /* host / non-RVV: no register-resident kernels, the reference VM runs */
unsigned int glsl_fs_derivative_mask(const glsl_program *p) { (void)p; return 0; }
void glsl_fs_classify(glsl_program *p) { p->fs_kernel = 0; p->vs_kernel = 0; p->dbg_kernel_name = "VM"; p->fs_alpha_zero_if_color_zero = 0; }
#endif
