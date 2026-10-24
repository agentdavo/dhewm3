/* OpenGL reference-first stencil comparisons, through both raster entry points. */
#include "libc/libc.h"
#include "sgl.h"
#include "sg_raster.h"
#include "boot/runtime.h"

extern int sgl_stencil_noop_prune;
extern int sgl_stencil_always_fast;

static int fragment(void *ctx, const float *v, float rgba[4])
{ (void)ctx; (void)v; rgba[0]=1; rgba[1]=rgba[2]=0; rgba[3]=1; return 1; }
static int batch(void *ctx, const float *v, int vl, float *rgba)
{
    (void)ctx; (void)v;
    for (int c=0;c<4;++c) for(int l=0;l<vl;++l)
        rgba[c*SGL_SIMT_LANES+l] = (c==0 || c==3) ? 1.f : 0.f;
    return 1;
}
static int reference(int func, int ref, int stored)
{
    switch (func) {
    case SGL_DEPTH_NEVER:return 0;
    case SGL_DEPTH_LESS:return ref < stored;
    case SGL_DEPTH_EQUAL:return ref == stored;
    case SGL_DEPTH_LEQUAL:return ref <= stored;
    case SGL_DEPTH_GREATER:return ref > stored;
    case SGL_DEPTH_NOTEQUAL:return ref != stored;
    case SGL_DEPTH_GEQUAL:return ref >= stored;
    default:return 1;
    }
}
static void draw(sgl_framebuffer *fb, sgl_drawstate *st, int prog)
{
    if (prog) {
        sgl_pvert a={-1,-1,.5f,1,0},b={1024,-1,.5f,1,0},c={-1,8,.5f,1,0};
        sgl_raster_tri_prog(fb,&a,&b,&c,0,st,fragment,batch,0);
    } else {
        sgl_scrvert a={0},b={0},c={0};
        a.x=c.x=-1; a.y=b.y=-1; b.x=1024; c.y=8;
        a.z=b.z=c.z=.5f; a.invw=b.invw=c.invw=1;
        a.r=b.r=c.r=1; a.a=b.a=c.a=1;
        sgl_raster_tri(fb,a,b,c,st);
    }
}
int kmain(void)
{
    sgl_framebuffer *fb=sgl_fb_create(256,2);
    if(!fb) { puts("[stencil] FAIL allocation"); return 1; }
    int fail=0, cases=0;
    int funcs[]={SGL_DEPTH_NEVER,SGL_DEPTH_LESS,SGL_DEPTH_EQUAL,SGL_DEPTH_LEQUAL,
                 SGL_DEPTH_GREATER,SGL_DEPTH_NOTEQUAL,SGL_DEPTH_GEQUAL,SGL_DEPTH_ALWAYS};
    int refs[]={0,1,127,128,255}, masks[]={0,0x55,0xf0,255};
    for(int prog=0;prog<2;++prog) for(int f=0;f<8;++f)
    for(int r=0;r<5;++r) for(int m=0;m<4;++m) {
        sgl_drawstate st={0}; st.color_mask=15; st.stencil_test=1; st.stencil_frontface=1;
        st.stencil_front.func=funcs[f]; st.stencil_front.ref=refs[r];
        st.stencil_front.read_mask=masks[m]; st.stencil_front.write_mask=255;
        for(int i=0;i<512;++i) { fb->color[i]=0xff000000u; fb->stencil[i]=(uint8_t)i; }
        draw(fb,&st,prog);
        for(int i=0;i<256;++i) {
            uint32_t want=reference(funcs[f],refs[r]&masks[m],i&masks[m]) ? 0xff0000ffu : 0xff000000u;
            if(fb->color[i]!=want) ++fail;
        }
        ++cases;
    }
    int ops[]={SGL_STENCIL_INCR,SGL_STENCIL_DECR,SGL_STENCIL_INCR_WRAP,SGL_STENCIL_DECR_WRAP};
    for(int prog=0;prog<2;++prog) for(int op=0;op<4;++op) {
        sgl_drawstate st={0}; st.stencil_test=1; st.stencil_frontface=1;
        st.stencil_front.func=SGL_DEPTH_ALWAYS; st.stencil_front.read_mask=255;
        st.stencil_front.write_mask=255; st.stencil_front.zpass=ops[op];
        for(int i=0;i<512;++i) fb->stencil[i]=(uint8_t)i;
        draw(fb,&st,prog);
        for(int i=0;i<256;++i) {
            int want= op==0 ? (i==255 ? 255 : i+1) : op==1 ? (i==0 ? 0 : i-1) :
                      op==2 ? ((i+1)&255) : ((i-1)&255);
            if(fb->stencil[i]!=want) ++fail;
        }
        ++cases;
    }
    /* Preserve color, depth and stencil for KEEP, zero and partial write masks,
     * both faces, failing stencil tests and mixed early-Z coverage. */
    sgl_framebuffer *oracle=sgl_fb_create(256,2);
    int allops[]={SGL_STENCIL_KEEP,0 /* GL_ZERO */,SGL_STENCIL_REPLACE,
        SGL_STENCIL_INCR,SGL_STENCIL_DECR,SGL_STENCIL_INVERT,
        SGL_STENCIL_INCR_WRAP,SGL_STENCIL_DECR_WRAP};
    for(int face=0;face<2;++face) for(int op=0;op<8;++op)
    for(int mask=0;mask<4;++mask) for(int which=0;which<3;++which)
    for(int mode=0;mode<4;++mode) {
        sgl_drawstate st={0}; st.color_mask=15; st.stencil_test=1;
        st.stencil_frontface=face; st.depth_test=1; st.depth_write=1;
        st.depth_func=SGL_DEPTH_LESS;
        sgl_stencil_face *s=face?&st.stencil_front:&st.stencil_back;
        s->func=mode&1?SGL_DEPTH_GREATER:SGL_DEPTH_ALWAYS;
        s->read_mask=255; s->ref=127; s->write_mask=masks[mask];
        s->sfail=s->zfail=s->zpass=SGL_STENCIL_KEEP;
        if(which==0)s->sfail=allops[op];
        if(which==1)s->zfail=allops[op];
        if(which==2)s->zpass=allops[op];
        for(int i=0;i<512;++i) {
            fb->color[i]=oracle->color[i]=0x12345678u;
            fb->stencil[i]=oracle->stencil[i]=(uint8_t)i;
            fb->depth[i]=oracle->depth[i]=(mode&2) && (i&1) ? .25f : .75f;
        }
        sgl_stencil_always_fast=0; sgl_stencil_noop_prune=0; draw(oracle,&st,1);
        sgl_stencil_always_fast=1; sgl_stencil_noop_prune=1; draw(fb,&st,1);
        if(memcmp(fb->color,oracle->color,512*4) ||
           memcmp(fb->depth,oracle->depth,512*4) ||
           memcmp(fb->stencil,oracle->stencil,512)) ++fail;
        ++cases;
    }
    sgl_drawstate bench={0}; bench.color_mask=15; bench.stencil_test=1;
    bench.stencil_frontface=1; bench.stencil_front.func=SGL_DEPTH_GEQUAL;
    bench.stencil_front.ref=255; bench.stencil_front.read_mask=255;
    bench.stencil_front.write_mask=255;
    bench.stencil_front.sfail=bench.stencil_front.zfail=bench.stencil_front.zpass=SGL_STENCIL_KEEP;
    for(int round=0;round<4;++round) {
        uint64_t elapsed[2];
        for(int turn=0;turn<2;++turn) {
            int enabled=turn^(round&1); sgl_stencil_noop_prune=enabled;
            uint64_t start=timer_ticks();
            for(int i=0;i<1000;++i)draw(fb,&bench,1);
            elapsed[enabled]=timer_ticks()-start;
        }
        printf("[stencil-perf] round=%d baseline_ticks=%lu optimized_ticks=%lu\n",
            round,(unsigned long)elapsed[0],(unsigned long)elapsed[1]);
    }
    sgl_stencil_noop_prune=1;
    bench.color_mask=0; bench.stencil_front.func=SGL_DEPTH_ALWAYS;
    bench.stencil_front.zpass=SGL_STENCIL_INCR_WRAP;
    for(int round=0;round<4;++round) {
        uint64_t elapsed[2];
        for(int turn=0;turn<2;++turn) {
            int enabled=turn^(round&1); sgl_stencil_always_fast=enabled;
            uint64_t start=timer_ticks();
            for(int i=0;i<4000;++i)draw(fb,&bench,1);
            elapsed[enabled]=timer_ticks()-start;
        }
        printf("[stencil-always-perf] round=%d baseline_ticks=%lu optimized_ticks=%lu\n",
            round,(unsigned long)elapsed[0],(unsigned long)elapsed[1]);
    }
    sgl_stencil_always_fast=1;
    printf("[stencil] %s cases=%d failures=%d\n",fail?"FAIL":"PASS",cases,fail);
    return fail!=0;
}
