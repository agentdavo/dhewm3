/* Alpha-only raster stores against the unchanged four-channel path. */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"
#include "sgl_gl.h"
#include "sg_raster.h"

extern int sgl_alpha_only_store;
static int batch(void *ctx,const float *v,int vl,float *out)
{
    (void)v;
    static const uint32_t alpha[]={0,0x3dcccccdu,0x3f000000u,0x3f800000u,
        0x3fc00000u,0xbe800000u,0x7fc00000u,0x7f800000u,0xff800000u};
    for(int l=0;l<vl;++l) {
        out[l]=.2f;out[SGL_SIMT_LANES+l]=.7f;out[2*SGL_SIMT_LANES+l]=.4f;
        memcpy(out+3*SGL_SIMT_LANES+l,alpha+l%9,4);
        if(ctx)out[4*SGL_SIMT_LANES+l]=(l%3)==0?1.f:0.f;
    }
    return 1;
}
static void draw(sgl_framebuffer *fb,sgl_drawstate *st)
{
    sgl_pvert a={-1,-1,.5f,1,0},b={1024,-1,.5f,1,0},c={-1,8,.5f,1,0};
    sgl_raster_tri_prog(fb,&a,&b,&c,0,st,0,batch,st->has_discard?(void*)1:0);
}
int kmain(void)
{
    sgl_framebuffer *fb=sgl_fb_create(257,2),*ref=sgl_fb_create(257,2);
    if(!fb||!ref){puts("[alpha] FAIL allocation");return 1;}
    int failures=0,cases=0;
    unsigned factors[]={GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,
        GL_DST_COLOR,GL_ONE_MINUS_DST_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,
        GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA,GL_CONSTANT_COLOR,GL_ONE_MINUS_CONSTANT_COLOR,
        GL_CONSTANT_ALPHA,GL_ONE_MINUS_CONSTANT_ALPHA,GL_SRC_ALPHA_SATURATE};
    unsigned eqs[]={GL_FUNC_ADD,GL_FUNC_SUBTRACT,GL_FUNC_REVERSE_SUBTRACT};
    for(int src=0;src<15;++src)for(int dst=0;dst<15;++dst)
    for(int eq=0;eq<3;++eq)for(int mode=0;mode<4;++mode) {
        sgl_drawstate st={0};st.color_mask=8;
        st.blend=mode==0?SGL_BLEND_NONE:SGL_BLEND_GENERIC;
        st.blendf.src_rgb=mode==2?GL_ONE:factors[src];st.blendf.src_a=factors[src];
        st.blendf.dst_rgb=mode==2?GL_ONE:factors[dst];st.blendf.dst_a=factors[dst];
        st.blendf.eq_rgb=GL_FUNC_ADD;st.blendf.eq_a=eqs[eq];
        st.blendf.color[0]=.3f;st.blendf.color[1]=.6f;st.blendf.color[2]=.9f;st.blendf.color[3]=.4f;
        st.depth_test=mode==3;st.depth_write=1;st.depth_func=SGL_DEPTH_LESS;
        st.has_discard=mode==3;
        for(int i=0;i<514;++i){fb->color[i]=ref->color[i]=(uint32_t)i*0x798641abu;
            fb->depth[i]=ref->depth[i]=(i&1)? .25f:.75f;}
        sgl_alpha_only_store=0;draw(ref,&st);
        sgl_alpha_only_store=1;draw(fb,&st);
        if(memcmp(fb->color,ref->color,514*4)||memcmp(fb->depth,ref->depth,514*4))++failures;
        ++cases;
    }
    sgl_drawstate st={0};st.color_mask=8;st.blend=SGL_BLEND_GENERIC;
    st.blendf.src_rgb=st.blendf.src_a=GL_ONE;
    st.blendf.dst_rgb=st.blendf.dst_a=GL_ONE;
    st.blendf.eq_rgb=st.blendf.eq_a=GL_FUNC_ADD;
    for(int round=0;round<4;++round){
        uint64_t elapsed[2];
        for(int turn=0;turn<2;++turn){int enabled=turn^(round&1);sgl_alpha_only_store=enabled;
            uint64_t start=timer_ticks();for(int i=0;i<1000;++i)draw(fb,&st);
            elapsed[enabled]=timer_ticks()-start;}
        printf("[alpha-perf] round=%d baseline_ticks=%lu optimized_ticks=%lu\n",round,
            (unsigned long)elapsed[0],(unsigned long)elapsed[1]);
    }
    sgl_alpha_only_store=1;
    printf("[alpha] %s cases=%d failures=%d\n",failures?"FAIL":"PASS",cases,failures);
    return failures!=0;
}
