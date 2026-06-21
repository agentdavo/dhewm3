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

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SG_TEX_RVV 1
#else
#  define SG_TEX_RVV 0
#endif

tex_obj g_textures[MAX_TEXTURES];

static void free_tex_storage(tex_obj *o)
{
    free(o->tex.texels);
    o->tex.texels = 0;
    for (int i = 0; i < 6; ++i) {
        free(o->cube[i].texels);
        o->cube[i].texels = 0;
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

static void decode_dxt_colors(const uint8_t *block, int alpha_mode, uint32_t out[16])
{
    uint16_t c0 = rd16le(block + 0);
    uint16_t c1 = rd16le(block + 2);
    uint8_t r0 = expand5(c0 >> 11), g0 = expand6((c0 >> 5) & 0x3f), b0 = expand5(c0 & 0x1f);
    uint8_t r1 = expand5(c1 >> 11), g1 = expand6((c1 >> 5) & 0x3f), b1 = expand5(c1 & 0x1f);
    uint32_t pal[4];
    pal[0] = pack_rgba8(r0, g0, b0, 255);
    pal[1] = pack_rgba8(r1, g1, b1, 255);
    if (!alpha_mode || c0 > c1) {
        pal[2] = pack_rgba8((uint8_t)((2 * r0 + r1) / 3), (uint8_t)((2 * g0 + g1) / 3), (uint8_t)((2 * b0 + b1) / 3), 255);
        pal[3] = pack_rgba8((uint8_t)((r0 + 2 * r1) / 3), (uint8_t)((g0 + 2 * g1) / 3), (uint8_t)((b0 + 2 * b1) / 3), 255);
    } else {
        pal[2] = pack_rgba8((uint8_t)((r0 + r1) / 2), (uint8_t)((g0 + g1) / 2), (uint8_t)((b0 + b1) / 2), 255);
        pal[3] = 0;
    }
    uint32_t code = rd32le(block + 4);
#if SG_TEX_RVV
    /* RVV: out[i] = pal[(code>>2i)&3] for the 16 texels as one vrgather over the
     * 4-entry palette, replacing 16 scalar shift/mask/index loads. The texel
     * codes are 2-bit indices in {0,1,2,3}; vrgather of the palette held in
     * lanes 0-3 is bit-identical to the scalar table lookup. */
    size_t vl = __riscv_vsetvl_e32m1(16);
    vuint32m1_t palv = __riscv_vle32_v_u32m1(pal, 4);                   /* pal[0..3] -> lanes 0-3 */
    vuint32m1_t shift = __riscv_vsll_vx_u32m1(__riscv_vid_v_u32m1(vl), 1, vl); /* 2*i */
    vuint32m1_t idx = __riscv_vand_vx_u32m1(
        __riscv_vsrl_vv_u32m1(__riscv_vmv_v_x_u32m1(code, vl), shift, vl), 3u, vl);
    __riscv_vse32_v_u32m1(out, __riscv_vrgather_vv_u32m1(palv, idx, vl), vl);
#else
    for (int i = 0; i < 16; ++i)
        out[i] = pal[(code >> (2 * i)) & 3u];
#endif
}

static void decode_dxt_block(GLenum format, const uint8_t *block, uint32_t out[16])
{
    if (format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT || format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) {
        decode_dxt_colors(block, format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, out);
        return;
    }

    decode_dxt_colors(block + 8, 0, out);
    if (format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT) {
        for (int row = 0; row < 4; ++row) {
            uint16_t bits = rd16le(block + row * 2);
            for (int col = 0; col < 4; ++col) {
                uint8_t a = (uint8_t)(((bits >> (col * 4)) & 0x0f) * 17);
                out[row * 4 + col] = (out[row * 4 + col] & 0x00ffffffu) | ((uint32_t)a << 24);
            }
        }
        return;
    }

    uint8_t a0 = block[0], a1 = block[1];
    uint8_t apal[8];
    apal[0] = a0; apal[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i)
            apal[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
    } else {
        for (int i = 2; i < 6; ++i)
            apal[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5);
        apal[6] = 0;
        apal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)block[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i) {
        uint8_t a = apal[(bits >> (3 * i)) & 7u];
        out[i] = (out[i] & 0x00ffffffu) | ((uint32_t)a << 24);
    }
}

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
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
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
    const uint8_t *src = (const uint8_t *)pixels;
    size_t stride = unpack_stride(w, comps);
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *dst = tx->texels + (size_t)(yoffset + y) * (size_t)tx->w + xoffset;
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
            dst[x] = r | (g << 8) | (b << 16) | ((uint32_t)a << 24);
        }
    }
}

static uint32_t fb_color_at_gl(GLint x, GLint y)
{
    int fy = C.fb->h - 1 - y;
    return C.fb->color[(size_t)fy * (size_t)C.fb->w + (size_t)x];
}

static int color_internalformat_ok(GLenum internalformat)
{
    return internalformat == GL_RGB || internalformat == GL_RGBA ||
           internalformat == GL_RGB8 || internalformat == GL_RGBA8;
}

static int source_rect_ok(GLint x, GLint y, GLsizei w, GLsizei h)
{
    return C.fb && x >= 0 && y >= 0 && w >= 0 && h >= 0 &&
           x + w <= C.fb->w && y + h <= C.fb->h;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    for (GLsizei k = 0; k < n; ++k) {
        textures[k] = 0;
        for (GLuint i = 1; i < MAX_TEXTURES; ++i)
            if (!g_textures[i].used) {
                memset(&g_textures[i], 0, sizeof(g_textures[i]));
                g_textures[i].used = 1;
                textures[k] = i;
                break;
            }
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
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

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (!tex_target_ok(target)) { set_err(GL_INVALID_ENUM); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES) return;
    tex_obj *o = &g_textures[t];
    sgl_tex *tx = &o->tex;
    if (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)
        tx->min_filter = (param == GL_LINEAR || param == GL_LINEAR_MIPMAP_NEAREST || param == GL_LINEAR_MIPMAP_LINEAR) ? 1 : 0;
    else if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R)
        tx->wrap = (param == GL_REPEAT) ? 1 : 0;
    else
        return;
    for (int i = 0; i < 6; ++i) {
        o->cube[i].min_filter = tx->min_filter;
        o->cube[i].wrap = tx->wrap;
    }
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    glTexParameteri(target, pname, (GLint)param);
}

void glTexParameteriv(GLenum target, GLenum pname, const GLint *params)
{
    if (params) glTexParameteri(target, pname, params[0]);
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params)
{
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

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const void *pixels)
{
    sg_framebin_flush();   /* mutating texels a pending deferred draw may sample */
    (void)internalformat;
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (level < 0) { set_err(GL_INVALID_VALUE); return; }
    if (border != 0 || w < 0 || h < 0) { set_err(GL_INVALID_VALUE); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    if (face >= 0) {
        o->target = GL_TEXTURE_CUBE_MAP;   /* a cube-face upload defines this texture as a cube */
        o->tex.is_cube = 1;
        if (level > 0) return;
        sgl_tex *tx = &o->cube[face];
        tx->min_filter = o->tex.min_filter;
        tx->wrap = o->tex.wrap;
        tx->is_cube = 0;
        free(tx->texels);
        tx->w = w; tx->h = h;
        tx->texels = (uint32_t *)malloc((size_t)w * h * 4);
        if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
        memset(tx->texels, 0, (size_t)w * h * 4);
        if (pixels) upload_tex_rect(tx, 0, 0, w, h, format, type, pixels);
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
        return;
    }
    if (o->target && o->target != GL_TEXTURE_2D) { set_err(GL_INVALID_OPERATION); return; }
    o->target = GL_TEXTURE_2D;
    if (level > 0) return;
    sgl_tex params = o->tex;
    free_tex_storage(o);
    o->tex.min_filter = params.min_filter;
    o->tex.wrap = params.wrap;
    sgl_tex *tx = &o->tex;
    free(tx->texels);
    tx->w = w; tx->h = h;
    tx->texels = (uint32_t *)malloc((size_t)w * h * 4);
    if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
    memset(tx->texels, 0, (size_t)w * h * 4);
    if (pixels) upload_tex_rect(tx, 0, 0, w, h, format, type, pixels);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei w, GLsizei h, GLenum format, GLenum type,
                     const void *pixels)
{
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

void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei w, GLsizei h, GLint border,
                            GLsizei imageSize, const void *data)
{
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (!compressed_format_block_bytes(internalformat)) { set_err(GL_INVALID_ENUM); return; }
    if (level < 0 || border != 0 || w < 0 || h < 0) { set_err(GL_INVALID_VALUE); return; }
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    if (face >= 0) {
        o->target = GL_TEXTURE_CUBE_MAP;   /* a cube-face upload defines this texture as a cube */
        o->tex.is_cube = 1;
        if (level > 0) return;
        sgl_tex *tx = &o->cube[face];
        tx->min_filter = o->tex.min_filter;
        tx->wrap = o->tex.wrap;
        tx->is_cube = 0;
        free(tx->texels);
        tx->w = w; tx->h = h;
        tx->texels = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
        if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
        memset(tx->texels, 0, (size_t)w * (size_t)h * 4);
        if (!upload_compressed_rect(tx, 0, 0, w, h, internalformat, imageSize, data)) return;
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
        return;
    }
    if (o->target && o->target != GL_TEXTURE_2D) { set_err(GL_INVALID_OPERATION); return; }
    o->target = GL_TEXTURE_2D;
    if (level > 0) return;
    sgl_tex params = o->tex;
    free_tex_storage(o);
    o->tex.min_filter = params.min_filter;
    o->tex.wrap = params.wrap;
    sgl_tex *tx = &o->tex;
    tx->w = w; tx->h = h;
    tx->texels = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
    memset(tx->texels, 0, (size_t)w * (size_t)h * 4);
    (void)upload_compressed_rect(tx, 0, 0, w, h, internalformat, imageSize, data);
}

void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLsizei w, GLsizei h, GLenum format,
                               GLsizei imageSize, const void *data)
{
    int face = cube_face_index(target);
    if (target != GL_TEXTURE_2D && face < 0) { set_err(GL_INVALID_ENUM); return; }
    if (!compressed_format_block_bytes(format)) { set_err(GL_INVALID_ENUM); return; }
    if (level != 0) return;
    GLuint t = cur_tex_for_target(target);
    if (!t || t >= MAX_TEXTURES || !g_textures[t].used) { set_err(GL_INVALID_OPERATION); return; }
    tex_obj *o = &g_textures[t];
    sgl_tex *tx = (face >= 0) ? &o->cube[face] : &o->tex;
    if (!tx->texels) { set_err(GL_INVALID_OPERATION); return; }
    if (xoffset < 0 || yoffset < 0 || w < 0 || h < 0 ||
        xoffset + w > tx->w || yoffset + h > tx->h) { set_err(GL_INVALID_VALUE); return; }
    if (!upload_compressed_rect(tx, xoffset, yoffset, w, h, format, imageSize, data)) return;
    if (face >= 0) {
        o->target = GL_TEXTURE_CUBE_MAP;
        o->tex.is_cube = 1;
        o->cube_mask |= (uint8_t)(1u << face);
        o->tex.cube_faces[face] = tx;
    }
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
    for (int row = 0; row < h; ++row) {
        uint32_t *dst = tx->texels + (size_t)(yoffset + row) * (size_t)tx->w + (size_t)xoffset;
        for (int col = 0; col < w; ++col)
            dst[col] = fb_color_at_gl(x + col, y + row);
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
    if (!tex_target_ok(target)) set_err(GL_INVALID_ENUM);
}
