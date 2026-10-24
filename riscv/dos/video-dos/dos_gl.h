/*
 * dos_gl.h — the dOS surface functions the SDL3 "dos" video driver calls.
 *
 * Implemented in the payload (riscv/dos, alongside gl4es + softgles2), NOT in
 * libSDL3 — so libSDL3 never includes softgles2/gl4es headers. The driver only
 * forwards function pointers to these.
 *
 * Naming convention:
 *   DOS_*  = static SDL video-driver callbacks inside SDL_dosvideo.c.
 *   dos_*  = this C ABI boundary, implemented by the payload.
 */
#ifndef DOS_GL_H
#define DOS_GL_H

#ifdef __cplusplus
extern "C" {
#endif

/* GL symbol resolver — provided by gl4es (gl4es_GetProcAddress). */
void *gl4es_GetProcAddress(const char *name);

/* GL context lifecycle. dos_gl_create_context creates a softgles2 framebuffer
 * of (w,h), binds it as gl4es's current surface (sgl_bridge_init), and returns
 * an opaque context token (non-NULL on success). It leaves the context current.
 * SDL assumes creation makes the context current. */
void *dos_gl_create_context(int w, int h);
int   dos_gl_make_current(void *ctx);       /* 0 ok; NULL = release (no-op) */

/* Supersampling factor (1..4). Call before dos_gl_create_context. When >1 the
 * (w,h) passed to create_context is treated as the supersampled render size and
 * the scanout is downsampled to (w/s, h/s) at present. Defaults to BM_SSAA. */
void  dos_gl_set_ssaa(int s);
void  dos_gl_destroy_context(void *ctx);

/* Present the current GL surface: scan it out to the virtio-gpu device. Capture
 * is host-side via the QEMU monitor `screendump`. 0 on success. */
int   dos_gl_present(void *ctx);

/* Software (non-GL) blit straight to the dOS framebuffer sink.
 * rects is numrects quadruples of (x,y,w,h) ints, or NULL for full surface.
 * pixels is XRGB8888, pitch in bytes. 0 on success. */
int   dos_fb_blit(const void *pixels, int pitch, int w, int h,
                  const int *rects, int numrects);

/* Framebuffer geometry for VideoInit's SDL_AddBasicVideoDisplay. */
void  dos_fb_get_size(int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* DOS_GL_H */
