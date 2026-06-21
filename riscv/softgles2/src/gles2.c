/* gles2.c is now decomposed into per-subsystem modules:
 *
 *   sg_state.h/c      — gl_ctx, state setters, glGet*, glClear, vertex attribs
 *   sg_buffer.h/c     — buffer objects
 *   sg_texture.h/c    — texture objects, DXT decoder, ReadPixels
 *   sg_framebuffer.h/c — FBO/renderbuffer objects
 *   sg_shader.h/c     — shader/program objects, uniforms
 *   sg_draw.h/c       — draw dispatch (glDrawArrays / glDrawElements)
 *
 * This file is kept as an include aggregator so the build system does not
 * need to change source lists for targets that #include "gles2.c".
 */
#include "sgl.h"
#include "sgl_gl.h"
#include "sg_core.h"
#include "sg_raster.h"
#include "glsl.h"
#include "sg_mt.h"
#include "sg_frag.h"
#include "sg_state.h"
#include "sg_buffer.h"
#include "sg_texture.h"
#include "sg_framebuffer.h"
#include "sg_shader.h"
#include "sg_draw.h"
