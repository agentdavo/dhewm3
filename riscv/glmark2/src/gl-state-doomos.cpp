#include "gl-state-doomos.h"

#include "gl-headers.h"
#include "gl-visual-config.h"
#include "log.h"

#include <cstring>

/* GLAD maps public gl* names to glad_gl* storage. This file is the one place
 * that must take addresses of the real softgles2 symbols for GLAD's loader. */
#undef glActiveTexture
#undef glAttachShader
#undef glBindAttribLocation
#undef glBindBuffer
#undef glBindFramebuffer
#undef glBindRenderbuffer
#undef glBindTexture
#undef glBlendColor
#undef glBlendEquation
#undef glBlendEquationSeparate
#undef glBlendFunc
#undef glBlendFuncSeparate
#undef glBufferData
#undef glBufferSubData
#undef glCheckFramebufferStatus
#undef glClear
#undef glClearColor
#undef glClearDepthf
#undef glClearStencil
#undef glColorMask
#undef glCompileShader
#undef glCompressedTexImage2D
#undef glCompressedTexSubImage2D
#undef glCopyTexImage2D
#undef glCopyTexSubImage2D
#undef glCreateProgram
#undef glCreateShader
#undef glCullFace
#undef glDeleteBuffers
#undef glDeleteFramebuffers
#undef glDeleteProgram
#undef glDeleteRenderbuffers
#undef glDeleteShader
#undef glDeleteTextures
#undef glDepthFunc
#undef glDepthMask
#undef glDepthRangef
#undef glDetachShader
#undef glDisable
#undef glDisableVertexAttribArray
#undef glDrawArrays
#undef glDrawElements
#undef glEnable
#undef glEnableVertexAttribArray
#undef glFinish
#undef glFlush
#undef glFramebufferRenderbuffer
#undef glFramebufferTexture2D
#undef glFrontFace
#undef glGenBuffers
#undef glGenerateMipmap
#undef glGenFramebuffers
#undef glGenRenderbuffers
#undef glGenTextures
#undef glGetActiveAttrib
#undef glGetActiveUniform
#undef glGetAttachedShaders
#undef glGetAttribLocation
#undef glGetBooleanv
#undef glGetBufferParameteriv
#undef glGetError
#undef glGetFloatv
#undef glGetFramebufferAttachmentParameteriv
#undef glGetIntegerv
#undef glGetProgramInfoLog
#undef glGetProgramiv
#undef glGetRenderbufferParameteriv
#undef glGetShaderInfoLog
#undef glGetShaderPrecisionFormat
#undef glGetShaderSource
#undef glGetShaderiv
#undef glGetString
#undef glGetTexParameterfv
#undef glGetTexParameteriv
#undef glGetUniformLocation
#undef glIsBuffer
#undef glIsEnabled
#undef glIsFramebuffer
#undef glIsProgram
#undef glIsRenderbuffer
#undef glIsShader
#undef glIsTexture
#undef glLinkProgram
#undef glPixelStorei
#undef glPolygonOffset
#undef glReadPixels
#undef glReleaseShaderCompiler
#undef glRenderbufferStorage
#undef glScissor
#undef glShaderBinary
#undef glShaderSource
#undef glStencilFunc
#undef glStencilFuncSeparate
#undef glStencilMask
#undef glStencilMaskSeparate
#undef glStencilOp
#undef glStencilOpSeparate
#undef glTexImage2D
#undef glTexParameterf
#undef glTexParameterfv
#undef glTexParameteri
#undef glTexParameteriv
#undef glTexSubImage2D
#undef glUniform1fv
#undef glUniform1i
#undef glUniform1iv
#undef glUniform2fv
#undef glUniform2iv
#undef glUniform3fv
#undef glUniform3iv
#undef glUniform4f
#undef glUniform4fv
#undef glUniform4iv
#undef glUniformMatrix4fv
#undef glUseProgram
#undef glValidateProgram
#undef glVertexAttrib1f
#undef glVertexAttrib1fv
#undef glVertexAttrib2f
#undef glVertexAttrib2fv
#undef glVertexAttrib3f
#undef glVertexAttrib3fv
#undef glVertexAttrib4f
#undef glVertexAttrib4fv
#undef glVertexAttribPointer
#undef glViewport

extern "C" {
#include "../../softgles2/include/sgl_gl.h"
#include "../../softgles2/src/sg_frame.h"
#include "../../softgles2/src/sg_framebin.h"
#include "../../softgles2/src/sg_prof.h"
}

#define RESOLVE_GL(fn) do { if (std::strcmp(name, #fn) == 0) return reinterpret_cast<GLADapiproc>(::fn); } while (0)

namespace
{

GLADapiproc
load_softgles2_proc(void *, const char *name)
{
    RESOLVE_GL(glActiveTexture);
    RESOLVE_GL(glAttachShader);
    RESOLVE_GL(glBindAttribLocation);
    RESOLVE_GL(glBindBuffer);
    RESOLVE_GL(glBindFramebuffer);
    RESOLVE_GL(glBindRenderbuffer);
    RESOLVE_GL(glBindTexture);
    RESOLVE_GL(glBlendColor);
    RESOLVE_GL(glBlendEquation);
    RESOLVE_GL(glBlendEquationSeparate);
    RESOLVE_GL(glBlendFunc);
    RESOLVE_GL(glBlendFuncSeparate);
    RESOLVE_GL(glBufferData);
    RESOLVE_GL(glBufferSubData);
    RESOLVE_GL(glCheckFramebufferStatus);
    RESOLVE_GL(glClear);
    RESOLVE_GL(glClearColor);
    RESOLVE_GL(glClearDepthf);
    RESOLVE_GL(glClearStencil);
    RESOLVE_GL(glColorMask);
    RESOLVE_GL(glCompileShader);
    RESOLVE_GL(glCompressedTexImage2D);
    RESOLVE_GL(glCompressedTexSubImage2D);
    RESOLVE_GL(glCopyTexImage2D);
    RESOLVE_GL(glCopyTexSubImage2D);
    RESOLVE_GL(glCreateProgram);
    RESOLVE_GL(glCreateShader);
    RESOLVE_GL(glCullFace);
    RESOLVE_GL(glDeleteBuffers);
    RESOLVE_GL(glDeleteFramebuffers);
    RESOLVE_GL(glDeleteProgram);
    RESOLVE_GL(glDeleteRenderbuffers);
    RESOLVE_GL(glDeleteShader);
    RESOLVE_GL(glDeleteTextures);
    RESOLVE_GL(glDepthFunc);
    RESOLVE_GL(glDepthMask);
    RESOLVE_GL(glDepthRangef);
    RESOLVE_GL(glDetachShader);
    RESOLVE_GL(glDisable);
    RESOLVE_GL(glDisableVertexAttribArray);
    RESOLVE_GL(glDrawArrays);
    RESOLVE_GL(glDrawElements);
    RESOLVE_GL(glEnable);
    RESOLVE_GL(glEnableVertexAttribArray);
    RESOLVE_GL(glFinish);
    RESOLVE_GL(glFlush);
    RESOLVE_GL(glFramebufferRenderbuffer);
    RESOLVE_GL(glFramebufferTexture2D);
    RESOLVE_GL(glFrontFace);
    RESOLVE_GL(glGenBuffers);
    RESOLVE_GL(glGenerateMipmap);
    RESOLVE_GL(glGenFramebuffers);
    RESOLVE_GL(glGenRenderbuffers);
    RESOLVE_GL(glGenTextures);
    RESOLVE_GL(glGetActiveAttrib);
    RESOLVE_GL(glGetActiveUniform);
    RESOLVE_GL(glGetAttachedShaders);
    RESOLVE_GL(glGetAttribLocation);
    RESOLVE_GL(glGetBooleanv);
    RESOLVE_GL(glGetBufferParameteriv);
    RESOLVE_GL(glGetError);
    RESOLVE_GL(glGetFloatv);
    RESOLVE_GL(glGetFramebufferAttachmentParameteriv);
    RESOLVE_GL(glGetIntegerv);
    RESOLVE_GL(glGetProgramInfoLog);
    RESOLVE_GL(glGetProgramiv);
    RESOLVE_GL(glGetRenderbufferParameteriv);
    RESOLVE_GL(glGetShaderInfoLog);
    RESOLVE_GL(glGetShaderPrecisionFormat);
    RESOLVE_GL(glGetShaderSource);
    RESOLVE_GL(glGetShaderiv);
    RESOLVE_GL(glGetString);
    RESOLVE_GL(glGetTexParameterfv);
    RESOLVE_GL(glGetTexParameteriv);
    RESOLVE_GL(glGetUniformLocation);
    RESOLVE_GL(glIsBuffer);
    RESOLVE_GL(glIsEnabled);
    RESOLVE_GL(glIsFramebuffer);
    RESOLVE_GL(glIsProgram);
    RESOLVE_GL(glIsRenderbuffer);
    RESOLVE_GL(glIsShader);
    RESOLVE_GL(glIsTexture);
    RESOLVE_GL(glLinkProgram);
    RESOLVE_GL(glPixelStorei);
    RESOLVE_GL(glPolygonOffset);
    RESOLVE_GL(glReadPixels);
    RESOLVE_GL(glReleaseShaderCompiler);
    RESOLVE_GL(glRenderbufferStorage);
    RESOLVE_GL(glScissor);
    RESOLVE_GL(glShaderBinary);
    RESOLVE_GL(glShaderSource);
    RESOLVE_GL(glStencilFunc);
    RESOLVE_GL(glStencilFuncSeparate);
    RESOLVE_GL(glStencilMask);
    RESOLVE_GL(glStencilMaskSeparate);
    RESOLVE_GL(glStencilOp);
    RESOLVE_GL(glStencilOpSeparate);
    RESOLVE_GL(glTexImage2D);
    RESOLVE_GL(glTexParameterf);
    RESOLVE_GL(glTexParameterfv);
    RESOLVE_GL(glTexParameteri);
    RESOLVE_GL(glTexParameteriv);
    RESOLVE_GL(glTexSubImage2D);
    RESOLVE_GL(glUniform1fv);
    RESOLVE_GL(glUniform1i);
    RESOLVE_GL(glUniform1iv);
    RESOLVE_GL(glUniform2fv);
    RESOLVE_GL(glUniform2iv);
    RESOLVE_GL(glUniform3fv);
    RESOLVE_GL(glUniform3iv);
    RESOLVE_GL(glUniform4f);
    RESOLVE_GL(glUniform4fv);
    RESOLVE_GL(glUniform4iv);
    RESOLVE_GL(glUniformMatrix4fv);
    RESOLVE_GL(glUseProgram);
    RESOLVE_GL(glValidateProgram);
    RESOLVE_GL(glVertexAttrib1f);
    RESOLVE_GL(glVertexAttrib1fv);
    RESOLVE_GL(glVertexAttrib2f);
    RESOLVE_GL(glVertexAttrib2fv);
    RESOLVE_GL(glVertexAttrib3f);
    RESOLVE_GL(glVertexAttrib3fv);
    RESOLVE_GL(glVertexAttrib4f);
    RESOLVE_GL(glVertexAttrib4fv);
    RESOLVE_GL(glVertexAttribPointer);
    RESOLVE_GL(glViewport);
    return nullptr;
}

} // namespace

GLStateDoomOS::GLStateDoomOS() :
    fb_(nullptr),
    width_(800),
    height_(600),
    gl_loaded_(false)
{
}

GLStateDoomOS::~GLStateDoomOS()
{
    if (fb_)
        sgl_fb_destroy(fb_);
}

bool
GLStateDoomOS::init_display(void *, GLVisualConfig&)
{
    return true;
}

bool
GLStateDoomOS::init_surface(void *)
{
    if (!fb_)
        fb_ = sgl_fb_create(width_, height_);
    if (!fb_)
        return false;
    sgl_framebin_enabled = GLMARK2_DOOMOS_FRAMEBIN ? 1 : 0;
    sgl_prof_enable(GLMARK2_DOOMOS_PROFILE ? 1 : 0);
    sglMakeCurrent(fb_);
    return true;
}

bool
GLStateDoomOS::init_gl_extensions()
{
    if (!gl_loaded_) {
        if (!gladLoadGLES2UserPtr(load_softgles2_proc, nullptr)) {
            Log::error("Loading softgles2 GLESv2 entry points failed\n");
            return false;
        }
        GLExtensions::GenFramebuffers = glGenFramebuffers;
        GLExtensions::DeleteFramebuffers = glDeleteFramebuffers;
        GLExtensions::BindFramebuffer = glBindFramebuffer;
        GLExtensions::FramebufferTexture2D = glFramebufferTexture2D;
        GLExtensions::FramebufferRenderbuffer = glFramebufferRenderbuffer;
        GLExtensions::CheckFramebufferStatus = glCheckFramebufferStatus;
        GLExtensions::GenRenderbuffers = glGenRenderbuffers;
        GLExtensions::DeleteRenderbuffers = glDeleteRenderbuffers;
        GLExtensions::BindRenderbuffer = glBindRenderbuffer;
        GLExtensions::RenderbufferStorage = glRenderbufferStorage;
        GLExtensions::GenerateMipmap = glGenerateMipmap;
        gl_loaded_ = true;
    }
    return true;
}

bool
GLStateDoomOS::valid()
{
    return fb_ != nullptr;
}

bool
GLStateDoomOS::reset()
{
    return true;
}

void
GLStateDoomOS::swap()
{
    glFinish();
    sgl_prof_frame_report();
    sg_frame_reset();
}

bool
GLStateDoomOS::gotNativeConfig(intptr_t& vid, std::vector<uint64_t>& mods)
{
    vid = 0;
    mods.clear();
    return true;
}

void
GLStateDoomOS::getVisualConfig(GLVisualConfig& vc)
{
    vc.id = 0;
    vc.red = 8;
    vc.green = 8;
    vc.blue = 8;
    vc.alpha = 8;
    vc.depth = 24;
    vc.stencil = 8;
    vc.buffer = 32;
    vc.samples = 0;
}

bool
GLStateDoomOS::supports_sync()
{
    return false;
}

std::unique_ptr<GLStateSync>
GLStateDoomOS::sync()
{
    return std::unique_ptr<GLStateSync>();
}
