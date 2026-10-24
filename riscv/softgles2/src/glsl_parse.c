/*
 * softgles2 — GLSL ES 1.00 parser. Extracted from glsl.c.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#endif

#include "glsl_internal.h"

/* The parser/evaluator use compact one-line guarded statements deliberately. */
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

/* ---------------- parser ---------------- */
typedef struct { toklist *tl; shader *sh; char *err; int errlen; int failed; } parser;

static token *pk(parser *p){ return &p->tl->t[p->tl->pos]; }
static token *adv(parser *p){ token *t=&p->tl->t[p->tl->pos]; if(t->kind!=TK_EOF)p->tl->pos++; return t; }
static int  is_op(parser *p, const char *o){ token *t=pk(p); return t->kind==TK_OP && !strcmp(t->text,o); }
static int  is_kw(parser *p, const char *o){ token *t=pk(p); return t->kind==TK_ID && !strcmp(t->text,o); }
static int  eat_op(parser *p, const char *o){ if(is_op(p,o)){adv(p);return 1;} return 0; }

static void perr(parser *p, const char *m)
{
    if (!p->failed && p->err && p->errlen>0) {
        int i=0; const char *s=m; while(*s&&i<p->errlen-1)p->err[i++]=*s++; p->err[i]=0;
    }
    p->failed = 1;
}

int kw_type(const char *s, int *t)
{
    if(!strcmp(s,"void")){*t=T_VOID;return 1;}
    if(!strcmp(s,"float")){*t=T_FLOAT;return 1;}
    if(!strcmp(s,"vec2")){*t=T_VEC2;return 1;}
    if(!strcmp(s,"vec3")){*t=T_VEC3;return 1;}
    if(!strcmp(s,"vec4")){*t=T_VEC4;return 1;}
    if(!strcmp(s,"mat4")){*t=T_MAT4;return 1;}
    if(!strcmp(s,"sampler2D")){*t=T_SAMPLER2D;return 1;}
    if(!strcmp(s,"samplerCube")){*t=T_SAMPLERCUBE;return 1;}
    if(!strcmp(s,"int")){*t=T_INT;return 1;}
    if(!strcmp(s,"bool")){*t=T_BOOL;return 1;}
    return 0;
}

static int is_precision_qual_name(const char *s)
{
    return !strcmp(s,"lowp") || !strcmp(s,"mediump") || !strcmp(s,"highp");
}

static int is_builtin_fn(const char *s)
{
    static const char *b[] = { "texture2D","texture2DProj","textureCube","dot","length","distance",
        "normalize","cross","mix","clamp","min","max","abs","floor","ceil","fract","sign","sqrt",
        "inversesqrt","mod","step","smoothstep","reflect","pow",
        "sin","cos","tan","exp","log","exp2","log2","radians","degrees",0 };
    for (int i=0;b[i];i++) if(!strcmp(s,b[i])) return 1;
    return 0;
}

static node *new_node(parser *p, int k){
    node *n = (node*)arena_alloc(&p->sh->ast_arena, (int)sizeof(node));
    if (n) n->k = k;
    return n;
}

static node *parse_expr(parser *p);

static int swizzle(const char *s, int out[4])
{
    int n=0;
    for (; s[n] && n<4; n++) {
        char c=s[n]; int idx;
        if(c=='x'||c=='r'||c=='s')idx=0; else if(c=='y'||c=='g'||c=='t')idx=1;
        else if(c=='z'||c=='b'||c=='p')idx=2; else if(c=='w'||c=='a'||c=='q')idx=3; else return -1;
        out[n]=idx;
    }
    if (s[n]) return -1;
    return n;
}

static node *parse_primary(parser *p)
{
    token *t = pk(p);
    if (t->kind==TK_NUM) { adv(p); node *n=new_node(p, N_NUM); n->num=t->num; return n; }
    if (eat_op(p,"(")) { node *n=parse_expr(p); if(!eat_op(p,")")) perr(p,"expected )"); return n; }
    if (t->kind==TK_ID) {
        char nm[40]; int i=0; while(t->text[i]&&i<39){nm[i]=t->text[i];i++;} nm[i]=0; adv(p);
        if (is_op(p,"(")) {
            adv(p);
            node *n=new_node(p, N_CALL); int k=0; for(int j=0;nm[j]&&j<39;j++)n->name[j]=nm[j];
            if (!is_op(p,")")) {
                do { if(k<8) n->args[k++]=parse_expr(p); } while (eat_op(p,","));
            }
            n->nargs=k; if(!eat_op(p,")")) perr(p,"expected ) in call");
            return n;
        }
        node *n=new_node(p, N_VAR); for(int j=0;nm[j]&&j<39;j++)n->name[j]=nm[j]; return n;
    }
    perr(p,"unexpected token in expression"); adv(p);
    return new_node(p, N_NUM);
}

static node *parse_postfix(parser *p)
{
    node *n=parse_primary(p);
    for (;;) {
        if (is_op(p,"[")) {
            adv(p); token *t=pk(p);
            int ix=0;
            if (t->kind==TK_NUM) { ix=(int)t->num; adv(p); }
            else { perr(p,"expected constant array index"); break; }
            if (!eat_op(p,"]")) { perr(p,"expected ]"); break; }
            node *q=new_node(p, N_INDEX); q->a=n; q->op=ix; n=q; continue;
        }
        if (is_op(p,".")) {
            adv(p); token *t=pk(p);
            if (t->kind!=TK_ID) { perr(p,"expected swizzle"); break; }
            int sw[4]; int sn=swizzle(t->text,sw);
            if (sn<0) { perr(p,"bad swizzle"); break; }
            adv(p);
            node *s=new_node(p, N_SWZ); s->a=n; s->swzn=sn; for(int i=0;i<sn;i++)s->swz[i]=sw[i]; n=s; continue;
        }
        break;
    }
    return n;
}

static node *parse_unary(parser *p)
{
    if (eat_op(p,"-")) { node *n=new_node(p, N_NEG); n->a=parse_unary(p); return n; }
    if (eat_op(p,"!")) { node *n=new_node(p, N_NOT); n->a=parse_unary(p); return n; }
    if (eat_op(p,"+")) return parse_unary(p);
    return parse_postfix(p);
}

static node *parse_mul(parser *p)
{
    node *n=parse_unary(p);
    while (is_op(p,"*")||is_op(p,"/")) { int o=pk(p)->text[0]; adv(p); node *r=parse_unary(p);
        node *b=new_node(p, N_BIN); b->op=o; b->a=n; b->b=r; n=b; }
    return n;
}
static node *parse_add(parser *p)
{
    node *n=parse_mul(p);
    while (is_op(p,"+")||is_op(p,"-")) { int o=pk(p)->text[0]; adv(p); node *r=parse_mul(p);
        node *b=new_node(p, N_BIN); b->op=o; b->a=n; b->b=r; n=b; }
    return n;
}
static node *parse_rel(parser *p)
{
    node *n=parse_add(p);
    for (;;) {
        int o=0;
        if(is_op(p,"<"))o='<'; else if(is_op(p,">"))o='>'; else if(is_op(p,"<="))o='L';
        else if(is_op(p,">="))o='G'; else if(is_op(p,"=="))o='E'; else if(is_op(p,"!="))o='N';
        if(!o) break; adv(p); node *r=parse_add(p);
        node *c=new_node(p, N_CMP); c->op=o; c->a=n; c->b=r; n=c;
    }
    return n;
}
static node *parse_logand(parser *p)
{
    node *n=parse_rel(p);
    while (is_op(p,"&&")) { adv(p); node *r=parse_rel(p);
        node *c=new_node(p, N_CMP); c->op='A'; c->a=n; c->b=r; n=c; }
    return n;
}
static node *parse_logor(parser *p)
{
    node *n=parse_logand(p);
    while (is_op(p,"||")) { adv(p); node *r=parse_logand(p);
        node *c=new_node(p, N_CMP); c->op='O'; c->a=n; c->b=r; n=c; }
    return n;
}
static node *parse_expr(parser *p)
{
    node *n=parse_logor(p);
    if (eat_op(p,"?")) {
        node *t=new_node(p, N_TERN); t->a=n; t->b=parse_expr(p);
        if(!eat_op(p,":")) perr(p,"expected : in ?:"); t->c=parse_expr(p); return t;
    }
    return n;
}

static void parse_body(parser *p)
{
    while (!is_op(p,"}") && pk(p)->kind!=TK_EOF && !p->failed) {
        token *t=pk(p); int ty;
        if (t->kind==TK_ID && !strcmp(t->text,"if")) { adv(p);
            if(!eat_op(p,"(")){perr(p,"expected ( after if");return;}
            node *cond=parse_expr(p);
            if(!eat_op(p,")")){perr(p,"expected ) after if condition");return;}
            token *t2=pk(p);
            if(t2->kind==TK_ID && !strcmp(t2->text,"discard")){ adv(p); eat_op(p,";");
                if(p->sh->nbody<256){ stmt *s=&p->sh->body[p->sh->nbody++]; s->k=S_DISCARD; s->expr=cond; } continue; }
            perr(p,"only 'if (...) discard;' is supported"); return;
        }
        if (t->kind==TK_ID && !strcmp(t->text,"discard")) { adv(p); eat_op(p,";");
            if(p->sh->nbody<256){ stmt *s=&p->sh->body[p->sh->nbody++]; s->k=S_DISCARD; s->expr=0; } continue; }
        if (t->kind==TK_ID && !strcmp(t->text,"return")) { adv(p); while(!is_op(p,";")&&pk(p)->kind!=TK_EOF)adv(p); eat_op(p,";");
            if(p->sh->nbody<256){ stmt *s=&p->sh->body[p->sh->nbody++]; s->k=S_NOP; } continue; }
        int decl_save = p->tl->pos;
        if (t->kind==TK_ID && !strcmp(t->text,"const")) { adv(p); t=pk(p); }
        if (t->kind==TK_ID && is_precision_qual_name(t->text)) { adv(p); t=pk(p); }
        if (t->kind==TK_ID && kw_type(t->text,&ty)) {
            adv(p);
            for (;;) {
                token *nm=pk(p); if(nm->kind!=TK_ID){perr(p,"expected name");return;}
                stmt s; memset(&s,0,sizeof(s)); s.k=S_DECL; s.type=ty;
                for(int i=0;nm->text[i]&&i<39;i++)s.name[i]=nm->text[i]; adv(p);
                if (eat_op(p,"=")) s.expr=parse_expr(p);
                if(p->sh->nbody<256) p->sh->body[p->sh->nbody++]=s;
                if (eat_op(p,",")) continue;
                break;
            }
            eat_op(p,";");
            continue;
        }
        p->tl->pos = decl_save;
        t = pk(p);
        if (t->kind==TK_ID) {
            int fi=-1;
            for (int k=0;k<p->sh->nfuncs;k++) if(!strcmp(p->sh->funcs[k].name,t->text)){fi=k;break;}
            if (fi>=0) {
                adv(p);
                if(!eat_op(p,"(")){perr(p,"expected ( in call");return;}
                if(!eat_op(p,")")){perr(p,"only parameterless call statements supported");return;}
                eat_op(p,";");
                int save=p->tl->pos;
                p->tl->pos=p->sh->funcs[fi].body_pos;
                parse_body(p);
                p->tl->pos=save;
                continue;
            }
        }
        if (t->kind==TK_ID) {
            stmt s; memset(&s,0,sizeof(s)); s.k=S_ASSIGN; s.idx=-1;
            for(int i=0;t->text[i]&&i<39;i++)s.name[i]=t->text[i]; adv(p);
            if (is_op(p,"[")) { adv(p); token *ix=pk(p); if(ix->kind!=TK_NUM){perr(p,"expected constant array index");return;} s.idx=(int)ix->num; adv(p); if(!eat_op(p,"]")){perr(p,"expected ]");return;} }
            if (is_op(p,".")) { adv(p); token *sw=pk(p); s.swzn=swizzle(sw->text,s.swz); if(s.swzn<0){perr(p,"bad lvalue swizzle");return;} adv(p); }
            int comp=0;
            if (eat_op(p,"=")) comp=0;
            else if (is_op(p,"+=")){adv(p);comp='+';} else if(is_op(p,"-=")){adv(p);comp='-';}
            else if (is_op(p,"*=")){adv(p);comp='*';} else if(is_op(p,"/=")){adv(p);comp='/';}
            else { perr(p,"expected = in assignment"); return; }
            node *rhs=parse_expr(p);
            if (comp) {
                node *lv=new_node(p, N_VAR); for(int i=0;s.name[i];i++)lv->name[i]=s.name[i];
                node *base=lv;
                if (s.idx>=0){ node *ix=new_node(p, N_INDEX); ix->a=base; ix->op=s.idx; base=ix; }
                if (s.swzn){ node *sz=new_node(p, N_SWZ); sz->a=base; sz->swzn=s.swzn; for(int i=0;i<s.swzn;i++)sz->swz[i]=s.swz[i]; base=sz; }
                node *b=new_node(p, N_BIN); b->op=comp; b->a=base; b->b=rhs; rhs=b;
            }
            s.expr=rhs; eat_op(p,";");
            if(p->sh->nbody<256) p->sh->body[p->sh->nbody++]=s; continue;
        }
        perr(p,"unexpected statement"); return;
    }
}

int parse_shader(shader *sh, const char *src, char *err, int errlen)
{
    toklist *tl = (toklist*)malloc(sizeof(toklist));
    if (!tl) return 0;
    lex(src, tl);
    parser p; p.tl=tl; p.sh=sh; p.err=err; p.errlen=errlen; p.failed=0;
    sh->ast_arena.head=0;
    sh->nglob=0; sh->nbody=0; sh->nfuncs=0; sh->has_main=0;
    int nattr=0;
    int main_body_pos=-1;

    while (pk(&p)->kind!=TK_EOF && !p.failed) {
        token *t=pk(&p);
        if (t->kind==TK_ID && !strcmp(t->text,"precision")) { while(!is_op(&p,";")&&pk(&p)->kind!=TK_EOF)adv(&p); eat_op(&p,";"); continue; }
        if (t->kind==TK_ID && !strcmp(t->text,"invariant")) { adv(&p); continue; }
        if (t->kind==TK_ID && !strcmp(t->text,"struct")) {
            adv(&p); while(!is_op(&p,"{")&&pk(&p)->kind!=TK_EOF)adv(&p);
            if (eat_op(&p,"{")) { int d=1; while(d&&pk(&p)->kind!=TK_EOF){ if(is_op(&p,"{"))d++; else if(is_op(&p,"}"))d--; adv(&p);} }
            while(!is_op(&p,";")&&pk(&p)->kind!=TK_EOF)adv(&p); eat_op(&p,";"); continue;
        }
        int qual=Q_NONE;
        if (t->kind==TK_ID) {
            if(!strcmp(t->text,"attribute")){qual=Q_ATTRIB;adv(&p);}
            else if(!strcmp(t->text,"uniform")){qual=Q_UNIFORM;adv(&p);}
            else if(!strcmp(t->text,"varying")){qual=Q_VARYING;adv(&p);}
            else if(!strcmp(t->text,"const")){qual=Q_CONST;adv(&p);}
        }
        if (pk(&p)->kind==TK_ID && is_precision_qual_name(pk(&p)->text)) adv(&p);
        int ty;
        if (pk(&p)->kind!=TK_ID || !kw_type(pk(&p)->text,&ty)) { perr(&p,"expected type"); break; }
        adv(&p);
        token *nm=pk(&p);
        if (nm->kind!=TK_ID) { perr(&p,"expected name"); break; }
        char name[40]; int i=0; for(;nm->text[i]&&i<39;i++)name[i]=nm->text[i]; name[i]=0; adv(&p);

        if (is_op(&p,"(")) {
            adv(&p);
            char params[4][40]; int nparam=0; char last[40]; last[0]=0;
            while(!is_op(&p,")")&&pk(&p)->kind!=TK_EOF){
                token *pt=pk(&p);
                if (pt->kind==TK_ID){ int j=0; for(;pt->text[j]&&j<39;j++)last[j]=pt->text[j]; last[j]=0; }
                else if (is_op(&p,",")){ if(nparam<4){int j=0;for(;last[j];j++)params[nparam][j]=last[j];params[nparam][j]=0;nparam++;} last[0]=0; }
                adv(&p);
            }
            eat_op(&p,")");
            if (last[0] && nparam<4){ int j=0;for(;last[j];j++)params[nparam][j]=last[j];params[nparam][j]=0;nparam++; }
            if (!eat_op(&p,"{")) { perr(&p,"expected {"); break; }
            if (!strcmp(name,"main")) {
                sh->has_main=1;
                main_body_pos=p.tl->pos;
                int depth=1;
                while(depth&&pk(&p)->kind!=TK_EOF){
                    if(is_op(&p,"{"))depth++;
                    else if(is_op(&p,"}"))depth--;
                    adv(&p);
                }
                continue;
            }
            else if (!is_builtin_fn(name)) {
                int bpos = p.tl->pos;
                node *ret=0; int depth=1;
                while(depth&&pk(&p)->kind!=TK_EOF){
                    if(is_op(&p,"{")){depth++;adv(&p);continue;}
                    if(is_op(&p,"}")){depth--;adv(&p);continue;}
                    if(depth==1 && is_kw(&p,"return")){ adv(&p); if(!is_op(&p,";")) ret=parse_expr(&p); eat_op(&p,";"); continue; }
                    adv(&p);
                }
                if (sh->nfuncs<8){ func_def *fd=&sh->funcs[sh->nfuncs++]; int j=0; for(;name[j];j++)fd->name[j]=name[j]; fd->name[j]=0;
                    fd->nparam=nparam; for(int a=0;a<nparam;a++){int c=0;for(;params[a][c];c++)fd->param[a][c]=params[a][c];fd->param[a][c]=0;} fd->ret=ret; fd->body_pos=bpos; }
                continue;
            }
            else { int depth=1; while(depth&&pk(&p)->kind!=TK_EOF){ if(is_op(&p,"{"))depth++; else if(is_op(&p,"}"))depth--; adv(&p);} continue; }
            if (!eat_op(&p,"}")) perr(&p,"expected }");
            continue;
        }
        int arr=0;
        if (is_op(&p,"[")) { adv(&p); token *ix=pk(&p); if(ix->kind==TK_NUM)arr=(int)ix->num; while(!is_op(&p,"]")&&pk(&p)->kind!=TK_EOF)adv(&p); eat_op(&p,"]"); }
        if (sh->nglob<64) {
            glvar *g=&sh->globals[sh->nglob++];
            for(int j=0;name[j];j++)g->name[j]=name[j]; g->name[i]=0;
            g->type=ty; g->qual=qual; g->location=-1; g->uoffset=-1; g->arr=arr;
            if (qual==Q_ATTRIB) g->location=nattr++;
        }
        while(!is_op(&p,";")&&pk(&p)->kind!=TK_EOF)adv(&p); eat_op(&p,";");
    }

    if (!p.failed && sh->has_main && main_body_pos>=0) {
        int save=p.tl->pos;
        p.tl->pos=main_body_pos;
        parse_body(&p);
        p.tl->pos=save;
    }

    free(tl);
    if (p.failed) return 0;
    if (!sh->has_main) { if(err&&errlen>0){const char*m="missing main()";int k=0;while(m[k]&&k<errlen-1){err[k]=m[k];k++;}err[k]=0;} return 0; }
    return 1;
}
