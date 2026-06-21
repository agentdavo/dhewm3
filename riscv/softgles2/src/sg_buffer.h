/* Buffer object management.
 *
 * Owns the g_buffers[] array and implements the glGen/Delete/Is/Bind/BufferData
 * family.  Included by sg_draw so the draw path can resolve vertex data from
 * bound buffers via the extern array.
 */
#ifndef SOFTGLES2_SG_BUFFER_H
#define SOFTGLES2_SG_BUFFER_H

#include "sgl_gl.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_BUFFERS 256

typedef struct { uint8_t *data; size_t size; GLenum usage; int used; } buf_obj;

extern buf_obj g_buffers[MAX_BUFFERS];

void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
GLboolean glIsBuffer(GLuint buffer);
void glBindBuffer(GLenum target, GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);

#endif /* SOFTGLES2_SG_BUFFER_H */
