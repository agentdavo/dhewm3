/*
 * softgles2 — GLSL ES 1.00 bytecode IR compiler (A2). Extracted from glsl.c.
 */
#include <stdlib.h>
#include <string.h>

#include "glsl_internal.h"

#pragma GCC diagnostic ignored "-Wmisleading-indentation"

/* ---- compiler — IR_MAXREG/IR_MAXINST in glsl_internal.h ---- */
typedef struct {
    glsl_program *p; shader *sh;
    int  stage;
    char lname[IR_MAXREG][40]; int nlocal;
    int  ntemp_base, ntemp;
    ir_inst inst[IR_MAXINST]; int ninst;
    int  fail;
} ircomp;

static int ir_local_slot(ircomp *c, const char *name, int create)
{
    for (int i=0;i<c->nlocal;i++) if(!strcmp(c->lname[i],name)) return i;
    if (!create) return -1;
    if (c->nlocal>=IR_MAXREG) { c->fail=1; return 0; }
    int s=c->nlocal++; int k=0; for(;name[k]&&k<39;k++)c->lname[s][k]=name[k]; c->lname[s][k]=0; return s;
}
static int ir_tmp(ircomp *c){ int r=c->ntemp_base + c->ntemp++; if(r>=IR_MAXREG){c->fail=1; r=0;} return r; }
static ir_inst *ir_emit(ircomp *c){ if(c->ninst>=IR_MAXINST){c->fail=1; return &c->inst[0];} ir_inst *o=&c->inst[c->ninst++]; memset(o,0,sizeof*o); o->dst=o->a=o->b=o->c=-1; return o; }

static int ir_expr(ircomp *c, node *n);

static int builtin_id(const char *f)
{
    static const char *nm[BI_COUNT] = { "texture2D","dot","length","distance","normalize",
        "cross","mix","clamp","min","max","abs","floor","ceil","fract","sign","sqrt",
        "inversesqrt","mod","step","smoothstep","reflect","pow","texture2DProj","textureCube",
        /* index-parallel with the BI_* enum in glsl_internal.h */
        "sin","cos","tan","exp","log","exp2","log2","radians","degrees" };
    for (int i=0;i<BI_COUNT;i++) if(!strcmp(f,nm[i])) return i;
    return -1;
}

static int ir_load_var(ircomp *c, const char *name)
{
    int ls = ir_local_slot(c, name, 0);
    if (ls>=0) return ls;
    int r = ir_tmp(c); ir_inst *o;
    if (!strcmp(name,"gl_FragCoord")) {
        o=ir_emit(c); o->op=IR_LOADFC; o->dst=r; o->i0=4; return r;
    }
    if (!strcmp(name,"gl_FragColor")||!strcmp(name,"gl_Position")) {
        o=ir_emit(c); o->op=IR_LOADZ; o->dst=r; o->i0=4; return r;
    }
    glvar *gv=find_global(c->sh,name);
    if (gv && gv->qual==Q_ATTRIB && c->stage==0) {
        o=ir_emit(c); o->op=IR_LOADA; o->dst=r; o->i0=gv->location; o->i1=type_comp(gv->type); return r; }
    if (gv && gv->qual==Q_VARYING) { int comp; int off=vary_slot(c->p,name,-1,&comp); if(off<0){o=ir_emit(c);o->op=IR_LOADZ;o->dst=r;o->i0=1;return r;}
        o=ir_emit(c); o->op=IR_LOADV; o->dst=r; o->i0=off; o->i1=comp; return r; }
    if (gv && gv->qual==Q_UNIFORM) { int ui=find_uniform(c->p,name); if(ui<0){o=ir_emit(c);o->op=IR_LOADZ;o->dst=r;o->i0=1;return r;}
        if (c->p->uni[ui].is_sampler){ o=ir_emit(c); o->op=IR_LOADS; o->dst=r; o->i0=ui; return r; }
        o=ir_emit(c); o->op=IR_LOADU; o->dst=r; o->i0=ui; o->i1=type_comp(gv->type); return r; }
    o=ir_emit(c); o->op=IR_LOADZ; o->dst=r; o->i0=1; return r;
}

static int ir_expr(ircomp *c, node *n)
{
    if (c->fail || !n) { int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_LOADZ; o->dst=r; o->i0=1; return r; }
    switch (n->k) {
    case N_NUM: { int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_LOADK; o->dst=r; o->k=n->num; return r; }
    case N_VAR: return ir_load_var(c, n->name);
    case N_INDEX: {
        if (n->a->k==N_VAR) { glvar *gv=find_global(c->sh,n->a->name);
            if (gv && gv->qual==Q_VARYING) { int comp; int off=vary_slot(c->p,n->a->name,n->op,&comp);
                int r=ir_tmp(c); ir_inst *o=ir_emit(c); if(off<0){o->op=IR_LOADZ;o->dst=r;o->i0=1;}else{o->op=IR_LOADV;o->dst=r;o->i0=off;o->i1=comp;} return r; } }
        int s=ir_expr(c,n->a); int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_INDEX; o->dst=r; o->a=s; o->i0=n->op; return r; }
    case N_SWZ: { int s=ir_expr(c,n->a); int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_SWZ; o->dst=r; o->a=s; o->swzn=n->swzn; for(int i=0;i<n->swzn;i++)o->swz[i]=(signed char)n->swz[i]; return r; }
    case N_NEG: { int s=ir_expr(c,n->a); int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_NEG; o->dst=r; o->a=s; return r; }
    case N_NOT: { int s=ir_expr(c,n->a); int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_NOT; o->dst=r; o->a=s; return r; }
    case N_TERN: { int rc=ir_expr(c,n->a); int rb=ir_expr(c,n->b); int rd=ir_expr(c,n->c);
        int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_TERN; o->dst=r; o->a=rc; o->b=rb; o->c=rd; return r; }
    case N_CMP: { int ra=ir_expr(c,n->a); int rb=ir_expr(c,n->b); int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_CMP; o->dst=r; o->a=ra; o->b=rb; o->i0=n->op; return r; }
    case N_BIN: { int ra=ir_expr(c,n->a); int rb=ir_expr(c,n->b); int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_BIN; o->dst=r; o->a=ra; o->b=rb; o->i0=n->op; return r; }
    case N_CALL: {
        int tt; if (kw_type(n->name,&tt) && (tt==T_VEC2||tt==T_VEC3||tt==T_VEC4||tt==T_FLOAT)) {
            short argr[8]; for(int i=0;i<n->nargs;i++) argr[i]=(short)ir_expr(c,n->args[i]);
            int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_CONSTRUCT; o->dst=r; o->i0=type_comp(tt); o->nargs=n->nargs;
            for(int i=0;i<n->nargs;i++) o->args[i]=argr[i]; return r; }
        int bid=builtin_id(n->name);
        if (bid<0) { c->fail=1; int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_LOADZ; o->dst=r; o->i0=1; return r; }
        short argr[8]; for(int i=0;i<n->nargs;i++) argr[i]=(short)ir_expr(c,n->args[i]);
        int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_BUILTIN; o->dst=r; o->i0=bid; o->nargs=n->nargs;
        for(int i=0;i<n->nargs;i++) o->args[i]=argr[i]; return r; }
    }
    c->fail=1; { int r=ir_tmp(c); ir_inst *o=ir_emit(c); o->op=IR_LOADZ; o->dst=r; o->i0=1; return r; }
}

static void ir_stmt(ircomp *c, stmt *s)
{
    c->ntemp = 0;
    if (s->k==S_NOP) return;
    if (s->k==S_DISCARD) { int a = s->expr ? ir_expr(c,s->expr) : -1;
        ir_inst *o=ir_emit(c); o->op=IR_DISCARD; o->a=a; return; }
    if (s->k==S_DECL) {
        int slot=ir_local_slot(c,s->name,1);
        if (s->expr){ int r=ir_expr(c,s->expr); ir_inst *o=ir_emit(c); o->op=IR_STOREL; o->dst=slot; o->a=r; o->swzn=0; o->i0=-1; o->i1=type_comp(s->type); }
        else { ir_inst *o=ir_emit(c); o->op=IR_LOADZ; o->dst=slot; o->i0=type_comp(s->type); }
        return;
    }
    int r=ir_expr(c,s->expr);
    if (c->stage==0) {
        if (!strcmp(s->name,"gl_Position")) { if(s->idx>=0){c->fail=1;return;} ir_inst *o=ir_emit(c); o->op=IR_STORECLIP; o->a=r; o->swzn=(signed char)s->swzn; for(int i=0;i<s->swzn;i++)o->swz[i]=(signed char)s->swz[i]; return; }
        if (!strcmp(s->name,"gl_FragColor")||!strcmp(s->name,"gl_FragCoord")||!strcmp(s->name,"gl_PointSize")) { c->fail=1; return; }
        { glvar *gv=find_global(c->sh,s->name);
          if (gv && gv->qual==Q_VARYING) { if(s->idx>=0&&s->swzn){c->fail=1;return;} int comp; int off=vary_slot(c->p,s->name,s->idx,&comp); if(off<0){c->fail=1;return;}
              ir_inst *o=ir_emit(c); o->op=IR_STOREV; o->a=r; o->i0=off; o->i1=comp; o->swzn=(signed char)s->swzn; for(int i=0;i<s->swzn;i++)o->swz[i]=(signed char)s->swz[i]; return; } }
        if (s->idx>=0) { c->fail=1; return; }
        int slot=ir_local_slot(c,s->name,1);
        ir_inst *o=ir_emit(c); o->op=IR_STOREL; o->dst=slot; o->a=r; o->swzn=(signed char)s->swzn; for(int i=0;i<s->swzn;i++)o->swz[i]=(signed char)s->swz[i]; o->i0=-1; o->i1=0; return;
    }
    if (!strcmp(s->name,"gl_FragColor")) { ir_inst *o=ir_emit(c); o->op=IR_STOREF; o->a=r; o->swzn=(signed char)s->swzn; for(int i=0;i<s->swzn;i++)o->swz[i]=(signed char)s->swz[i]; return; }
    if (!strcmp(s->name,"gl_Position")||!strcmp(s->name,"gl_FragCoord")) { c->fail=1; return; }
    glvar *gv=find_global(c->sh,s->name);
    if (gv && gv->qual==Q_VARYING) { c->fail=1; return; }
    if (s->idx>=0) { c->fail=1; return; }
    int slot=ir_local_slot(c,s->name,1);
    ir_inst *o=ir_emit(c); o->op=IR_STOREL; o->dst=slot; o->a=r; o->swzn=(signed char)s->swzn; for(int i=0;i<s->swzn;i++)o->swz[i]=(signed char)s->swz[i]; o->i0=-1; o->i1=0;
}

void ir_free(ir_prog *ir){ if(ir->inst){ free(ir->inst); ir->inst=0; } ir->ok=0; ir->ninst=0; }

void ir_set_simt_ok(ir_prog *ir)
{
    ir->simt_ok = 1;
    for (int i=0;i<ir->ninst && ir->simt_ok;i++){ ir_inst *o=&ir->inst[i];
        if (o->op==IR_INDEX) { ir->simt_ok=0; break; }   /* IR_DISCARD now SIMT-vectorised (kill mask) */
        if (o->op==IR_CMP && (o->i0=='A' || o->i0=='O')) { ir->simt_ok=0; break; }
        if (o->op==IR_LOADV && o->i1>4) { ir->simt_ok=0; break; }
        if (o->op==IR_LOADU && o->i1>4) {
            if (o->i1!=16 && o->i1!=9) { ir->simt_ok=0; break; }
            int dr=o->dst, bad=0;
            for (int j=i+1;j<ir->ninst;j++){ ir_inst *u=&ir->inst[j];
                int usesa=(u->a==dr), usesb=(u->b==dr), usesc=(u->c==dr), usesarg=0;
                for(int k=0;k<u->nargs;k++) if(u->args[k]==dr) usesarg=1;
                if (usesa||usesb||usesc||usesarg) {
                    if (!(u->op==IR_BIN && u->i0=='*' && u->a==dr && !usesb && !usesc && !usesarg)) { bad=1; break; }
                }
                if (u->dst==dr) break;
            }
            if (bad) { ir->simt_ok=0; break; }
        }
        if (o->op==IR_LOADZ && o->i0>4) { ir->simt_ok=0; break; }
        if (o->op==IR_CONSTRUCT && o->i0>4) { ir->simt_ok=0; break; }
    }
}

void ir_compile_fs(glsl_program *p)
{
    ir_free(&p->fs_ir);
    if (!p->fs.has_main) return;
    if (p->fs.nfuncs>0) return;
    ircomp *c = (ircomp*)malloc(sizeof(ircomp));
    if (!c) return;
    c->p=p; c->sh=&p->fs; c->stage=1; c->nlocal=0; c->ninst=0; c->fail=0; c->ntemp=0;
    c->ntemp_base=0;
    for (int i=0;i<p->fs.nbody;i++){ stmt *s=&p->fs.body[i]; if(s->k==S_DECL) ir_local_slot(c,s->name,1); }
    for (int i=0;i<p->fs.nbody;i++){ stmt *s=&p->fs.body[i]; if(s->k==S_ASSIGN){
        if(strcmp(s->name,"gl_FragColor")&&strcmp(s->name,"gl_Position")&&strcmp(s->name,"gl_FragCoord")){
            glvar *gv=find_global(&p->fs,s->name); if(!gv) ir_local_slot(c,s->name,1); } } }
    c->ntemp_base=c->nlocal;
    for (int i=0;i<p->fs.nbody && !c->fail;i++) ir_stmt(c,&p->fs.body[i]);
    int nreg = c->ntemp_base + 32; if (nreg>IR_MAXREG) nreg=IR_MAXREG;
    if (c->fail || c->ninst==0 || c->ntemp_base>=IR_MAXREG) { free(c); return; }
    p->fs_ir.inst = (ir_inst*)malloc(sizeof(ir_inst)*c->ninst);
    if (!p->fs_ir.inst){ free(c); return; }
    memcpy(p->fs_ir.inst, c->inst, sizeof(ir_inst)*c->ninst);
    p->fs_ir.ninst=c->ninst; p->fs_ir.nlocal=c->nlocal; p->fs_ir.nreg=nreg; p->fs_ir.ok=1;
    p->fs_has_discard = 0;
    for (int i=0;i<p->fs_ir.ninst;i++) if (p->fs_ir.inst[i].op==IR_DISCARD) { p->fs_has_discard = 1; break; }
    ir_set_simt_ok(&p->fs_ir);
    free(c);
}

void ir_compile_vs(glsl_program *p)
{
    ir_free(&p->vs_ir);
    if (!p->vs.has_main) return;
    if (p->vs.nfuncs>0) return;
    ircomp *c = (ircomp*)malloc(sizeof(ircomp));
    if (!c) return;
    c->p=p; c->sh=&p->vs; c->stage=0; c->nlocal=0; c->ninst=0; c->fail=0; c->ntemp=0; c->ntemp_base=0;
    for (int i=0;i<p->vs.nbody;i++){ stmt *s=&p->vs.body[i]; if(s->k==S_DECL) ir_local_slot(c,s->name,1); }
    for (int i=0;i<p->vs.nbody;i++){ stmt *s=&p->vs.body[i]; if(s->k==S_ASSIGN){
        if(strncmp(s->name,"gl_",3)){ glvar *gv=find_global(&p->vs,s->name); if(!gv) ir_local_slot(c,s->name,1); } } }
    c->ntemp_base=c->nlocal;
    for (int i=0;i<p->vs.nbody && !c->fail;i++) ir_stmt(c,&p->vs.body[i]);
    int nreg = c->ntemp_base + 32; if (nreg>IR_MAXREG) nreg=IR_MAXREG;
    if (c->fail || c->ninst==0 || c->ntemp_base>=IR_MAXREG) { free(c); return; }
    p->vs_ir.inst = (ir_inst*)malloc(sizeof(ir_inst)*c->ninst);
    if (!p->vs_ir.inst){ free(c); return; }
    memcpy(p->vs_ir.inst, c->inst, sizeof(ir_inst)*c->ninst);
    p->vs_ir.ninst=c->ninst; p->vs_ir.nlocal=c->nlocal; p->vs_ir.nreg=nreg; p->vs_ir.ok=1;
    ir_set_simt_ok(&p->vs_ir);
    free(c);
}
