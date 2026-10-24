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
 * Default OFF: Doom's interaction pipeline has order-sensitive mid-frame
 * program/texture/depth transitions that the current deferred snapshot does
 * not preserve completely. Tests may enable it explicitly.
 */
#ifndef SOFTGLES2_SG_FRAMEBIN_H
#define SOFTGLES2_SG_FRAMEBIN_H

#include "sgl_gl.h"
#include "sg_core.h"
#include "sg_mt.h"
#include "glsl.h"

extern int sgl_framebin_enabled;   /* 0 = immediate, 1 = deferred */

/* Record a programmable draw. pv must be frame-arena owned (outlives the draw).
 * Deep-copies the draw packet, fragment ctx and a uniform snapshot. Returns 1. */
int  sg_framebin_record_draw(const sg_draw_packet *draw, GLenum mode, GLsizei count,
                             const sgl_pvert *pv, glsl_program *gp);

/* Record a clear (already-packed color + depth + stencil + buffer mask). */
void sg_framebin_record_clear(sgl_framebuffer *fb, GLbitfield mask,
                              uint32_t color, float depth, unsigned char stencil);

/* Drain all recorded commands. Idempotent; resets the per-frame record. */
void sg_framebin_flush(void);

/* Ordering barrier: drain the record NOW because something is about to break the
 * deferral's assumptions. Deferral is only sound while (a) nothing a recorded
 * command still points at is mutated, and (b) nobody observes the framebuffer
 * before the commands that write it have run. A recorded draw holds sgl_tex
 * POINTERS in its fragment ctx, not texel copies, so any mid-frame texture
 * upload or parameter change would retroactively alter draws already recorded;
 * and glReadPixels / glCopyTex* read a framebuffer those draws have not yet
 * touched. Doom 3 does exactly this for _currentRender (heat haze, mirrors).
 *
 * Calling this before such an operation restores correct ordering at the cost of
 * ending the current batch early -- rare in practice, so the parallelism is kept
 * for the long uninterrupted interaction runs that dominate the frame.
 * No-op when deferral is off or nothing is recorded. */
void sg_framebin_barrier(void);

#endif /* SOFTGLES2_SG_FRAMEBIN_H */
