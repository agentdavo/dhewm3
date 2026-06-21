/*
 * Namespacing header for the softgles2 GLES2 backend when linked together with
 * gl4es.
 *
 * gl4es provides the *desktop* GL entry points (glClear, glDrawArrays, ...) as
 * the application-facing API, and reaches the GLES2 driver only through the
 * function pointers returned by its set_getprocaddress() callback — never by
 * direct symbol reference. softgles2 implements those same GLES2 names. Linking
 * both statically would collide, so we compile the softgles2 backend with this
 * header force-included (-include), renaming every exported GLES2 entry to an
 * sgl2_ prefix. The proc-address shim (bridge.c) then maps the GLES2 name
 * strings to the sgl2_ functions.
 *
 * Non-GL symbols (sgl_fb_create, sglMakeCurrent, sgl_fnv1a, ...) are untouched.
 */
#ifndef SGL2_NAMES_H
#define SGL2_NAMES_H

#define glViewport                 sgl2_glViewport
#define glClearColor               sgl2_glClearColor
#define glClearDepthf              sgl2_glClearDepthf
#define glClearStencil             sgl2_glClearStencil
#define glClear                    sgl2_glClear
#define glEnable                   sgl2_glEnable
#define glDisable                  sgl2_glDisable
#define glIsEnabled                sgl2_glIsEnabled
#define glDepthFunc                sgl2_glDepthFunc
#define glDepthMask                sgl2_glDepthMask
#define glDepthRangef              sgl2_glDepthRangef
#define glColorMask                sgl2_glColorMask
#define glStencilFunc              sgl2_glStencilFunc
#define glStencilFuncSeparate      sgl2_glStencilFuncSeparate
#define glStencilMask              sgl2_glStencilMask
#define glStencilMaskSeparate      sgl2_glStencilMaskSeparate
#define glStencilOp                sgl2_glStencilOp
#define glStencilOpSeparate        sgl2_glStencilOpSeparate
#define glScissor                  sgl2_glScissor
#define glDepthBoundsEXT           sgl2_glDepthBoundsEXT
#define glPolygonOffset            sgl2_glPolygonOffset
#define glBlendFunc                sgl2_glBlendFunc
#define glBlendFuncSeparate        sgl2_glBlendFuncSeparate
#define glBlendEquation            sgl2_glBlendEquation
#define glBlendEquationSeparate    sgl2_glBlendEquationSeparate
#define glBlendColor               sgl2_glBlendColor
#define glCullFace                 sgl2_glCullFace
#define glFrontFace                sgl2_glFrontFace
#define glGetError                 sgl2_glGetError
#define glGetString                sgl2_glGetString
#define glGetIntegerv              sgl2_glGetIntegerv
#define glGetFloatv                sgl2_glGetFloatv
#define glGetBooleanv              sgl2_glGetBooleanv
#define glGenBuffers               sgl2_glGenBuffers
#define glDeleteBuffers            sgl2_glDeleteBuffers
#define glIsBuffer                 sgl2_glIsBuffer
#define glBindBuffer               sgl2_glBindBuffer
#define glBufferData               sgl2_glBufferData
#define glBufferSubData            sgl2_glBufferSubData
#define glGetBufferParameteriv     sgl2_glGetBufferParameteriv
#define glGenFramebuffers          sgl2_glGenFramebuffers
#define glDeleteFramebuffers       sgl2_glDeleteFramebuffers
#define glIsFramebuffer            sgl2_glIsFramebuffer
#define glBindFramebuffer          sgl2_glBindFramebuffer
#define glCheckFramebufferStatus   sgl2_glCheckFramebufferStatus
#define glFramebufferTexture2D     sgl2_glFramebufferTexture2D
#define glFramebufferRenderbuffer  sgl2_glFramebufferRenderbuffer
#define glGetFramebufferAttachmentParameteriv sgl2_glGetFramebufferAttachmentParameteriv
#define glGenRenderbuffers         sgl2_glGenRenderbuffers
#define glDeleteRenderbuffers      sgl2_glDeleteRenderbuffers
#define glIsRenderbuffer           sgl2_glIsRenderbuffer
#define glBindRenderbuffer         sgl2_glBindRenderbuffer
#define glRenderbufferStorage      sgl2_glRenderbufferStorage
#define glGetRenderbufferParameteriv sgl2_glGetRenderbufferParameteriv
#define glEnableVertexAttribArray  sgl2_glEnableVertexAttribArray
#define glDisableVertexAttribArray sgl2_glDisableVertexAttribArray
#define glVertexAttribPointer      sgl2_glVertexAttribPointer
#define glVertexAttribIPointer     sgl2_glVertexAttribIPointer
#define glVertexAttrib1f           sgl2_glVertexAttrib1f
#define glVertexAttrib2f           sgl2_glVertexAttrib2f
#define glVertexAttrib3f           sgl2_glVertexAttrib3f
#define glVertexAttrib4f           sgl2_glVertexAttrib4f
#define glVertexAttrib1fv          sgl2_glVertexAttrib1fv
#define glVertexAttrib2fv          sgl2_glVertexAttrib2fv
#define glVertexAttrib3fv          sgl2_glVertexAttrib3fv
#define glVertexAttrib4fv          sgl2_glVertexAttrib4fv
#define glGenTextures              sgl2_glGenTextures
#define glDeleteTextures           sgl2_glDeleteTextures
#define glIsTexture                sgl2_glIsTexture
#define glActiveTexture            sgl2_glActiveTexture
#define glBindTexture              sgl2_glBindTexture
#define glTexParameteri            sgl2_glTexParameteri
#define glTexParameterf            sgl2_glTexParameterf
#define glTexParameteriv           sgl2_glTexParameteriv
#define glTexParameterfv           sgl2_glTexParameterfv
#define glGetTexParameteriv        sgl2_glGetTexParameteriv
#define glGetTexParameterfv        sgl2_glGetTexParameterfv
#define glPixelStorei              sgl2_glPixelStorei
#define glTexImage2D               sgl2_glTexImage2D
#define glTexSubImage2D            sgl2_glTexSubImage2D
#define glCompressedTexImage2D     sgl2_glCompressedTexImage2D
#define glCompressedTexSubImage2D  sgl2_glCompressedTexSubImage2D
#define glCopyTexImage2D           sgl2_glCopyTexImage2D
#define glCopyTexSubImage2D        sgl2_glCopyTexSubImage2D
#define glGenerateMipmap           sgl2_glGenerateMipmap
#define glReadPixels               sgl2_glReadPixels
#define glCreateShader             sgl2_glCreateShader
#define glDeleteShader             sgl2_glDeleteShader
#define glIsShader                 sgl2_glIsShader
#define glShaderSource             sgl2_glShaderSource
#define glCompileShader            sgl2_glCompileShader
#define glGetShaderiv              sgl2_glGetShaderiv
#define glGetShaderInfoLog         sgl2_glGetShaderInfoLog
#define glGetShaderSource          sgl2_glGetShaderSource
#define glGetShaderPrecisionFormat sgl2_glGetShaderPrecisionFormat
#define glReleaseShaderCompiler    sgl2_glReleaseShaderCompiler
#define glShaderBinary             sgl2_glShaderBinary
#define glCreateProgram            sgl2_glCreateProgram
#define glDeleteProgram            sgl2_glDeleteProgram
#define glIsProgram                sgl2_glIsProgram
#define glAttachShader             sgl2_glAttachShader
#define glDetachShader             sgl2_glDetachShader
#define glLinkProgram              sgl2_glLinkProgram
#define glValidateProgram          sgl2_glValidateProgram
#define glGetProgramiv             sgl2_glGetProgramiv
#define glGetProgramInfoLog        sgl2_glGetProgramInfoLog
#define glGetAttachedShaders       sgl2_glGetAttachedShaders
#define glGetActiveAttrib          sgl2_glGetActiveAttrib
#define glGetActiveUniform         sgl2_glGetActiveUniform
#define glUseProgram               sgl2_glUseProgram
#define glBindAttribLocation       sgl2_glBindAttribLocation
#define glGetAttribLocation        sgl2_glGetAttribLocation
#define glGetUniformLocation       sgl2_glGetUniformLocation
#define glUniform1i                sgl2_glUniform1i
#define glUniform1iv               sgl2_glUniform1iv
#define glUniform1fv               sgl2_glUniform1fv
#define glUniform2iv               sgl2_glUniform2iv
#define glUniform2fv               sgl2_glUniform2fv
#define glUniform3iv               sgl2_glUniform3iv
#define glUniform3fv               sgl2_glUniform3fv
#define glUniform4f                sgl2_glUniform4f
#define glUniform4iv               sgl2_glUniform4iv
#define glUniform4fv               sgl2_glUniform4fv
#define glUniformMatrix4fv         sgl2_glUniformMatrix4fv
#define glDrawArrays               sgl2_glDrawArrays
#define glDrawElements             sgl2_glDrawElements
#define glFlush                    sgl2_glFlush
#define glFinish                   sgl2_glFinish

#endif /* SGL2_NAMES_H */
