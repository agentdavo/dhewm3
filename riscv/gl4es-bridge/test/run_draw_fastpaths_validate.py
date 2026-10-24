#!/usr/bin/env python3
"""Exercise production cache and index-binding code extracted verbatim."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[3]
vendor=root/'riscv/gl4es'
cc=os.environ.get('CC','C:/msys64/ucrt64/bin/gcc.exe' if os.name=='nt' else 'cc')
os.environ['PATH']=str(Path(cc).parent)+os.pathsep+os.environ.get('PATH','')
cache=(vendor/'src/gl/fpe_cache.c').read_text().split('typedef struct psa_s')[0]
cache+='''
#include <assert.h>
void APIENTRY_GL4ES gl4es_glDeleteProgram(GLuint p) { (void)p; }
int main(void) {
 fpe_cache_t *a=fpe_NewCache(), *b=fpe_NewCache();
 fpe_state_t s={0}, t={0}; ((unsigned char*)&t)[sizeof(t)-1]=1;
 fpe_fpe_t *x=fpe_GetCache(a,&s,1), *y=fpe_GetCache(a,&t,1);
 assert(x!=y); assert(fpe_GetCache(a,&t,1)==y);
 assert(fpe_GetCache(a,&s,1)==x); assert(fpe_GetCache(b,&s,1)!=x);
 gl4es_cache_mru=0; assert(fpe_GetCache(a,&s,1)==x);
 gl4es_cache_mru=1; fpe_disposeCache(a,0); fpe_disposeCache(b,0);
 puts("PASS: per-cache MRU, changed state, hash fallback and disposal");
}
'''
src=(vendor/'src/gl/fpe.c').read_text()
draw=src[src.index('void APIENTRY_GL4ES fpe_glDrawElements('):src.index('void APIENTRY_GL4ES fpe_glDrawArraysInstanced(')]
unit='''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef unsigned GLenum, GLuint; typedef int GLsizei; typedef void GLvoid;
#define APIENTRY_GL4ES
#define DBG(a)
#define LOAD_GLES(a)
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_POINTS 0
typedef struct { int size; } scratch_t;
struct buffer { void *data; size_t size; GLuint real_buffer; };
struct vao { struct buffer *elements; } va;
struct state { struct vao *vao; } st={&va}, *glstate=&st;
static GLuint bound=77, want=77; static const void *seen; static GLenum seen_type;
static void realize_glenv(int a,int b,int c,GLenum d,const void*e,scratch_t*f) {}
static void bindBuffer(GLenum target,GLuint n) { assert(target==GL_ELEMENT_ARRAY_BUFFER); bound=want=n; }
static void realize_bufferIndex(void) { bound=want; }
static void wantBufferIndex(GLuint n) { want=n; }
static void free_scratch(scratch_t*s) {}
static void gles_glDrawElements(GLenum m,int count,GLenum type,const void*p) { seen=p;seen_type=type; }
'''+draw+'''
int main(void) {
 GLuint indices[8]={70000,2,1}; struct buffer b={indices,sizeof(indices),7};
 fpe_glDrawElements(4,3,0x1405,indices);
 assert(bound==0 && seen==indices && seen_type==0x1405 && indices[0]==70000);
 va.elements=&b; fpe_glDrawElements(4,3,0x1405,indices+1);
 assert(bound==7 && seen==(void*)4 && want==0 && va.elements==&b);
 b.real_buffer=0; fpe_glDrawElements(4,3,0x1405,indices);
 assert(bound==0 && seen==indices);
 puts("PASS: stale EBO cleared for client indices; real EBO uses byte offset; uint preserved");
}
'''
drawsrc=(vendor/'src/gl/drawing.c').read_text()
poolblock=drawsrc[drawsrc.index('        void *reuse = NULL;'):drawsrc.index('        sindices = copy_gl_array(',drawsrc.index('        void *reuse = NULL;'))]
pool='''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "glstate.h"
glstate_t *glstate;
int gl4es_index_reuse=1, fail_resize=0;
static void *test_resize(void *p,size_t n) { return fail_resize?NULL:realloc(p,n); }
#define realloc test_resize
static void *borrow(int count,int compiling,int intercept,int *flag) {
 int borrowed_indices=0;
'''+poolblock+'''
 *flag=borrowed_indices; return reuse;
}
int main(void) {
 glstate=calloc(1,sizeof(*glstate)); int used;
 void *a=borrow(128,0,0,&used); assert(a && used && glstate->draw_index_busy);
 assert(!borrow(32,0,0,&used) && !used);
 glstate->draw_index_busy=0;
 assert(borrow(32,0,0,&used)==a && used); glstate->draw_index_busy=0;
 assert(!borrow(32,1,0,&used) && !used);
 assert(!borrow(32,0,1,&used) && !used);
 fail_resize=1;
 assert(!borrow(4096,0,0,&used) && !used && glstate->draw_index_scratch==a);
 assert(glstate->draw_index_capacity==256 && !glstate->draw_index_busy);
 free(a);free(glstate);
 puts("PASS: index storage reused; lists, nested draws and failed growth retain ownership");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 for name,source in [('cache',cache),('indices',unit),('index-pool',pool)]:
  c=Path(tmp)/(name+'.c'); exe=Path(tmp)/(name+'.exe'); c.write_text(source)
  subprocess.run([cc,'-O2','-DNOX11','-DNOEGL','-DSTATICLIB','-I'+str(vendor/'include'),'-I'+str(vendor/'src/gl'),str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
