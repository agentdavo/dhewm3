/* Production texture-copy API against an independent pixel/guard oracle. */
#include "libc/libc.h"
#include "sg_state.h"
#include "sg_texture.h"
#include "boot/runtime.h"
gl_ctx C;
void set_err(GLenum e) { C.error=e; }
void sg_framebin_barrier(void) {}
void sg_framebin_flush(void) {}
static uint32_t src[263*7], dst[269*11], expected[269*11];
static float depth[263*7];
int kmain(void) {
    sgl_framebuffer fb={0}; fb.w=263; fb.h=7; fb.color=src; fb.depth=depth;
    C.fb=&fb; C.unit_tex[0]=1; C.unpack_alignment=4;
    g_textures[1].used=1;
    sgl_tex *tx=&g_textures[1].tex;
    for(int i=0;i<263*7;++i) { src[i]=0x01020304u*(unsigned)(i+3); depth[i]=(float)i/2048.f; }
    int widths[]={1,3,31,32,33,127,128,129,255,256,257};
    unsigned cases=0;
    for(int kind=0;kind<3;++kind) for(unsigned n=0;n<sizeof(widths)/sizeof(widths[0]);++n) {
        int w=widths[n];
        tx->w=269;tx->h=11;tx->texels=dst;tx->opaque_alpha=kind==1;tx->is_depth=kind==2;
        for(int i=0;i<269*11;++i)dst[i]=expected[i]=0xa55a33cc;
        for(int row=0;row<5;++row)for(int x=0;x<w;++x) {
            int si=(7-1-(1+row))*263+2+x;
            uint32_t p=src[si]; if(kind==1)p|=0xff000000u;
            if(kind==2)memcpy(&p,&depth[si],4);
            expected[(2+row)*269+3+x]=p;
        }
        C.error=0;glCopyTexSubImage2D(GL_TEXTURE_2D,0,3,2,2,1,w,5);
        if(C.error || memcmp(dst,expected,sizeof(dst))) { printf("[copy] FAIL kind=%d width=%d\n",kind,w);return 1; }
        ++cases;
    }
    /* Upload oracle: odd widths exercise alignment padding; offset source
     * tests unaligned client pointers and guards detect writes outside rows. */
    static unsigned char upload[264*7*4+8] __attribute__((aligned(8)));
    for (unsigned i=0;i<sizeof(upload);++i) upload[i]=(unsigned char)(i*73+19);
    for (int offset=0;offset<=1;++offset)
    for (int opaque=0;opaque<2;++opaque) for (int align=1;align<=8;align*=2)
    for (unsigned n=0;n<sizeof(widths)/sizeof(widths[0]);++n) {
        int w=widths[n]; size_t stride=((size_t)w*4+align-1)&~(size_t)(align-1);
        tx->w=269;tx->h=11;tx->texels=dst;tx->opaque_alpha=opaque;tx->is_depth=0;
        C.unpack_alignment=align;
        for(int i=0;i<269*11;++i)dst[i]=expected[i]=0xa55a33cc;
        for(int row=0;row<5;++row)for(int x=0;x<w;++x) {
            const unsigned char *p=upload+offset+row*stride+x*4;
            expected[(2+row)*269+3+x]=(uint32_t)p[0]|((uint32_t)p[1]<<8)|
                ((uint32_t)p[2]<<16)|((uint32_t)(opaque?255:p[3])<<24);
        }
        C.error=0;glTexSubImage2D(GL_TEXTURE_2D,0,3,2,w,5,GL_RGBA,GL_UNSIGNED_BYTE,upload+offset);
        if(C.error || memcmp(dst,expected,sizeof(dst))) { printf("[copy] FAIL upload alpha=%d align=%d width=%d\n",opaque,align,w);return 1; }
        ++cases;
    }
    tx->texels=0;tx->is_depth=0;
    uint32_t pixel=0x12345678;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,&pixel);
    if(!tx->opaque_alpha || tx->texels[0]!=0xff345678u) { puts("[copy] FAIL RGB alpha");return 1; }
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,&pixel);
    if(tx->opaque_alpha || tx->texels[0]!=pixel) { puts("[copy] FAIL redefinition");return 1; }
    /* All sixteen DXT pixels must be decoded even when e32m1 VLMAX < 16.
     * Literal palette/index oracle also covers DXT1 transparent mode. */
    const GLenum formats[]={GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
        GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
        GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
    for (int kind=0;kind<4;++kind) {
        unsigned char block[16]; memset(block,255,sizeof(block));
        int off=kind>=2?8:0;
        if(kind==3) { block[0]=255;block[1]=0;memset(block+2,0,6); }
        block[off]=kind==1?0:255;block[off+1]=block[off];
        block[off+2]=kind==1?255:0;block[off+3]=block[off+2];
        for(int i=4;i<8;++i)block[off+i]=0xe4; /* 0,1,2,3 repeated */
        glCompressedTexImage2D(GL_TEXTURE_2D,0,formats[kind],4,4,0,kind>=2?16:8,block);
        const uint32_t opaque[4]={0xffffffffu,0xff000000u,0xffaaaaaau,0xff555555u};
        const uint32_t alpha[4]={0xff000000u,0xffffffffu,0xff7f7f7fu,0};
        for(int i=0;i<16;++i) if(tx->texels[i]!=(kind==1?alpha:opaque)[i&3]) {
            printf("[copy] FAIL DXT kind=%d pixel=%d\n",kind,i);return 1;
        }
        ++cases;
    }
    tx->w=269;tx->h=11;tx->texels=dst;tx->opaque_alpha=1;tx->is_depth=0;
    C.unpack_alignment=8;
    for(int offset=0;offset<=1;++offset) {
    uint64_t begin=timer_ticks();
    for(int repeat=0;repeat<4096;++repeat)
        glTexSubImage2D(GL_TEXTURE_2D,0,3,2,257,5,GL_RGBA,GL_UNSIGNED_BYTE,upload+offset);
    printf("[upload-bench] opaque RGBA offset=%d pixels=5263360 ns=%llu\n",
           offset, (unsigned long long)timer_ns_from_ticks(timer_ticks()-begin));
    }
    printf("[copy] PASS %u tail/stride/guard/DXT cases plus RGB alpha and redefinition\n",cases);
    return 0;
}
