/* Framebuffer allocation and verification helpers.
 *
 * Owns sgl_fb_create/destroy, checksum, and the ASCII-art console dump.
 * Declarations duplicated from sgl.h; this header is the internal module
 * contract so sg_fb.c can be compiled standalone.
 */
#ifndef SOFTGLES2_SG_FB_H
#define SOFTGLES2_SG_FB_H

#include "sgl.h"

sgl_framebuffer *sgl_fb_create(int w, int h);
sgl_framebuffer *sgl_fb_create_ex(int w, int h, unsigned flags);
void             sgl_fb_destroy(sgl_framebuffer *fb);
void             sgl_fb_swap_storage(sgl_framebuffer *a, sgl_framebuffer *b);
void             sgl_present_copy(sgl_framebuffer *dst, const sgl_framebuffer *src);
int              sgl_present(sgl_framebuffer *front, sgl_framebuffer *back, int mode);
void             sgl_blit_rgba32(sgl_framebuffer *dst, const void *pixels, int pitch,
                                  int w, int h, const int *rects, int numrects);
void            *sgl_alloc_aligned_bytes(size_t n, size_t align, void **owner);
uint32_t         sgl_pixel(const sgl_framebuffer *fb, int x, int y);
uint32_t         sgl_fnv1a(const sgl_framebuffer *fb);
uint32_t         sgl_fnv1a_rgb(const sgl_framebuffer *fb);   /* RGB only (alpha byte masked) */
void             sgl_dump_ascii(const sgl_framebuffer *fb, int cols, int rows);

#endif /* SOFTGLES2_SG_FB_H */
