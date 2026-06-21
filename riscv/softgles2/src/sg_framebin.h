/* FrameBin — frame-scoped sort-middle deferred tiled rasteriser.
 *
 * When sgl_framebin_enabled is set, programmable draws and clears are recorded
 * (vertices already transformed, uniforms snapshotted) instead of rasterised
 * immediately. At the frame boundary (present / glFinish / sg_framebin_flush)
 * the whole frame's primitives are binned once into a fixed 64x16 tile grid and
 * drained across all worker harts in a single scheduler run — so every draw,
 * however small, contributes parallel work and there is no per-draw single-hart
 * fallback. Per-tile primitive references are replayed in submission order, so
 * output is pixel-identical to immediate mode (no front-to-back reorder).
 *
 * Default OFF — enabled per build/scene until the mid-frame read/target-switch
 * hazards (Stage 4) are handled for the full engine.
 */
#ifndef SOFTGLES2_SG_FRAMEBIN_H
#define SOFTGLES2_SG_FRAMEBIN_H

#include "sgl_gl.h"
#include "sg_core.h"
#include "sg_mt.h"
#include "glsl.h"

extern int sgl_framebin_enabled;   /* 0 = immediate (default), 1 = deferred */

/* Record a programmable draw. pv must be frame-arena owned (outlives the draw).
 * Deep-copies the draw packet, fragment ctx and a uniform snapshot. Returns 1. */
int  sg_framebin_record_draw(const sg_draw_packet *draw, GLenum mode, GLsizei count,
                             const sgl_pvert *pv, glsl_program *gp);

/* Record a clear (already-packed color + depth + stencil + buffer mask). */
void sg_framebin_record_clear(sgl_framebuffer *fb, GLbitfield mask,
                              uint32_t color, float depth, unsigned char stencil);

/* Drain all recorded commands. Idempotent; resets the per-frame record. */
void sg_framebin_flush(void);

#endif /* SOFTGLES2_SG_FRAMEBIN_H */
