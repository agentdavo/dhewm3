/*
 * gl4es <-> softgles2 bridge.
 *
 * Wires gl4es's GLES2 driver callback to the softgles2 backend: gl4es asks for
 * GLES2 entry points by name via set_getprocaddress(); we return the matching
 * sgl2_* function (softgles2 compiled with sgl2_names.h). Unimplemented GLES2
 * functions resolve to a no-op so gl4es never jumps through a NULL pointer
 * during a path softgles2 does not yet cover.
 */
#include "sgl2_names.h"     /* renames the gl* prototypes below to sgl2_*      */
#include "sgl.h"
#include "sgl_gl.h"
#include "gl4esinit.h"      /* from riscv/gl4es/include                        */
#include "bridge.h"

#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <string.h>
#  include <stdio.h>
#  include <stdlib.h>
#endif

static sgl_framebuffer *g_fb;

static void getfbsize(int *w, int *h) { if (g_fb) { *w = g_fb->w; *h = g_fb->h; } else { *w = 0; *h = 0; } }

/* Fallback for GLES2 entry points softgles2 does not implement yet. */
static void sgl2_noop(void) { }

static void bridge_trace_impl(const char *name)
{
#if !defined(SGL_BAREMETAL)
    if (getenv("BRIDGE_TRACE")) fprintf(stderr, "[proc] %-26s -> impl\n", name);
#else
    (void)name;
#endif
}

static void bridge_trace_stub(const char *name)
{
    enum { MAX_STUB_LOG = 64, MAX_STUB_NAME = 48 };
    static char seen[MAX_STUB_LOG][MAX_STUB_NAME];
    static unsigned nseen;
    if (!name) return;
    for (unsigned i = 0; i < nseen; ++i)
        if (!strcmp(seen[i], name)) return;
    if (nseen < MAX_STUB_LOG) {
        unsigned j = 0;
        for (; j + 1 < MAX_STUB_NAME && name[j]; ++j) seen[nseen][j] = name[j];
        seen[nseen][j] = 0;
        ++nseen;
    }
#if defined(SGL_BAREMETAL)
    printf("gl4es-bridge: STUB %s\n", name);
#else
    if (getenv("BRIDGE_TRACE")) fprintf(stderr, "[proc] %-26s -> STUB\n", name);
#endif
}

typedef struct { const char *name; void *fn; } proc_entry;

/* identifiers here are rewritten to sgl2_* by sgl2_names.h; the name strings
 * are the real GLES2 names gl4es will ask for. */
static const proc_entry g_table[] = {
    { "glViewport",                 (void *)glViewport },
    { "glClearColor",               (void *)glClearColor },
    { "glClearDepthf",              (void *)glClearDepthf },
    { "glClearStencil",             (void *)glClearStencil },
    { "glClear",                    (void *)glClear },
    { "glEnable",                   (void *)glEnable },
    { "glDisable",                  (void *)glDisable },
    { "glIsEnabled",                (void *)glIsEnabled },
    { "glDepthFunc",                (void *)glDepthFunc },
    { "glDepthMask",                (void *)glDepthMask },
    { "glDepthRangef",              (void *)glDepthRangef },
    { "glDepthBoundsEXT",           (void *)glDepthBoundsEXT },
    { "glColorMask",                (void *)glColorMask },
    { "glStencilFunc",              (void *)glStencilFunc },
    { "glStencilFuncSeparate",      (void *)glStencilFuncSeparate },
    { "glStencilMask",              (void *)glStencilMask },
    { "glStencilMaskSeparate",      (void *)glStencilMaskSeparate },
    { "glStencilOp",                (void *)glStencilOp },
    { "glStencilOpSeparate",        (void *)glStencilOpSeparate },
    { "glScissor",                  (void *)glScissor },
    { "glPolygonOffset",            (void *)glPolygonOffset },
    { "glBlendFunc",                (void *)glBlendFunc },
    { "glBlendFuncSeparate",        (void *)glBlendFuncSeparate },
    { "glBlendEquation",            (void *)glBlendEquation },
    { "glBlendEquationSeparate",    (void *)glBlendEquationSeparate },
    { "glBlendColor",               (void *)glBlendColor },
    { "glCullFace",                 (void *)glCullFace },
    { "glFrontFace",                (void *)glFrontFace },
    { "glGetError",                 (void *)glGetError },
    { "glGetString",                (void *)glGetString },
    { "glGetIntegerv",              (void *)glGetIntegerv },
    { "glGetFloatv",                (void *)glGetFloatv },
    { "glGetBooleanv",              (void *)glGetBooleanv },
    { "glGenBuffers",               (void *)glGenBuffers },
    { "glDeleteBuffers",            (void *)glDeleteBuffers },
    { "glIsBuffer",                 (void *)glIsBuffer },
    { "glBindBuffer",               (void *)glBindBuffer },
    { "glBufferData",               (void *)glBufferData },
    { "glBufferSubData",            (void *)glBufferSubData },
    { "glGetBufferParameteriv",     (void *)glGetBufferParameteriv },
    { "glGenFramebuffers",          (void *)glGenFramebuffers },
    { "glDeleteFramebuffers",       (void *)glDeleteFramebuffers },
    { "glIsFramebuffer",            (void *)glIsFramebuffer },
    { "glBindFramebuffer",          (void *)glBindFramebuffer },
    { "glCheckFramebufferStatus",   (void *)glCheckFramebufferStatus },
    { "glFramebufferTexture2D",     (void *)glFramebufferTexture2D },
    { "glFramebufferRenderbuffer",  (void *)glFramebufferRenderbuffer },
    { "glGetFramebufferAttachmentParameteriv", (void *)glGetFramebufferAttachmentParameteriv },
    { "glGenRenderbuffers",         (void *)glGenRenderbuffers },
    { "glDeleteRenderbuffers",      (void *)glDeleteRenderbuffers },
    { "glIsRenderbuffer",           (void *)glIsRenderbuffer },
    { "glBindRenderbuffer",         (void *)glBindRenderbuffer },
    { "glRenderbufferStorage",      (void *)glRenderbufferStorage },
    { "glGetRenderbufferParameteriv", (void *)glGetRenderbufferParameteriv },
    { "glEnableVertexAttribArray",  (void *)glEnableVertexAttribArray },
    { "glDisableVertexAttribArray", (void *)glDisableVertexAttribArray },
    { "glVertexAttribPointer",      (void *)glVertexAttribPointer },
    { "glVertexAttribIPointer",     (void *)glVertexAttribIPointer },
    { "glVertexAttrib1f",           (void *)glVertexAttrib1f },
    { "glVertexAttrib2f",           (void *)glVertexAttrib2f },
    { "glVertexAttrib3f",           (void *)glVertexAttrib3f },
    { "glVertexAttrib4f",           (void *)glVertexAttrib4f },
    { "glVertexAttrib1fv",          (void *)glVertexAttrib1fv },
    { "glVertexAttrib2fv",          (void *)glVertexAttrib2fv },
    { "glVertexAttrib3fv",          (void *)glVertexAttrib3fv },
    { "glVertexAttrib4fv",          (void *)glVertexAttrib4fv },
    { "glGenTextures",              (void *)glGenTextures },
    { "glDeleteTextures",           (void *)glDeleteTextures },
    { "glIsTexture",                (void *)glIsTexture },
    { "glActiveTexture",            (void *)glActiveTexture },
    { "glBindTexture",              (void *)glBindTexture },
    { "glTexParameteri",            (void *)glTexParameteri },
    { "glTexParameterf",            (void *)glTexParameterf },
    { "glTexParameteriv",           (void *)glTexParameteriv },
    { "glTexParameterfv",           (void *)glTexParameterfv },
    { "glGetTexParameteriv",        (void *)glGetTexParameteriv },
    { "glGetTexParameterfv",        (void *)glGetTexParameterfv },
    { "glPixelStorei",              (void *)glPixelStorei },
    { "glTexImage2D",               (void *)glTexImage2D },
    { "glTexSubImage2D",            (void *)glTexSubImage2D },
    { "glCompressedTexImage2D",     (void *)glCompressedTexImage2D },
    { "glCompressedTexSubImage2D",  (void *)glCompressedTexSubImage2D },
    { "glCopyTexImage2D",           (void *)glCopyTexImage2D },
    { "glCopyTexSubImage2D",        (void *)glCopyTexSubImage2D },
    { "glGenerateMipmap",           (void *)glGenerateMipmap },
    { "glReadPixels",               (void *)glReadPixels },
    { "glCreateShader",             (void *)glCreateShader },
    { "glDeleteShader",             (void *)glDeleteShader },
    { "glIsShader",                 (void *)glIsShader },
    { "glShaderSource",             (void *)glShaderSource },
    { "glCompileShader",            (void *)glCompileShader },
    { "glGetShaderiv",              (void *)glGetShaderiv },
    { "glGetShaderInfoLog",         (void *)glGetShaderInfoLog },
    { "glGetShaderSource",          (void *)glGetShaderSource },
    { "glGetShaderPrecisionFormat", (void *)glGetShaderPrecisionFormat },
    { "glReleaseShaderCompiler",    (void *)glReleaseShaderCompiler },
    { "glShaderBinary",             (void *)glShaderBinary },
    { "glCreateProgram",            (void *)glCreateProgram },
    { "glDeleteProgram",            (void *)glDeleteProgram },
    { "glIsProgram",                (void *)glIsProgram },
    { "glAttachShader",             (void *)glAttachShader },
    { "glDetachShader",             (void *)glDetachShader },
    { "glLinkProgram",              (void *)glLinkProgram },
    { "glValidateProgram",          (void *)glValidateProgram },
    { "glGetProgramiv",             (void *)glGetProgramiv },
    { "glGetProgramInfoLog",        (void *)glGetProgramInfoLog },
    { "glGetAttachedShaders",       (void *)glGetAttachedShaders },
    { "glGetActiveAttrib",          (void *)glGetActiveAttrib },
    { "glGetActiveUniform",         (void *)glGetActiveUniform },
    { "glUseProgram",               (void *)glUseProgram },
    { "glBindAttribLocation",       (void *)glBindAttribLocation },
    { "glGetAttribLocation",        (void *)glGetAttribLocation },
    { "glGetUniformLocation",       (void *)glGetUniformLocation },
    { "glUniform1i",                (void *)glUniform1i },
    { "glUniform1f",                (void *)glUniform1f },
    { "glUniform2f",                (void *)glUniform2f },
    { "glUniform3f",                (void *)glUniform3f },
    { "glUniform1iv",               (void *)glUniform1iv },
    { "glUniform1fv",               (void *)glUniform1fv },
    { "glUniform2iv",               (void *)glUniform2iv },
    { "glUniform2fv",               (void *)glUniform2fv },
    { "glUniform3iv",               (void *)glUniform3iv },
    { "glUniform3fv",               (void *)glUniform3fv },
    { "glUniform4f",                (void *)glUniform4f },
    { "glUniform2i",                (void *)glUniform2i },
    { "glUniform3i",                (void *)glUniform3i },
    { "glUniform4i",                (void *)glUniform4i },
    { "glUniform4iv",               (void *)glUniform4iv },
    { "glUniform4fv",               (void *)glUniform4fv },
    { "glUniformMatrix4fv",         (void *)glUniformMatrix4fv },
    { "glUniformMatrix2fv",         (void *)glUniformMatrix2fv },
    { "glUniformMatrix3fv",         (void *)glUniformMatrix3fv },
    { "glDrawArrays",               (void *)glDrawArrays },
    { "glDrawElements",             (void *)glDrawElements },
    { "glFlush",                    (void *)glFlush },
    { "glFinish",                   (void *)glFinish },
};

void *sgl2_getprocaddress(const char *name)
{
    if (!name) return (void *)sgl2_noop;
    for (unsigned i = 0; i < sizeof(g_table) / sizeof(g_table[0]); ++i)
        if (!strcmp(name, g_table[i].name)) { bridge_trace_impl(name); return g_table[i].fn; }
    bridge_trace_stub(name);
    return (void *)sgl2_noop;     /* unimplemented GLES2 fn -> harmless no-op */
}

void sgl_bridge_init(sgl_framebuffer *fb)
{
    g_fb = fb;
    sglMakeCurrent(fb);
    set_getprocaddress(sgl2_getprocaddress);
    set_getmainfbsize(getfbsize);
    initialize_gl4es();
}
