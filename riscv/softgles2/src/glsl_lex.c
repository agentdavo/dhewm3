/*
 * softgles2 — GLSL ES 1.00 lexer. Extracted from glsl.c.
 */
#include "glsl_internal.h"

static int is_id0(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_idn(char c){ return is_id0(c)||(c>='0'&&c<='9'); }
static int is_dig(char c){ return c>='0'&&c<='9'; }

int lex(const char *s, toklist *tl)
{
    tl->n = 0; tl->pos = 0;
    while (*s) {
        if (*s==' '||*s=='\t'||*s=='\r'||*s=='\n') { s++; continue; }
        if (*s=='#') { while (*s && *s!='\n') s++; continue; }
        if (s[0]=='/'&&s[1]=='/') { while (*s && *s!='\n') s++; continue; }
        if (s[0]=='/'&&s[1]=='*') { s+=2; while (*s && !(s[0]=='*'&&s[1]=='/')) s++; if (*s) s+=2; continue; }
        if (tl->n >= MAX_TOK-1) return 0;
        token *t = &tl->t[tl->n];
        if (is_id0(*s)) {
            int i=0; while (is_idn(*s) && i<39) t->text[i++]=*s++; t->text[i]=0; t->kind=TK_ID; tl->n++; continue;
        }
        if (is_dig(*s) || (*s=='.'&&is_dig(s[1]))) {
            int i=0; char buf[40];
            while ((is_dig(*s)||*s=='.'||*s=='e'||*s=='E'||((*s=='+'||*s=='-')&&(s[-1]=='e'||s[-1]=='E'))) && i<38) buf[i++]=*s++;
            if (*s=='f'||*s=='F') s++;
            buf[i]=0;
            float val=0,frac=0,div=1; int j=0,neg=0,seen=0; int exp=0,esign=1; int inexp=0;
            if(buf[0]=='-'){neg=1;j=1;}
            for(; buf[j]; j++){
                char c=buf[j];
                if(inexp){ if(c=='-')esign=-1; else if(c=='+'); else if(is_dig(c)) exp=exp*10+(c-'0'); }
                else if(c=='.'){ seen=1; }
                else if(c=='e'||c=='E'){ inexp=1; }
                else if(is_dig(c)){ if(!seen) val=val*10+(c-'0'); else { div*=10; frac+=(c-'0')/div; } }
            }
            val+=frac; if(neg)val=-val;
            while(exp-->0) { if(esign>0) val*=10; else val/=10; }
            t->kind=TK_NUM; t->num=val; tl->n++; continue;
        }
        {
            static const char *ops2[] = {"==","!=","<=",">=","&&","||","+=","-=","*=","/=",0};
            int matched=0;
            for (int k=0; ops2[k]; k++) if (s[0]==ops2[k][0] && s[1]==ops2[k][1]) {
                t->text[0]=s[0]; t->text[1]=s[1]; t->text[2]=0; t->kind=TK_OP; tl->n++; s+=2; matched=1; break;
            }
            if (matched) continue;
            t->text[0]=*s; t->text[1]=0; t->kind=TK_OP; tl->n++; s++; continue;
        }
    }
    tl->t[tl->n].kind = TK_EOF; tl->t[tl->n].text[0]=0;
    return 1;
}
