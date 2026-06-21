/* Draw dispatch (glDrawArrays / glDrawElements).
 *
 * Implements vertex fetching, the fixed-function built-in pipeline, and the
 * GLSL program-driven draw path (via sg_shader's gp_of/cur_gp helpers).
 * Debug counters are defined here.
 */
#ifndef SOFTGLES2_SG_DRAW_H
#define SOFTGLES2_SG_DRAW_H

#include "sgl_gl.h"

extern volatile unsigned long sgl_dbg_draw_glsl;
extern volatile unsigned long sgl_dbg_draw_builtin;
extern volatile unsigned long sgl_dbg_draw_fbo;
extern volatile unsigned long sgl_dbg_draw_default;
extern volatile unsigned long sgl_dbg_draw_scalar;
extern volatile unsigned long sgl_dbg_draw_simt;

void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
void glFlush(void);
void glFinish(void);

#endif /* SOFTGLES2_SG_DRAW_H */
