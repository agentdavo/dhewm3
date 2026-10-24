/* Same geometry and draw counts at every VLEN; setup pruning against baseline. */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sg_raster.h"
#ifndef SGL_SETUP_BENCH_FOCUSED
#define SGL_SETUP_BENCH_FOCUSED 0
#endif
extern int sgl_depth_only_setup_prune;
static float av[32],bv[32],cv[32];
static unsigned calls;
static int shader(void *ctx,const float *v,int n,float *out) {
    ++calls;
    for(int l=0;l<n;++l) {
        for(int c=0;c<4;++c)out[c*SGL_SIMT_LANES+l]=v[c*SGL_SIMT_LANES+l];
        if(ctx)out[4*SGL_SIMT_LANES+l]=v[l]>.4f?1.f:0.f;
    }
    return 1;
}
static void draw(sgl_framebuffer *fb,sgl_drawstate *st,int nv,int small,int reverse) {
    sgl_pvert a={-1,-1,.5f,1,av},b={1024,-1,.6f,.75f,bv},c={-1,32,.4f,.5f,cv};
    if(small){b.x=6;c.y=5;}
    sgl_raster_tri_prog(fb,&a,reverse?&c:&b,reverse?&b:&c,nv,st,0,shader,st->has_discard?(void*)1:0);
}
static void reset(sgl_framebuffer *fb) {
    for(int i=0;i<257*8;++i){fb->color[i]=0x98765432u+i*7919u;
        fb->depth[i]=(i%3)? .75f:.25f;fb->stencil[i]=(uint8_t)i;}
}
int kmain(void) {
    sgl_framebuffer *fb=sgl_fb_create(257,8),*ref=sgl_fb_create(257,8);
    if(!fb||!ref){puts("[setup] FAIL allocation");return 1;}
    for(int i=0;i<32;++i){av[i]=.2f+i*.01f;bv[i]=.8f-i*.01f;cv[i]=.6f;}
    int widths[]={4,8,12,32}, failures=0,cases=0;
    for(int n=0;n<4;++n)for(int small=0;small<2;++small)
    for(int reverse=0;reverse<2;++reverse)for(int mode=0;mode<4;++mode)
    for(int mask=0;mask<2;++mask) {
        sgl_drawstate st={0};st.color_mask=mask?15:0;
        st.depth_test=1;st.depth_write=1;st.depth_func=SGL_DEPTH_LESS;
        st.stencil_test=mode&1;st.stencil_frontface=1;
        st.stencil_front.func=SGL_DEPTH_ALWAYS;st.stencil_front.read_mask=255;
        st.stencil_front.write_mask=255;st.stencil_front.zpass=SGL_STENCIL_INCR_WRAP;
        st.stencil_front.zfail=SGL_STENCIL_KEEP;st.stencil_front.sfail=SGL_STENCIL_KEEP;
        st.has_discard=mode&2;st.skip_derivatives=1;st.skip_fragcoord=1;
        reset(fb);reset(ref);calls=0;
        sgl_depth_only_setup_prune=0;draw(ref,&st,widths[n],small,reverse);
        unsigned before=calls;calls=0;
        sgl_depth_only_setup_prune=1;draw(fb,&st,widths[n],small,reverse);
        if(calls!=before || (!mask&&!st.has_discard&&calls) ||
           memcmp(fb->color,ref->color,257*8*4)||memcmp(fb->depth,ref->depth,257*8*4)||
           memcmp(fb->stencil,ref->stencil,257*8))++failures;
        ++cases;
    }
    for(int n=0;n<4;++n)for(int small=0;small<2;++small)for(int color=0;color<2;++color) {
        if(SGL_SETUP_BENCH_FOCUSED && (widths[n]!=SGL_SETUP_BENCH_FOCUSED || !small || color))continue;
        sgl_drawstate st={0};st.color_mask=color?15:0;
        st.depth_test=1;st.depth_func=SGL_DEPTH_ALWAYS;st.depth_write=1;
        st.stencil_test=1;st.stencil_frontface=1;
        st.stencil_front.func=SGL_DEPTH_ALWAYS;st.stencil_front.read_mask=255;
        st.stencil_front.write_mask=255;st.stencil_front.zpass=SGL_STENCIL_INCR_WRAP;
        st.stencil_front.zfail=st.stencil_front.sfail=SGL_STENCIL_KEEP;
        st.skip_derivatives=st.skip_fragcoord=1;
        int draws=SGL_SETUP_BENCH_FOCUSED?200000:(small?20000:(color?200:1000));
        for(int arm=0;arm<2;++arm){sgl_depth_only_setup_prune=arm;for(int i=0;i<100;++i)draw(fb,&st,widths[n],small,0);}
        for(int round=0;round<(SGL_SETUP_BENCH_FOCUSED?8:4);++round) {
            uint64_t ticks[2];
            for(int turn=0;turn<2;++turn){int arm=turn^(round&1);sgl_depth_only_setup_prune=arm;
                uint64_t start=timer_ticks();for(int i=0;i<draws;++i)draw(fb,&st,widths[n],small,0);
                ticks[arm]=timer_ticks()-start;
            }
            printf("[setup-perf] nvary=%d small=%d color=%d draws=%d round=%d baseline_ticks=%lu optimized_ticks=%lu\n",
                widths[n],small,color,draws,round,(unsigned long)ticks[0],(unsigned long)ticks[1]);
        }
    }
    sgl_depth_only_setup_prune=1;
    printf("[setup] %s cases=%d failures=%d\n",failures?"FAIL":"PASS",cases,failures);
    return failures!=0;
}
