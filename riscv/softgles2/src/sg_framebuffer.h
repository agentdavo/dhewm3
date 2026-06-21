/* Framebuffer / renderbuffer object management.
 *
 * Owns the g_framebuffers[] and g_renderbuffers[] arrays, implements the FBO
 * and renderbuffer entry points, and updates C.fb through the shared context.
 */
#ifndef SOFTGLES2_SG_FRAMEBUFFER_H
#define SOFTGLES2_SG_FRAMEBUFFER_H

#include "sgl_gl.h"
#include "sg_core.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_FRAMEBUFFERS  64
#define MAX_RENDERBUFFERS 64

typedef struct {
    int used;
    GLenum internalformat;
    GLsizei w, h;
    uint32_t *color;
    float *depth;
    uint8_t *stencil;
    void *color_alloc, *depth_alloc, *stencil_alloc;
} rb_obj;
typedef struct {
    int used;
    GLuint color_tex, depth_rb, stencil_rb;
    GLenum color_target;
    GLint color_level;
    sgl_framebuffer view;
} fb_obj;

extern fb_obj g_framebuffers[MAX_FRAMEBUFFERS];
extern rb_obj g_renderbuffers[MAX_RENDERBUFFERS];

/* GL FBO API (declared public in sgl_gl.h). */
void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
GLboolean glIsFramebuffer(GLuint framebuffer);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
GLenum glCheckFramebufferStatus(GLenum target);
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                            GLuint texture, GLint level);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer);
void glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                           GLenum pname, GLint *params);
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
GLboolean glIsRenderbuffer(GLuint renderbuffer);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void glRenderbufferStorage(GLenum target, GLenum internalformat,
                           GLsizei width, GLsizei height);
void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params);

#endif /* SOFTGLES2_SG_FRAMEBUFFER_H */
