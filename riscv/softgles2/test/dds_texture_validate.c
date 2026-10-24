/* S3TC production API against a bit-reader reference, including all mip/faces. */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sg_state.h"
#include "sg_texture.h"
gl_ctx C;
static unsigned barriers;
void set_err(GLenum e) { C.error=e; }
void sg_framebin_barrier(void) { ++barriers; }
void sg_framebin_flush(void) {}
static unsigned seed=0x12345678;
static unsigned random_byte(void) { seed=seed*1664525u+1013904223u;return seed>>24; }
static unsigned bits(const unsigned char *p,unsigned start,unsigned count) {
    unsigned v=0;for(unsigned k=0;k<count;++k)v|=((p[(start+k)/8]>>((start+k)%8))&1u)<<k;return v;
}
static uint32_t reference(const unsigned char *p,int kind,unsigned pixel) {
    const unsigned char *c=p+(kind>=2?8:0);
    unsigned a=bits(c,0,16),b=bits(c,16,16),code=bits(c,32+pixel*2,2),out=0;
    unsigned alpha=255;
    for(unsigned channel=0;channel<3;++channel) {
        unsigned shift=channel==0?11:channel==1?5:0, count=channel==1?6:5;
        unsigned v0=(a>>shift)&((1u<<count)-1),v1=(b>>shift)&((1u<<count)-1);
        v0=(v0<<(8-count))|(v0>>(2*count-8));v1=(v1<<(8-count))|(v1>>(2*count-8));
        unsigned value=code==0?v0:code==1?v1:0;
        if(code>=2) {
            if(kind>=2 || a>b)value=code==2?(2*v0+v1)/3:(v0+2*v1)/3;
            else if(code==2)value=(v0+v1)/2;
        }
        out|=value<<(channel*8);
    }
    if(kind==1 && a<=b && code==3)alpha=0;
    if(kind==2)alpha=bits(p,pixel*4,4)*17;
    if(kind==3) {
        unsigned index=bits(p,16+pixel*3,3),a0=p[0],a1=p[1];
        if(index<2)alpha=index?a1:a0;
        else if(a0>a1)alpha=((8-index)*a0+(index-1)*a1)/7;
        else if(index<6)alpha=((6-index)*a0+(index-1)*a1)/5;
        else alpha=index==6?0:255;
    }
    return out|(alpha<<24);
}
static unsigned char payload[64*64];
static const GLenum formats[]={GL_COMPRESSED_RGB_S3TC_DXT1_EXT,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
static int verify(const sgl_tex *tx,int level,int kind,int w,int h,const unsigned char *data) {
    const uint32_t *pixels=level?tx->mips[level].texels:tx->texels;
    int stride=(w+3)/4,bytes=kind<2?8:16;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const unsigned char *block=data+((y/4)*stride+x/4)*bytes;
        if(pixels[y*w+x]!=reference(block,kind,(y%4)*4+x%4))return 0;
    }
    return 1;
}
#define CHECK(x) do{++cases;if(!(x)){printf("[dds] FAIL line=%d error=%x\n",__LINE__,(unsigned)C.error);return 1;}}while(0)
int kmain(void) {
    unsigned cases=0;
    for(unsigned i=0;i<sizeof(payload);++i)payload[i]=(unsigned char)random_byte();
    GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    for(int kind=0;kind<4;++kind) {
        for(int level=0,w=64;level<7;++level,w=w>1?w/2:1) {
            int size=((w+3)/4)*((w+3)/4)*(kind<2?8:16);C.error=0;
            glCompressedTexImage2D(GL_TEXTURE_2D,level,formats[kind],w,w,0,size,payload);
            CHECK(C.error==0);CHECK(verify(&g_textures[tex].tex,level,kind,w,w,payload));
        }
        CHECK(g_textures[tex].tex.nmip==7);
        // Redefinition resets upper levels and preserves sampler state.
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
        glCompressedTexImage2D(GL_TEXTURE_2D,0,formats[kind],7,5,0,4*(kind<2?8:16),payload);
        CHECK(g_textures[tex].tex.nmip==1 && g_textures[tex].tex.mip_filter==2);
        CHECK(verify(&g_textures[tex].tex,0,kind,7,5,payload));
        // A partial block is valid only at the edge; preserve the rest.
        uint32_t before[35];memcpy(before,g_textures[tex].tex.texels,sizeof(before));
        unsigned prev=barriers;
        glCompressedTexSubImage2D(GL_TEXTURE_2D,0,4,4,3,1,formats[kind],kind<2?8:16,payload+32);
        CHECK(C.error==0 && barriers>prev);
        for(int y=0;y<5;++y)for(int x=0;x<7;++x) {
            uint32_t want=y==4&&x>=4?reference(payload+32,kind,x-4):before[y*7+x];
            CHECK(g_textures[tex].tex.texels[y*7+x]==want);
        }
        uint32_t *old=g_textures[tex].tex.texels;
        C.error=0;glCompressedTexImage2D(GL_TEXTURE_2D,0,formats[kind],7,5,0,1,payload);
        CHECK(C.error==GL_INVALID_VALUE && old==g_textures[tex].tex.texels);
        C.error=0;glCompressedTexSubImage2D(GL_TEXTURE_2D,0,1,0,4,4,formats[kind],kind<2?8:16,payload);
        CHECK(C.error==GL_INVALID_OPERATION);
        C.error=0;glCompressedTexSubImage2D(GL_TEXTURE_2D,0,0,0,4,4,formats[(kind+1)%4],kind<2?8:16,payload);
        CHECK(C.error==GL_INVALID_OPERATION);
    }
    glDeleteTextures(1,&tex);glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_CUBE_MAP,tex);
    for(int kind=0;kind<4;++kind)for(int face=0;face<6;++face) {
        for(int level=0,w=8;level<4;++level,w=w>1?w/2:1) {
            GLenum target=GL_TEXTURE_CUBE_MAP_POSITIVE_X+face;
            int size=((w+3)/4)*((w+3)/4)*(kind<2?8:16);C.error=0;
            glCompressedTexImage2D(target,level,formats[kind],w,w,0,size,payload+face*16);
            CHECK(C.error==0);CHECK(verify(&g_textures[tex].cube[face],level,kind,w,w,payload+face*16));
            if(level) {
                glCompressedTexSubImage2D(target,level,0,0,w,w,formats[kind],size,payload);
                CHECK(C.error==0);CHECK(verify(&g_textures[tex].cube[face],level,kind,w,w,payload));
            }
        }
        CHECK(g_textures[tex].cube[face].nmip==4);
    }
    CHECK(g_textures[tex].cube_mask==63);
    glDeleteTextures(1,&tex);glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    for(int kind=0;kind<4;++kind) {
        int size=256*(kind<2?8:16);
        glCompressedTexImage2D(GL_TEXTURE_2D,0,formats[kind],64,64,0,size,payload);
        uint64_t start=timer_ticks();
        for(int repeat=0;repeat<128;++repeat)
            glCompressedTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,formats[kind],size,payload);
        printf("[dds-bench] kind=%d pixels=524288 ns=%llu\n",kind,(unsigned long long)timer_ns_from_ticks(timer_ticks()-start));
        CHECK(verify(&g_textures[tex].tex,0,kind,64,64,payload));
        glGenerateMipmap(GL_TEXTURE_2D);
        CHECK(C.error==0);
        glCompressedTexSubImage2D(GL_TEXTURE_2D,1,0,0,32,32,formats[kind],
                                 64*(kind<2?8:16),payload);
        CHECK(C.error==0);
        CHECK(verify(&g_textures[tex].tex,1,kind,32,32,payload));
    }
    glDeleteTextures(1,&tex);
    printf("[dds] PASS %u cases\n",cases);return 0;
}
