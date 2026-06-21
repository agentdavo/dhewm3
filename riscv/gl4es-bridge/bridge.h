/* gl4es <-> softgles2 bridge: bring up gl4es against a softgles2 surface. */
#ifndef SGL_BRIDGE_H
#define SGL_BRIDGE_H

#include "sgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* softgles2's GLES2 proc lookup for gl4es's set_getprocaddress(). */
void *sgl2_getprocaddress(const char *name);

/* Bind a softgles2 framebuffer as the current surface and initialise gl4es to
 * drive it. Call once before issuing desktop-GL calls. */
void  sgl_bridge_init(sgl_framebuffer *fb);

#ifdef __cplusplus
}
#endif

#endif /* SGL_BRIDGE_H */
