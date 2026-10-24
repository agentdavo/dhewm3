/*
 * softgles2 — software rasteriser core (v0).
 *
 * The seed of the bare-metal OpenGL ES 2.0 renderer (see ROADMAP Phase 3).
 * This v0 exposes a tiny, GL-flavoured API: a clip-space vertex with colour,
 * indexed/array triangle drawing, an RGBA8 + float-depth framebuffer, and an
 * RVV-accelerated rasteriser. It builds both freestanding (RISC-V target,
 * define SGL_BAREMETAL) and hosted (for golden-image generation).
 */
#ifndef SOFTGLES2_SGL_H
#define SOFTGLES2_SGL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int       w, h;
    uint32_t *color;   /* RGBA8, 0xAABBGGRR, w*h           */
    float    *depth;   /* w*h, 1.0 = far                   */
    uint8_t  *stencil; /* w*h, 8-bit stencil               */
    void     *color_alloc, *depth_alloc, *stencil_alloc; /* allocation owners */
    uint8_t   color_external; /* color is borrowed, not freed by sgl_fb_destroy */
    uint32_t  clear_color;
    float     clear_depth;
    uint8_t   clear_stencil;
    uint8_t   clear_valid; /* clear metadata is advisory; buffers are materialised */
} sgl_framebuffer;

enum {
    SGL_PRESENT_AUTO = 0,
    SGL_PRESENT_SWAP = 1,
    SGL_PRESENT_COPY_RGBA32 = 2
};

enum {
    SGL_FB_COLOR   = 1 << 0,
    SGL_FB_DEPTH   = 1 << 1,
    SGL_FB_STENCIL = 1 << 2,
    SGL_FB_DEFAULT = SGL_FB_COLOR | SGL_FB_DEPTH | SGL_FB_STENCIL
};

/* Depth comparison functions (match GL enum values). Threaded into the raster
 * draw state so the lit-world multi-pass renderer works: the depth-fill pass
 * uses LEQUAL, the additive interaction (lighting) passes use EQUAL. Without a
 * real depth-func the EQUAL passes fail the hardcoded LESS test and the world
 * stays unlit/black. */
enum {
    SGL_DEPTH_NEVER    = 0x0200,
    SGL_DEPTH_LESS     = 0x0201,
    SGL_DEPTH_EQUAL    = 0x0202,
    SGL_DEPTH_LEQUAL   = 0x0203,
    SGL_DEPTH_GREATER  = 0x0204,
    SGL_DEPTH_NOTEQUAL = 0x0205,
    SGL_DEPTH_GEQUAL   = 0x0206,
    SGL_DEPTH_ALWAYS   = 0x0207
};

/* Clip-space vertex with a vertex colour (the only attribute in v0). */
typedef struct {
    float x, y, z, w;  /* clip-space position              */
    float r, g, b, a;  /* vertex colour, 0..1              */
} sgl_vertex;

sgl_framebuffer *sgl_fb_create(int w, int h);
sgl_framebuffer *sgl_fb_create_ex(int w, int h, unsigned flags);
void             sgl_fb_destroy(sgl_framebuffer *fb);
void             sgl_fb_swap_storage(sgl_framebuffer *a, sgl_framebuffer *b);
void             sgl_fb_bind_external_color(sgl_framebuffer *fb, uint32_t *color);
void             sgl_present_copy(sgl_framebuffer *dst, const sgl_framebuffer *src);
int              sgl_present(sgl_framebuffer *front, sgl_framebuffer *back, int mode);
void             sgl_blit_rgba32(sgl_framebuffer *dst, const void *pixels, int pitch,
                                  int w, int h, const int *rects, int numrects);
int              sgl_diag_present_report(const sgl_framebuffer *fb, int capture_armed);
void             sgl_diag_draw_filter_set(int enabled, int frame_first, int frame_last,
                                          int draw_first, int draw_last, int detail);

/* Clear colour (0xAABBGGRR) and depth (RVV-accelerated). */
void sgl_clear(sgl_framebuffer *fb, uint32_t rgba, float depth);

/* Draw triangles from a vertex array (count must be a multiple of 3).
 * Pipeline: perspective divide -> viewport -> edge-function rasterise with
 * perspective-correct colour interpolation, depth test, opaque write. */
void sgl_draw_triangles(sgl_framebuffer *fb, const sgl_vertex *verts, int count);

/* Verification helpers (no file I/O, work on host and bare metal). */
uint32_t sgl_fnv1a(const sgl_framebuffer *fb);                  /* checksum   */
uint32_t sgl_fnv1a_rgb(const sgl_framebuffer *fb);             /* RGB-only checksum (alpha masked) */
void     sgl_dump_ascii(const sgl_framebuffer *fb, int cols, int rows);
uint32_t sgl_pixel(const sgl_framebuffer *fb, int x, int y);

/* 1 if this build rasterises with RVV intrinsics, 0 if scalar fallback. */
int sgl_rvv_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* SOFTGLES2_SGL_H */
