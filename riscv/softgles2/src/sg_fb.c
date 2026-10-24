#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdio.h>
#  include <stdint.h>
#endif

#include "sg_fb.h"
#include "sg_frame.h"
#include "sg_framebin.h"

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SG_FB_RVV 1
#else
#  define SG_FB_RVV 0
#endif

#define SGL_FB_ALIGN 128u


void *sgl_alloc_aligned_bytes(size_t n, size_t align, void **owner)
{
    uintptr_t mask = (uintptr_t)align - 1u;
    unsigned char *raw = (unsigned char *)malloc(n + align - 1u);
    if (!raw) {
        if (owner) *owner = 0;
        return 0;
    }
    uintptr_t p = ((uintptr_t)raw + mask) & ~mask;
    if (owner) *owner = raw;
    return (void *)p;
}

sgl_framebuffer *sgl_fb_create_ex(int w, int h, unsigned flags)
{
    sgl_framebuffer *fb = (sgl_framebuffer *)malloc(sizeof(*fb));
    if (!fb) return 0;
    memset(fb, 0, sizeof(*fb));
    fb->w = w;
    fb->h = h;
    size_t n = (size_t)w * (size_t)h;
    if (flags & SGL_FB_COLOR) {
        fb->color = (uint32_t *)sgl_alloc_aligned_bytes(n * sizeof(uint32_t), SGL_FB_ALIGN, &fb->color_alloc);
        /* Zero it: Doom 3 does not color-clear the 3D view every frame and softgles2
         * swaps front/back at present, so any pixel a surface never writes (e.g. the
         * sky opening the skybox stage fails to rasterise) would otherwise show raw
         * heap/stale junk as high-frequency noise. Zeroing makes such regions black. */
        if (fb->color) memset(fb->color, 0, n * sizeof(uint32_t));
    }
    if (flags & SGL_FB_DEPTH)
        fb->depth = (float *)sgl_alloc_aligned_bytes(n * sizeof(float), SGL_FB_ALIGN, &fb->depth_alloc);
    if (flags & SGL_FB_STENCIL)
        fb->stencil = (uint8_t *)sgl_alloc_aligned_bytes(n * sizeof(uint8_t), SGL_FB_ALIGN, &fb->stencil_alloc);
    if (((flags & SGL_FB_COLOR) && !fb->color) ||
        ((flags & SGL_FB_DEPTH) && !fb->depth) ||
        ((flags & SGL_FB_STENCIL) && !fb->stencil)) {
        sgl_fb_destroy(fb);
        return 0;
    }
    return fb;
}

sgl_framebuffer *sgl_fb_create(int w, int h)
{
    return sgl_fb_create_ex(w, h, SGL_FB_DEFAULT);
}

void sgl_fb_destroy(sgl_framebuffer *fb)
{
    if (!fb) return;
    if (!fb->color_external)
        free(fb->color_alloc ? fb->color_alloc : fb->color);
    free(fb->depth_alloc ? fb->depth_alloc : fb->depth);
    free(fb->stencil_alloc ? fb->stencil_alloc : fb->stencil);
    free(fb);
}

void sgl_fb_swap_storage(sgl_framebuffer *a, sgl_framebuffer *b)
{
    if (!a || !b || a->w != b->w || a->h != b->h) return;
    uint32_t *color = a->color; a->color = b->color; b->color = color;
    float *depth = a->depth; a->depth = b->depth; b->depth = depth;
    uint8_t *stencil = a->stencil; a->stencil = b->stencil; b->stencil = stencil;
    void *owner = a->color_alloc; a->color_alloc = b->color_alloc; b->color_alloc = owner;
    owner = a->depth_alloc; a->depth_alloc = b->depth_alloc; b->depth_alloc = owner;
    owner = a->stencil_alloc; a->stencil_alloc = b->stencil_alloc; b->stencil_alloc = owner;
    uint8_t ext = a->color_external; a->color_external = b->color_external; b->color_external = ext;
    uint32_t cc = a->clear_color; a->clear_color = b->clear_color; b->clear_color = cc;
    float cd = a->clear_depth; a->clear_depth = b->clear_depth; b->clear_depth = cd;
    uint8_t cs = a->clear_stencil; a->clear_stencil = b->clear_stencil; b->clear_stencil = cs;
    uint8_t cv = a->clear_valid; a->clear_valid = b->clear_valid; b->clear_valid = cv;
}

void sgl_fb_bind_external_color(sgl_framebuffer *fb, uint32_t *color)
{
    if (!fb) return;
    if (!fb->color_external)
        free(fb->color_alloc ? fb->color_alloc : fb->color);
    fb->color = color;
    fb->color_alloc = 0;
    fb->color_external = color ? 1u : 0u;
    fb->clear_valid = 0;
}

void sgl_present_copy(sgl_framebuffer *dst, const sgl_framebuffer *src)
{
    if (!dst || !src || !dst->color || !src->color || dst->w != src->w || dst->h != src->h) return;
    size_t n = (size_t)dst->w * (size_t)dst->h;
#if SG_FB_RVV
    for (size_t i = 0; i < n; ) {
        size_t vl = __riscv_vsetvl_e32m8(n - i);
        __riscv_vse32_v_u32m8(&dst->color[i], __riscv_vle32_v_u32m8(&src->color[i], vl), vl);
        i += vl;
    }
#else
    memcpy(dst->color, src->color, n * sizeof(uint32_t));
#endif
    dst->clear_color = src->clear_color;
    dst->clear_depth = src->clear_depth;
    dst->clear_stencil = src->clear_stencil;
    dst->clear_valid = src->clear_valid;
}

static void sgl_copy_u32(uint32_t *dst, const uint32_t *src, size_t n)
{
#if SG_FB_RVV
    for (size_t i = 0; i < n; ) {
        size_t vl = __riscv_vsetvl_e32m8(n - i);
        __riscv_vse32_v_u32m8(&dst[i], __riscv_vle32_v_u32m8(&src[i], vl), vl);
        i += vl;
    }
#else
    memcpy(dst, src, n * sizeof(uint32_t));
#endif
}

static void sgl_blit_rgba32_rect(sgl_framebuffer *dst, const uint8_t *src, int pitch,
                                 int src_w, int src_h, int rx, int ry, int rw, int rh)
{
    if (!dst || !dst->color || !src || pitch <= 0 || rw <= 0 || rh <= 0) return;
    for (int y = 0; y < rh; ++y) {
        int sy = ry + y;
        if (sy < 0 || sy >= src_h || sy >= dst->h) continue;
        int x0 = rx;
        int x1 = rx + rw;
        if (x0 < 0) x0 = 0;
        if (x1 > src_w) x1 = src_w;
        if (x1 > dst->w) x1 = dst->w;
        if (x1 <= x0) continue;
        const uint32_t *srow = (const uint32_t *)(src + (size_t)sy * (size_t)pitch);
        sgl_copy_u32(&dst->color[(size_t)sy * (size_t)dst->w + (size_t)x0], &srow[x0], (size_t)(x1 - x0));
    }
}

void sgl_blit_rgba32(sgl_framebuffer *dst, const void *pixels, int pitch,
                     int w, int h, const int *rects, int numrects)
{
    const uint8_t *src = (const uint8_t *)pixels;
    if (!dst || !dst->color || !src || pitch <= 0 || w <= 0 || h <= 0) return;
    if (rects && numrects > 0) {
        for (int r = 0; r < numrects; ++r) {
            sgl_blit_rgba32_rect(dst, src, pitch, w, h,
                                 rects[r * 4 + 0], rects[r * 4 + 1],
                                 rects[r * 4 + 2], rects[r * 4 + 3]);
        }
    } else {
        sgl_blit_rgba32_rect(dst, src, pitch, w, h, 0, 0, w, h);
    }
    dst->clear_valid = 0;
}

static int sgl_fb_can_swap_storage(const sgl_framebuffer *a, const sgl_framebuffer *b)
{
    return a && b &&
           a->w == b->w && a->h == b->h &&
           !!a->color == !!b->color &&
           !!a->depth == !!b->depth &&
           !!a->stencil == !!b->stencil &&
           (!a->color || (a->color_alloc && b->color_alloc)) &&
           (!a->depth || (a->depth_alloc && b->depth_alloc)) &&
           (!a->stencil || (a->stencil_alloc && b->stencil_alloc));
}

int sgl_present(sgl_framebuffer *front, sgl_framebuffer *back, int mode)
{
    /* Frame boundary: drain any deferred (FrameBin) commands to the framebuffer
     * BEFORE reclaiming the arena, then reset this frame's transient draw
     * scratch (pv/vb/idx). Present only touches framebuffers, never the arena. */
    sg_framebin_flush();
    sg_frame_reset();
    if (!front || !back || front->w != back->w || front->h != back->h)
        return 0;
    if (mode == SGL_PRESENT_AUTO) {
        mode = sgl_fb_can_swap_storage(front, back) ? SGL_PRESENT_SWAP : SGL_PRESENT_COPY_RGBA32;
    }
    if (mode == SGL_PRESENT_SWAP) {
        if (!sgl_fb_can_swap_storage(front, back))
            return 0;
        sgl_fb_swap_storage(front, back);
        return 1;
    }
    if (mode == SGL_PRESENT_COPY_RGBA32) {
        if (!front->color || !back->color)
            return 0;
        sgl_present_copy(front, back);
        return 1;
    }
    return 0;
}

uint32_t sgl_pixel(const sgl_framebuffer *fb, int x, int y)
{
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return 0;
    return fb->color[y * fb->w + x];
}

uint32_t sgl_fnv1a(const sgl_framebuffer *fb)
{
    const unsigned char *p = (const unsigned char *)fb->color;
    size_t n = (size_t)fb->w * fb->h * sizeof(uint32_t);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* FNV-1a over RGB only — alpha byte masked out. Lets the golden gate prove an
 * alpha-channel change (e.g. storing real framebuffer alpha) left RGB intact. */
uint32_t sgl_fnv1a_rgb(const sgl_framebuffer *fb)
{
    const uint32_t *p = fb->color;
    size_t n = (size_t)fb->w * fb->h;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        uint32_t w = p[i];
        h ^= (unsigned char)(w);        h *= 16777619u;
        h ^= (unsigned char)(w >> 8);   h *= 16777619u;
        h ^= (unsigned char)(w >> 16);  h *= 16777619u;
    }
    return h;
}


void sgl_dump_ascii(const sgl_framebuffer *fb, int cols, int rows)
{
    static const char ramp[] = " .:-=+*#%@";
    const int levels = (int)sizeof(ramp) - 2;

    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            int x0 = (int)((long)cx * fb->w / cols);
            int x1 = (int)((long)(cx + 1) * fb->w / cols);
            int y0 = (int)((long)cy * fb->h / rows);
            int y1 = (int)((long)(cy + 1) * fb->h / rows);
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;

            unsigned long sum = 0, cnt = 0;
            for (int y = y0; y < y1 && y < fb->h; ++y)
                for (int x = x0; x < x1 && x < fb->w; ++x) {
                    uint32_t c = fb->color[y * fb->w + x];
                    unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
                    sum += (r * 30 + g * 59 + b * 11) / 100;
                    ++cnt;
                }
            unsigned luma = cnt ? (unsigned)(sum / cnt) : 0;
            int idx = (int)((luma * levels) / 255);
            if (idx > levels) idx = levels;
            putchar(ramp[idx]);
        }
        putchar('\n');
    }
}

/* Box-downsample an s*s block average from a render-res RGBA8 buffer (src,
 * sw*sh, little-endian 0xAABBGGRR) into an output-res buffer (dst, dw*dh).
 * dw*s must equal the covered width; edge blocks are clamped to src bounds. */
void sgl_downsample_rgba_box(const uint32_t *src, int sw, int sh,
                                uint32_t *dst, int dw, int dh, int s)
{
#if SG_FB_RVV
    /* Four 8-bit samples fit in each 16-bit half of these packed sums.
     * Average R/B and G/A together, preserving scalar round-to-nearest. */
    if (s == 2 && sw == dw * 2 && sh == dh * 2) {
        const uint32_t mask = 0x00ff00ffu;
        for (int y = 0; y < dh; ++y) {
            const uint32_t *top = src + (size_t)(y * 2) * sw;
            const uint32_t *bottom = top + sw;
            for (int x = 0; x < dw;) {
                size_t vl = __riscv_vsetvl_e32m4((size_t)(dw - x));
                vuint32m4x2_t t = __riscv_vlseg2e32_v_u32m4x2(top + x * 2, vl);
                vuint32m4x2_t b = __riscv_vlseg2e32_v_u32m4x2(bottom + x * 2, vl);
                vuint32m4_t p0 = __riscv_vget_v_u32m4x2_u32m4(t, 0);
                vuint32m4_t p1 = __riscv_vget_v_u32m4x2_u32m4(t, 1);
                vuint32m4_t p2 = __riscv_vget_v_u32m4x2_u32m4(b, 0);
                vuint32m4_t p3 = __riscv_vget_v_u32m4x2_u32m4(b, 1);
                vuint32m4_t result = __riscv_vmv_v_x_u32m4(0, vl);
                for (int shift = 0; shift <= 8; shift += 8) {
#define CH(P) __riscv_vand_vx_u32m4(__riscv_vsrl_vx_u32m4((P), shift, vl), mask, vl)
                    vuint32m4_t sum = __riscv_vadd_vv_u32m4(
                        __riscv_vadd_vv_u32m4(CH(p0), CH(p1), vl),
                        __riscv_vadd_vv_u32m4(CH(p2), CH(p3), vl), vl);
#undef CH
                    sum = __riscv_vand_vx_u32m4(__riscv_vsrl_vx_u32m4(
                        __riscv_vadd_vx_u32m4(sum, 0x00020002u, vl), 2, vl), mask, vl);
                    result = __riscv_vor_vv_u32m4(result,
                        __riscv_vsll_vx_u32m4(sum, shift, vl), vl);
                }
                __riscv_vse32_v_u32m4(dst + (size_t)y * dw + x, result, vl);
                x += (int)vl;
            }
        }
        return;
    }
#endif
    const uint32_t inv = (uint32_t)(s * s);
    int oy;
    for (oy = 0; oy < dh; ++oy) {
        int by0 = oy * s;
        int ox;
        for (ox = 0; ox < dw; ++ox) {
            int bx0 = ox * s;
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int jy;
            for (jy = 0; jy < s; ++jy) {
                int sy = by0 + jy;
                const uint32_t *row;
                int jx;
                if (sy >= sh) sy = sh - 1;
                row = src + (size_t)sy * sw;
                for (jx = 0; jx < s; ++jx) {
                    int sx = bx0 + jx;
                    uint32_t px;
                    if (sx >= sw) sx = sw - 1;
                    px = row[sx];
                    r += (px      ) & 0xffu;
                    g += (px >>  8) & 0xffu;
                    b += (px >> 16) & 0xffu;
                    a += (px >> 24) & 0xffu;
                }
            }
            r = (r + inv / 2) / inv;
            g = (g + inv / 2) / inv;
            b = (b + inv / 2) / inv;
            a = (a + inv / 2) / inv;
            dst[(size_t)oy * dw + ox] =
                r | (g << 8) | (b << 16) | (a << 24);
        }
    }
}
