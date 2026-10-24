/*
 * softgles2 — GLSL ES 1.00 interpreter (subset). See glsl.h for scope.
 * Scalar tree-walking evaluator (correctness reference + host/bare-metal path).
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "sg_core.h"
#include "sg_math.h"
#include "sg_raster.h"
#include "glsl.h"
#include "glsl_internal.h"
#include "sg_frag.h"

/* The parser/evaluator use compact one-line guarded statements deliberately. */
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

/* no libm in the lean profile: use deterministic polynomial kernels from sg_math. */
static inline float g_sqrt(float x) { return sgl_math_sqrt_f(x); }
static inline float g_abs(float x)  { return sgl_math_abs_f(x); }
static inline float g_floor(float x){ return sgl_math_floor_f(x); }
static inline float g_ceil(float x) { return sgl_math_ceil_f(x); }
/* log2 for the GLSL builtin: the sg_math kernel (sgl_math_log2_f) that backs
 * pow() is a coarse placeholder (~30%% mid-mantissa error, exact only at powers
 * of two), unfit to expose as log2(). Use the exact identity
 *   log2(m) = (2/ln2)*artanh((m-1)/(m+1))   on the mantissa m in [1,2), + exp
 * (max abs err vs libm ~2.3e-6). simt_log2_m2() in glsl_simt.c uses the
 * identical formula so scalar and RVV lanes agree; pow() is left unchanged. */
static inline float g_log2(float x) {
    union { float f; unsigned u; } v; v.f = x;
    float e = (float)((int)((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u; float m = v.f;
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    float s = 0.11111111f;                 /* 1/9 */
    s = 0.14285715f + t2 * s;              /* 1/7 */
    s = 0.2f + t2 * s;                     /* 1/5 */
    s = 0.33333334f + t2 * s;              /* 1/3 */
    s = 1.0f + t2 * s;
    return e + (t * s) * 2.8853900817779268f;   /* 2/ln2 */
}
static inline float g_exp2(float x) { return sgl_math_exp2_f(x); }
static inline float g_pow(float x, float y){ return sgl_math_pow_f(x, y); }
/* Transcendentals for the tree-walk + IR scalar paths (no libm in the lean
 * profile). exp2 reuses the deterministic sg_math kernel (sgl_math_exp2_f, ~8e-4
 * rel), matched on the RVV SIMT path by sgl_rvv_exp2_m2. log2 uses the accurate
 * artanh series in g_log2 above, matched by simt_log2_m2. sin/cos use the SAME
 * minimax range-reduced polynomial as sgl_rvv_sincos_f_m1. In every case the
 * scalar and vector lanes share constants/op-order so they agree closely. */
#define G_LOG2E  1.4426950408889634f   /* 1/ln2   */
#define G_LN2    0.6931471805599453f   /* ln2     */
#define G_DEG2R  0.017453292519943295f /* PI/180  */
#define G_R2DEG  57.29577951308232f    /* 180/PI  */
#define G_PI_2         1.5707963267948966f
#define G_TWO_OVER_PI  0.63661977236758134f
#define G_SC_S1 (-0.16666667f)
#define G_SC_S2  0.0083333310f
#define G_SC_S3 (-0.00019840874f)
#define G_SC_C1 (-0.5f)
#define G_SC_C2  0.041666418f
#define G_SC_C3 (-0.0013888397f)
static inline float g_exp(float x)  { return sgl_math_exp2_f(x * G_LOG2E); }
static inline float g_log(float x)  { return g_log2(x) * G_LN2; }
static inline float g_radians(float x){ return x * G_DEG2R; }
static inline float g_degrees(float x){ return x * G_R2DEG; }
static inline void g_sincos(float a, float *so, float *co)
{
    float t  = a * G_TWO_OVER_PI;
    int   ki = (int)(t + (t >= 0.f ? 0.5f : -0.5f));   /* round to nearest quadrant */
    float kf = (float)ki;
    float r  = a - kf * G_PI_2;                        /* r in [-pi/4, pi/4] */
    float r2 = r * r;
    float cosv = 1.f + r2 * (G_SC_C1 + r2 * (G_SC_C2 + r2 * G_SC_C3));
    float sinv = r + r * r2 * (G_SC_S1 + r2 * (G_SC_S2 + r2 * G_SC_S3));
    int kq = ki & 3, b0 = kq & 1, b2 = kq & 2;
    int swap = b0 != 0, sinneg = b2 != 0, cosneg = (((b0 << 1) ^ b2) != 0);
    float s_sel = swap ? cosv : sinv;
    float c_sel = swap ? sinv : cosv;
    *so = sinneg ? -s_sel : s_sel;
    *co = cosneg ? -c_sel : c_sel;
}
static inline float g_sin(float x){ float s,c; g_sincos(x,&s,&c); return s; }
static inline float g_cos(float x){ float s,c; g_sincos(x,&s,&c); return c; }
static inline float g_tan(float x){ float s,c; g_sincos(x,&s,&c); return c!=0.f ? s/c : 0.f; }
static inline float g_dot(const float *a, const float *b, int n) { return sgl_math_dot_f(a, b, n); }
static inline void g_matvec(const float *a, const float *b, float *out, int n)
{ sgl_math_matvec_f(a, b, out, n); }

/* non-static: the gl4es-interaction kernel (sg_kernels.c) calls this per-lane for
 * the cubemap light lookup, so its result is bit-identical to the VM's scalar path. */
void sample_cube_tex(const sgl_tex *t, float dx, float dy, float dz, float out[4])
{
    float ax = g_abs(dx), ay = g_abs(dy), az = g_abs(dz), sc, tc, ma;
    int face;
    if (ax >= ay && ax >= az) {
        if (dx >= 0.f) { face = 0; ma = ax; sc = -dz; tc = -dy; }
        else           { face = 1; ma = ax; sc =  dz; tc = -dy; }
    } else if (ay >= az) {
        if (dy >= 0.f) { face = 2; ma = ay; sc =  dx; tc =  dz; }
        else           { face = 3; ma = ay; sc =  dx; tc = -dz; }
    } else {
        if (dz >= 0.f) { face = 4; ma = az; sc =  dx; tc = -dy; }
        else           { face = 5; ma = az; sc = -dx; tc = -dy; }
    }
    if (ma == 0.f) ma = 1.f;
    const sgl_tex *src = (t && t->is_cube && t->cube_faces[face]) ? t->cube_faces[face] : t;
    if (src && src->texels) sgl_sample(src, sc / ma * 0.5f + 0.5f, tc / ma * 0.5f + 0.5f, out);
}

/* types/enums (T_*, Q_*, N_*, S_*, IR_*, BI_*, TK_*), node/stmt/glvar/func_def/shader,
 * ir_inst/ir_prog/glsl_program, arena, gval, token/toklist — all in glsl_internal.h */
int type_comp(int t)
{
    switch (t) { case T_FLOAT: case T_INT: case T_BOOL: return 1;
                 case T_VEC2: return 2; case T_VEC3: return 3; case T_VEC4: return 4;
                 case T_MAT4: return 16; default: return 1; }
}

/* arena allocator (types in glsl_internal.h) */
void *arena_alloc(arena *a, int size){
    size = (size + 7) & ~7;
    if (!a->head || a->head->used + size > SGL_ARENA_BLOCK_SIZE) {
        arena_block *b = (arena_block*)malloc(sizeof(arena_block));
        if (!b) return 0;
        b->used = 0; b->next = a->head; a->head = b;
    }
    void *ptr = a->head->data + a->head->used;
    a->head->used += size;
    memset(ptr, 0, (size_t)size);
    return ptr;
}
void arena_free(arena *a){
    arena_block *b = a->head;
    while (b) { arena_block *nx = b->next; free(b); b = nx; }
    a->head = 0;
}

/* parser in glsl_parse.c */

/* ---------------- evaluation ---------------- */
typedef struct {
    glsl_program *prog;
    /* Uniform source: normally &prog->ustore[0], but the deferred (FrameBin)
     * raster path points this at a per-draw snapshot so a worker hart shades
     * each recorded command with that command's uniforms, not whatever the
     * shared prog->ustore currently holds. Fragment stage only — the vertex
     * stage runs at record time and bakes live uniforms into pv/vb. */
    const float *ustore;
    shader *sh;
    int stage;
    const float (*attribs)[4];
    const float *varying_in;
    const sgl_tex *const *samplers;
    float *clip;
    float *varying_out;
    float *fragcolor;
    int discarded;
    struct { char name[40]; gval val; } locals[64]; int nlocals;
} env;

/* Flat float offset of a varying access. Varyings are declared in the VS; the FS
 * may match by name, OR — gl4es convention — the VS declares an array
 * `vec4 X[N]` while the FS reads unrolled scalars `X_0..X_{N-1}`. So both an
 * explicit element index (VS `X[i]`) and a `_<N>` name suffix (FS `X_N`) map onto
 * the same packed slots. index<0 + no suffix => element 0. */
int vary_slot(glsl_program *p, const char *name, int index, int *comp)
{
    for (int i=0;i<p->nvary;i++) if(!strcmp(p->vary[i].name,name)){
        if(comp)*comp=p->vary[i].comp; int e=index>=0?index:0; return p->vary[i].offset + e*p->vary[i].comp; }
    /* FS scalar suffix  base_N  -> VS array  base[N] */
    int len=0; while(name[len])len++;
    int j=len-1, n=0, mul=1, ndig=0;
    while(j>=0 && name[j]>='0' && name[j]<='9'){ n+=(name[j]-'0')*mul; mul*=10; j--; ndig++; }
    if (ndig>0 && j>=1 && name[j]=='_') {
        char base[40]; int k=0; for(;k<j&&k<39;k++)base[k]=name[k]; base[k]=0;
        for (int i=0;i<p->nvary;i++) if(!strcmp(p->vary[i].name,base)){
            if(comp)*comp=p->vary[i].comp; return p->vary[i].offset + n*p->vary[i].comp; }
    }
    return -1;
}
glvar *find_global(shader *sh, const char *name)
{
    for (int i=0;i<sh->nglob;i++) if(!strcmp(sh->globals[i].name,name)) return &sh->globals[i];
    return 0;
}
int find_uniform(glsl_program *p, const char *name)
{
    for (int i=0;i<p->nuni;i++) if(!strcmp(p->uni[i].name,name)) return i;
    return -1;
}

static gval eval(env *e, node *n);

static gval gconst(float x){ gval g; g.n=1; g.v[0]=x; return g; }

static gval eval_var(env *e, const char *name)
{
    gval g; memset(&g,0,sizeof(g)); g.n=1;
    for (int i=0;i<e->nlocals;i++) if(!strcmp(e->locals[i].name,name)) return e->locals[i].val;
    if (!strcmp(name,"gl_Position")||!strcmp(name,"gl_FragColor")||!strcmp(name,"gl_FragCoord")) { g.n=4; return g; }
    glvar *gv=find_global(e->sh,name);
    if (gv) {
        int nc=type_comp(gv->type);
        if (gv->qual==Q_ATTRIB && e->stage==0) { g.n=nc; for(int i=0;i<nc&&i<4;i++)g.v[i]=e->attribs[gv->location][i]; return g; }
        if (gv->qual==Q_VARYING) {
            int comp; int off=vary_slot(e->prog,name,-1,&comp); if(off<0){return g;}
            g.n=comp; const float *src = (e->stage==0)? e->varying_out : e->varying_in;
            for(int i=0;i<comp;i++) g.v[i]=src[off+i]; return g;
        }
        if (gv->qual==Q_UNIFORM) {
            int ui=find_uniform(e->prog,name); if(ui<0) return g;
            if (e->prog->uni[ui].is_sampler) { g.n=1; g.v[0]=(float)e->prog->uni[ui].unit; return g; }
            g.n=nc; for(int i=0;i<nc;i++) g.v[i]=e->ustore[e->prog->uni[ui].offset+i]; return g;
        }
    }
    return g;
}

static gval eval(env *e, node *n)
{
    gval g; memset(&g,0,sizeof(g)); g.n=1;
    if (!n) return g;
    switch (n->k) {
    case N_NUM: g.n=1; g.v[0]=n->num; return g;
    case N_VAR: return eval_var(e, n->name);
    case N_INDEX: {
        int ix=n->op;
        if (n->a->k==N_VAR) {                     /* array varying element X[i] */
            glvar *gv=find_global(e->sh,n->a->name);
            if (gv && gv->qual==Q_VARYING) {
                int comp; int off=vary_slot(e->prog,n->a->name,ix,&comp); if(off<0)return g;
                g.n=comp; const float *src=(e->stage==0)?e->varying_out:e->varying_in;
                for(int i=0;i<comp;i++)g.v[i]=src[off+i]; return g;
            }
        }
        gval a=eval(e,n->a);
        if (a.n==16) { g.n=4; for(int i=0;i<4;i++)g.v[i]=a.v[ix*4+i]; return g; }  /* mat4 column (col-major) */
        g.n=1; if(ix>=0&&ix<a.n)g.v[0]=a.v[ix]; return g;                          /* vec component */
    }
    case N_SWZ: { gval a=eval(e,n->a); g.n=n->swzn; for(int i=0;i<n->swzn;i++)g.v[i]=a.v[n->swz[i]]; if(g.n==1)return g; return g; }
    case N_NEG: { gval a=eval(e,n->a); g.n=a.n; for(int i=0;i<a.n;i++)g.v[i]=-a.v[i]; return g; }
    case N_NOT: { gval a=eval(e,n->a); g.n=1; g.v[0]=(a.v[0]==0.f)?1.f:0.f; return g; }
    case N_TERN: { gval c=eval(e,n->a); return eval(e, c.v[0]!=0.f ? n->b : n->c); }
    case N_CMP: {
        gval a=eval(e,n->a), b=eval(e,n->b); float x=a.v[0],y=b.v[0],r=0;
        switch(n->op){case '<':r=x<y;break;case '>':r=x>y;break;case 'L':r=x<=y;break;
                      case 'G':r=x>=y;break;case 'E':r=x==y;break;case 'N':r=x!=y;break;
                      case 'O':r=(x!=0.f||y!=0.f);break;case 'A':r=(x!=0.f&&y!=0.f);break;}
        g.n=1; g.v[0]=r?1.f:0.f; return g;
    }
    case N_BIN: {
        gval a=eval(e,n->a), b=eval(e,n->b);
        if (n->op=='*' && a.n==16 && b.n==4) {       /* mat4 (col-major) * vec4 */
            g.n=4; g_matvec(a.v, b.v, g.v, 4); return g;
        }
        int n1=a.n, n2=b.n, rn = n1>n2?n1:n2; if(n1==16||n2==16)rn=(n1>n2?n1:n2);
        g.n = rn;
        for (int i=0;i<rn;i++) {
            float x = (n1==1)?a.v[0]:a.v[i];
            float y = (n2==1)?b.v[0]:b.v[i];
            switch(n->op){case '+':g.v[i]=x+y;break;case '-':g.v[i]=x-y;break;
                          case '*':g.v[i]=x*y;break;case '/':g.v[i]=y!=0.f?x/y:0.f;break;}
        }
        return g;
    }
    case N_CALL: {
        const char *f=n->name;
        gval a[8]; for(int i=0;i<n->nargs;i++) a[i]=eval(e,n->args[i]);
        int tt;
        if (kw_type(f,&tt) && (tt==T_VEC2||tt==T_VEC3||tt==T_VEC4||tt==T_FLOAT)) {
            int want = type_comp(tt);
            float flat[16]; int fn=0;
            for(int i=0;i<n->nargs;i++) for(int j=0;j<a[i].n && fn<16;j++) flat[fn++]=a[i].v[j];
            g.n=want;
            if (fn==1) { for(int i=0;i<want;i++)g.v[i]=flat[0]; }     /* vecN(scalar) */
            else { for(int i=0;i<want;i++) g.v[i]=(i<fn)?flat[i]:0.f; }
            return g;
        }
        if (!strcmp(f,"texture2D")) {
            int unit=(int)a[0].v[0]; const sgl_tex *t = (unit>=0&&unit<GLSL_MAX_SAMPLERS)? e->samplers[unit] : 0;
            float out[4]={0,0,0,1}; if(t) sgl_sample(t, a[1].v[0], a[1].v[1], out);
            g.n=4; for(int i=0;i<4;i++)g.v[i]=out[i]; return g;
        }
        if (!strcmp(f,"dot")) { int m=a[0].n<a[1].n?a[0].n:a[1].n; return gconst(g_dot(a[0].v,a[1].v,m)); }
        if (!strcmp(f,"length")) { return gconst(g_sqrt(g_dot(a[0].v,a[0].v,a[0].n))); }
        if (!strcmp(f,"distance")) { float dd[16]; int m=a[0].n; for(int i=0;i<m;i++)dd[i]=a[0].v[i]-a[1].v[i]; return gconst(g_sqrt(g_dot(dd,dd,m))); }
        if (!strcmp(f,"normalize")) { float s=g_dot(a[0].v,a[0].v,a[0].n); float inv=s>0?1.f/g_sqrt(s):0.f; g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=a[0].v[i]*inv; return g; }
        if (!strcmp(f,"cross")) { g.n=3; g.v[0]=a[0].v[1]*a[1].v[2]-a[0].v[2]*a[1].v[1]; g.v[1]=a[0].v[2]*a[1].v[0]-a[0].v[0]*a[1].v[2]; g.v[2]=a[0].v[0]*a[1].v[1]-a[0].v[1]*a[1].v[0]; return g; }
        if (!strcmp(f,"mix")) { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float t=(a[2].n==1)?a[2].v[0]:a[2].v[i]; g.v[i]=a[0].v[i]*(1.f-t)+a[1].v[i]*t;} return g; }
        if (!strcmp(f,"clamp")) { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float lo=(a[1].n==1)?a[1].v[0]:a[1].v[i],hi=(a[2].n==1)?a[2].v[0]:a[2].v[i],x=a[0].v[i]; g.v[i]=x<lo?lo:(x>hi?hi:x);} return g; }
        if (!strcmp(f,"min")) { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=a[0].v[i]<y?a[0].v[i]:y;} return g; }
        if (!strcmp(f,"max")) { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=a[0].v[i]>y?a[0].v[i]:y;} return g; }
        if (!strcmp(f,"abs")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_abs(a[0].v[i]); return g; }
        if (!strcmp(f,"floor")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_floor(a[0].v[i]); return g; }
        if (!strcmp(f,"ceil")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_ceil(a[0].v[i]); return g; }
        if (!strcmp(f,"fract")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=a[0].v[i]-g_floor(a[0].v[i]); return g; }
        if (!strcmp(f,"sign")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=(a[0].v[i]>0)-(a[0].v[i]<0); return g; }
        if (!strcmp(f,"sqrt")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_sqrt(a[0].v[i]); return g; }
        if (!strcmp(f,"inversesqrt")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=a[0].v[i]>0?1.f/g_sqrt(a[0].v[i]):0.f; return g; }
        if (!strcmp(f,"mod")) { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=a[0].v[i]-y*g_floor(a[0].v[i]/y);} return g; }
        if (!strcmp(f,"step")) { int m=a[1].n; g.n=m; for(int i=0;i<m;i++){float ed=(a[0].n==1)?a[0].v[0]:a[0].v[i]; g.v[i]=a[1].v[i]<ed?0.f:1.f;} return g; }
        if (!strcmp(f,"smoothstep")) { int m=a[2].n; g.n=m; for(int i=0;i<m;i++){float e0=(a[0].n==1)?a[0].v[0]:a[0].v[i],e1=(a[1].n==1)?a[1].v[0]:a[1].v[i],x=a[2].v[i]; float t=(x-e0)/(e1-e0); t=t<0?0:(t>1?1:t); g.v[i]=t*t*(3.f-2.f*t);} return g; }
        if (!strcmp(f,"reflect")) { int m=a[0].n; float d=g_dot(a[1].v,a[0].v,m); float td=d+d; g.n=m; for(int i=0;i<m;i++)g.v[i]=__builtin_fmaf(-td,a[1].v[i],a[0].v[i]); return g; }
        if (!strcmp(f,"pow")) { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=g_pow(a[0].v[i],y);} return g; }
        if (!strcmp(f,"sin")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_sin(a[0].v[i]); return g; }
        if (!strcmp(f,"cos")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_cos(a[0].v[i]); return g; }
        if (!strcmp(f,"tan")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_tan(a[0].v[i]); return g; }
        if (!strcmp(f,"exp")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_exp(a[0].v[i]); return g; }
        if (!strcmp(f,"log")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_log(a[0].v[i]); return g; }
        if (!strcmp(f,"exp2")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_exp2(a[0].v[i]); return g; }
        if (!strcmp(f,"log2")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_log2(a[0].v[i]); return g; }
        if (!strcmp(f,"radians")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_radians(a[0].v[i]); return g; }
        if (!strcmp(f,"degrees")) { g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_degrees(a[0].v[i]); return g; }
        if (!strcmp(f,"texture2DProj")) {
            int unit=(int)a[0].v[0]; const sgl_tex *t=(unit>=0&&unit<GLSL_MAX_SAMPLERS)?e->samplers[unit]:0;
            float q=(a[1].n>=4)?a[1].v[3]:a[1].v[2]; if(q==0.f)q=1.f;
            float out[4]={0,0,0,1}; if(t) sgl_sample(t, a[1].v[0]/q, a[1].v[1]/q, out);
            g.n=4; for(int i=0;i<4;i++)g.v[i]=out[i]; return g;
        }
        if (!strcmp(f,"textureCube")) {   /* crude cube->2D so the 2D rasteriser can serve it */
            int unit=(int)a[0].v[0]; const sgl_tex *t=(unit>=0&&unit<GLSL_MAX_SAMPLERS)?e->samplers[unit]:0;
            float out[4]={0,0,0,1}; if(t) sample_cube_tex(t, a[1].v[0], a[1].v[1], a[1].v[2], out);
            g.n=4; for(int i=0;i<4;i++)g.v[i]=out[i]; return g;
        }
        /* user-defined function (e.g. ftransform) — builtins above take precedence */
        for (int fi=0;fi<e->sh->nfuncs;fi++) if(!strcmp(e->sh->funcs[fi].name,f)) {
            func_def *fd=&e->sh->funcs[fi]; if(!fd->ret) return g;
            int saved=e->nlocals;
            for(int pi=0;pi<fd->nparam && e->nlocals<64;pi++){ int li=e->nlocals++; int k=0;
                for(;fd->param[pi][k]&&k<39;k++)e->locals[li].name[k]=fd->param[pi][k]; e->locals[li].name[k]=0;
                e->locals[li].val=(pi<n->nargs)?a[pi]:gconst(0.f); }
            gval r=eval(e,fd->ret); e->nlocals=saved; return r;
        }
        return g;   /* unknown -> zeros */
    }
    }
    return g;
}

static void write_lvalue(env *e, const char *name, int index, int *swz, int swzn, gval val)
{
    float *dst=0; int dstn=0;
    if (!strcmp(name,"gl_Position")) { dst=e->clip; dstn=4; }
    else if (!strcmp(name,"gl_FragColor")) { dst=e->fragcolor; dstn=4; }
    else {
        glvar *gv=find_global(e->sh,name);
        if (gv && gv->qual==Q_VARYING && e->stage==0) { int comp; int off=vary_slot(e->prog,name,index,&comp); if(off>=0){dst=e->varying_out+off; dstn=comp;} }
        else {
            int li=-1; for(int i=0;i<e->nlocals;i++) if(!strcmp(e->locals[i].name,name)){li=i;break;}
            if (li<0 && e->nlocals<64){ li=e->nlocals++; int k=0; for(;name[k]&&k<39;k++)e->locals[li].name[k]=name[k]; e->locals[li].name[k]=0; memset(&e->locals[li].val,0,sizeof(gval)); }
            if (li>=0){ dst=e->locals[li].val.v; dstn = val.n > e->locals[li].val.n ? val.n : e->locals[li].val.n; e->locals[li].val.n = swzn? (e->locals[li].val.n<4?4:e->locals[li].val.n) : val.n; }
        }
    }
    if (!dst) return;
    if (swzn) { for(int i=0;i<swzn;i++) dst[swz[i]] = (val.n==1)?val.v[0]:val.v[i]; }
    else { int c = val.n; for(int i=0;i<c;i++) dst[i]=val.v[i]; (void)dstn; }
}

static void exec_body(env *e, shader *sh)
{
    for (int i=0;i<sh->nbody && !e->discarded;i++) {
        stmt *s=&sh->body[i];
        if (s->k==S_DISCARD) { if(!s->expr || eval(e,s->expr).v[0]!=0.f){ e->discarded=1; return; } continue; }
        if (s->k==S_NOP) continue;
        if (s->k==S_DECL) {
            gval v; memset(&v,0,sizeof(v)); v.n=type_comp(s->type);
            if (s->expr) v=eval(e,s->expr);
            if (e->nlocals<64){ int li=e->nlocals++; int k=0; for(;s->name[k]&&k<39;k++)e->locals[li].name[k]=s->name[k]; e->locals[li].name[k]=0; e->locals[li].val=v; }
            continue;
        }
        if (s->k==S_ASSIGN) { gval v=eval(e,s->expr); write_lvalue(e,s->name,s->idx,s->swz,s->swzn,v); }
    }
}

/* ================= flat bytecode IR ================= */
/* builtin compute in glsl.c; compiler + executor moved to glsl_ir.c */
gval ir_do_builtin(int id, const gval *a, int nargs, const sgl_tex *const *samplers)
{
    gval g; memset(&g,0,sizeof g); g.n=1; (void)nargs;
    switch (id) {
    case BI_TEXTURE2D: { int u=(int)a[0].v[0]; const sgl_tex *t=(u>=0&&u<GLSL_MAX_SAMPLERS)?samplers[u]:0;
        float o[4]={0,0,0,1}; if(t) sgl_sample(t,a[1].v[0],a[1].v[1],o); g.n=4; for(int i=0;i<4;i++)g.v[i]=o[i]; return g; }
    case BI_DOT: { int m=a[0].n<a[1].n?a[0].n:a[1].n; return gconst(g_dot(a[0].v,a[1].v,m)); }
    case BI_LENGTH: { return gconst(g_sqrt(g_dot(a[0].v,a[0].v,a[0].n))); }
    case BI_DISTANCE: { float dd[16]; int m=a[0].n; for(int i=0;i<m;i++)dd[i]=a[0].v[i]-a[1].v[i]; return gconst(g_sqrt(g_dot(dd,dd,m))); }
    case BI_NORMALIZE: { float s=g_dot(a[0].v,a[0].v,a[0].n); float iv=s>0?1.f/g_sqrt(s):0.f; g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=a[0].v[i]*iv; return g; }
    case BI_CROSS: g.n=3; g.v[0]=a[0].v[1]*a[1].v[2]-a[0].v[2]*a[1].v[1]; g.v[1]=a[0].v[2]*a[1].v[0]-a[0].v[0]*a[1].v[2]; g.v[2]=a[0].v[0]*a[1].v[1]-a[0].v[1]*a[1].v[0]; return g;
    case BI_MIX: { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float t=(a[2].n==1)?a[2].v[0]:a[2].v[i]; g.v[i]=a[0].v[i]*(1.f-t)+a[1].v[i]*t;} return g; }
    case BI_CLAMP: { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float lo=(a[1].n==1)?a[1].v[0]:a[1].v[i],hi=(a[2].n==1)?a[2].v[0]:a[2].v[i],x=a[0].v[i]; g.v[i]=x<lo?lo:(x>hi?hi:x);} return g; }
    case BI_MIN: { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=a[0].v[i]<y?a[0].v[i]:y;} return g; }
    case BI_MAX: { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=a[0].v[i]>y?a[0].v[i]:y;} return g; }
    case BI_ABS: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_abs(a[0].v[i]); return g;
    case BI_FLOOR: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_floor(a[0].v[i]); return g;
    case BI_CEIL: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_ceil(a[0].v[i]); return g;
    case BI_FRACT: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=a[0].v[i]-g_floor(a[0].v[i]); return g;
    case BI_SIGN: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=(a[0].v[i]>0)-(a[0].v[i]<0); return g;
    case BI_SQRT: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_sqrt(a[0].v[i]); return g;
    case BI_INVERSESQRT: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=a[0].v[i]>0?1.f/g_sqrt(a[0].v[i]):0.f; return g;
    case BI_MOD: { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=a[0].v[i]-y*g_floor(a[0].v[i]/y);} return g; }
    case BI_STEP: { int m=a[1].n; g.n=m; for(int i=0;i<m;i++){float ed=(a[0].n==1)?a[0].v[0]:a[0].v[i]; g.v[i]=a[1].v[i]<ed?0.f:1.f;} return g; }
    case BI_SMOOTHSTEP: { int m=a[2].n; g.n=m; for(int i=0;i<m;i++){float e0=(a[0].n==1)?a[0].v[0]:a[0].v[i],e1=(a[1].n==1)?a[1].v[0]:a[1].v[i],x=a[2].v[i]; float t=(x-e0)/(e1-e0); t=t<0?0:(t>1?1:t); g.v[i]=t*t*(3.f-2.f*t);} return g; }
    case BI_REFLECT: { int m=a[0].n; float d=g_dot(a[1].v,a[0].v,m); float td=d+d; g.n=m; for(int i=0;i<m;i++)g.v[i]=__builtin_fmaf(-td,a[1].v[i],a[0].v[i]); return g; }
    case BI_POW: { int m=a[0].n; g.n=m; for(int i=0;i<m;i++){float y=(a[1].n==1)?a[1].v[0]:a[1].v[i]; g.v[i]=g_pow(a[0].v[i],y);} return g; }
    case BI_SIN: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_sin(a[0].v[i]); return g;
    case BI_COS: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_cos(a[0].v[i]); return g;
    case BI_TAN: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_tan(a[0].v[i]); return g;
    case BI_EXP: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_exp(a[0].v[i]); return g;
    case BI_LOG: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_log(a[0].v[i]); return g;
    case BI_EXP2: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_exp2(a[0].v[i]); return g;
    case BI_LOG2: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_log2(a[0].v[i]); return g;
    case BI_RADIANS: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_radians(a[0].v[i]); return g;
    case BI_DEGREES: g.n=a[0].n; for(int i=0;i<a[0].n;i++)g.v[i]=g_degrees(a[0].v[i]); return g;
    case BI_TEXTURE2DPROJ: { int u=(int)a[0].v[0]; const sgl_tex *t=(u>=0&&u<GLSL_MAX_SAMPLERS)?samplers[u]:0; float q=(a[1].n>=4)?a[1].v[3]:a[1].v[2]; if(q==0.f)q=1.f; float o[4]={0,0,0,1}; if(t) sgl_sample(t,a[1].v[0]/q,a[1].v[1]/q,o); g.n=4; for(int i=0;i<4;i++)g.v[i]=o[i]; return g; }
    case BI_TEXTURECUBE: { int u=(int)a[0].v[0]; const sgl_tex *t=(u>=0&&u<GLSL_MAX_SAMPLERS)?samplers[u]:0; float o[4]={0,0,0,1}; if(t) sample_cube_tex(t,a[1].v[0],a[1].v[1],a[1].v[2],o); g.n=4; for(int i=0;i<4;i++)g.v[i]=o[i]; return g; }
    }
    return g;
}

/* ---- executor (compiler in glsl_ir.c) ---- */
int ir_run_fragment_u(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], const float *ustore, float rgba[4])
{
    if (!ustore) ustore = p->ustore;
    ir_prog *ir=&p->fs_ir;
    gval reg[IR_MAXREG];
    for (int i=0;i<ir->nlocal;i++){ reg[i].n=1; reg[i].v[0]=0; }   /* locals start zeroed */
    rgba[0]=rgba[1]=rgba[2]=rgba[3]=0; rgba[3]=1;
    int discarded=0;
    for (int ip=0; ip<ir->ninst; ++ip) {
        ir_inst *o=&ir->inst[ip];
        switch (o->op) {
        case IR_LOADK: { gval *d=&reg[o->dst]; d->n=1; d->v[0]=o->k; break; }
        case IR_LOADZ: { gval *d=&reg[o->dst]; d->n=o->i0; for(int i=0;i<o->i0&&i<16;i++)d->v[i]=0; break; }
        case IR_LOADFC: { /* gl_FragCoord for scalar path */
            gval *d=&reg[o->dst]; d->n=4;
            d->v[0]=sgl_scalar_frag_x; d->v[1]=sgl_scalar_frag_y; d->v[2]=0.f; d->v[3]=1.f; break; }
        case IR_LOADV: { gval *d=&reg[o->dst]; d->n=o->i1; for(int i=0;i<o->i1;i++)d->v[i]=varying[o->i0+i]; break; }
        case IR_LOADU: { gval *d=&reg[o->dst]; int ui=o->i0; d->n=o->i1; for(int i=0;i<o->i1;i++)d->v[i]=ustore[p->uni[ui].offset+i]; break; }
        case IR_LOADS: { gval *d=&reg[o->dst]; d->n=1; d->v[0]=(float)p->uni[o->i0].unit; break; }
        case IR_SWZ: { gval *s=&reg[o->a]; gval *d=&reg[o->dst]; gval t; t.n=o->swzn; for(int i=0;i<o->swzn;i++)t.v[i]=s->v[o->swz[i]]; *d=t; break; }
        case IR_NEG: { gval *s=&reg[o->a]; gval *d=&reg[o->dst]; d->n=s->n; for(int i=0;i<s->n;i++)d->v[i]=-s->v[i]; break; }
        case IR_NOT: { gval *s=&reg[o->a]; gval *d=&reg[o->dst]; d->n=1; d->v[0]=(s->v[0]==0.f)?1.f:0.f; break; }
        case IR_INDEX: { gval *s=&reg[o->a]; gval *d=&reg[o->dst]; int ix=o->i0;
            if (s->n==16){ d->n=4; for(int i=0;i<4;i++)d->v[i]=s->v[ix*4+i]; } else { d->n=1; d->v[0]=(ix>=0&&ix<s->n)?s->v[ix]:0.f; } break; }
        case IR_CMP: { gval *a=&reg[o->a],*b=&reg[o->b]; gval *d=&reg[o->dst]; float x=a->v[0],y=b->v[0],r=0;
            switch(o->i0){case '<':r=x<y;break;case '>':r=x>y;break;case 'L':r=x<=y;break;case 'G':r=x>=y;break;case 'E':r=x==y;break;case 'N':r=x!=y;break;case 'O':r=(x!=0.f||y!=0.f);break;case 'A':r=(x!=0.f&&y!=0.f);break;}
            d->n=1; d->v[0]=r?1.f:0.f; break; }
        case IR_TERN: { gval *cnd=&reg[o->a]; *(&reg[o->dst]) = (cnd->v[0]!=0.f)? reg[o->b] : reg[o->c]; break; }
        case IR_BIN: { gval *a=&reg[o->a],*b=&reg[o->b]; gval *d=&reg[o->dst]; int op=o->i0;
            if (op=='*' && a->n==16 && b->n==4){ gval t; t.n=4; g_matvec(a->v, b->v, t.v, 4); *d=t; break; }
            int n1=a->n,n2=b->n,rn=n1>n2?n1:n2; gval t; t.n=rn;
            for(int i=0;i<rn;i++){ float x=(n1==1)?a->v[0]:a->v[i]; float y=(n2==1)?b->v[0]:b->v[i];
                switch(op){case '+':t.v[i]=x+y;break;case '-':t.v[i]=x-y;break;case '*':t.v[i]=x*y;break;case '/':t.v[i]=y!=0.f?x/y:0.f;break;} }
            *d=t; break; }
        case IR_CONSTRUCT: { gval *d=&reg[o->dst]; int want=o->i0; float flat[16]; int fn=0;
            for(int i=0;i<o->nargs;i++){ gval *s=&reg[o->args[i]]; for(int j=0;j<s->n&&fn<16;j++)flat[fn++]=s->v[j]; }
            d->n=want; if(fn==1){for(int i=0;i<want;i++)d->v[i]=flat[0];} else {for(int i=0;i<want;i++)d->v[i]=(i<fn)?flat[i]:0.f;} break; }
        case IR_BUILTIN: { gval ar[8]; for(int i=0;i<o->nargs;i++)ar[i]=reg[o->args[i]]; reg[o->dst]=ir_do_builtin(o->i0,ar,o->nargs,samplers); break; }
        case IR_STOREL: { gval *d=&reg[o->dst]; gval *v=&reg[o->a];
            if (o->swzn){ for(int i=0;i<o->swzn;i++) d->v[(int)o->swz[i]]=(v->n==1)?v->v[0]:v->v[i]; if(d->n<4)d->n=4; }
            else { d->n=v->n; for(int i=0;i<v->n;i++)d->v[i]=v->v[i]; if(o->i0==-2 && o->i1>d->n)d->n=o->i1; } break; }
        case IR_STOREF: { gval *v=&reg[o->a];
            if (o->swzn){ for(int i=0;i<o->swzn;i++) rgba[(int)o->swz[i]]=(v->n==1)?v->v[0]:v->v[i]; }
            else { for(int i=0;i<v->n&&i<4;i++) rgba[i]=v->v[i]; } break; }
        case IR_DISCARD: if(o->a<0 || reg[o->a].v[0]!=0.f){ discarded=1; goto done; } break;
        }
    }
done:
    return discarded ? 0 : 1;
}

int ir_run_fragment(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float rgba[4])
{
    return ir_run_fragment_u(p, varying, samplers, p->ustore, rgba);
}

/* SIMT executor (A3) moved to glsl_simt.c */

/* ---------------- public API ---------------- */
glsl_program *glsl_program_create(void){ glsl_program *p=(glsl_program*)malloc(sizeof(glsl_program)); if(p) memset(p,0,sizeof(*p)); return p; }
static void glsl_dbg_unregister(glsl_program *p);   /* fwd: drop from the workload registry */
void glsl_program_destroy(glsl_program *p){ if(p){ glsl_dbg_unregister(p); arena_free(&p->vs.ast_arena); arena_free(&p->fs.ast_arena); ir_free(&p->fs_ir); ir_free(&p->vs_ir); free(p); } }
static int glsl_src_contains(const char *s, const char *needle)
{ int n=0; while(needle[n])++n; for(;*s;++s) if(!strncmp(s,needle,(size_t)n))return 1; return 0; }

int glsl_attach_source(glsl_program *p, int stage, const char *src, char *err, int errlen)
{
    shader *sh = stage==0 ? &p->vs : &p->fs;
    if (glsl_link_dump && stage == 1 && src) {   /* dump the exact gl4es-generated FS GLSL */
        printf("[fssrc] >>>>>>>>>>\n%s\n[fssrc] <<<<<<<<<<\n", src);
    }
    if (glsl_link_dump && stage == 0 && src && glsl_src_contains(src, "_gl4es_Vertex_ProgramEnv_17"))
        printf("[vssrc-interaction] >>>>>>>>>>\n%s\n[vssrc-interaction] <<<<<<<<<<\n", src);
    arena_free(&sh->ast_arena);   /* reclaim a prior parse's AST before re-parsing this stage */
    return parse_shader(sh, src, err, errlen);
}

/* declared #version number, or 0 if none */
static int find_glsl_version(const char *src)
{
    for (const char *p = src; *p; ++p) {
        if (*p != '#') continue;
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t') ++q;
        if (!strncmp(q, "version", 7)) {
            q += 7; while (*q == ' ' || *q == '\t') ++q;
            int v = 0; while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); ++q; }
            return v;
        }
        while (*p && *p != '\n') ++p;
        if (!*p) break;
    }
    return 0;
}

int glsl_try_compile(int stage, const char *src, char *err, int errlen)
{
    (void)stage;
    int ver = find_glsl_version(src);
    if (ver != 0 && ver != 100) {          /* GLSL ES 1.00 only — softgles2 is a conformant ES2 driver */
        if (err && errlen > 0) { const char *m = "only GLSL ES 1.00 (#version 100) is supported"; int i=0; for(;m[i]&&i<errlen-1;i++)err[i]=m[i]; err[i]=0; }
        return 0;
    }
    shader *sh = (shader *)malloc(sizeof(shader));
    if (!sh) return 0;
    int ok = parse_shader(sh, src, err, errlen);   /* sets ast_arena.head before allocating nodes */
    arena_free(&sh->ast_arena);                     /* was leaking the whole probe AST */
    free(sh);
    return ok;
}

void glsl_bind_attrib(glsl_program *p, int index, const char *name)
{
    if (p->nabind >= GLSL_MAX_ATTRIBS) return;
    int i = p->nabind++, k = 0;
    for (; name[k] && k < 39; ++k) p->abind[i].name[k] = name[k];
    p->abind[i].name[k] = 0;
    p->abind[i].index = index;
}

int glsl_link_dump = 0;   /* when set, print each linked FS's fingerprint at link */

static uint32_t glsl_ir_signature(const ir_prog *ir)
{
    uint32_t h = 2166136261u;
#define HSIG(V) do { uint32_t _v=(uint32_t)(V); for(int _b=0;_b<4;++_b){h^=(_v>>(8*_b))&255u;h*=16777619u;} } while(0)
    HSIG(ir->ninst); HSIG(ir->nreg); HSIG(ir->nlocal);
    for (int i=0;i<ir->ninst;++i) {
        const ir_inst *o=&ir->inst[i];
        HSIG(o->op); HSIG(o->dst); HSIG(o->a); HSIG(o->b); HSIG(o->c);
        HSIG(o->nargs); for(int j=0;j<8;++j) HSIG(o->args[j]);
        HSIG(o->i0); HSIG(o->i1); HSIG(o->swzn);
        for(int j=0;j<4;++j) HSIG(o->swz[j]);
        union { float f; uint32_t u; } k={o->k}; HSIG(k.u);
    }
#undef HSIG
    return h;
}

/* Shader-workload registry (active with glsl_link_dump): track linked programs so
 * the per-frame report (glsl_dbg_workload_report) can rank them by REAL fragment
 * cost — the archetype-specialisation work-list. */
#define GLSL_DBG_MAXPROG 128
static glsl_program *g_dbg_progs[GLSL_DBG_MAXPROG];
static int g_dbg_nprogs;
static int g_dbg_prog_seq;

static void glsl_dbg_register(glsl_program *p)
{
    for (int i = 0; i < g_dbg_nprogs; ++i) if (g_dbg_progs[i] == p) return;
    if (g_dbg_nprogs < GLSL_DBG_MAXPROG) g_dbg_progs[g_dbg_nprogs++] = p;
}

static void glsl_dbg_unregister(glsl_program *p)
{
    for (int i = 0; i < g_dbg_nprogs; ++i)
        if (g_dbg_progs[i] == p) { g_dbg_progs[i] = g_dbg_progs[--g_dbg_nprogs]; return; }
}

/* Record one draw's blend class against its program (glsl_link_dump only):
 * cls 0=opaque (blend off), 1=additive, 2=alpha/other. Resolves whether a hot
 * shader is opaque surface shading or additive light overdraw. */
void glsl_dbg_record_draw_class(glsl_program *p, int cls)
{
    if (!glsl_link_dump || !p) return;
    if (cls == 0) p->dbg_draws_opaque++;
    else if (cls == 1) p->dbg_draws_add++;
    else p->dbg_draws_alpha++;
}

/* Registry totals for callers that render this themselves (the on-screen stats
 * overlay) rather than parsing the [fswork] serial dump. Counts only programs
 * that actually shaded a fragment -- "linked but never used" is noise on a HUD.
 * nkernel is how many of those ran a specialised kernel rather than the VM,
 * which is the single number that says whether shader specialisation is
 * covering the real workload. */
void glsl_dbg_workload_totals(int *nprogs, int *nkernel,
                              unsigned long *frags_kernel, unsigned long *frags_vm)
{
    int np = 0, nk = 0;
    unsigned long fk = 0, fv = 0;
    for (int i = 0; i < g_dbg_nprogs; ++i) {
        glsl_program *p = g_dbg_progs[i];
        if (!p) continue;
        unsigned long f = p->dbg_frags_kernel + p->dbg_frags_vm;
        if (!f) continue;
        ++np;
        if (p->dbg_kernel_name) ++nk;
        fk += p->dbg_frags_kernel;
        fv += p->dbg_frags_vm;
    }
    if (nprogs) *nprogs = np;
    if (nkernel) *nkernel = nk;
    if (frags_kernel) *frags_kernel = fk;
    if (frags_vm) *frags_vm = fv;
}

void glsl_dbg_workload_report(void)
{
    if (!g_dbg_nprogs) return;
    unsigned long total = 0;
    for (int i = 0; i < g_dbg_nprogs; ++i)
        if (g_dbg_progs[i]) total += g_dbg_progs[i]->dbg_frags_kernel + g_dbg_progs[i]->dbg_frags_vm;
    if (!total) return;
    printf("[fswork] === shader workload (cumulative frags=%lu) ===\n", total);
    /* non-destructive selection-rank by total frags desc; N is tiny (<=128). */
    char done[GLSL_DBG_MAXPROG];
    for (int i = 0; i < g_dbg_nprogs; ++i) done[i] = 0;
    for (int rank = 0; rank < g_dbg_nprogs; ++rank) {
        int best = -1; unsigned long bestf = 0;
        for (int i = 0; i < g_dbg_nprogs; ++i) {
            if (done[i] || !g_dbg_progs[i]) continue;
            unsigned long f = g_dbg_progs[i]->dbg_frags_kernel + g_dbg_progs[i]->dbg_frags_vm;
            if (best < 0 || f > bestf) { bestf = f; best = i; }
        }
        if (best < 0) break;
        done[best] = 1;
        glsl_program *p = g_dbg_progs[best];
        unsigned long f = p->dbg_frags_kernel + p->dbg_frags_vm;
        if (f == 0) continue;   /* linked but never shaded a fragment this run */
        printf("[fswork]  id=%d %3lu%% frags=%lu (kernel=%lu vm=%lu) path=%s ninst=%d nvaryf=%d nuni=%d draws[op=%lu add=%lu alpha=%lu]\n",
               p->dbg_id, f * 100 / total, f, p->dbg_frags_kernel, p->dbg_frags_vm,
               p->dbg_kernel_name ? p->dbg_kernel_name : "VM",
               p->fs_ir.ninst, p->nvaryfloats, p->nuni,
               p->dbg_draws_opaque, p->dbg_draws_add, p->dbg_draws_alpha);
    }
}

int glsl_link(glsl_program *p, char *err, int errlen)
{
    /* honour app-bound attribute locations (glBindAttribLocation before link) */
    for (int i = 0; i < p->vs.nglob; ++i)
        if (p->vs.globals[i].qual == Q_ATTRIB)
            for (int b = 0; b < p->nabind; ++b)
                if (!strcmp(p->abind[b].name, p->vs.globals[i].name)) { p->vs.globals[i].location = p->abind[b].index; break; }

    p->nvary=0; p->nvaryfloats=0; p->nuni=0;
    /* The uniform/attribute tables are about to be rebuilt, so any cached
     * name->index answers held by the RVV kernels are now meaningless. */
    /* glsl_fs_classify rebuilds kernel_binding after successful linking. */
    /* varyings: those declared in the VS (FS should match by name) */
    for (int i=0;i<p->vs.nglob;i++) if(p->vs.globals[i].qual==Q_VARYING) {
        glvar *g=&p->vs.globals[i]; int comp=type_comp(g->type); int arr=g->arr>0?g->arr:1;
        if (p->nvary<16){ int k=0; for(;g->name[k];k++)p->vary[p->nvary].name[k]=g->name[k]; p->vary[p->nvary].name[k]=0;
            p->vary[p->nvary].comp=comp; p->vary[p->nvary].arr=arr; p->vary[p->nvary].offset=p->nvaryfloats; p->nvary++; p->nvaryfloats+=comp*arr; }
    }
    /* uniforms: union of vs+fs */
    shader *st[2]={&p->vs,&p->fs};
    int ucur=0;
    for (int s=0;s<2;s++) for(int i=0;i<st[s]->nglob;i++) if(st[s]->globals[i].qual==Q_UNIFORM) {
        glvar *g=&st[s]->globals[i];
        if (find_uniform(p,g->name)>=0) continue;
        if (p->nuni>=GLSL_MAX_UNIFORMS) {
            if (err && errlen>0) { const char *m="too many uniforms"; int z=0; for(;m[z]&&z<errlen-1;z++)err[z]=m[z]; err[z]=0; }
            p->linked=0; return 0;
        }
        int nc = (g->type==T_SAMPLER2D||g->type==T_SAMPLERCUBE) ? 0 : type_comp(g->type)*(g->arr>0?g->arr:1);
        if (ucur + nc > GLSL_USTORE_FLOATS) {   /* truthful cap: fail, don't silently clamp/corrupt */
            if (err && errlen>0) { const char *m="uniform storage exceeded"; int z=0; for(;m[z]&&z<errlen-1;z++)err[z]=m[z]; err[z]=0; }
            p->linked=0; return 0;
        }
        int idx=p->nuni++; int k=0; for(;g->name[k];k++)p->uni[idx].name[k]=g->name[k]; p->uni[idx].name[k]=0;
        p->uni[idx].type=g->type;
        if (g->type==T_SAMPLER2D||g->type==T_SAMPLERCUBE){
            p->uni[idx].is_sampler=1;
            p->uni[idx].unit=0;       /* GLES 2.0 default for ordinary samplers */
            p->uni[idx].offset=-1;

            /* gl4es' ARB-program converter encodes the source texture image
             * unit in these internal uniform names. Unlike its FPE
             * _gl4es_TexSampler_N path, gl4es 1.1.7 does not upload initial
             * values for _gl4es_Sampler2D_N/_gl4es_SamplerCube_N. Leaving all
             * of them at the GLES default unit zero makes the Doom 3
             * interaction shader sample the normalization cube for bump,
             * projection, falloff, diffuse, specular and lookup inputs, which
             * collapses the light term and renders the world black.
             *
             * Treat this as driver-side compatibility for gl4es-generated
             * shaders. A later explicit glUniform1i still overrides it. */
            static const char s2d[] = "_gl4es_Sampler2D_";
            static const char scube[] = "_gl4es_SamplerCube_";
            const char *q = 0;
            int plen = 0;
            if (!strncmp(g->name, s2d, sizeof(s2d)-1)) { q = g->name + sizeof(s2d)-1; plen = 1; }
            else if (!strncmp(g->name, scube, sizeof(scube)-1)) { q = g->name + sizeof(scube)-1; plen = 1; }
            if (plen && q && *q >= '0' && *q <= '9') {
                int unit = 0;
                while (*q >= '0' && *q <= '9') { unit = unit * 10 + (*q++ - '0'); }
                if (*q == 0 && unit < GLSL_MAX_SAMPLERS) p->uni[idx].unit = unit;
            }
        }
        else { p->uni[idx].is_sampler=0; p->uni[idx].offset=ucur; ucur+=nc; }
    }
    (void)err;(void)errlen;
    p->fs_kernel = 0;
    p->linked = p->vs.has_main && p->fs.has_main;
    if (p->linked) { ir_compile_fs(p);   /* A2: best-effort FS bytecode IR (else tree-walk) */
                     ir_compile_vs(p);   /* SIMT T&L: best-effort VS bytecode IR (else scalar) */
                     glsl_fs_classify(p); } /* bind a register-resident kernel if FS matches */
    if (glsl_link_dump && p->linked) {
        p->dbg_id = ++g_dbg_prog_seq;
        glsl_dbg_register(p);
        const ir_prog *ir = &p->fs_ir;
        int t2=0,tp=0,tc=0,nm=0,pw=0,dt=0,ot=0;
        for (int i=0;i<ir->ninst;i++){ const ir_inst *o=&ir->inst[i]; if(o->op==IR_BUILTIN){ switch(o->i0){
            case BI_TEXTURE2D:t2++;break; case BI_TEXTURE2DPROJ:tp++;break; case BI_TEXTURECUBE:tc++;break;
            case BI_NORMALIZE:nm++;break; case BI_POW:pw++;break; case BI_DOT:dt++;break; default:ot++; } } }
        printf("[fsdump] id=%d path=%s ok=%d simt=%d ninst=%d nvaryf=%d nuni=%d vs(ninst=%d sig=%08x) | tex2d=%d proj=%d cube=%d norm=%d pow=%d dot=%d oth=%d\n",
               p->dbg_id, p->dbg_kernel_name ? p->dbg_kernel_name : "VM",
               ir->ok, ir->simt_ok, ir->ninst, p->nvaryfloats, p->nuni,
               p->vs_ir.ninst, glsl_ir_signature(&p->vs_ir), t2,tp,tc,nm,pw,dt,ot);
        for (int i=0;i<p->nvary;i++) printf("[fsdump]   vary[%d] %s comp=%d off=%d\n", i, p->vary[i].name, p->vary[i].comp, p->vary[i].offset);
        for (int i=0;i<p->nuni;i++)  printf("[fsdump]   uni  %s %s=%d\n", p->uni[i].name, p->uni[i].is_sampler?"unit":"off", p->uni[i].is_sampler?p->uni[i].unit:p->uni[i].offset);
    }
    return p->linked;
}

int glsl_attrib_location(const glsl_program *p, const char *name)
{
    for (int i=0;i<p->vs.nglob;i++) if(p->vs.globals[i].qual==Q_ATTRIB && !strcmp(p->vs.globals[i].name,name)) return p->vs.globals[i].location;
    return -1;
}
int glsl_uniform_location(glsl_program *p, const char *name){ return find_uniform(p,name); }
int glsl_num_varying_floats(const glsl_program *p){ return p->nvaryfloats; }
const float *glsl_ustore(const glsl_program *p){ return p->ustore; }
int glsl_fs_has_ir(const glsl_program *p){ return p->fs_ir.ok; }  /* A2: FS ran via bytecode IR? */

/* GLES2 type enum from our internal type. */
static int gltype_of(int t)
{
    switch (t) {
    case T_FLOAT:     return 0x1406; /* GL_FLOAT       */
    case T_VEC2:      return 0x8B50; /* GL_FLOAT_VEC2  */
    case T_VEC3:      return 0x8B51; /* GL_FLOAT_VEC3  */
    case T_VEC4:      return 0x8B52; /* GL_FLOAT_VEC4  */
    case T_MAT4:      return 0x8B5C; /* GL_FLOAT_MAT4  */
    case T_SAMPLER2D: return 0x8B5E; /* GL_SAMPLER_2D  */
    case T_SAMPLERCUBE: return 0x8B60; /* GL_SAMPLER_CUBE */
    default:          return 0x1406;
    }
}
static void copy_name(char *dst, int dlen, const char *src)
{
    int k = 0; if (dlen <= 0) return;
    for (; src[k] && k < dlen - 1; ++k) dst[k] = src[k];
    dst[k] = 0;
}

int glsl_num_attribs(const glsl_program *p)
{
    int n = 0;
    for (int i = 0; i < p->vs.nglob; ++i) if (p->vs.globals[i].qual == Q_ATTRIB) ++n;
    return n;
}
int glsl_get_attrib(const glsl_program *p, int index, char *name, int namelen,
                    int *size, int *gltype, int *location)
{
    int n = 0;
    for (int i = 0; i < p->vs.nglob; ++i) if (p->vs.globals[i].qual == Q_ATTRIB) {
        if (n == index) {
            const glvar *g = &p->vs.globals[i];
            if (name) copy_name(name, namelen, g->name);
            if (size) *size = 1;
            if (gltype) *gltype = gltype_of(g->type);
            if (location) *location = g->location;
            return 1;
        }
        ++n;
    }
    return 0;
}
int glsl_num_uniforms(const glsl_program *p){ return p->nuni; }
/* Bitmask of texture units bound to samplerCube uniforms — lets the draw path
 * resolve a samplerCube to the unit's CUBE binding (vs a sampler2D to the 2D
 * binding) now that softgles2 tracks the two bind points separately. */
unsigned glsl_cube_sampler_unit_mask(const glsl_program *p)
{
    unsigned m = 0;
    for (int i = 0; i < p->nuni; ++i)
        if (p->uni[i].is_sampler && p->uni[i].type == T_SAMPLERCUBE &&
            p->uni[i].unit >= 0 && p->uni[i].unit < 32)
            m |= 1u << p->uni[i].unit;
    return m;
}
int glsl_get_uniform(const glsl_program *p, int index, char *name, int namelen,
                     int *size, int *gltype)
{
    if (index < 0 || index >= p->nuni) return 0;
    if (name) copy_name(name, namelen, p->uni[index].name);
    if (size) *size = 1;
    if (gltype) *gltype = gltype_of(p->uni[index].type);
    return 1;
}

void glsl_uniform(glsl_program *p, int loc, const float *data, int nfloats)
{
    if (loc<0||loc>=p->nuni||p->uni[loc].is_sampler) return;
    int off=p->uni[loc].offset; for(int i=0;i<nfloats && off+i<GLSL_USTORE_FLOATS;i++) p->ustore[off+i]=data[i];
}
void glsl_uniform_sampler(glsl_program *p, int loc, int unit)
{
    if (loc < 0 || loc >= p->nuni || !p->uni[loc].is_sampler) return;

    /* gl4es ARB-program samplers have a fixed source texture unit encoded in
     * their internal name. gl4es may subsequently replay a zero-initialised
     * uniform cache over the driver after link; accepting that replay collapses
     * every input back onto unit zero and undoes the link-time compatibility
     * default above. Preserve the converter's explicit unit contract. */
    static const char s2d[] = "_gl4es_Sampler2D_";
    static const char scube[] = "_gl4es_SamplerCube_";
    const char *q = 0;
    if (!strncmp(p->uni[loc].name, s2d, sizeof(s2d)-1)) q = p->uni[loc].name + sizeof(s2d)-1;
    else if (!strncmp(p->uni[loc].name, scube, sizeof(scube)-1)) q = p->uni[loc].name + sizeof(scube)-1;
    if (q && *q >= '0' && *q <= '9') {
        int fixed = 0;
        while (*q >= '0' && *q <= '9') fixed = fixed * 10 + (*q++ - '0');
        if (*q == 0 && fixed < GLSL_MAX_SAMPLERS) { p->uni[loc].unit = fixed; return; }
    }
    p->uni[loc].unit = unit;
}

void glsl_run_vertex(glsl_program *p, const float attribs[GLSL_MAX_ATTRIBS][4], float clip[4], float *varying_out)
{
    env e; memset(&e,0,sizeof(e));
    e.prog=p; e.ustore=p->ustore; e.sh=&p->vs; e.stage=0; e.attribs=attribs; e.clip=clip; e.varying_out=varying_out;
    clip[0]=0;clip[1]=0;clip[2]=0;clip[3]=1;
    for (int i=0;i<p->nvaryfloats;i++) varying_out[i]=0;
    exec_body(&e,&p->vs);
}

/* Tree-walk fragment evaluation — the correctness ORACLE (and the fallback when
 * the bytecode IR can't compile a shader). */
int glsl_run_fragment_ref_u(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], const float *ustore, float rgba[4])
{
    /* A1 (perf): do NOT memset the whole env per pixel — env.locals[64] is ~6.9 KB,
     * so the old `memset(&e,0,sizeof e)` zeroed ~7 KB PER FRAGMENT (~1-2 GB/frame).
     * locals[] is only ever read up to nlocals (eval_var/write_lvalue), and each
     * local is fully initialised when created, so zeroing nlocals+discarded is
     * sufficient. The fragment path never reads attribs/clip/varying_out (those
     * are stage-0 only), so they can stay uninitialised. */
    env e;
    e.prog=p; e.ustore=ustore?ustore:p->ustore; e.sh=&p->fs; e.stage=1; e.varying_in=varying; e.samplers=samplers; e.fragcolor=rgba;
    e.nlocals=0; e.discarded=0;
    rgba[0]=0;rgba[1]=0;rgba[2]=0;rgba[3]=1;
    exec_body(&e,&p->fs);
    return e.discarded ? 0 : 1;
}

int glsl_run_fragment_ref(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float rgba[4])
{
    return glsl_run_fragment_ref_u(p, varying, samplers, p->ustore, rgba);
}

/* ustore: per-call uniform source (NULL -> p->ustore). The deferred FrameBin
 * raster passes a per-draw snapshot so worker harts shade each recorded command
 * with its own uniforms. */
int glsl_run_fragment_u(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], const float *ustore, float rgba[4])
{
    /* A2: run the flat bytecode IR when the FS compiled to one; else fall back to
     * the tree-walk oracle (so correctness is never at risk). */
    if (p->fs_ir.ok)
        return ir_run_fragment_u(p, varying, samplers, ustore?ustore:p->ustore, rgba);
    return glsl_run_fragment_ref_u(p, varying, samplers, ustore, rgba);
}

int glsl_run_fragment(glsl_program *p, const float *varying, const sgl_tex *const samplers[GLSL_MAX_SAMPLERS], float rgba[4])
{
    return glsl_run_fragment_u(p, varying, samplers, p->ustore, rgba);
}
