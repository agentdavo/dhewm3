/* Texture object management.
 *
 * Owns the g_textures[] array, the DXT software decoder, and all texel upload
 * / readback entry points.  Exposes helpers the FBO and draw modules need.
 */
#ifndef SOFTGLES2_SG_TEXTURE_H
#define SOFTGLES2_SG_TEXTURE_H

#include "sgl_gl.h"
#include "sg_core.h"
#include <stdint.h>
#include <stddef.h>

/* Doom 3 keeps roughly 2,100 image objects resident in Mars City alone.  The
 * old 256-entry table made glGenTextures silently return name 0 after 255,
 * leaving most interaction-light sampler units unbound and the world black.
 * The descriptors contain pointers, not texel storage, so 4096 is inexpensive
 * relative to the uploaded image data and still keeps constant-time lookup. */
#define MAX_TEXTURES 4096

typedef struct {
    sgl_tex tex;
    sgl_tex cube[6];
    uint8_t cube_mask;
    /* 0 = uncompressed, 1..4 = S3TC enum offset. Keep format metadata out of
     * the hot sampler view, which is copied for mip selection. */
    uint8_t compressed_format[7][SGL_MAX_MIP];
    GLenum target;
    int used;
} tex_obj;

extern tex_obj g_textures[MAX_TEXTURES];

/* Shared helpers (needed by sg_framebuffer and sg_draw). */
int  cube_face_index(GLenum target);
int  tex_target_ok(GLenum target);
sgl_tex *tex_for_target(GLuint id, GLenum target);
int  tex_has_storage(const tex_obj *o);
size_t unpack_stride(GLsizei w, int comps);
size_t pack_stride(GLsizei w, int bytes_per_pixel);

/* GL texture API (declared public in sgl_gl.h). */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
GLboolean glIsTexture(GLuint texture);
void glActiveTexture(GLenum texture);
void glBindTexture(GLenum target, GLuint texture);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
void glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
void glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
void glPixelStorei(GLenum pname, GLint param);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const void *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei w, GLsizei h, GLenum format, GLenum type,
                     const void *pixels);
void glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                            GLsizei w, GLsizei h, GLint border,
                            GLsizei imageSize, const void *data);
void glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                               GLsizei w, GLsizei h, GLenum format,
                               GLsizei imageSize, const void *data);
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei w, GLsizei h, GLint border);
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei w, GLsizei h);
void glGenerateMipmap(GLenum target);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, void *pixels);

#endif /* SOFTGLES2_SG_TEXTURE_H */
