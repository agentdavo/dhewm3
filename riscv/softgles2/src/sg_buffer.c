#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "sg_state.h"
#include "sg_buffer.h"

buf_obj g_buffers[MAX_BUFFERS];

void glGenBuffers(GLsizei n, GLuint *buffers)
{
    for (GLsizei k = 0; k < n; ++k) {
        buffers[k] = 0;
        for (GLuint i = 1; i < MAX_BUFFERS; ++i)
            if (!g_buffers[i].used) { g_buffers[i].used = 1; g_buffers[i].data = 0; g_buffers[i].size = 0; g_buffers[i].usage = GL_STATIC_DRAW; buffers[k] = i; break; }
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers)
{
    for (GLsizei k = 0; k < n; ++k) {
        GLuint b = buffers[k];
        if (b && b < MAX_BUFFERS && g_buffers[b].used) { free(g_buffers[b].data); g_buffers[b].data = 0; g_buffers[b].size = 0; g_buffers[b].usage = GL_STATIC_DRAW; g_buffers[b].used = 0; }
    }
}

GLboolean glIsBuffer(GLuint buffer)
{
    return (buffer && buffer < MAX_BUFFERS && g_buffers[buffer].used) ? GL_TRUE : GL_FALSE;
}

void glBindBuffer(GLenum target, GLuint buffer)
{
    GLTRACE("[bindbuf] target=0x%x buf=%u\n", target, buffer);
    if (target == GL_ARRAY_BUFFER) C.array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) C.element_buffer = buffer;
    else set_err(GL_INVALID_ENUM);
}

void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    (void)usage;
    GLTRACE("[bufdata] target=0x%x size=%ld arraybuf=%u\n", target, (long)size, C.array_buffer);
    GLuint b = (target == GL_ARRAY_BUFFER) ? C.array_buffer : C.element_buffer;
    if (!b || b >= MAX_BUFFERS) { set_err(GL_INVALID_OPERATION); return; }
    free(g_buffers[b].data);
    g_buffers[b].data = (uint8_t *)malloc((size_t)size);
    g_buffers[b].size = (size_t)size;
    g_buffers[b].usage = usage;
    if (g_buffers[b].data && data) memcpy(g_buffers[b].data, data, (size_t)size);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    GLuint b = (target == GL_ARRAY_BUFFER) ? C.array_buffer : C.element_buffer;
    if (!b || b >= MAX_BUFFERS || !g_buffers[b].data) { set_err(GL_INVALID_OPERATION); return; }
    if ((size_t)(offset + size) <= g_buffers[b].size && data)
        memcpy(g_buffers[b].data + offset, data, (size_t)size);
}

void glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params)
{
    if (!params) return;
    GLuint b;
    if (target == GL_ARRAY_BUFFER) b = C.array_buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) b = C.element_buffer;
    else { set_err(GL_INVALID_ENUM); *params = 0; return; }
    if (!b || b >= MAX_BUFFERS || !g_buffers[b].used) { *params = 0; return; }
    if (pname == GL_BUFFER_SIZE) *params = (GLint)g_buffers[b].size;
    else if (pname == GL_BUFFER_USAGE) *params = (GLint)g_buffers[b].usage;
    else *params = 0;
}
