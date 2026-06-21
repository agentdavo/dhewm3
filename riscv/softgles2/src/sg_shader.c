#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <stdio.h>
#endif

#include "sg_state.h"
#include "sg_diag.h"
#include "sg_shader.h"

shader_obj g_shaders[MAX_SHADERS];
prog_obj   g_programs[MAX_PROGRAMS];

volatile unsigned long sgl_dbg_shader_link_glsl = 0;
volatile unsigned long sgl_dbg_shader_link_builtin = 0;
volatile unsigned long sgl_dbg_shader_link_fail = 0;

static void set_log(char *dst, size_t dstsz, const char *src)
{
    if (!dstsz) return;
    size_t i = 0;
    if (src) for (; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

static void copy_info(const char *src, GLsizei bufSize, GLsizei *length, GLchar *dst)
{
    GLsizei n = 0;
    if (dst && bufSize > 0) {
        for (; n + 1 < bufSize && src && src[n]; ++n) dst[n] = src[n];
        dst[n] = 0;
    } else if (src) {
        while (src[n]) ++n;
    }
    if (length) *length = n;
}

glsl_program *gp_of(GLuint p) { return (p && p < MAX_PROGRAMS && g_programs[p].used) ? g_programs[p].gp : 0; }
glsl_program *cur_gp(void)    { return gp_of(C.cur_program); }

GLuint glCreateShader(GLenum type)
{
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) { set_err(GL_INVALID_ENUM); return 0; }
    for (GLuint i = 1; i < MAX_SHADERS; ++i)
        if (!g_shaders[i].used) { g_shaders[i].used = 1; g_shaders[i].stage = (type == GL_VERTEX_SHADER) ? 0 : 1; g_shaders[i].src = 0; g_shaders[i].compiled = 0; g_shaders[i].log[0] = 0; return i; }
    set_err(GL_INVALID_OPERATION); return 0;
}

void glDeleteShader(GLuint s)
{
    if (!s || s >= MAX_SHADERS || !g_shaders[s].used) return;
    free(g_shaders[s].src);
    memset(&g_shaders[s], 0, sizeof(g_shaders[s]));
}

GLboolean glIsShader(GLuint s)
{
    return (s && s < MAX_SHADERS && g_shaders[s].used) ? GL_TRUE : GL_FALSE;
}

void glShaderSource(GLuint s, GLsizei count, const GLchar *const *str, const GLint *len)
{
    if (!s || s >= MAX_SHADERS || !g_shaders[s].used) return;
    size_t total = 0;
    for (GLsizei i = 0; i < count; ++i) if (str[i]) total += (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
    free(g_shaders[s].src);
    g_shaders[s].src = (char *)malloc(total + 1);
    if (!g_shaders[s].src) return;
    size_t off = 0;
    for (GLsizei i = 0; i < count; ++i) if (str[i]) { size_t l = (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]); memcpy(g_shaders[s].src + off, str[i], l); off += l; }
    g_shaders[s].src[off] = 0;
}

void glCompileShader(GLuint s)
{
    if (!s || s >= MAX_SHADERS || !g_shaders[s].used) return;
    char err[256]; err[0] = 0;
    g_shaders[s].compiled = g_shaders[s].src
        ? glsl_try_compile(g_shaders[s].stage, g_shaders[s].src, err, (int)sizeof err) : 0;
    set_log(g_shaders[s].log, sizeof(g_shaders[s].log), g_shaders[s].compiled ? "" : (err[0] ? err : "compile failed"));
    if (!g_shaders[s].compiled) {
        static const char vermsg[] = "only GLSL ES 1.00";
        int probe = 1;
        for (int i = 0; vermsg[i]; ++i) if (err[i] != vermsg[i]) { probe = 0; break; }
        if (!probe) {
            printf("softgles2: %s shader COMPILE FAILED: %s\n",
                   g_shaders[s].stage ? "fragment" : "vertex", err[0] ? err : "(no message)");
            if (g_shaders[s].src)
                printf("---- shader source ----\n%s\n-----------------------\n", g_shaders[s].src);
        }
    }
}

void glGetShaderiv(GLuint s, GLenum pname, GLint *params)
{
    if (!params) return;
    if (pname == GL_COMPILE_STATUS)
        *params = (s && s < MAX_SHADERS && g_shaders[s].used) ? g_shaders[s].compiled : 0;
    else if (pname == GL_DELETE_STATUS) *params = GL_FALSE;
    else if (pname == GL_SHADER_TYPE) *params = (s && s < MAX_SHADERS && g_shaders[s].used) ? (g_shaders[s].stage ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER) : 0;
    else if (pname == GL_INFO_LOG_LENGTH) *params = (s < MAX_SHADERS && g_shaders[s].log[0]) ? (GLint)strlen(g_shaders[s].log) + 1 : 0;
    else if (pname == GL_SHADER_SOURCE_LENGTH) *params = (s < MAX_SHADERS && g_shaders[s].src) ? (GLint)strlen(g_shaders[s].src) + 1 : 0;
    else *params = 0;
}

void glGetShaderInfoLog(GLuint s, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
    const char *log = (s && s < MAX_SHADERS && g_shaders[s].used) ? g_shaders[s].log : "";
    copy_info(log, bufSize, length, infoLog);
}

void glGetShaderSource(GLuint s, GLsizei bufSize, GLsizei *length, GLchar *source)
{
    const char *src = (s && s < MAX_SHADERS && g_shaders[s].used && g_shaders[s].src) ? g_shaders[s].src : "";
    copy_info(src, bufSize, length, source);
}

void glGetShaderPrecisionFormat(GLenum shaderType, GLenum precisionType, GLint *range, GLint *precision)
{
    (void)precisionType;
    if (shaderType != GL_VERTEX_SHADER && shaderType != GL_FRAGMENT_SHADER) { set_err(GL_INVALID_ENUM); return; }
    if (range) { range[0] = 127; range[1] = 127; }
    if (precision) *precision = 23;
}

void glReleaseShaderCompiler(void) { }

void glShaderBinary(GLsizei n, const GLuint *shaders, GLenum binaryFormat, const void *binary, GLsizei length)
{
    (void)n; (void)shaders; (void)binaryFormat; (void)binary; (void)length;
    set_err(GL_INVALID_OPERATION);
}

GLuint glCreateProgram(void)
{
    for (GLuint i = 1; i < MAX_PROGRAMS; ++i)
        if (!g_programs[i].used) { g_programs[i].used = 1; g_programs[i].vs = 0; g_programs[i].fs = 0; g_programs[i].gp = 0; g_programs[i].log[0] = 0; return i; }
    set_err(GL_INVALID_OPERATION); return 0;
}

void glDeleteProgram(GLuint p)
{
    if (!p || p >= MAX_PROGRAMS || !g_programs[p].used) return;
    if (C.cur_program == p) C.cur_program = 0;
    if (g_programs[p].gp) glsl_program_destroy(g_programs[p].gp);
    memset(&g_programs[p], 0, sizeof(g_programs[p]));
}

GLboolean glIsProgram(GLuint p)
{
    return (p && p < MAX_PROGRAMS && g_programs[p].used) ? GL_TRUE : GL_FALSE;
}

void glAttachShader(GLuint p, GLuint s)
{
    if (!p || p >= MAX_PROGRAMS || !g_programs[p].used || !s || s >= MAX_SHADERS || !g_shaders[s].used) return;
    if (g_shaders[s].stage == 0) g_programs[p].vs = s; else g_programs[p].fs = s;
}

void glDetachShader(GLuint p, GLuint s)
{
    if (!p || p >= MAX_PROGRAMS || !g_programs[p].used) return;
    if (g_programs[p].vs == s) g_programs[p].vs = 0;
    if (g_programs[p].fs == s) g_programs[p].fs = 0;
}

void glLinkProgram(GLuint p)
{
    if (!p || p >= MAX_PROGRAMS || !g_programs[p].used) return;
    prog_obj *po = &g_programs[p];
    po->log[0] = 0;
    if (po->gp) { glsl_program_destroy(po->gp); po->gp = 0; }
    GLuint vs = po->vs, fs = po->fs;
    if (vs && fs && g_shaders[vs].src && g_shaders[fs].src) {
        glsl_program *gp = glsl_program_create();
        char err[128]; err[0] = 0;
        int ok = gp && glsl_attach_source(gp, 0, g_shaders[vs].src, err, sizeof err)
                    && glsl_attach_source(gp, 1, g_shaders[fs].src, err, sizeof err);
        if (ok) {
            for (int i = 0; i < po->nabind; ++i) glsl_bind_attrib(gp, po->abind[i].index, po->abind[i].name);
            ok = glsl_link(gp, err, sizeof err);
        }
        if (ok) {
            po->gp = gp;
            if (sgl_diag_enabled) sgl_dbg_shader_link_glsl++;
        } else {
            if (sgl_diag_enabled) sgl_dbg_shader_link_fail++;
            set_log(po->log, sizeof(po->log), err[0] ? err : "link failed");
            if (gp) glsl_program_destroy(gp);
        }
#if !defined(SGL_BAREMETAL)
        if (getenv("SGL_TRACE")) {
            fprintf(stderr, "[sgl link] prog=%u -> %s (err='%s')\n", p, ok ? "GLSL" : "FALLBACK", err);
            if (!ok) fprintf(stderr, "==VS==\n%s\n==FS==\n%s\n", g_shaders[vs].src, g_shaders[fs].src);
        }
#endif
    } else {
        if (sgl_diag_enabled) sgl_dbg_shader_link_builtin++;
    }
}

void glValidateProgram(GLuint p)
{
    if (!p || p >= MAX_PROGRAMS || !g_programs[p].used) set_err(GL_INVALID_VALUE);
}

void glGetProgramiv(GLuint p, GLenum pname, GLint *params)
{
    if (!params) return;
    glsl_program *gp = gp_of(p);
    switch (pname) {
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:  *params = (p && p < MAX_PROGRAMS && g_programs[p].used) ? GL_TRUE : GL_FALSE; break;
    case GL_DELETE_STATUS:    *params = GL_FALSE; break;
    case GL_INFO_LOG_LENGTH:  *params = (p < MAX_PROGRAMS && g_programs[p].log[0]) ? (GLint)strlen(g_programs[p].log) + 1 : 0; break;
    case GL_ATTACHED_SHADERS: *params = (p && p < MAX_PROGRAMS && g_programs[p].used ? (g_programs[p].vs ? 1 : 0) + (g_programs[p].fs ? 1 : 0) : 0); break;
    case GL_ACTIVE_ATTRIBUTES: *params = gp ? glsl_num_attribs(gp) : 0; break;
    case GL_ACTIVE_UNIFORMS:   *params = gp ? glsl_num_uniforms(gp) : 0; break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: *params = 40; break;
    default: *params = 0; break;
    }
}

void glGetProgramInfoLog(GLuint p, GLsizei bufSize, GLsizei *length, GLchar *infoLog)
{
    const char *log = (p && p < MAX_PROGRAMS && g_programs[p].used) ? g_programs[p].log : "";
    copy_info(log, bufSize, length, infoLog);
}

void glGetAttachedShaders(GLuint p, GLsizei maxCount, GLsizei *count, GLuint *shaders)
{
    GLsizei n = 0;
    if (p && p < MAX_PROGRAMS && g_programs[p].used) {
        if (g_programs[p].vs) { if (shaders && n < maxCount) shaders[n] = g_programs[p].vs; ++n; }
        if (g_programs[p].fs) { if (shaders && n < maxCount) shaders[n] = g_programs[p].fs; ++n; }
    }
    if (count) *count = n < maxCount ? n : maxCount;
}

void glGetActiveAttrib(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                       GLint *size, GLenum *type, GLchar *name)
{
    glsl_program *gp = gp_of(program);
    int sz = 1, ty = 0, loc = 0;
    if (gp && glsl_get_attrib(gp, (int)index, name, bufSize, &sz, &ty, &loc)) {
        int n = 0;
        if (name) while (name[n]) ++n;
        if (length) *length = n;
        if (size) *size = sz;
        if (type) *type = (GLenum)ty;
    } else {
        if (length) *length = 0;
        if (name && bufSize) name[0] = 0;
    }
}

void glGetActiveUniform(GLuint program, GLuint index, GLsizei bufSize, GLsizei *length,
                        GLint *size, GLenum *type, GLchar *name)
{
    glsl_program *gp = gp_of(program);
    int sz = 1, ty = 0;
    if (gp && glsl_get_uniform(gp, (int)index, name, bufSize, &sz, &ty)) {
        int n = 0;
        if (name) while (name[n]) ++n;
        if (length) *length = n;
        if (size) *size = sz;
        if (type) *type = (GLenum)ty;
    } else {
        if (length) *length = 0;
        if (name && bufSize) name[0] = 0;
    }
}

void glUseProgram(GLuint p) { GLTRACE("[useprog] %u (gp=%d)\n", p, gp_of(p)?1:0); C.cur_program = p; }

void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
    if (!program || program >= MAX_PROGRAMS || !g_programs[program].used) return;
    prog_obj *po = &g_programs[program];
    if (po->nabind >= SGL_MAX_VERTEX_ATTRIBS) return;
    int i = po->nabind++, k = 0;
    for (; name[k] && k < 39; ++k) po->abind[i].name[k] = name[k];
    po->abind[i].name[k] = 0;
    po->abind[i].index = (int)index;
}

GLint glGetAttribLocation(GLuint program, const GLchar *name)
{
    glsl_program *gp = gp_of(program);
    if (gp) return glsl_attrib_location(gp, name);
    if (!strcmp(name, "position") || !strcmp(name, "a_position") || !strcmp(name, "aPosition")) return SGL_ATTRIB_POSITION;
    if (!strcmp(name, "color")    || !strcmp(name, "a_color")    || !strcmp(name, "aColor"))    return SGL_ATTRIB_COLOR;
    if (!strcmp(name, "texcoord") || !strcmp(name, "a_texcoord") || !strcmp(name, "aTexCoord")) return SGL_ATTRIB_TEXCOORD;
    return -1;
}

GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
    glsl_program *gp = gp_of(program);
    if (gp) return glsl_uniform_location(gp, name);
    if (!strcmp(name, "mvp") || !strcmp(name, "u_mvp") || !strcmp(name, "uMVP") || !strcmp(name, "matrix")) return 0;
    if (!strcmp(name, "tex0") || !strcmp(name, "sampler") || !strcmp(name, "u_tex") || !strcmp(name, "texture")) return 1;
    return -1;
}

void glUniform1i(GLint location, GLint v0)
{
    glsl_program *gp = cur_gp();
    if (gp) { glsl_uniform_sampler(gp, location, v0); float f = (float)v0; glsl_uniform(gp, location, &f, 1); return; }
    if (location == 1) C.sampler_unit = v0;
}

void glUniform1iv(GLint location, GLsizei count, const GLint *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        glsl_uniform_sampler(gp, location, value[0]);
        float v[16];
        int n = count < 16 ? count : 16;
        for (int i = 0; i < n; ++i) v[i] = (float)value[i];
        glsl_uniform(gp, location, v, n);
        return;
    }
    if (location == 1) C.sampler_unit = value[0];
}

void glUniform1fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        int n = count < 16 ? count : 16;
        glsl_uniform(gp, location, value, n);
        return;
    }
    (void)location;
}

void glUniform2iv(GLint location, GLsizei count, const GLint *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        float v[32];
        int nvec = count < 16 ? count : 16;
        int nf = nvec * 2;
        for (int i = 0; i < nf; ++i) v[i] = (float)value[i];
        glsl_uniform(gp, location, v, nf);
        return;
    }
    (void)location;
}

void glUniform2fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        int nvec = count < 16 ? count : 16;
        glsl_uniform(gp, location, value, nvec * 2);
        return;
    }
    (void)location;
}

void glUniform3iv(GLint location, GLsizei count, const GLint *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        float v[48];
        int nvec = count < 16 ? count : 16;
        int nf = nvec * 3;
        for (int i = 0; i < nf; ++i) v[i] = (float)value[i];
        glsl_uniform(gp, location, v, nf);
        return;
    }
    (void)location;
}

void glUniform3fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        int nvec = count < 16 ? count : 16;
        glsl_uniform(gp, location, value, nvec * 3);
        return;
    }
    (void)location;
}

void glUniform4f(GLint location, GLfloat a, GLfloat b, GLfloat c, GLfloat d)
{
    glsl_program *gp = cur_gp();
    if (gp) { float v[4] = { a, b, c, d }; glsl_uniform(gp, location, v, 4); return; }
    (void)location; (void)a; (void)b; (void)c; (void)d;
}

void glUniform4iv(GLint location, GLsizei count, const GLint *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        float v[64];
        int nvec = count < 16 ? count : 16;
        int nf = nvec * 4;
        for (int i = 0; i < nf; ++i) v[i] = (float)value[i];
        glsl_uniform(gp, location, v, nf);
        return;
    }
    (void)location;
}

void glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (!value || count <= 0) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        int nvec = count < 16 ? count : 16;
        glsl_uniform(gp, location, value, nvec * 4);
        return;
    }
    (void)location;
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)
{
    (void)count;
    if (!value) return;
    glsl_program *gp = cur_gp();
    if (gp) {
        if (!transpose) glsl_uniform(gp, location, value, 16);
        else { float t[16]; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) t[c * 4 + r] = value[r * 4 + c]; glsl_uniform(gp, location, t, 16); }
        return;
    }
    if (location != 0) return;
    if (!transpose) memcpy(C.mvp, value, sizeof(C.mvp));
    else for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) C.mvp[c * 4 + r] = value[r * 4 + c];
    C.mvp_set = 1;
}
