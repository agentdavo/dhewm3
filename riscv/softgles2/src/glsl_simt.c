/*
 * softgles2 — GLSL ES 1.00 SIMT-on-RVV executor (A3). Extracted from glsl.c.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#endif

#include "glsl_internal.h"
#include "sg_diag.h"
#include "sg_layout.h"
#include "sg_math.h"
#include "sg_raster.h"

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define GLSL_SIMT_RVV 1
#else
#  define GLSL_SIMT_RVV 0
#endif
#define SIMT_MAXLANE GLSL_SIMT_MAXLANE

/* SoA register file row. For vec1..4 each c[k] is a contiguous SIMT_MAXLANE-float
 * lane row (component k). A mat4 is a per-lane-IDENTICAL uniform, so instead of
 * spreading 16 components across lanes we pack its 16 col-major scalars linearly
 * into c[0][0..15] (SIMT_MAXLANE==16 holds exactly 16) and flag is_mat — the
 * mat*vec kernel then broadcasts those scalars with vfmacc.vf, no SoA storage and
 * no struct growth. */
typedef sg_simt_reg_soa simt_reg;

/* texture2d sample counters: referenced by sg_diag.c on every build, so defined
 * unconditionally (the scalar sampler path increments _scalar on host too). */
volatile unsigned long sgl_dbg_tex2d_rvv = 0;
volatile unsigned long sgl_dbg_tex2d_scalar = 0;

#if GLSL_SIMT_RVV
int glsl_rvv_texture2d_enabled = 1;   /* RVV gather sampler: bit-exact vs scalar, ~40% faster */
#ifdef SGL_GATHER_RUNTIME_TOGGLE
int sgl_rvv_coherent_gather_enabled = 1;  /* coherent-row vle32 fast path (Lever 2); bit-identical.
                                           * Runtime A/B only for overlay_validate; the shipping build
                                           * folds it to a compile-time 1 (RVV audit item 4). */
#endif
int sgl_rvv_mag_downgrade_enabled = 0;    /* nearest for magnified bilinear (Lever 5); visual, not bit-exact -> opt-in */

typedef struct {
    const float *in_vary;
    const float *in_attr;
    const int   *in_attr_map;
    const sgl_tex *const *samplers;
    float *out_frag;
    float *out_clip;
    float *out_vary;
    float *out_discard;   /* if non-NULL, IR_DISCARD writes a per-lane kill mask here (1=discard) */
} simt_ctx;

static inline void simt_copy_row(float *dst, const float *src, size_t VL)
{
    __riscv_vse32_v_f32m2(dst, __riscv_vle32_v_f32m2(src, VL), VL);
}

static void __attribute__((noinline))
simt_builtin_scalar_fallback(const ir_inst *o, int vl, simt_reg *reg,
                             unsigned char *rn,
                             const sgl_tex *const *samplers)
{
    for (int L = 0; L < vl; L++) {
        gval ar[8];
        for (int ai = 0; ai < o->nargs; ai++) {
            int s = o->args[ai];
            int n = rn[s];
            ar[ai].n = n;
            for (int i = 0; i < n; i++) ar[ai].v[i] = reg[s].c[i][L];
        }
        gval r = ir_do_builtin(o->i0, ar, o->nargs, samplers);
        rn[o->dst] = r.n;
        for (int i = 0; i < r.n; i++) reg[o->dst].c[i][L] = r.v[i];
    }
}

static void simt_run_ir(glsl_program *p, ir_prog *ir, simt_ctx *cx, int vl,
                        simt_reg *reg, unsigned char *rn, unsigned char *rmat, size_t VL,
                        const float *ustore)
{
    if (!ustore) ustore = p->ustore;
    const float *varys = cx->in_vary;
    const sgl_tex *const *samplers = cx->samplers;
    for (int ip=0; ip<ir->ninst; ++ip) {
        ir_inst *o=&ir->inst[ip];
        if (o->dst>=0 && o->dst<IR_MAXREG && o->op!=IR_LOADU) rmat[o->dst]=0;
        switch (o->op) {
        case IR_LOADK: { rn[o->dst]=1; __riscv_vse32_v_f32m2(reg[o->dst].c[0], __riscv_vfmv_v_f_f32m2(o->k,VL), VL); break; }
        case IR_LOADZ: { rn[o->dst]=o->i0; vfloat32m2_t z=__riscv_vfmv_v_f_f32m2(0.f,VL); for(int i=0;i<o->i0;i++)__riscv_vse32_v_f32m2(reg[o->dst].c[i],z,VL); break; }
        case IR_LOADV: { rn[o->dst]=o->i1; for(int i=0;i<o->i1;i++)__riscv_vse32_v_f32m2(reg[o->dst].c[i], __riscv_vle32_v_f32m2(&varys[(o->i0+i)*SIMT_MAXLANE],VL), VL); break; }
        case IR_LOADU: { int off=p->uni[o->i0].offset; rmat[o->dst]=0;
            if (o->i1>4) {
                rn[o->dst]=o->i1; rmat[o->dst]=1; for(int i=0;i<o->i1&&i<SIMT_MAXLANE;i++)reg[o->dst].c[0][i]=ustore[off+i]; break; }
            rn[o->dst]=o->i1; for(int i=0;i<o->i1;i++)__riscv_vse32_v_f32m2(reg[o->dst].c[i], __riscv_vfmv_v_f_f32m2(ustore[off+i],VL), VL); break; }
        case IR_LOADS: { rn[o->dst]=1; __riscv_vse32_v_f32m2(reg[o->dst].c[0], __riscv_vfmv_v_f_f32m2((float)p->uni[o->i0].unit,VL), VL); break; }
        case IR_SWZ:  { rn[o->dst]=o->swzn; rmat[o->dst]=0; for(int i=0;i<o->swzn;i++)simt_copy_row(reg[o->dst].c[i],reg[o->a].c[o->swz[i]],VL); break; }
        case IR_NEG:  { rn[o->dst]=rn[o->a]; for(int i=0;i<rn[o->a];i++)__riscv_vse32_v_f32m2(reg[o->dst].c[i], __riscv_vfneg_v_f32m2(__riscv_vle32_v_f32m2(reg[o->a].c[i],VL),VL),VL); break; }
        case IR_NOT:  { rn[o->dst]=1; vfloat32m2_t a=__riscv_vle32_v_f32m2(reg[o->a].c[0],VL); vbool16_t m=__riscv_vmfeq_vf_f32m2_b16(a,0.f,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[0], __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f,VL),__riscv_vfmv_v_f_f32m2(1.f,VL),m,VL),VL); break; }
        case IR_CMP:  { vfloat32m2_t a=__riscv_vle32_v_f32m2(reg[o->a].c[0],VL), b=__riscv_vle32_v_f32m2(reg[o->b].c[0],VL); vbool16_t m;
            switch(o->i0){case '<':m=__riscv_vmflt_vv_f32m2_b16(a,b,VL);break;case '>':m=__riscv_vmfgt_vv_f32m2_b16(a,b,VL);break;case 'L':m=__riscv_vmfle_vv_f32m2_b16(a,b,VL);break;case 'G':m=__riscv_vmfge_vv_f32m2_b16(a,b,VL);break;case 'E':m=__riscv_vmfeq_vv_f32m2_b16(a,b,VL);break;default:m=__riscv_vmfne_vv_f32m2_b16(a,b,VL);break;}
            rn[o->dst]=1; __riscv_vse32_v_f32m2(reg[o->dst].c[0], __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f,VL),__riscv_vfmv_v_f_f32m2(1.f,VL),m,VL),VL); break; }
        case IR_TERN: { vbool16_t m=__riscv_vmfne_vf_f32m2_b16(__riscv_vle32_v_f32m2(reg[o->a].c[0],VL),0.f,VL); int rcount=rn[o->b]>rn[o->c]?rn[o->b]:rn[o->c]; rn[o->dst]=rcount;
            for(int i=0;i<rcount;i++){ vfloat32m2_t tb=__riscv_vle32_v_f32m2(reg[o->b].c[i<rn[o->b]?i:0],VL), tc=__riscv_vle32_v_f32m2(reg[o->c].c[i<rn[o->c]?i:0],VL); __riscv_vse32_v_f32m2(reg[o->dst].c[i], __riscv_vmerge_vvm_f32m2(tc,tb,m,VL),VL);} break; }
        case IR_BIN:  {
            if (o->i0=='*' && rmat[o->a] && rn[o->a]==16) {
                const float *M=reg[o->a].c[0]; simt_reg *v=&reg[o->b]; rn[o->dst]=4; rmat[o->dst]=0;
                sgl_rvv_mat4xvec4_m2(&reg[o->dst].c[0], &reg[o->dst].c[1], &reg[o->dst].c[2], &reg[o->dst].c[3], M,
                                    __riscv_vle32_v_f32m2(v->c[0], VL), __riscv_vle32_v_f32m2(v->c[1], VL),
                                    __riscv_vle32_v_f32m2(v->c[2], VL), __riscv_vle32_v_f32m2(v->c[3], VL), VL);
                break; }
            if (o->i0=='*' && rmat[o->a] && rn[o->a]==9) {
                const float *M=reg[o->a].c[0]; simt_reg *v=&reg[o->b]; rn[o->dst]=3; rmat[o->dst]=0;
                sgl_rvv_mat3xvec3_m2(&reg[o->dst].c[0], &reg[o->dst].c[1], &reg[o->dst].c[2], M,
                                    __riscv_vle32_v_f32m2(v->c[0], VL), __riscv_vle32_v_f32m2(v->c[1], VL),
                                    __riscv_vle32_v_f32m2(v->c[2], VL), VL);
                break; }
            rmat[o->dst]=0;
            int n1=rn[o->a],n2=rn[o->b],rcount=n1>n2?n1:n2; rn[o->dst]=rcount;
            for(int i=0;i<rcount;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(reg[o->a].c[n1==1?0:i],VL), y=__riscv_vle32_v_f32m2(reg[o->b].c[n2==1?0:i],VL), r;
                switch(o->i0){case '+':r=__riscv_vfadd_vv_f32m2(x,y,VL);break;case '-':r=__riscv_vfsub_vv_f32m2(x,y,VL);break;case '*':r=__riscv_vfmul_vv_f32m2(x,y,VL);break;
                    default:{ vbool16_t z=__riscv_vmfeq_vf_f32m2_b16(y,0.f,VL); r=__riscv_vfdiv_vv_f32m2(x,y,VL); r=__riscv_vmerge_vvm_f32m2(r,__riscv_vfmv_v_f_f32m2(0.f,VL),z,VL);}break; }
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],r,VL);} break; }
        case IR_CONSTRUCT: { int want=o->i0; rn[o->dst]=want; int comps[16]; int srcr[16]; int fn=0;
            for(int ai=0;ai<o->nargs&&fn<16;ai++){ int s=o->args[ai]; for(int j=0;j<rn[s]&&fn<16;j++){srcr[fn]=s;comps[fn]=j;fn++;} }
            if(fn==1){ vfloat32m2_t v=__riscv_vle32_v_f32m2(reg[srcr[0]].c[comps[0]],VL); for(int i=0;i<want;i++)__riscv_vse32_v_f32m2(reg[o->dst].c[i],v,VL); }
            else { for(int i=0;i<want;i++){ if(i<fn) simt_copy_row(reg[o->dst].c[i],reg[srcr[i]].c[comps[i]],VL); else { vfloat32m2_t z=__riscv_vfmv_v_f_f32m2(0.f,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[i],z,VL);} } } break; }
        case IR_STOREL: { simt_reg *d=&reg[o->dst], *v=&reg[o->a]; int vn=rn[o->a], dn=rn[o->dst];
            if(o->swzn){ for(int i=0;i<o->swzn;i++) simt_copy_row(d->c[(int)o->swz[i]], v->c[vn==1?0:i], VL); if(dn<4)rn[o->dst]=4; }
            else { rn[o->dst]=vn; for(int i=0;i<vn;i++)simt_copy_row(d->c[i],v->c[i],VL); if(o->i0==-2&&o->i1>rn[o->dst])rn[o->dst]=o->i1; } break; }
        case IR_STOREF: { simt_reg *v=&reg[o->a]; float *out=cx->out_frag;
            int vn=rn[o->a];
            if(o->swzn){ for(int i=0;i<o->swzn;i++) simt_copy_row(&out[((int)o->swz[i])*SIMT_MAXLANE], v->c[vn==1?0:i], VL); }
            else { for(int i=0;i<vn&&i<4;i++) simt_copy_row(&out[i*SIMT_MAXLANE], v->c[i], VL); } break; }
        case IR_LOADA: {
            int loc = o->i0;
            if (cx->in_attr_map) {
                loc = cx->in_attr_map[o->i0];
                if (loc < 0) {
                    vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, VL);
                    rn[o->dst] = o->i1;
                    for (int i = 0; i < o->i1; ++i) __riscv_vse32_v_f32m2(reg[o->dst].c[i], z, VL);
                    break;
                }
            }
            rn[o->dst] = o->i1;
            for (int i = 0; i < o->i1; ++i)
                __riscv_vse32_v_f32m2(reg[o->dst].c[i], __riscv_vle32_v_f32m2(&cx->in_attr[(loc * 4 + i) * SIMT_MAXLANE], VL), VL);
            break;
        }
        case IR_STORECLIP: { simt_reg *v=&reg[o->a]; float *clip=cx->out_clip;
            int vn=rn[o->a];
            if(o->swzn){ for(int i=0;i<o->swzn;i++) simt_copy_row(&clip[((int)o->swz[i])*SIMT_MAXLANE], v->c[vn==1?0:i], VL); }
            else { for(int i=0;i<vn&&i<4;i++) simt_copy_row(&clip[i*SIMT_MAXLANE], v->c[i], VL); } break; }
        case IR_STOREV: { simt_reg *v=&reg[o->a]; float *vo=cx->out_vary; int comp=o->i1, off=o->i0;
            int vn=rn[o->a];
            if(o->swzn){ for(int i=0;i<o->swzn;i++) simt_copy_row(&vo[(off+(int)o->swz[i])*SIMT_MAXLANE], v->c[vn==1?0:i], VL); }
            else { for(int i=0;i<comp;i++) simt_copy_row(&vo[(off+i)*SIMT_MAXLANE], v->c[vn==1?0:i], VL); } break; }
        case IR_BUILTIN: switch(o->i0) {
        case BI_DOT: {
            int ai=o->args[0], bi=o->args[1], an=rn[ai], bn=rn[bi];
            simt_reg *a=&reg[ai], *b=&reg[bi]; int m=an<bn?an:bn;
            vfloat32m2_t acc=(m==3) ? sgl_rvv_dot3_v2_m2(a->c, b->c, VL) :
                              (m==4) ? sgl_rvv_dot4_v2_m2(a->c, b->c, VL) :
                                       sgl_rvv_dot_v2_m2(a->c, b->c, m, VL);
            rn[o->dst]=1; __riscv_vse32_v_f32m2(reg[o->dst].c[0],acc,VL); break; }
        case BI_LENGTH: {
            int ai=o->args[0], an=rn[ai]; simt_reg *a=&reg[ai];
            rn[o->dst]=1;
            if (an==3) __riscv_vse32_v_f32m2(reg[o->dst].c[0], sgl_rvv_length3_v2(a->c, VL), VL);
            else {
                vfloat32m2_t acc=(an==4) ? sgl_rvv_dot4_v2_m2(a->c, a->c, VL) :
                                           sgl_rvv_dot_v2_m2(a->c, a->c, an, VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[0],__riscv_vfsqrt_v_f32m2(acc,VL),VL);
            }
            break; }
        case BI_DISTANCE: {
            int ai=o->args[0], bi=o->args[1], an=rn[ai], bn=rn[bi];
            simt_reg *a=&reg[ai], *b=&reg[bi];
            vfloat32m2_t acc=(an == 3 && bn >= 3) ? sgl_rvv_distance2_3_v2(a->c, b->c, VL)
                                                   : sgl_rvv_distance2_v2(a->c, b->c, an, VL);
            rn[o->dst]=1; __riscv_vse32_v_f32m2(reg[o->dst].c[0],__riscv_vfsqrt_v_f32m2(acc,VL),VL); break; }
        case BI_NORMALIZE: {
            int ai=o->args[0], an=rn[ai]; simt_reg *a=&reg[ai];
            rn[o->dst]=an;
            if (an == 3) sgl_rvv_normalize3_v2(a->c, reg[o->dst].c, VL);
            else sgl_rvv_normalize_v2(a->c, an, reg[o->dst].c, VL);
            break; }
        case BI_MIN: {
            int ai=o->args[0], bi=o->args[1], m=rn[ai], bn=rn[bi]; simt_reg *a=&reg[ai], *b=&reg[bi]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL), y=__riscv_vle32_v_f32m2(b->c[bn==1?0:i],VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],__riscv_vmerge_vvm_f32m2(y,x,__riscv_vmflt_vv_f32m2_b16(x,y,VL),VL),VL); } break; }
        case BI_MAX: {
            int ai=o->args[0], bi=o->args[1], m=rn[ai], bn=rn[bi]; simt_reg *a=&reg[ai], *b=&reg[bi]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL), y=__riscv_vle32_v_f32m2(b->c[bn==1?0:i],VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],__riscv_vmerge_vvm_f32m2(y,x,__riscv_vmfgt_vv_f32m2_b16(x,y,VL),VL),VL); } break; }
        case BI_CLAMP: {
            int ai=o->args[0], loi=o->args[1], hii=o->args[2], m=rn[ai], lon=rn[loi], hin=rn[hii];
            simt_reg *a=&reg[ai], *lo=&reg[loi], *hi=&reg[hii]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL),
                L=__riscv_vle32_v_f32m2(lo->c[lon==1?0:i],VL), H=__riscv_vle32_v_f32m2(hi->c[hin==1?0:i],VL);
                vfloat32m2_t r=__riscv_vmerge_vvm_f32m2(x,H,__riscv_vmfgt_vv_f32m2_b16(x,H,VL),VL);
                r=__riscv_vmerge_vvm_f32m2(r,L,__riscv_vmflt_vv_f32m2_b16(x,L,VL),VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],r,VL); } break; }
        case BI_ABS: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            for(int i=0;i<m;i++) {
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],
                    __riscv_vfabs_v_f32m2(__riscv_vle32_v_f32m2(a->c[i],VL),VL),VL);
            }
            break; }
        case BI_SQRT: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            for(int i=0;i<m;i++) {
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],
                    __riscv_vfsqrt_v_f32m2(__riscv_vle32_v_f32m2(a->c[i],VL),VL),VL);
            }
            break; }
        case BI_INVERSESQRT: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, VL);
            for(int i=0;i<m;i++) {
                vfloat32m2_t x = __riscv_vle32_v_f32m2(a->c[i], VL);
                vfloat32m2_t r = sgl_rvv_rsqrt_fast_m2(x, VL);
                r = __riscv_vmerge_vvm_f32m2(z, r, __riscv_vmfgt_vf_f32m2_b16(x, 0.f, VL), VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i], r, VL);
            }
            break; }
        case BI_SIGN: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL), r=__riscv_vfmv_v_f_f32m2(0.f,VL);
                r=__riscv_vmerge_vvm_f32m2(r,__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vmfgt_vf_f32m2_b16(x,0.f,VL),VL);
                r=__riscv_vmerge_vvm_f32m2(r,__riscv_vfmv_v_f_f32m2(-1.f,VL),__riscv_vmflt_vf_f32m2_b16(x,0.f,VL),VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],r,VL); } break; }
        case BI_CROSS: {
            simt_reg *a=&reg[o->args[0]], *b=&reg[o->args[1]]; rn[o->dst]=3;
            sgl_rvv_cross3_v2(a->c, b->c, reg[o->dst].c, VL);
            break; }
        case BI_MIX: {
            int ai=o->args[0], bi=o->args[1], ti=o->args[2], m=rn[ai], tn=rn[ti];
            simt_reg *a=&reg[ai], *b=&reg[bi], *t=&reg[ti]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t tv=__riscv_vle32_v_f32m2(t->c[tn==1?0:i],VL);
                vfloat32m2_t omt=__riscv_vfrsub_vf_f32m2(tv,1.f,VL);
                vfloat32m2_t r=__riscv_vfmul_vv_f32m2(__riscv_vle32_v_f32m2(a->c[i],VL),omt,VL);
                r=__riscv_vfmacc_vv_f32m2(r,__riscv_vle32_v_f32m2(b->c[i],VL),tv,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],r,VL); } break; }
        case BI_STEP: {
            int edi=o->args[0], xi=o->args[1], m=rn[xi], edn=rn[edi]; simt_reg *ed=&reg[edi], *x=&reg[xi]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t xv=__riscv_vle32_v_f32m2(x->c[i],VL), ev=__riscv_vle32_v_f32m2(ed->c[edn==1?0:i],VL);
                vfloat32m2_t r=__riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vfmv_v_f_f32m2(0.f,VL),__riscv_vmflt_vv_f32m2_b16(xv,ev,VL),VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],r,VL); } break; }
        case BI_SMOOTHSTEP: {
            int e0i=o->args[0], e1i=o->args[1], xi=o->args[2], m=rn[xi], e0n=rn[e0i], e1n=rn[e1i];
            simt_reg *e0=&reg[e0i], *e1=&reg[e1i], *x=&reg[xi]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t xv=__riscv_vle32_v_f32m2(x->c[i],VL),
                E0=__riscv_vle32_v_f32m2(e0->c[e0n==1?0:i],VL), E1=__riscv_vle32_v_f32m2(e1->c[e1n==1?0:i],VL);
                vfloat32m2_t t=__riscv_vfdiv_vv_f32m2(__riscv_vfsub_vv_f32m2(xv,E0,VL),__riscv_vfsub_vv_f32m2(E1,E0,VL),VL);
                t=__riscv_vmerge_vvm_f32m2(t,__riscv_vfmv_v_f_f32m2(0.f,VL),__riscv_vmflt_vf_f32m2_b16(t,0.f,VL),VL);
                t=__riscv_vmerge_vvm_f32m2(t,__riscv_vfmv_v_f_f32m2(1.f,VL),__riscv_vmfgt_vf_f32m2_b16(t,1.f,VL),VL);
                vfloat32m2_t h=__riscv_vfrsub_vf_f32m2(__riscv_vfadd_vv_f32m2(t,t,VL),3.f,VL);
                vfloat32m2_t r=__riscv_vfmul_vv_f32m2(__riscv_vfmul_vv_f32m2(t,t,VL),h,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],r,VL); } break; }
        case BI_REFLECT: {
            int ii=o->args[0], ni=o->args[1], in=rn[ii], nn=rn[ni]; simt_reg *I=&reg[ii], *N=&reg[ni];
            if (in == 3 && nn >= 3) sgl_rvv_reflect3_v2(I->c, N->c, reg[o->dst].c, VL);
            else sgl_rvv_reflect_v2(I->c, N->c, in, reg[o->dst].c, VL);
            rn[o->dst] = in;
            break; }
        case BI_FLOOR: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL);
                vint32m2_t t=__riscv_vfcvt_x_f_v_i32m2_rm(x,__RISCV_FRM_RDN,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],__riscv_vfcvt_f_x_v_f32m2(t,VL),VL); } break; }
        case BI_CEIL: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL);
                vint32m2_t t=__riscv_vfcvt_x_f_v_i32m2_rm(x,__RISCV_FRM_RUP,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],__riscv_vfcvt_f_x_v_f32m2(t,VL),VL); } break; }
        case BI_FRACT: {
            int ai=o->args[0], m=rn[ai]; simt_reg *a=&reg[ai]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL);
                vint32m2_t t=__riscv_vfcvt_x_f_v_i32m2_rm(x,__RISCV_FRM_RDN,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],__riscv_vfsub_vv_f32m2(x,__riscv_vfcvt_f_x_v_f32m2(t,VL),VL),VL); } break; }
        case BI_MOD: {
            int ai=o->args[0], bi=o->args[1], m=rn[ai], bn=rn[bi]; simt_reg *a=&reg[ai], *b=&reg[bi]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t av=__riscv_vle32_v_f32m2(a->c[i],VL), bv=__riscv_vle32_v_f32m2(b->c[bn==1?0:i],VL);
                vfloat32m2_t q=__riscv_vfdiv_vv_f32m2(av,bv,VL);
                vint32m2_t t=__riscv_vfcvt_x_f_v_i32m2_rm(q,__RISCV_FRM_RDN,VL);
                vfloat32m2_t fq=__riscv_vfcvt_f_x_v_f32m2(t,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i],__riscv_vfnmsac_vv_f32m2(av,fq,bv,VL),VL); } break; }
        case BI_POW: {
            int ai=o->args[0], bi=o->args[1], m=rn[ai], bn=rn[bi]; simt_reg *a=&reg[ai], *b=&reg[bi]; rn[o->dst]=m;
            for(int i=0;i<m;i++){ vfloat32m2_t x=__riscv_vle32_v_f32m2(a->c[i],VL), y=__riscv_vle32_v_f32m2(b->c[bn==1?0:i],VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[i], sgl_rvv_pow_m2(x,y,VL), VL); } break; }
        case BI_TEXTURE2DPROJ: {
            int si=o->args[0], uvi=o->args[1], uvn=rn[uvi]; simt_reg *s=&reg[si], *uv=&reg[uvi];
            int unit=(int)s->c[0][0];
            const sgl_tex *t=(samplers&&unit>=0&&unit<GLSL_MAX_SAMPLERS)?samplers[unit]:0;
            if (!glsl_rvv_texture2d_enabled || !t || uvn < 3) {
                if (sgl_diag_enabled) sgl_dbg_tex2d_scalar += (unsigned long)vl;
                simt_builtin_scalar_fallback(o, vl, reg, rn, samplers);
            } else {
                rn[o->dst]=4;
                if (sgl_diag_enabled) sgl_dbg_tex2d_rvv += (unsigned long)vl;
                /* q = (uvn>=4 ? uv.w : uv.z); if q==0 q=1; sample(uv.x/q, uv.y/q) */
                vfloat32m2_t q=__riscv_vle32_v_f32m2(uv->c[uvn>=4?3:2],VL);
                vbool16_t qz=__riscv_vmfeq_vf_f32m2_b16(q,0.f,VL);
                q=__riscv_vmerge_vvm_f32m2(q,__riscv_vfmv_v_f_f32m2(1.f,VL),qz,VL);
                vfloat32m2_t u=__riscv_vfdiv_vv_f32m2(__riscv_vle32_v_f32m2(uv->c[0],VL),q,VL);
                vfloat32m2_t v=__riscv_vfdiv_vv_f32m2(__riscv_vle32_v_f32m2(uv->c[1],VL),q,VL);
                vfloat32m2_t r,g,bb,aa;
                sgl_rvv_sample2d_m2(t, u, v, VL, 1.f / 255.f, t->min_filter, t->wrap, &r,&g,&bb,&aa);
                __riscv_vse32_v_f32m2(reg[o->dst].c[0],r,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[1],g,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[2],bb,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[3],aa,VL);
            }
            break; }
        case BI_TEXTURE2D: {
            int si=o->args[0], uvi=o->args[1], uvn=rn[uvi]; simt_reg *s=&reg[si], *uv=&reg[uvi];
            int unit=(int)s->c[0][0];
            const sgl_tex *t=(samplers&&unit>=0&&unit<GLSL_MAX_SAMPLERS)?samplers[unit]:0;
            if (!glsl_rvv_texture2d_enabled) {
                if (sgl_diag_enabled) sgl_dbg_tex2d_scalar += (unsigned long)vl;
                simt_builtin_scalar_fallback(o, vl, reg, rn, samplers);
            } else if (!t || uvn < 2) {
                rn[o->dst]=4;
                vfloat32m2_t z=__riscv_vfmv_v_f_f32m2(0.f,VL), one=__riscv_vfmv_v_f_f32m2(1.f,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[0],z,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[1],z,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[2],z,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[3],one,VL);
            } else {
                rn[o->dst]=4;
                if (sgl_diag_enabled) sgl_dbg_tex2d_rvv += (unsigned long)vl;
                vfloat32m2_t r,g,b,a;
                sgl_rvv_sample2d_m2(t, __riscv_vle32_v_f32m2(uv->c[0],VL), __riscv_vle32_v_f32m2(uv->c[1],VL), VL,
                                    1.f / 255.f, t->min_filter, t->wrap, &r,&g,&b,&a);
                __riscv_vse32_v_f32m2(reg[o->dst].c[0],r,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[1],g,VL);
                __riscv_vse32_v_f32m2(reg[o->dst].c[2],b,VL); __riscv_vse32_v_f32m2(reg[o->dst].c[3],a,VL);
            }
            break; }
        default: {
            simt_builtin_scalar_fallback(o, vl, reg, rn, samplers); break; }
            } break;
        case IR_DISCARD: {   /* SIMT alpha-test: accumulate a per-lane kill mask (1=discard) */
            if (cx->out_discard) {
                vbool16_t kill = (o->a < 0)
                    ? __riscv_vmfne_vf_f32m2_b16(__riscv_vfmv_v_f_f32m2(1.f, VL), 0.f, VL)   /* `discard;` -> all lanes */
                    : __riscv_vmfne_vf_f32m2_b16(__riscv_vle32_v_f32m2(reg[o->a].c[0], VL), 0.f, VL);
                vbool16_t acc = __riscv_vmor_mm_b16(
                    __riscv_vmfne_vf_f32m2_b16(__riscv_vle32_v_f32m2(cx->out_discard, VL), 0.f, VL), kill, VL);
                __riscv_vse32_v_f32m2(cx->out_discard,
                    __riscv_vmerge_vvm_f32m2(__riscv_vfmv_v_f_f32m2(0.f, VL), __riscv_vfmv_v_f_f32m2(1.f, VL), acc, VL), VL);
            }
            break; }
        default: break;
        }
    }
}


extern int glsl_link_dump;   /* shader-workload profiling gate (glsl.c) */

int glsl_run_fragment_simt_u(glsl_program *p, const float *varys, int nvaryf, int vl,
                             const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                             const float *ustore, float *out)
{
    (void)nvaryf;
    if (p->fs_kernel) {  /* register-resident specialised kernel (sg_kernels.c) */
        if (p->fs_kernel(p, varys, vl, samplers, ustore ? ustore : p->ustore, out)) {
            if (glsl_link_dump) p->dbg_frags_kernel += (unsigned long)vl;  /* Phase 0 workload */
            return 1;
        }
        /* kernel bailed (e.g. a sampler was null) -> fall through to the VM */
    }
    if (glsl_link_dump) p->dbg_frags_vm += (unsigned long)vl;   /* Phase 0 workload */
    ir_prog *ir=&p->fs_ir;
    simt_reg reg[IR_MAXREG];
    unsigned char rn[IR_MAXREG];
    unsigned char rmat[IR_MAXREG];
    (void)nvaryf;
    size_t VL=__riscv_vsetvl_e32m2((size_t)vl);
    { vfloat32m2_t z=__riscv_vfmv_v_f_f32m2(0.f,VL), one=__riscv_vfmv_v_f_f32m2(1.f,VL);
      for (int i=0;i<ir->nlocal;i++){ rn[i]=1; rmat[i]=0; }
      __riscv_vse32_v_f32m2(&out[0*SIMT_MAXLANE],z,VL); __riscv_vse32_v_f32m2(&out[1*SIMT_MAXLANE],z,VL);
      __riscv_vse32_v_f32m2(&out[2*SIMT_MAXLANE],z,VL); __riscv_vse32_v_f32m2(&out[3*SIMT_MAXLANE],one,VL); }
    simt_ctx cx; memset(&cx,0,sizeof cx); cx.in_vary=varys; cx.samplers=samplers; cx.out_frag=out;
    if (p->fs_has_discard) {   /* SIMT discard: kill mask -> shade-buffer row 4; raster ANDs into coverage */
        cx.out_discard = &out[4*SIMT_MAXLANE];
        __riscv_vse32_v_f32m2(cx.out_discard, __riscv_vfmv_v_f_f32m2(0.f, VL), VL);
    }
    simt_run_ir(p, ir, &cx, vl, reg, rn, rmat, VL, ustore);
    return 1;
}

int glsl_run_fragment_simt(glsl_program *p, const float *varys, int nvaryf, int vl,
                           const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float *out)
{
    return glsl_run_fragment_simt_u(p, varys, nvaryf, vl, samplers, p->ustore, out);
}

int glsl_run_vertex_simt(glsl_program *p, const float *attrs, int vl,
                         float *clip, float *vary_out)
{
    ir_prog *ir=&p->vs_ir;
    simt_reg vreg[IR_MAXREG];
    unsigned char rn[IR_MAXREG];
    unsigned char rmat[IR_MAXREG];
    size_t VL=__riscv_vsetvl_e32m2((size_t)vl);
    { vfloat32m2_t z=__riscv_vfmv_v_f_f32m2(0.f,VL), one=__riscv_vfmv_v_f_f32m2(1.f,VL);
      for (int i=0;i<ir->nlocal;i++){ rn[i]=1; rmat[i]=0; }
      __riscv_vse32_v_f32m2(&clip[0*SIMT_MAXLANE],z,VL); __riscv_vse32_v_f32m2(&clip[1*SIMT_MAXLANE],z,VL);
      __riscv_vse32_v_f32m2(&clip[2*SIMT_MAXLANE],z,VL); __riscv_vse32_v_f32m2(&clip[3*SIMT_MAXLANE],one,VL);
      for(int f=0;f<p->nvaryfloats;f++)__riscv_vse32_v_f32m2(&vary_out[f*SIMT_MAXLANE],z,VL); }
    simt_ctx cx; memset(&cx,0,sizeof cx); cx.in_attr=attrs; cx.out_clip=clip; cx.out_vary=vary_out;
    simt_run_ir(p, ir, &cx, vl, vreg, rn, rmat, VL, p->ustore);
    return 1;
}

int glsl_run_vertex_simt_mapped(glsl_program *p, const float *attrs,
                                const int *loc_map, int vl, float *clip,
                                float *vary_out)
{
    ir_prog *ir = &p->vs_ir;
    simt_reg vreg[IR_MAXREG];
    unsigned char rn[IR_MAXREG];
    unsigned char rmat[IR_MAXREG];
    size_t VL = __riscv_vsetvl_e32m2((size_t)vl);
    { vfloat32m2_t z = __riscv_vfmv_v_f_f32m2(0.f, VL), one = __riscv_vfmv_v_f_f32m2(1.f, VL);
      for (int i = 0; i < ir->nlocal; ++i) {
          rn[i] = 1;
          rmat[i] = 0;
      }
      __riscv_vse32_v_f32m2(&clip[0 * SIMT_MAXLANE], z, VL); __riscv_vse32_v_f32m2(&clip[1 * SIMT_MAXLANE], z, VL);
      __riscv_vse32_v_f32m2(&clip[2 * SIMT_MAXLANE], z, VL); __riscv_vse32_v_f32m2(&clip[3 * SIMT_MAXLANE], one, VL);
      for (int f = 0; f < p->nvaryfloats; ++f) __riscv_vse32_v_f32m2(&vary_out[f * SIMT_MAXLANE], z, VL); }
    simt_ctx cx; memset(&cx, 0, sizeof cx);
    cx.in_attr = attrs; cx.in_attr_map = loc_map; cx.out_clip = clip; cx.out_vary = vary_out;
    simt_run_ir(p, ir, &cx, vl, vreg, rn, rmat, VL, p->ustore);
    return 1;
}
#else  /* host / non-RVV: correct reference (loop scalar IR/tree-walk per lane) */
int glsl_run_fragment_simt_u(glsl_program *p, const float *varys, int nvaryf, int vl,
                             const sgl_tex *const samplers[GLSL_MAX_SAMPLERS],
                             const float *ustore, float *out)
{
    for (int L=0;L<vl;L++){ float vy[64]; for(int f=0;f<nvaryf&&f<64;f++)vy[f]=varys[f*SIMT_MAXLANE+L];
        float rgba[4]; ir_run_fragment_u(p,vy,samplers,ustore,rgba); for(int i=0;i<4;i++)out[i*SIMT_MAXLANE+L]=rgba[i]; }
    return 1;
}
int glsl_run_fragment_simt(glsl_program *p, const float *varys, int nvaryf, int vl,
                           const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float *out)
{
    return glsl_run_fragment_simt_u(p, varys, nvaryf, vl, samplers, p->ustore, out);
}
int glsl_run_vertex_simt(glsl_program *p, const float *attrs, int vl, float *clip, float *vary_out)
{
    for (int L=0;L<vl;L++){
        float a[GLSL_MAX_ATTRIBS][4];
        for(int loc=0;loc<GLSL_MAX_ATTRIBS;loc++) for(int i=0;i<4;i++) a[loc][i]=attrs[(loc*4+i)*SIMT_MAXLANE+L];
        float cl[4]; float vy[GLSL_MAX_VARYING_FLOATS];
        glsl_run_vertex(p,a,cl,vy);
        for(int i=0;i<4;i++) clip[i*SIMT_MAXLANE+L]=cl[i];
        for(int f=0;f<p->nvaryfloats;f++) vary_out[f*SIMT_MAXLANE+L]=vy[f];
    }
    return 1;
}

int glsl_run_vertex_simt_mapped(glsl_program *p, const float *attrs,
                               const int *loc_map, int vl, float *clip,
                               float *vary_out)
{
    if (!loc_map) {
        for (int L = 0; L < vl; ++L) {
            float a[GLSL_MAX_ATTRIBS][4];
            for (int loc = 0; loc < GLSL_MAX_ATTRIBS; ++loc) {
                for (int i = 0; i < 4; ++i) a[loc][i] = attrs[(loc * 4 + i) * SIMT_MAXLANE + L];
            }
            float cl[4], vy[GLSL_MAX_VARYING_FLOATS];
            glsl_run_vertex(p, a, cl, vy);
            for (int i = 0; i < 4; ++i) clip[i * SIMT_MAXLANE + L] = cl[i];
            for (int f = 0; f < p->nvaryfloats; ++f) vary_out[f * SIMT_MAXLANE + L] = vy[f];
        }
        return 1;
    }

    for (int L = 0; L < vl; ++L) {
        float a[GLSL_MAX_ATTRIBS][4];
        for (int loc = 0; loc < GLSL_MAX_ATTRIBS; ++loc) {
            int src_loc = loc_map[loc];
            if (src_loc >= 0 && src_loc < GLSL_MAX_ATTRIBS) {
                for (int i = 0; i < 4; ++i)
                    a[loc][i] = attrs[(src_loc * 4 + i) * SIMT_MAXLANE + L];
            } else {
                for (int i = 0; i < 4; ++i) a[loc][i] = 0.f;
            }
        }
        float cl[4]; float vy[GLSL_MAX_VARYING_FLOATS];
        glsl_run_vertex(p, a, cl, vy);
        for (int i = 0; i < 4; ++i) clip[i * SIMT_MAXLANE + L] = cl[i];
        for (int f = 0; f < p->nvaryfloats; ++f) vary_out[f * SIMT_MAXLANE + L] = vy[f];
    }
    return 1;
}
/* glsl_fs_classify + the register-resident kernels now live in sg_kernels.c */
#endif

int glsl_fs_has_kernel(const glsl_program *p){ return p->fs_kernel != 0; }
int glsl_fs_simt_ok(const glsl_program *p){ return p->fs_ir.ok && p->fs_ir.simt_ok; }
int glsl_fs_has_discard(const glsl_program *p){ return p->fs_has_discard; }
int glsl_fs_alpha_zero_if_color_zero(const glsl_program *p){ return p->fs_alpha_zero_if_color_zero; }
int glsl_fs_cost(const glsl_program *p)
{
    const ir_prog *ir = &p->fs_ir;
    if (!ir->ok) return 96;
    int cost = ir->ninst;
    for (int i = 0; i < ir->ninst; ++i) {
        const ir_inst *o = &ir->inst[i];
        if (o->op == IR_BUILTIN) {
            switch (o->i0) {
            case BI_TEXTURE2D:
            case BI_TEXTURE2DPROJ:
            case BI_TEXTURECUBE:
                cost += 32;
                break;
            case BI_NORMALIZE:
            case BI_REFLECT:
            case BI_POW:
                cost += 12;
                break;
            case BI_LENGTH:
            case BI_DISTANCE:
            case BI_SMOOTHSTEP:
                cost += 8;
                break;
            default:
                cost += 3;
                break;
            }
        }
    }
    return cost > 1 ? cost : 1;
}
int glsl_vs_simt_ok(const glsl_program *p){ return p->vs_ir.ok && p->vs_ir.simt_ok; }
