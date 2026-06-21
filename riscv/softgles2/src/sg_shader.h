/* Shader / program object management.
 *
 * Owns the g_shaders[], g_programs[] arrays and implements the full GLSL-ES
 * program pipeline (compile, link, uniform upload).  Exposes gp_of/cur_gp so
 * the draw module can select the active GLSL program.
 */
#ifndef SOFTGLES2_SG_SHADER_H
#define SOFTGLES2_SG_SHADER_H

#include "sgl_gl.h"
#include "glsl.h"
#include <stddef.h>

/* Doom3 via gl4es creates a vertex+fragment shader (and a program) per unique
 * FPE/ARB render state; mars_city1 alone needs well over the old 64. Size the
 * pools generously so glCreateShader/glCreateProgram never return 0 mid-map (the
 * gl4es FPE path did not null-check that and stored through it -> frame-7 trap). */
#define MAX_SHADERS  1024
#define MAX_PROGRAMS 1024

typedef struct { int used; int stage; char *src; int compiled; char log[128]; }  shader_obj;
typedef struct { int used; GLuint vs, fs; glsl_program *gp;
                 char log[128];
                 struct { char name[40]; int index; } abind[SGL_MAX_VERTEX_ATTRIBS]; int nabind; } prog_obj;

extern shader_obj g_shaders[MAX_SHADERS];
extern prog_obj   g_programs[MAX_PROGRAMS];

/* Debug counters (defined in sg_shader.c). */
extern volatile unsigned long sgl_dbg_shader_link_glsl;
extern volatile unsigned long sgl_dbg_shader_link_builtin;
extern volatile unsigned long sgl_dbg_shader_link_fail;

/* Helpers for the draw path. */
glsl_program *gp_of(GLuint p);
glsl_program *cur_gp(void);

/* GL shader/program API (declared public in sgl_gl.h). */
GLuint glCreateShader(GLenum type);
void   glDeleteShader(GLuint s);
GLboolean glIsShader(GLuint s);
void   glShaderSource(GLuint s, GLsizei count, const GLchar *const *str, const GLint *len);
void   glCompileShader(GLuint s);
void   glGetShaderiv(GLuint s, GLenum pname, GLint *params);
void   glGetShaderInfoLog(GLuint s, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glGetShaderSource(GLuint s, GLsizei bufSize, GLsizei *length, GLchar *source);
void   glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint *range, GLint *precision);
void   glReleaseShaderCompiler(void);
void   glShaderBinary(GLsizei n, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length);
GLuint glCreateProgram(void);
void   glDeleteProgram(GLuint p);
GLboolean glIsProgram(GLuint p);
void   glAttachShader(GLuint p, GLuint s);
void   glDetachShader(GLuint p, GLuint s);
void   glLinkProgram(GLuint p);
void   glValidateProgram(GLuint p);
void   glGetProgramiv(GLuint p, GLenum pname, GLint *params);
void   glGetProgramInfoLog(GLuint p, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
void   glGetAttachedShaders(GLuint p, GLsizei maxCount, GLsizei *count, GLuint *shaders);
void   glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                         GLint *size, GLenum *type, GLchar *name);
void   glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                          GLint *size, GLenum *type, GLchar *name);
void   glUseProgram(GLuint p);
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

#endif /* SOFTGLES2_SG_SHADER_H */
