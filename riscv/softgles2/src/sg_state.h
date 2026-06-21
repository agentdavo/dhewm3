/* Pipeline state: GL context struct, state setters, shared debug helpers.
 *
 * Defines the single gl_ctx "C" that every softgles2 module reads/writes
 * directly.  State-setter GL entry points live here; draw/program/texture
 * modules include this header to reach the context.
 */
#ifndef SOFTGLES2_SG_STATE_H
#define SOFTGLES2_SG_STATE_H

#include "sgl_gl.h"
#include "sg_core.h"

#if !defined(SGL_BAREMETAL)
#  include <stdio.h>
#  include <stdlib.h>
#  define GLTRACE(...) do { if (getenv("SGL_TRACE")) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#  define GLTRACE(...) ((void)0)
#endif

/* Implementation limits (used by the gl_ctx struct and static arrays). */
#define MAX_TEXUNITS 8

/* Per-vertex attribute slot (bound to C.attribs[slot]). */
typedef struct {
    int      enabled, size;
    GLenum   type;
    int      normalized, stride;
    const void *ptr;
    GLuint   buffer;
    float    cur[4];
    int      has_cur;
} attrib_t;

/* The entire GL context — a single instance allocated as "C". */
typedef struct {
    sgl_framebuffer *default_fb;
    sgl_framebuffer *fb;
    GLuint  framebuffer, renderbuffer;
    GLint   vp_x, vp_y; GLsizei vp_w, vp_h;
    float   depth_near, depth_far;   /* glDepthRangef; default [0,1] (window-Z = near + z01*(far-near)) */
    float   clear_r, clear_g, clear_b, clear_a, clear_depth;
    GLint   clear_stencil;
    int     depth_test, cull_face, front_ccw;
    int     stencil_test;
    int     depth_func;
    int     depth_write;
    sgl_stencil_face stencil_front, stencil_back;
    int     color_mask;
    int     scissor_on;
    GLint   scissor_x, scissor_y; GLsizei scissor_w, scissor_h;
    int     depth_bounds_on;            /* GL_EXT_depth_bounds_test */
    float   depth_bounds_min, depth_bounds_max;  /* window-Z range; cull where dst depth outside */
    float   poly_off_factor, poly_off_units; int poly_off_on;
    GLenum  cull_mode;
    int     blend_on;
    int     blend_mode;
    GLenum  blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLenum  blend_eq_rgb, blend_eq_alpha;
    float   blend_color[4];
    GLuint  array_buffer, element_buffer;
    attrib_t attribs[SGL_MAX_VERTEX_ATTRIBS];
    GLuint  active_unit;
    GLuint  unit_tex[MAX_TEXUNITS];    /* GL_TEXTURE_2D binding per unit       */
    GLuint  unit_cube[MAX_TEXUNITS];   /* GL_TEXTURE_CUBE_MAP binding per unit  */
    GLint   pack_alignment, unpack_alignment;
    GLuint  cur_program;
    float   mvp[16];
    int     mvp_set;
    GLint   sampler_unit;
    GLenum  error;
} gl_ctx;

/* The single context instance (defined in sg_state.c). */
extern gl_ctx C;

/* ---- shared internal helpers ---- */
void set_err(GLenum e);

/* ---- state setter API (declared public in sgl_gl.h) ---- */
void sglMakeCurrent(sgl_framebuffer *fb);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepthf(GLclampf d);
void glClearStencil(GLint s);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glDepthRangef(GLclampf n, GLclampf f);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void glDepthBoundsEXT(GLclampf zmin, GLclampf zmax);   /* GL_EXT_depth_bounds_test */
void glPolygonOffset(GLfloat factor, GLfloat units);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
void glBlendEquation(GLenum mode);
void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
void glBlendColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask);
void glStencilMask(GLuint mask);
void glStencilMaskSeparate(GLenum face, GLuint mask);
void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void glStencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass);
GLenum glGetError(void);
const GLubyte *glGetString(GLenum name);
void glGetIntegerv(GLenum pname, GLint *data);
void glGetFloatv(GLenum pname, GLfloat *data);
void glGetBooleanv(GLenum pname, GLboolean *data);
void glClear(GLbitfield mask);
void glEnableVertexAttribArray(GLuint i);
void glDisableVertexAttribArray(GLuint i);
void glVertexAttrib1f(GLuint i, GLfloat x);
void glVertexAttrib2f(GLuint i, GLfloat x, GLfloat y);
void glVertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z);
void glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertexAttrib1fv(GLuint i, const GLfloat *v);
void glVertexAttrib2fv(GLuint i, const GLfloat *v);
void glVertexAttrib3fv(GLuint i, const GLfloat *v);
void glVertexAttrib4fv(GLuint i, const GLfloat *v);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *ptr);
void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *ptr);

/* ---- sg_state_inputs + pack function (existing contract) ---- */
typedef struct {
    int depth_test;
    int blend_mode;
    int depth_func;
    int depth_write;
    int color_mask;
    int stencil_test;
    sgl_stencil_face stencil_front;
    sgl_stencil_face stencil_back;
    int scissor_on;
    GLint scissor_x, scissor_y;
    GLsizei scissor_w, scissor_h;
    int depth_bounds_on;
    float depth_bounds_min, depth_bounds_max;
    sgl_framebuffer *fb;
    sgl_blend_state blendf;
    float poly_off_factor, poly_off_units;
    int   poly_off_on;
} sg_state_inputs;

void sg_state_pack_drawstate(sgl_drawstate *out, const sg_state_inputs *in);
void sg_state_inputs_from_context(sg_state_inputs *out);

#endif /* SOFTGLES2_SG_STATE_H */
