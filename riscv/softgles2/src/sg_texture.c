#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#endif

#include "sg_state.h"
#include "sg_texture.h"
#include "sg_framebin.h"
#if defined(SGL_BAREMETAL)
#include "engine/texture_profile.h"
#endif

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SG_TEX_RVV 1
#else
#  define SG_TEX_RVV 0
#endif
#ifndef SGL_UPLOAD_RVV
#define SGL_UPLOAD_RVV SG_TEX_RVV
#endif
#ifndef SGL_DXT_RVV
#define SGL_DXT_RVV SG_TEX_RVV
#endif
#ifndef SGL_DXT_VECTOR_DEFAULT
#define SGL_DXT_VECTOR_DEFAULT 0
#endif
/* Keep the batch implementation available for hardware A/B runs. The measured
 * QEMU default uses scalar block decoding; other rendering RVV paths are intact. */
int sgl_dxt_vector_decode = SGL_DXT_VECTOR_DEFAULT;
#if defined(__GNUC__) && !defined(__clang__) && defined(__riscv_vector)
/* Four/eight-entry tables and sixteen pixels are too small to amortize the
 * automatic gather/SLP sequences emitted by GCC 16 in the tested QEMU paths. */
#define DXT_SMALL_SCALAR __attribute__((optimize("no-tree-vectorize")))
#else
#define DXT_SMALL_SCALAR
#endif

tex_obj g_textures[MAX_TEXTURES];

/* ---- mip pyramid -------------------------------------------------------- */

/* Drop levels 1..n. Level 0 is owned by tx->texels and freed by the caller;
 * mip[0] only aliases it. */
static void mip_free_upper(sgl_tex *tx)
{
    if (tx->mips) {
        for (int l = 1; l < SGL_MAX_MIP; ++l) {
            free(tx->mips[l].texels);
            tx->mips[l].texels = 0;
            tx->mips[l].w = tx->mips[l].h = 0;
        }
    }
    tx->nmip = tx->texels ? 1 : 0;
}

/* Allocate the level table lazily: textures that never receive a level > 0 and
 * never see glGenerateMipmap pay nothing at all. */
static int mip_table(sgl_tex *tx)
{
    if (!tx->mips) {
        tx->mips = (struct sgl_mip_level *)malloc(sizeof(*tx->mips) * SGL_MAX_MIP);
        if (!tx->mips) return 0;
        memset(tx->mips, 0, sizeof(*tx->mips) * SGL_MAX_MIP);
    }
    return 1;
}

/* Republish level 0 into mip[0] after any change to w/h/texels. */
static void mip_sync0(sgl_tex *tx)
{
    if (tx->mips) {
        tx->mips[0].w = tx->w;
        tx->mips[0].h = tx->h;
        tx->mips[0].texels = tx->texels;
    }
    if (tx->texels && tx->nmip < 1) tx->nmip = 1;
}

/* Allocate storage for an application-supplied level. Returns a scratch sgl_tex
 * view so upload_tex_rect() -- which only looks at w/h/texels -- can fill it
 * without knowing about levels at all. */
/* Counters so a run can prove the pyramid is actually being populated rather
 * than silently doing nothing (the previous behaviour was exactly "silently
 * doing nothing", and it looked identical from the outside). */
unsigned long sgl_dbg_mip_levels_stored = 0;
unsigned long sgl_dbg_mip_generated = 0;

static int mip_alloc_level(sgl_tex *tx, int level, int w, int h, sgl_tex *view)
{
    if (level <= 0 || level >= SGL_MAX_MIP || w <= 0 || h <= 0) return 0;
    if (!mip_table(tx)) return 0;
    ++sgl_dbg_mip_levels_stored;
    mip_sync0(tx);
    free(tx->mips[level].texels);
    tx->mips[level].texels = (uint32_t *)malloc((size_t)w * h * 4);
    if (!tx->mips[level].texels) return 0;
    memset(tx->mips[level].texels, 0, (size_t)w * h * 4);
    tx->mips[level].w = w;
    tx->mips[level].h = h;
    if (level + 1 > tx->nmip) tx->nmip = level + 1;
    *view = *tx;
    view->w = w; view->h = h; view->texels = tx->mips[level].texels;
    return 1;
}

/* Box-filter the pyramid down from level 0, for glGenerateMipmap. Averages in
 * integer space per channel; odd dimensions clamp (the extra row/column is
 * sampled twice), which is what GL's "round down, filter over available texels"
 * amounts to for our purposes. */
static void mip_generate(sgl_tex *tx)
{
    if (!tx->texels || tx->w <= 0 || tx->h <= 0) return;
    if (!mip_table(tx)) return;
    ++sgl_dbg_mip_generated;
    mip_free_upper(tx);
    mip_sync0(tx);
    int sw = tx->w, sh = tx->h;
    const uint32_t *src = tx->texels;
    for (int level = 1; level < SGL_MAX_MIP && (sw > 1 || sh > 1); ++level) {
        int dw = sw > 1 ? sw >> 1 : 1;
        int dh = sh > 1 ? sh >> 1 : 1;
        uint32_t *dst = (uint32_t *)malloc((size_t)dw * dh * 4);
        if (!dst) return;
        for (int y = 0; y < dh; ++y) {
            int y0 = (sh > 1) ? (y * 2) : 0;
            int y1 = (sh > 1 && y0 + 1 < sh) ? y0 + 1 : y0;
            for (int x = 0; x < dw; ++x) {
                int x0 = (sw > 1) ? (x * 2) : 0;
                int x1 = (sw > 1 && x0 + 1 < sw) ? x0 + 1 : x0;
                uint32_t a = src[(size_t)y0 * sw + x0], b = src[(size_t)y0 * sw + x1];
                uint32_t c = src[(size_t)y1 * sw + x0], d = src[(size_t)y1 * sw + x1];
                uint32_t out = 0;
                for (int ch = 0; ch < 4; ++ch) {
                    unsigned s = ((a >> (ch * 8)) & 0xffu) + ((b >> (ch * 8)) & 0xffu) +
                                 ((c >> (ch * 8)) & 0xffu) + ((d >> (ch * 8)) & 0xffu);
                    out |= ((s + 2u) >> 2) << (ch * 8);
                }
                dst[(size_t)y * dw + x] = out;
            }
        }
        tx->mips[level].texels = dst;
        tx->mips[level].w = dw;
        tx->mips[level].h = dh;
        tx->nmip = level + 1;
        src = dst; sw = dw; sh = dh;
    }
}

static void free_tex_storage(tex_obj *o)
{
    memset(o->compressed_format, 0, sizeof(o->compressed_format));
    mip_free_upper(&o->tex);
    free(o->tex.mips); o->tex.mips = 0;
    free(o->tex.texels);
    o->tex.texels = 0;
    o->tex.nmip = 0;
    for (int i = 0; i < 6; ++i) {
        mip_free_upper(&o->cube[i]);
        free(o->cube[i].mips); o->cube[i].mips = 0;
        free(o->cube[i].texels);
        o->cube[i].texels = 0;
        o->cube[i].nmip = 0;
    }
    o->cube_mask = 0;
    o->tex.is_cube = 0;
    for (int i = 0; i < 6; ++i) o->tex.cube_faces[i] = 0;
}

int cube_face_index(GLenum target)
{
    if (target < GL_TEXTURE_CUBE_MAP_POSITIVE_X || target > GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
        return -1;
    return (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
}

int tex_target_ok(GLenum target)
{
    return target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP;
}

sgl_tex *tex_for_target(GLuint id, GLenum target)
{
    if (!id || id >= MAX_TEXTURES || !g_textures[id].used) return 0;
    int face = cube_face_index(target);
    if (target == GL_TEXTURE_2D) return &g_textures[id].tex;
    if (face >= 0) return &g_textures[id].cube[face];
    return 0;
}

int tex_has_storage(const tex_obj *o)
{
    return (o->tex.texels != 0) || (o->tex.is_cube && o->cube_mask != 0);
}

size_t unpack_stride(GLsizei w, int comps)
{
    size_t row = (size_t)w * (size_t)comps;
    size_t align = (size_t)(C.unpack_alignment ? C.unpack_alignment : 4);
    return (row + align - 1) & ~(align - 1);
}

size_t pack_stride(GLsizei w, int bytes_per_pixel)
{
    size_t row = (size_t)w * (size_t)bytes_per_pixel;
    size_t align = (size_t)(C.pack_alignment ? C.pack_alignment : 4);
    return (row + align - 1) & ~(align - 1);
}

static int compressed_format_block_bytes(GLenum format)
{
    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        return 8;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return 16;
    default:
        return 0;
    }
}

static uint8_t expand5(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t expand6(unsigned v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint32_t pack_rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static uint16_t rd16le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32le(const uint8_t *p) { return (uint32_t)rd16le(p) | ((uint32_t)rd16le(p + 2) << 16); }

/* mode: 0 = DXT1 RGB, 1 = DXT1 RGBA, 2 = DXT3/5 four-color block. */
static DXT_SMALL_SCALAR uint32_t dxt_color_palette(const uint8_t *block, int mode, uint32_t pal[4])
{
    uint16_t c0 = rd16le(block + 0);
    uint16_t c1 = rd16le(block + 2);
    uint8_t r0 = expand5(c0 >> 11), g0 = expand6((c0 >> 5) & 0x3f), b0 = expand5(c0 & 0x1f);
    uint8_t r1 = expand5(c1 >> 11), g1 = expand6((c1 >> 5) & 0x3f), b1 = expand5(c1 & 0x1f);
    pal[0] = pack_rgba8(r0, g0, b0, 255);
    pal[1] = pack_rgba8(r1, g1, b1, 255);
    if (mode == 2 || c0 > c1) {
        pal[2] = pack_rgba8((uint8_t)((2 * r0 + r1) / 3), (uint8_t)((2 * g0 + g1) / 3), (uint8_t)((2 * b0 + b1) / 3), 255);
        pal[3] = pack_rgba8((uint8_t)((r0 + 2 * r1) / 3), (uint8_t)((g0 + 2 * g1) / 3), (uint8_t)((b0 + 2 * b1) / 3), 255);
    } else {
        pal[2] = pack_rgba8((uint8_t)((r0 + r1) / 2), (uint8_t)((g0 + g1) / 2), (uint8_t)((b0 + b1) / 2), 255);
        pal[3] = mode == 1 ? 0 : 0xff000000u;
    }
    return rd32le(block + 4);
}

static DXT_SMALL_SCALAR uint64_t dxt_alpha_palette(const uint8_t *block, uint32_t pal[8])
{
    unsigned a0=block[0], a1=block[1];
    pal[0]=a0;pal[1]=a1;
    if(a0>a1) for(int i=2;i<8;++i)pal[i]=((8-i)*a0+(i-1)*a1)/7;
    else {
        for(int i=2;i<6;++i)pal[i]=((6-i)*a0+(i-1)*a1)/5;
        pal[6]=0;pal[7]=255;
    }
    uint64_t bits=0;
    for(int i=0;i<6;++i)bits|=(uint64_t)block[2+i]<<(i*8);
    return bits;
}

static DXT_SMALL_SCALAR void decode_dxt_block(GLenum format, const uint8_t *block, uint32_t out[16])
{
    int kind=(int)(format-GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    uint32_t pal[4],alpha[8];
    uint32_t code=dxt_color_palette(block+(kind>=2?8:0),kind<2?kind:2,pal);
    uint64_t bits=0;
    if(kind==2)bits=(uint64_t)rd32le(block)|((uint64_t)rd32le(block+4)<<32);
    if(kind==3)bits=dxt_alpha_palette(block,alpha);
    for(int i=0;i<16;++i) {
        uint32_t pixel=pal[(code>>(2*i))&3u];
        if(kind>=2) {
            unsigned a=kind==2?((bits>>(4*i))&15u)*17:alpha[(bits>>(3*i))&7u];
            pixel=(pixel&0xffffffu)|(a<<24);
        }
        out[i]=pixel;
    }
}

#if SG_TEX_RVV && SGL_DXT_RVV
/* Decode several neighbouring blocks in one vector, then gather each output
 * row for contiguous stores. Palette construction is per block, not per pixel.
 * Small VLENs/images use the scalar block path rather than underfilled vectors. */
static int decode_dxt_batch(sgl_tex *tx, int x, int y, const uint8_t *source,
                            int available, GLenum format)
{
    size_t capacity=__riscv_vsetvlmax_e32m2();
    int n=(int)(capacity/16);
    if(n>16)n=16;
    if(n>available)n=available;
    if(n<1)return 0;
    int kind=(int)(format-GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
    int bytes=kind<2?8:16;
    uint32_t palettes[64],codes[16],alphas[128];
    uint64_t alphaBits[16];
    for(int block=0;block<n;++block) {
        const uint8_t *p=source+block*bytes;
        codes[block]=dxt_color_palette(p+(kind>=2?8:0),kind<2?kind:2,palettes+block*4);
        if(kind==2)alphaBits[block]=(uint64_t)rd32le(p)|((uint64_t)rd32le(p+4)<<32);
        if(kind==3)alphaBits[block]=dxt_alpha_palette(p,alphas+block*8);
    }
    size_t vl=(size_t)n*16;
    vuint32m2_t lane=__riscv_vid_v_u32m2(vl);
    vuint32m2_t block=__riscv_vsrl_vx_u32m2(lane,4,vl);
    vuint32m2_t within=__riscv_vand_vx_u32m2(lane,15,vl);
    vuint32m2_t code=__riscv_vluxei32_v_u32m2(codes,__riscv_vsll_vx_u32m2(block,2,vl),vl);
    vuint32m2_t index=__riscv_vand_vx_u32m2(__riscv_vsrl_vv_u32m2(code,__riscv_vsll_vx_u32m2(within,1,vl),vl),3,vl);
    index=__riscv_vadd_vv_u32m2(index,__riscv_vsll_vx_u32m2(block,2,vl),vl);
    vuint32m2_t pal=__riscv_vle32_v_u32m2(palettes,(size_t)n*4);
    vuint32m2_t pixels=__riscv_vrgather_vv_u32m2(pal,index,vl);
    if(kind>=2) {
        vuint64m4_t bits=__riscv_vluxei32_v_u64m4(alphaBits,__riscv_vsll_vx_u32m2(block,3,vl),vl);
        vuint64m4_t shift=__riscv_vwaddu_vx_u64m4(within,0,vl);
        shift=__riscv_vmul_vx_u64m4(shift,kind==2?4:3,vl);
        vuint32m2_t ai=__riscv_vnsrl_wx_u32m2(__riscv_vsrl_vv_u64m4(bits,shift,vl),0,vl);
        ai=__riscv_vand_vx_u32m2(ai,kind==2?15:7,vl);
        vuint32m2_t alpha;
        if(kind==2)alpha=__riscv_vmul_vx_u32m2(ai,17,vl);
        else {
            ai=__riscv_vadd_vv_u32m2(ai,__riscv_vsll_vx_u32m2(block,3,vl),vl);
            alpha=__riscv_vrgather_vv_u32m2(__riscv_vle32_v_u32m2(alphas,(size_t)n*8),ai,vl);
        }
        pixels=__riscv_vor_vv_u32m2(__riscv_vand_vx_u32m2(pixels,0xffffffu,vl),__riscv_vsll_vx_u32m2(alpha,24,vl),vl);
    }
    size_t rowVl=(size_t)n*4;
    vuint32m2_t col=__riscv_vid_v_u32m2(rowVl);
    vuint32m2_t rowIndex=__riscv_vadd_vv_u32m2(__riscv_vsll_vx_u32m2(__riscv_vsrl_vx_u32m2(col,2,rowVl),4,rowVl),__riscv_vand_vx_u32m2(col,3,rowVl),rowVl);
    for(int row=0;row<4;++row) {
        vuint32m2_t idx=__riscv_vadd_vx_u32m2(rowIndex,row*4,rowVl);
        __riscv_vse32_v_u32m2(tx->texels+(size_t)(y+row)*tx->w+x,__riscv_vrgather_vv_u32m2(pixels,idx,rowVl),rowVl);
    }
    return n;
}
#endif

static int upload_compressed_rect(sgl_tex *tx, GLint xoffset, GLint yoffset,
                                  GLsizei w, GLsizei h, GLenum format,
                                  GLsizei imageSize, const void *data)
{
    int block_bytes = compressed_format_block_bytes(format);
    if (!block_bytes) { set_err(GL_INVALID_ENUM); return 0; }
    if (!data) return 1;
    int bw = (w + 3) / 4;
    int bh = (h + 3) / 4;
    size_t need = (size_t)bw * (size_t)bh * (size_t)block_bytes;
    if (imageSize < 0 || (size_t)imageSize < need) { set_err(GL_INVALID_VALUE); return 0; }
    const uint8_t *src = (const uint8_t *)data;
#if SG_TEX_RVV && SGL_DXT_RVV
    const int use_batch = sgl_dxt_vector_decode && __riscv_vsetvlmax_e32m2() >= 32;
#endif
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
#if SG_TEX_RVV && SGL_DXT_RVV
            if (use_batch && by*4+3<h && bx<w/4) {
                int done=decode_dxt_batch(tx,xoffset+bx*4,yoffset+by*4,
                    src+((size_t)by*bw+bx)*block_bytes,w/4-bx,format);
                if(done) { bx+=done-1;continue; }
            }
#endif
            uint32_t block[16];
            decode_dxt_block(format, src + ((size_t)by * (size_t)bw + (size_t)bx) * (size_t)block_bytes, block);
            if (by * 4 + 3 < h && bx * 4 + 3 < w) {
                /* Interior block: all 16 texels in bounds -> drop the 32 per-texel
                 * bounds compares, write the 4x4 as four contiguous 4-texel rows. */
                for (int py = 0; py < 4; ++py) {
                    uint32_t *d = &tx->texels[(size_t)(yoffset + by * 4 + py) * (size_t)tx->w +
                                              (size_t)(xoffset + bx * 4)];
                    const uint32_t *s = &block[py * 4];
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                }
                continue;
            }
            for (int py = 0; py < 4; ++py) {
                int dy = by * 4 + py;
                if (dy >= h) continue;
                for (int px = 0; px < 4; ++px) {
                    int dx = bx * 4 + px;
                    if (dx >= w) continue;
                    tx->texels[(size_t)(yoffset + dy) * (size_t)tx->w + (size_t)(xoffset + dx)] = block[py * 4 + px];
                }
            }
        }
    }
    return 1;
}

static void upload_tex_rect(sgl_tex *tx, GLint xoffset, GLint yoffset,
                            GLsizei w, GLsizei h, GLenum format, GLenum type,
                            const void *pixels)
{
    if (!pixels) return;
    /* GLES2 unsized formats. The single-channel forms carry Doom3's per-light FALLOFF and
     * PROJECTION textures (and monochrome maps); rejecting them left those textures zero-
     * filled, so the interaction pass multiplied light by a zero sample and every lit
     * surface rendered BLACK. Accept + unpack per GLES2 sampling semantics. */
    int comps;
    switch (format) {
        case GL_RGBA:            comps = 4; break;
        case GL_RGB:             comps = 3; break;
        case GL_LUMINANCE_ALPHA: comps = 2; break;
        case GL_LUMINANCE:
        case GL_ALPHA:           comps = 1; break;
        default:                 set_err(GL_INVALID_ENUM); return;
    }
    if (type != GL_UNSIGNED_BYTE) { set_err(GL_INVALID_ENUM); return; }
#if defined(SGL_BAREMETAL)
    uint64_t profile_start = BM_TextureBegin ? BM_TextureBegin() : 0;
#endif
    const uint8_t *src = (const uint8_t *)pixels;
    size_t stride = unpack_stride(w, comps);
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *dst = tx->texels + (size_t)(yoffset + y) * (size_t)tx->w + xoffset;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        /* RGBA8 already matches storage. Preserve unpack padding, destination
         * stride and RGB's forced alpha; memcpy also accepts unaligned input. */
        if (format == GL_RGBA && !tx->opaque_alpha) {
            memcpy(dst, row, (size_t)w * 4);
            continue;
        }
#if SG_TEX_RVV && SGL_UPLOAD_RVV
        if (format == GL_RGBA && ((uintptr_t)row & 3u) == 0) {
            /* Aligned packed RGBA: three vector operations per chunk. Keep
             * unaligned input on the byte-safe scalar path for hardware that
             * does not support misaligned vector element loads. */
            for (size_t x = 0; x < (size_t)w;) {
                size_t vl = __riscv_vsetvl_e32m8((size_t)w - x);
                vuint32m8_t p = __riscv_vle32_v_u32m8((const uint32_t *)row + x, vl);
                p = __riscv_vor_vx_u32m8(p, 0xff000000u, vl);
                __riscv_vse32_v_u32m8(dst + x, p, vl);
                x += vl;
            }
            continue;
        }
#endif
#endif
        for (int x = 0; x < w; ++x) {
            const uint8_t *p = row + (size_t)x * comps;
            uint8_t r, g, b, a;
            switch (format) {
                case GL_RGBA:            r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
                case GL_RGB:             r = p[0]; g = p[1]; b = p[2]; a = 255;  break;
                case GL_LUMINANCE_ALPHA: r = g = b = p[0]; a = p[1]; break;      /* (L,L,L,A) */
                case GL_LUMINANCE:       r = g = b = p[0]; a = 255;  break;      /* (L,L,L,1) */
                default:                 r = g = b = 0;    a = p[0]; break;      /* GL_ALPHA -> (0,0,0,A) */
            }
            if (tx->opaque_alpha) a = 255;
            dst[x] = r | (g << 8) | (b << 16) | ((uint32_t)a << 24);
        }
    }
#if defined(SGL_BAREMETAL)
    if (BM_TextureEnd) BM_TextureEnd(BM_TEX_COPY, profile_start, (uint64_t)w*h*4, 0);
#endif

}

static uint32_t fb_color_at_gl(GLint x, GLint y)
{
    int fy = C.fb->h - 1 - y;
    return C.fb->color[(size_t)fy * (size_t)C.fb->w + (size_t)x];
}

/* Depth-buffer capture: return the raw IEEE-754 bits of the window-z float, so a
 * depth texel holds bit-for-bit what glReadPixels(GL_DEPTH_COMPONENT,GL_FLOAT)
 * would return. Same top-to-bottom flip as fb_color_at_gl. */
static inline uint32_t fb_depth_at_gl(GLint x, GLint y)
{
    if (!C.fb || !C.fb->depth) return 0;
    if (x < 0 || y < 0 || x >= C.fb->w || y >= C.fb->h) return 0;
    int fy = C.fb->h - 1 - y;
    float d = C.fb->depth[(size_t)fy * (size_t)C.fb->w + (size_t)x];
    uint32_t bits;
    memcpy(&bits, &d, sizeof bits);
    return bits;
}

static int color_internalformat_ok(GLenum internalformat)
{
    return internalformat == GL_RGB || internalformat == GL_RGBA ||
           internalformat == GL_RGB8 || internalformat == GL_RGBA8;
}

/* True for the depth internalformats Doom 3 / gl4es hand to glTexImage2D for the
 * _currentDepth capture. Enum literals are used because the GLES2 headers do not
 * always define the 24/32/32F tokens. */
static int is_depth_internalformat(GLint f)
{
    switch (f) {
        case GL_DEPTH_COMPONENT: /* 0x1902 */
        case 0x81A5:             /* GL_DEPTH_COMPONENT16 */
        case 0x81A6:             /* GL_DEPTH_COMPONENT24 / _ARB */
        case 0x81A7:             /* GL_DEPTH_COMPONENT32 */
        case 0x8CAC:             /* GL_DEPTH_COMPONENT32F */
            return 1;
        default:
            return 0;
    }
}

static int source_rect_ok(GLint x, GLint y, GLsizei w, GLsizei h)
{
    return C.fb && x >= 0 && y >= 0 && w >= 0 && h >= 0 &&
           x + w <= C.fb->w && y + h <= C.fb->h;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    if (n < 0) { set_err(GL_INVALID_VALUE); return; }
    if (!textures && n) { set_err(GL_INVALID_VALUE); return; }
    for (GLsizei k = 0; k < n; ++k) {
        textures[k] = 0;
        for (GLuint i = 1; i < MAX_TEXTURES; ++i)
            if (!g_textures[i].used) {
                memset(&g_textures[i], 0, sizeof(g_textures[i]));
                g_textures[i].used = 1;
                textures[k] = i;
                break;
            }
        if (!textures[k]) set_err(GL_OUT_OF_MEMORY);
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    sg_framebin_barrier();   /* frees texels a recorded draw still points at */
    sg_framebin_flush();   /* pending deferred draws may reference these textures */
    for (GLsizei k = 0; k < n; ++k) {
        GLuint t = textures[k];
        if (t && t < MAX_TEXTURES && g_textures[t].used) {
            free_tex_storage(&g_textures[t]);
            g_textures[t].target = 0;
            g_textures[t].used = 0;
        }
    }
}

GLboolean glIsTexture(GLuint texture)
{
    return (texture && texture < MAX_TEXTURES && g_textures[texture].used) ? GL_TRUE : GL_FALSE;
}

void glActiveTexture(GLenum texture)
{
    GLuint u = texture - GL_TEXTURE0;
    if (u < MAX_TEXUNITS) C.active_unit = u; else set_err(GL_INVALID_ENUM);
}

/* Texture object bound at the active unit for an operation's target: cube-map
 * (incl. the 6 face targets) -> the unit's CUBE slot, else the 2D slot. Routing
 * by target (not by which slot gl4es last scratch-bound) keeps cube uploads /
 * params on the cube and 2D ops on the 2D texture. */
static GLuint cur_tex_for_target(GLenum target)
{
    return (target == GL_TEXTURE_CUBE_MAP || cube_face_index(target) >= 0)
           ? C.unit_cube[C.active_unit] : C.unit_tex[C.active_unit];
}

void glBindTexture(GLenum target, GLuint texture)
{
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_CUBE_MAP) { set_err(GL_INVALID_ENUM); return; }
    if (texture && texture < MAX_TEXTURES && g_textures[texture].used) {
        tex_obj *o = &g_textures[texture];
        if (!o->target) o->target = target;
        if (target == GL_TEXTURE_CUBE_MAP) o->tex.is_cube = 1;
    }
    /* GLES2 texture units have SEPARATE 2D and CUBE bind points; track them
     * separately so a samplerCube and a sampler2D at the same unit each resolve
     * to the right texture. gl4es scratch-binds a cube's name via GL_TEXTURE_2D
     * to set its params, then restores the prior 2D binding (realize_1texture),
     * so the cube must survive in its own slot — collapsing them loses it. */
    if (target == GL_TEXTURE_CUBE_MAP) C.unit_cube[C.active_unit] = texture;
    else                              C.unit_tex[C.active_unit]  = texture;
}

/* GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE. Clamp to [1,16] and propagate to the
 * 6 cube faces (a cube's params are mirrored onto each face like min_filter/wrap).
 * Accepts the float value directly so glTexParameterf keeps fractional requests. */
#define SGL_GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
static void tex_set_max_aniso(tex_obj *o, float v)
{
    if (v < 1.f) v = 1.f; else if (v > 16.f) v = 16.f;
    o->tex.max_aniso = v;
    for (int i = 0; i < 6; ++i) o->cube[i].max_aniso = v;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    sg_framebin_barrier();   /* filter/wrap are read at shade time */
    if (!tex_target_ok(target)) { set_err(GL_INVALID_ENUM); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES) return;
    tex_obj *o = &g_textures[t];
    sgl_tex *tx = &o->tex;
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER) {
        tx->min_filter = (param == GL_LINEAR || param == GL_LINEAR_MIPMAP_NEAREST || param == GL_LINEAR_MIPMAP_LINEAR) ? 1 : 0;
        /* The mipmap part of the MIN filter used to be thrown away here, which
         * is why GL_LINEAR_MIPMAP_LINEAR behaved as plain GL_LINEAR on level 0.
         * MAG filtering has no mip modes, so only MIN sets this. */
        if (pname == GL_TEXTURE_MIN_FILTER) {
            if (param == GL_NEAREST_MIPMAP_LINEAR || param == GL_LINEAR_MIPMAP_LINEAR)
                tx->mip_filter = 2;      /* trilinear: blend two levels */
            else if (param == GL_NEAREST_MIPMAP_NEAREST || param == GL_LINEAR_MIPMAP_NEAREST)
                tx->mip_filter = 1;      /* pick nearest level */
            else
                tx->mip_filter = 0;      /* GL_NEAREST / GL_LINEAR: level 0 only */
        }
    } else if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R)
        tx->wrap = (param == GL_REPEAT) ? 1 : 0;
    else if (pname == SGL_GL_TEXTURE_MAX_ANISOTROPY_EXT) {
        tex_set_max_aniso(o, (float)param);   /* handles the 6 cube faces itself */
        return;
    }
    else
        return;
    for (int i = 0; i < 6; ++i) {
        o->cube[i].min_filter = tx->min_filter;
        o->cube[i].mip_filter = tx->mip_filter;
        o->cube[i].wrap = tx->wrap;
    }
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    sg_framebin_barrier();   /* filter/wrap are read at shade time */
    if (pname == SGL_GL_TEXTURE_MAX_ANISOTROPY_EXT) {
        /* Accept the float value (Doom sends 8.0f); (GLint) truncation below would
         * still work for integers but loses fractional maxima. */
        if (!tex_target_ok(target)) { set_err(GL_INVALID_ENUM); return; }
        GLuint t = cur_tex_for_target(target);
        if (!t || t >= MAX_TEXTURES) return;
        tex_set_max_aniso(&g_textures[t], param);
        return;
    }
    glTexParameteri(target, pname, (GLint)param);
}

void glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    sg_framebin_barrier();   /* filter/wrap are read at shade time */
    if (params) glTexParameteri(target, pname, params[0]);
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
    sg_framebin_barrier();   /* filter/wrap are read at shade time */
    if (params) glTexParameterf(target, pname, params[0]);
}

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params)
{
    if (!params) return;
    *params = 0;
    if (!tex_target_ok(target)) { set_err(GL_INVALID_ENUM); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) return;
    sgl_tex *tx = &g_textures[t].tex;
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)
        *params = tx->min_filter ? GL_LINEAR : GL_NEAREST;
    else if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R)
        *params = tx->wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
}

void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params)
{
    GLint v = 0;
    if (!params) return;
    glGetTexParameteriv(target, pname, &v);
    *params = (GLfloat)v;
}

void glPixelStorei(GLenum pname, GLint param)
{
    if (param != 1 && param != 2 && param != 4 && param != 8) { set_err(GL_INVALID_VALUE); return; }
    if (pname == GL_PACK_ALIGNMENT) C.pack_alignment = param;
    else if (pname == GL_UNPACK_ALIGNMENT) C.unpack_alignment = param;
    else set_err(GL_INVALID_ENUM);
}

static void tex_image_2d_impl(GLenum target, GLint level, GLint internalformat,
                  GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const void *pixels)
{
    sg_framebin_barrier();   /* reallocates/overwrites texels a recorded draw points at */
    sg_framebin_flush();   /* mutating texels a pending deferred draw may sample */
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (level < 0 || level >= SGL_MAX_MIP) { set_err(GL_INVALID_VALUE); return; }
    if (border != 0 || w < 0 || h < 0) { set_err(GL_INVALID_VALUE); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    if (level == 0) memset(o->compressed_format[face + 1], 0, SGL_MAX_MIP);
    else o->compressed_format[face + 1][level] = 0;
    if (face >= 0) {
        o->target = GL_TEXTURE_CUBE_MAP;   /* a cube-face upload defines this texture as a cube */
        o->tex.is_cube = 1;
        sgl_tex *tx = &o->cube[face];
        if (level > 0) {
            /* Application-supplied mip level (Doom 3 uploads a full chain per
             * face). Previously dropped on the floor. */
            sgl_tex view;
            if (!mip_alloc_level(tx, level, w, h, &view)) { set_err(GL_INVALID_OPERATION); return; }
            if (pixels) {
                upload_tex_rect(&view, 0, 0, w, h, format, type, pixels);
                tx->mips[level].texels = view.texels;
            }
            return;
        }
        tx->min_filter = o->tex.min_filter;
        tx->wrap = o->tex.wrap;
        tx->is_cube = 0;
        tx->opaque_alpha = internalformat == GL_RGB || internalformat == GL_RGB8;
        mip_free_upper(tx);
        free(tx->texels);
        tx->w = w; tx->h = h;
        tx->texels = (uint32_t *)malloc((size_t)w * h * 4);
        if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
        memset(tx->texels, 0, (size_t)w * h * 4);
        if (pixels) upload_tex_rect(tx, 0, 0, w, h, format, type, pixels);
        tx->nmip = 1;
        mip_sync0(tx);
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
        return;
    }
    if (o->target && o->target != GL_TEXTURE_2D) { set_err(GL_INVALID_OPERATION); return; }
    o->target = GL_TEXTURE_2D;
    if (level > 0) {
        /* Application-supplied mip level. idImage::GenerateImage uploads the
         * whole chain; every level past 0 used to be silently discarded, so the
         * sampler then read full-resolution texels however minified the surface
         * was -- the single largest source of texture cache misses. */
        sgl_tex view;
        sgl_tex *tx2 = &o->tex;
        if (!mip_alloc_level(tx2, level, w, h, &view)) { set_err(GL_INVALID_OPERATION); return; }
        if (pixels) {
            upload_tex_rect(&view, 0, 0, w, h, format, type, pixels);
            tx2->mips[level].texels = view.texels;
        }
        return;
    }
    sgl_tex params = o->tex;
    free_tex_storage(o);
    o->tex.min_filter = params.min_filter;
    o->tex.wrap = params.wrap;
    o->tex.mip_filter = params.mip_filter;
    sgl_tex *tx = &o->tex;
    tx->is_depth = is_depth_internalformat(internalformat);
    tx->opaque_alpha = internalformat == GL_RGB || internalformat == GL_RGB8;
    free(tx->texels);
    tx->w = w; tx->h = h;
    tx->texels = (uint32_t *)malloc((size_t)w * h * 4);
    if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
    memset(tx->texels, 0, (size_t)w * h * 4);
    if (pixels) upload_tex_rect(tx, 0, 0, w, h, format, type, pixels);
    tx->nmip = 1;
    mip_sync0(tx);
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type,
                  const void *pixels)
{
#if defined(SGL_BAREMETAL)
    uint64_t start = BM_TextureBegin ? BM_TextureBegin() : 0;
#endif
    tex_image_2d_impl(target, level, internalformat, w, h, border, format, type, pixels);
#if defined(SGL_BAREMETAL)
    if (BM_TextureEnd) BM_TextureEnd(BM_TEX_SOFT, start,
        w>0 && h>0 ? (uint64_t)w*h*4 : 0, 0);
#endif
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei w, GLsizei h, GLenum format, GLenum type,
                     const void *pixels)
{
    sg_framebin_barrier();   /* overwrites texels a recorded draw points at */
    sg_framebin_flush();   /* mutating texels a pending deferred draw may sample */
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (level != 0) return;
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    sgl_tex *tx = (face >= 0) ? &o->cube[face] : &o->tex;
    if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
    if (xoffset < 0 || yoffset < 0 || w < 0 || h < 0 ||
        xoffset + w > tx->w || yoffset + h > tx->h) { set_err(GL_INVALID_VALUE); return; }
    upload_tex_rect(tx, xoffset, yoffset, w, h, format, type, pixels);
    if (face >= 0) {
        o->tex.is_cube = 1;
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
    }
}

/* Check sizes before touching existing storage, even for NULL client data. */
static int compressed_size_ok(GLsizei w, GLsizei h, GLenum format, GLsizei size)
{
    size_t bytes = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) *
                   (size_t)compressed_format_block_bytes(format);
    if (size < 0 || (size_t)size != bytes) { set_err(GL_INVALID_VALUE); return 0; }
    return 1;
}

unsigned long sgl_dbg_dds_uploads, sgl_dbg_dds_mips, sgl_dbg_dds_bytes;

static void compressed_tex_image_2d_impl(GLenum target, GLint level, GLenum internalformat,
                            GLsizei w, GLsizei h, GLint border,
                            GLsizei imageSize, const void *data)
{
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (!compressed_format_block_bytes(internalformat)) { set_err(GL_INVALID_ENUM); return; }
    if (level < 0 || level >= SGL_MAX_MIP || w < 0 || h < 0 ||
        w > (4096 >> level) || h > (4096 >> level) || (face >= 0 && w != h)) {
        set_err(GL_INVALID_VALUE); return;
    }
    if (border != 0) { set_err(GL_INVALID_OPERATION); return; }
    if (!compressed_size_ok(w, h, internalformat, imageSize)) return;
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    GLenum binding = face >= 0 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    if (o->target && o->target != binding) { set_err(GL_INVALID_OPERATION); return; }
    /* Deferred draws must consume the previous texels before replacement. */
    sg_framebin_barrier();
    sg_framebin_flush();
    sgl_tex *tx = face >= 0 ? &o->cube[face] : &o->tex;
    size_t bytes = (size_t)w * (size_t)h * 4;
    uint32_t *pixels = bytes ? (uint32_t *)malloc(bytes) : NULL;
    if (bytes && !pixels) { set_err(GL_OUT_OF_MEMORY); return; }
    if (level && !mip_table(tx)) { free(pixels); set_err(GL_OUT_OF_MEMORY); return; }
    sgl_tex view = *tx;
    view.w = w; view.h = h; view.texels = pixels;
    if (data && bytes) upload_compressed_rect(&view, 0, 0, w, h, internalformat, imageSize, data);
    else if (bytes) memset(pixels, 0, bytes);
    if (level == 0) {
        mip_free_upper(tx);
        free(tx->texels);
        tx->w = w; tx->h = h; tx->texels = pixels;
        tx->is_depth = 0; tx->opaque_alpha = internalformat == GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        tx->nmip = pixels ? 1 : 0;
        mip_sync0(tx);
        memset(o->compressed_format[face + 1], 0, SGL_MAX_MIP);
        if (face >= 0) {
            tx->min_filter = o->tex.min_filter; tx->wrap = o->tex.wrap;
            tx->mip_filter = o->tex.mip_filter; tx->max_aniso = o->tex.max_aniso;
        }
    } else {
        free(tx->mips[level].texels);
        tx->mips[level].texels = pixels;
        tx->mips[level].w = w; tx->mips[level].h = h;
        if (level + 1 > tx->nmip) tx->nmip = level + 1;
        ++sgl_dbg_mip_levels_stored;
        mip_sync0(tx);
    }
    o->compressed_format[face + 1][level] = (uint8_t)(internalformat - GL_COMPRESSED_RGB_S3TC_DXT1_EXT + 1);
    o->target = binding;
    ++sgl_dbg_dds_uploads;
    if (level) ++sgl_dbg_dds_mips;
    sgl_dbg_dds_bytes += imageSize;
    if (face >= 0) {
        o->tex.is_cube = 1;
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
    }
}

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei w, GLsizei h, GLint border,
                            GLsizei imageSize, const void *data)
{
#if defined(SGL_BAREMETAL)
    uint64_t start = BM_TextureBegin ? BM_TextureBegin() : 0;
#endif
    compressed_tex_image_2d_impl(target, level, internalformat, w, h, border, imageSize, data);
#if defined(SGL_BAREMETAL)
    if (BM_TextureEnd) BM_TextureEnd(BM_TEX_SOFT, start,
        imageSize > 0 ? (uint64_t)imageSize : 0, "DDS");
#endif
}

void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLsizei w, GLsizei h, GLenum format,
                               GLsizei imageSize, const void *data)
{
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (!compressed_format_block_bytes(format)) { set_err(GL_INVALID_ENUM); return; }
    if (level < 0 || level >= SGL_MAX_MIP || xoffset < 0 || yoffset < 0 || w < 0 || h < 0) {
        set_err(GL_INVALID_VALUE); return;
    }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    sgl_tex *tx = face >= 0 ? &o->cube[face] : &o->tex;
    sgl_tex view = *tx;
    if (level > 0) {
        if (!tx->mips || level >= tx->nmip) { set_err(GL_INVALID_OPERATION); return; }
        view.w = tx->mips[level].w; view.h = tx->mips[level].h;
        view.texels = tx->mips[level].texels;
    }
    if (!view.texels || o->compressed_format[face + 1][level] !=
        format - GL_COMPRESSED_RGB_S3TC_DXT1_EXT + 1) { set_err(GL_INVALID_OPERATION); return; }
    if (xoffset > view.w || yoffset > view.h || w > view.w - xoffset || h > view.h - yoffset) {
        set_err(GL_INVALID_VALUE); return;
    }
    if ((xoffset & 3) || (yoffset & 3) ||
        ((w & 3) && w != view.w - xoffset) || ((h & 3) && h != view.h - yoffset)) {
        set_err(GL_INVALID_OPERATION); return;
    }
    if (!compressed_size_ok(w, h, format, imageSize)) return;
    sg_framebin_barrier();
    sg_framebin_flush();
    if (data && w && h) upload_compressed_rect(&view, xoffset, yoffset, w, h, format, imageSize, data);
}

static sgl_tex *bound_tex_for_target(GLenum target, tex_obj **owner)
{
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return 0; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) { set_err(GL_INVALID_OPERATION); return 0; }
    tex_obj *o = &g_textures[t];
    if (owner) *owner = o;
    return (face >= 0) ? &o->cube[face] : &o->tex;
}

void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
{
    sg_framebin_barrier();   /* READS the framebuffer AND overwrites texels */
    if (level < 0 || border != 0 || !source_rect_ok(x, y, w, h)) { set_err(GL_INVALID_VALUE); return; }
    if (!color_internalformat_ok(internalformat)) { set_err(GL_INVALID_ENUM); return; }
    glTexImage2D(target, level, internalformat, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    sgl_tex *tx = bound_tex_for_target(target, 0);
    if (!tx || !tx->texels) return;
    glCopyTexSubImage2D(target, level, 0, 0, x, y, w, h);
}

void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei w, GLsizei h)
{
    sg_framebin_barrier();   /* READS the framebuffer AND overwrites texels (Doom _currentRender) */
    sg_framebin_flush();   /* reads the framebuffer (e.g. _currentRender capture) */
    if (level != 0) return;
    if (!source_rect_ok(x, y, w, h)) { set_err(GL_INVALID_VALUE); return; }
    tex_obj *o = 0;
    sgl_tex *tx = bound_tex_for_target(target, &o);
    if (!tx || !tx->texels) { set_err(GL_INVALID_OPERATION); return; }
    if (xoffset < 0 || yoffset < 0 || xoffset + w > tx->w || yoffset + h > tx->h) {
        set_err(GL_INVALID_VALUE);
        return;
    }
    int face = cube_face_index(target);
    if (tx->is_depth && C.fb->depth) {
        /* Capture scene depth: a depth texel = the window-z float's raw IEEE-754
         * bits (matches fb_depth_at_gl / glReadPixels). The source row and the
         * destination texel row are both contiguous and float->u32 is a bitwise
         * no-op, so copy each row straight (RVV vle32/vse32, mirroring
         * sgl_copy_u32 in sg_fb.c). source_rect_ok already bounded x/y/w/h. */
        for (int row = 0; row < h; ++row) {
            uint32_t *dst = tx->texels + (size_t)(yoffset + row) * (size_t)tx->w + (size_t)xoffset;
#if SG_TEX_RVV
            int fy = C.fb->h - 1 - (y + row);
            const uint32_t *src = (const uint32_t *)(C.fb->depth + (size_t)fy * (size_t)C.fb->w + (size_t)x);
            for (int col = 0; col < w; ) {
                size_t vl = __riscv_vsetvl_e32m8((size_t)(w - col));
                __riscv_vse32_v_u32m8(&dst[col], __riscv_vle32_v_u32m8(&src[col], vl), vl);
                col += (int)vl;
            }
#else
            for (int col = 0; col < w; ++col)
                dst[col] = fb_depth_at_gl(x + col, y + row);
#endif
        }
    } else {
        for (int row = 0; row < h; ++row) {
            uint32_t *dst = tx->texels + (size_t)(yoffset + row) * (size_t)tx->w + (size_t)xoffset;
            const uint32_t *src = C.fb->color + (size_t)(C.fb->h - 1 - (y + row)) * (size_t)C.fb->w + (size_t)x;
            uint32_t alpha = tx->opaque_alpha ? 0xff000000u : 0;
#if SG_TEX_RVV
            for (int col = 0; col < w; ) {
                size_t vl = __riscv_vsetvl_e32m8((size_t)(w - col));
                vuint32m8_t pixels = __riscv_vle32_v_u32m8(src + col, vl);
                pixels = __riscv_vor_vx_u32m8(pixels, alpha, vl);
                __riscv_vse32_v_u32m8(dst + col, pixels, vl);
                col += (int)vl;
            }
#else
            for (int col = 0; col < w; ++col)
                dst[col] = src[col] | alpha;
#endif
        }
    }
    if (face >= 0 && o) {
        o->target = GL_TEXTURE_CUBE_MAP;
        o->tex.is_cube = 1;
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
    }
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, void *pixels)
{
    sg_framebin_barrier();   /* READS the framebuffer before recorded draws have written it */
    sg_framebin_flush();   /* reads back the framebuffer */
    if (!pixels) return;
    if (!source_rect_ok(x, y, width, height)) { set_err(GL_INVALID_VALUE); return; }
    int bpp = 0;
    if ((format == GL_RGB || format == GL_RGBA) && type == GL_UNSIGNED_BYTE) bpp = (format == GL_RGB) ? 3 : 4;
    else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) bpp = 2;
    else if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT) bpp = 4;
    else if (format == GL_STENCIL_INDEX && type == GL_UNSIGNED_BYTE) bpp = 1;
    else { set_err(GL_INVALID_ENUM); return; }

    uint8_t *dst0 = (uint8_t *)pixels;
    size_t stride = pack_stride(width, bpp);
    for (int row = 0; row < height; ++row) {
        uint8_t *dst = dst0 + (size_t)row * stride;
        int fy = C.fb->h - 1 - (y + row);
        for (int col = 0; col < width; ++col) {
            int fx = x + col;
            if (format == GL_DEPTH_COMPONENT) {
                float z = C.fb->depth[(size_t)fy * (size_t)C.fb->w + (size_t)fx];
                memcpy(dst + (size_t)col * 4, &z, 4);
            } else if (format == GL_STENCIL_INDEX) {
                dst[col] = C.fb->stencil ? C.fb->stencil[(size_t)fy * (size_t)C.fb->w + (size_t)fx] : 0;
            } else {
                uint32_t c = C.fb->color[(size_t)fy * (size_t)C.fb->w + (size_t)fx];
                uint8_t r = (uint8_t)(c & 0xFF);
                uint8_t g = (uint8_t)((c >> 8) & 0xFF);
                uint8_t b = (uint8_t)((c >> 16) & 0xFF);
                if (type == GL_UNSIGNED_SHORT_5_6_5) {
                    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                    memcpy(dst + (size_t)col * 2, &v, 2);
                } else {
                    uint8_t *p = dst + (size_t)col * bpp;
                    p[0] = r; p[1] = g; p[2] = b;
                    if (format == GL_RGBA) p[3] = (uint8_t)((c >> 24) & 0xFF);
                }
            }
        }
    }
}

void glGenerateMipmap(GLenum target)
{
    sg_framebin_barrier();   /* rewrites texel levels */
    sg_framebin_flush();     /* a pending deferred draw may sample these texels */
    if (!tex_target_ok(target)) { set_err(GL_INVALID_ENUM); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    if (o->tex.is_cube) {
        for (int f = 0; f < 6; ++f) {
            if (o->cube_mask & (1u << f)) {
                mip_generate(&o->cube[f]);
                for (int l = 1; l < o->cube[f].nmip; ++l)
                    o->compressed_format[f + 1][l] = o->compressed_format[f + 1][0];
            }
        }
        return;
    }
    mip_generate(&o->tex);
    for (int l = 1; l < o->tex.nmip; ++l)
        o->compressed_format[0][l] = o->compressed_format[0][0];
}
