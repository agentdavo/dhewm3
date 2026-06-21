/*
 * softgles2 — OpenGL ES 2.0 API subset.
 *
 * Real GLES2 names / enum values / signatures, so this is a direct step toward
 * the surface gl4es links against (full headers/coverage land in Phase 3/4).
 * v1 covers: context+state, buffers, textures, vertex-attrib arrays, a built-in
 * program (position/color/texcoord + one sampler — the shape gl4es generates
 * for fixed-function and simple ARB programs), and glDrawArrays/Elements.
 * The GLSL ES compiler behind glCompileShader is the next milestone; for now a
 * compiled shader selects the built-in pipeline.
 */
#ifndef SOFTGLES2_GL_H
#define SOFTGLES2_GL_H

#include <stdint.h>
#include <stddef.h>
#include "sgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef unsigned char  GLubyte;
typedef char           GLchar;
typedef long           GLsizeiptr;
typedef long           GLintptr;

/* booleans / errors */
#define GL_FALSE 0
#define GL_TRUE  1
#define GL_NO_ERROR          0
#define GL_INVALID_ENUM      0x0500
#define GL_INVALID_VALUE     0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_INVALID_FRAMEBUFFER_OPERATION 0x0506

/* clear bits */
#define GL_DEPTH_BUFFER_BIT  0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT  0x00004000

/* primitives */
#define GL_TRIANGLES         0x0004
#define GL_TRIANGLE_STRIP    0x0005
#define GL_TRIANGLE_FAN      0x0006

/* caps / state */
#define GL_CULL_FACE         0x0B44
#define GL_DEPTH_TEST        0x0B71
#define GL_BLEND             0x0BE2
#define GL_BLEND_SRC         0x0BE1
#define GL_BLEND_DST         0x0BE0
#define GL_BLEND_SRC_RGB     0x80C9
#define GL_BLEND_DST_RGB     0x80C8
#define GL_BLEND_SRC_ALPHA   0x80CB
#define GL_BLEND_DST_ALPHA   0x80CA
#define GL_BLEND_EQUATION    0x8009
#define GL_BLEND_EQUATION_RGB 0x8009
#define GL_BLEND_EQUATION_ALPHA 0x883D
#define GL_BLEND_COLOR       0x8005
#define GL_FUNC_ADD          0x8006
#define GL_FUNC_SUBTRACT     0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_NEVER             0x0200
#define GL_LESS              0x0201
#define GL_EQUAL             0x0202
#define GL_LEQUAL            0x0203
#define GL_GREATER           0x0204
#define GL_NOTEQUAL          0x0205
#define GL_GEQUAL            0x0206
#define GL_ALWAYS            0x0207
#define GL_CCW               0x0901
#define GL_CW                0x0900
#define GL_BACK              0x0405
#define GL_FRONT             0x0404
#define GL_FRONT_AND_BACK    0x0408
#define GL_NONE              0
#define GL_SCISSOR_TEST      0x0C11
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_DEPTH_BOUNDS_TEST_EXT 0x8890   /* GL_EXT_depth_bounds_test */
#define GL_DEPTH_BOUNDS_EXT      0x8891
#define GL_STENCIL_TEST      0x0B90
#define GL_STENCIL_BITS      0x0D57
#define GL_STENCIL_CLEAR_VALUE 0x0B91
#define GL_STENCIL_FUNC      0x0B92
#define GL_STENCIL_VALUE_MASK 0x0B93
#define GL_STENCIL_FAIL      0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL 0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS 0x0B96
#define GL_STENCIL_REF       0x0B97
#define GL_STENCIL_WRITEMASK 0x0B98
#define GL_STENCIL_BACK_FUNC 0x8800
#define GL_STENCIL_BACK_FAIL 0x8801
#define GL_STENCIL_BACK_PASS_DEPTH_FAIL 0x8802
#define GL_STENCIL_BACK_PASS_DEPTH_PASS 0x8803
#define GL_STENCIL_BACK_REF  0x8CA3
#define GL_STENCIL_BACK_VALUE_MASK 0x8CA4
#define GL_STENCIL_BACK_WRITEMASK 0x8CA5

/* stencil ops */
#define GL_KEEP              0x1E00
#define GL_REPLACE           0x1E01
#define GL_INCR              0x1E02
#define GL_DECR              0x1E03
#define GL_INVERT            0x150A
#define GL_INCR_WRAP         0x8507
#define GL_DECR_WRAP         0x8508

/* blend factors (subset) */
#define GL_ZERO              0
#define GL_ONE               1
#define GL_SRC_COLOR         0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA         0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA         0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR         0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_CONSTANT_COLOR    0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA    0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004

/* buffers */
#define GL_ARRAY_BUFFER          0x8892
#define GL_ELEMENT_ARRAY_BUFFER  0x8893
#define GL_STATIC_DRAW           0x88E4
#define GL_DYNAMIC_DRAW          0x88E8

/* types */
#define GL_BYTE              0x1400
#define GL_UNSIGNED_BYTE     0x1401
#define GL_SHORT             0x1402
#define GL_UNSIGNED_SHORT    0x1403
#define GL_INT               0x1404
#define GL_UNSIGNED_INT      0x1405
#define GL_FLOAT             0x1406
#define GL_UNSIGNED_SHORT_5_6_5 0x8363

/* textures */
#define GL_TEXTURE_2D        0x0DE1
#define GL_TEXTURE0          0x84C0
#define GL_TEXTURE_CUBE_MAP  0x8513
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_RGB               0x1907
#define GL_RGBA              0x1908
#define GL_ALPHA             0x1906
#define GL_LUMINANCE         0x1909
#define GL_LUMINANCE_ALPHA   0x190A
#ifndef GL_RGB8
#define GL_RGB8              0x8051
#endif
#ifndef GL_RGBA8
#define GL_RGBA8             0x8058
#endif
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS 0x86A3
#define GL_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#define GL_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_STENCIL_INDEX     0x1901
#define GL_DEPTH_COMPONENT   0x1902
#define GL_DEPTH_COMPONENT16 0x81A5
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif
#define GL_STENCIL_INDEX8    0x8D48
#define GL_DEPTH24_STENCIL8  0x88F0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S    0x2802
#define GL_TEXTURE_WRAP_T    0x2803
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_WRAP_R    0x8072
#define GL_NEAREST           0x2600
#define GL_LINEAR            0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#define GL_REPEAT            0x2901
#define GL_CLAMP_TO_EDGE     0x812F
#define GL_ACTIVE_TEXTURE    0x84E0

/* shaders / programs */
#define GL_FRAGMENT_SHADER   0x8B30
#define GL_VERTEX_SHADER     0x8B31
#define GL_DELETE_STATUS     0x8B80
#define GL_COMPILE_STATUS    0x8B81
#define GL_LINK_STATUS       0x8B82
#define GL_VALIDATE_STATUS   0x8B83
#define GL_INFO_LOG_LENGTH   0x8B84
#define GL_ATTACHED_SHADERS  0x8B85
#define GL_ACTIVE_UNIFORMS   0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87
#define GL_SHADER_SOURCE_LENGTH 0x8B88
#define GL_ACTIVE_ATTRIBUTES 0x8B89
#define GL_ACTIVE_ATTRIBUTE_MAX_LENGTH 0x8B8A
#define GL_SHADER_TYPE       0x8B4F

/* pixel store / queries */
#define GL_PACK_ALIGNMENT    0x0D05
#define GL_UNPACK_ALIGNMENT  0x0CF5
#define GL_RED_BITS          0x0D52
#define GL_GREEN_BITS        0x0D53
#define GL_BLUE_BITS         0x0D54
#define GL_ALPHA_BITS        0x0D55
#define GL_DEPTH_BITS        0x0D56
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_BUFFER_SIZE       0x8764
#define GL_BUFFER_USAGE      0x8765

/* framebuffer/renderbuffer objects */
#define GL_FRAMEBUFFER       0x8D40
#define GL_RENDERBUFFER      0x8D41
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#define GL_RENDERBUFFER_RED_SIZE 0x8D50
#define GL_RENDERBUFFER_GREEN_SIZE 0x8D51
#define GL_RENDERBUFFER_BLUE_SIZE 0x8D52
#define GL_RENDERBUFFER_ALPHA_SIZE 0x8D53
#define GL_RENDERBUFFER_DEPTH_SIZE 0x8D54
#define GL_RENDERBUFFER_STENCIL_SIZE 0x8D55
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT  0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE 0x8CD3

/* glGetString / glGetIntegerv */
#define GL_VENDOR            0x1F00
#define GL_RENDERER          0x1F01
#define GL_VERSION           0x1F02
#define GL_EXTENSIONS        0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAX_TEXTURE_SIZE  0x0D33
#define GL_MAX_VERTEX_ATTRIBS 0x8869

/* Built-in attribute slots (the gl4es-style convention used by v1). */
#define SGL_ATTRIB_POSITION  0
#define SGL_ATTRIB_COLOR     1
#define SGL_ATTRIB_TEXCOORD  2
#define SGL_MAX_VERTEX_ATTRIBS 16

/* ---- EGL-ish surface binding (our extension; eglMakeCurrent later) ---- */
void   sglMakeCurrent(sgl_framebuffer *fb);

/* ---- state ---- */
void   glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void   glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void   glClearDepthf(GLclampf d);
void   glClearStencil(GLint s);
void   glClear(GLbitfield mask);
void   glEnable(GLenum cap);
void   glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void   glDepthFunc(GLenum func);
void   glDepthMask(GLboolean flag);
void   glDepthRangef(GLclampf n, GLclampf f);
void   glDepthBoundsEXT(GLclampf zmin, GLclampf zmax);
void   glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void   glStencilFunc(GLenum func, GLint ref, GLuint mask);
void   glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
void   glStencilMask(GLuint mask);
void   glStencilMaskSeparate(GLenum face, GLuint mask);
void   glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void   glStencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass);
void   glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void   glPolygonOffset(GLfloat factor, GLfloat units);
void   glBlendFunc(GLenum sfactor, GLenum dfactor);
void   glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
void   glBlendEquation(GLenum mode);
void   glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
void   glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void   glCullFace(GLenum mode);
void   glFrontFace(GLenum mode);
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void   glGetIntegerv(GLenum pname, GLint *data);
void   glGetFloatv(GLenum pname, GLfloat *data);
void   glGetBooleanv(GLenum pname, GLboolean *data);

/* ---- buffers ---- */
void   glGenBuffers(GLsizei n, GLuint *buffers);
void   glDeleteBuffers(GLsizei n, const GLuint *buffers);
GLboolean glIsBuffer(GLuint buffer);
void   glBindBuffer(GLenum target, GLuint buffer);
void   glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void   glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
void   glGetBufferParameteriv(GLenum target, GLenum pname, GLint *params);

/* ---- framebuffer/renderbuffer objects ---- */
void   glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void   glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
GLboolean glIsFramebuffer(GLuint framebuffer);
void   glBindFramebuffer(GLenum target, GLuint framebuffer);
GLenum glCheckFramebufferStatus(GLenum target);
void   glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                              GLuint texture, GLint level);
void   glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                                 GLenum renderbuffertarget, GLuint renderbuffer);
void   glGetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment,
                                             GLenum pname, GLint *params);
void   glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void   glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
GLboolean glIsRenderbuffer(GLuint renderbuffer);
void   glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void   glRenderbufferStorage(GLenum target, GLenum internalformat,
                             GLsizei width, GLsizei height);
void   glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params);

/* ---- vertex attributes ---- */
void   glEnableVertexAttribArray(GLuint index);
void   glDisableVertexAttribArray(GLuint index);
void   glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                             GLboolean normalized, GLsizei stride, const void *ptr);
void   glVertexAttribIPointer(GLuint index, GLint size, GLenum type,
                              GLsizei stride, const void *ptr);
void   glVertexAttrib1f(GLuint index, GLfloat x);
void   glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
void   glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
void   glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void   glVertexAttrib1fv(GLuint index, const GLfloat *v);
void   glVertexAttrib2fv(GLuint index, const GLfloat *v);
void   glVertexAttrib3fv(GLuint index, const GLfloat *v);
void   glVertexAttrib4fv(GLuint index, const GLfloat *v);

/* ---- textures ---- */
void   glGenTextures(GLsizei n, GLuint *textures);
void   glDeleteTextures(GLsizei n, const GLuint *textures);
GLboolean glIsTexture(GLuint texture);
void   glActiveTexture(GLenum texture);
void   glBindTexture(GLenum target, GLuint texture);
void   glTexParameteri(GLenum target, GLenum pname, GLint param);
void   glTexParameterf(GLenum target, GLenum pname, GLfloat param);
void   glTexParameteriv(GLenum target, GLenum pname, const GLint *params);
void   glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
void   glGetTexParameteriv(GLenum target, GLenum pname, GLint *params);
void   glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params);
void   glPixelStorei(GLenum pname, GLint param);
void   glTexImage2D(GLenum target, GLint level, GLint internalformat,
                    GLsizei w, GLsizei h, GLint border,
                    GLenum format, GLenum type, const void *pixels);
void   glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                       GLsizei w, GLsizei h, GLenum format, GLenum type,
                       const void *pixels);
void   glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                              GLsizei w, GLsizei h, GLint border,
                              GLsizei imageSize, const void *data);
void   glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                 GLsizei w, GLsizei h, GLenum format,
                                 GLsizei imageSize, const void *data);
void   glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                        GLint x, GLint y, GLsizei w, GLsizei h, GLint border);
void   glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                           GLint x, GLint y, GLsizei w, GLsizei h);
void   glGenerateMipmap(GLenum target);
void   glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                    GLenum format, GLenum type, void *pixels);

/* ---- program / shader (built-in pipeline for now) ---- */
GLuint glCreateShader(GLenum type);
void   glDeleteShader(GLuint shader);
GLboolean glIsShader(GLuint shader);
void   glShaderSource(GLuint shader, GLsizei count, const GLchar *const *str, const GLint *len);
void   glCompileShader(GLuint shader);
void   glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void   glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glGetShaderSource(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *source);
void   glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint *range, GLint *precision);
void   glReleaseShaderCompiler(void);
void   glShaderBinary(GLsizei n, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length);
GLuint glCreateProgram(void);
void   glDeleteProgram(GLuint program);
GLboolean glIsProgram(GLuint program);
void   glAttachShader(GLuint program, GLuint shader);
void   glDetachShader(GLuint program, GLuint shader);
void   glLinkProgram(GLuint program);
void   glValidateProgram(GLuint program);
void   glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void   glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glGetAttachedShaders(GLuint program, GLsizei maxCount, GLsizei *count, GLuint *shaders);
void   glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void   glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name);
void   glUseProgram(GLuint program);
void   glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
GLint  glGetAttribLocation(GLuint program, const GLchar *name);
GLint  glGetUniformLocation(GLuint program, const GLchar *name);
void   glUniform1i(GLint location, GLint v0);
void   glUniform1iv(GLint location, GLsizei count, const GLint *value);
void   glUniform1fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform2iv(GLint location, GLsizei count, const GLint *value);
void   glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform3iv(GLint location, GLsizei count, const GLint *value);
void   glUniform3fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform4f(GLint location, GLfloat a, GLfloat b, GLfloat c, GLfloat d);
void   glUniform4iv(GLint location, GLsizei count, const GLint *value);
void   glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

/* ---- draw ---- */
void   glDrawArrays(GLenum mode, GLint first, GLsizei count);
void   glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
void   glFlush(void);
void   glFinish(void);

#ifdef __cplusplus
}
#endif

#endif /* SOFTGLES2_GL_H */
