/* softgles2 on-screen statistics overlay.
 *
 * Draws a per-frame HUD (fps, frame time, geometry/fragment counts, pipeline
 * stage breakdown, hart balance, shader kernel-vs-VM share) straight into the
 * framebuffer's colour buffer. It is a debugging instrument for the renderer
 * itself, so it deliberately bypasses the GL pipeline: no GL state is touched,
 * no program is bound, nothing is allocated. That keeps it usable when the
 * thing being debugged is the pipeline.
 *
 * Renderer-generic on purpose (see the softgles2 "stay generic" rule): every
 * figure shown here is a GLES2/rasteriser concept. Application-level counts the
 * renderer cannot know -- lights, entities, visible surfaces -- are pushed in by
 * the host with sgl_overlay_set_stat() and displayed verbatim.
 *
 * The counters come from the sg_prof accumulators, which only run when
 * sgl_prof_enabled is set, so sgl_overlay_enable() turns profiling on. It also
 * sets sgl_prof_quiet, because the serial [rprof] block is ~8 lines per frame
 * and an overlay is precisely the case where you do not want that.
 */
#ifndef SOFTGLES2_SGL_OVERLAY_H
#define SOFTGLES2_SGL_OVERLAY_H

#include "sgl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SGL_OVERLAY_TOP_LEFT     = 0,
    SGL_OVERLAY_TOP_RIGHT    = 1,
    SGL_OVERLAY_BOTTOM_LEFT  = 2,
    SGL_OVERLAY_BOTTOM_RIGHT = 3
};

/* Enable/disable. Enabling also enables (quiet) profiling; disabling restores
 * whatever the profiler was doing before. */
void sgl_overlay_enable(int on);
int  sgl_overlay_enabled(void);

/* Integer pixel scale, 1..4 (default 1). */
void sgl_overlay_set_scale(int scale);
/* One of SGL_OVERLAY_*, default TOP_LEFT. */
void sgl_overlay_set_corner(int corner);

/* Publish an application-level counter, e.g. sgl_overlay_set_stat("lights", 7).
 * `name` is displayed verbatim and never interpreted; it must stay valid (use a
 * literal). Setting the same name again updates it. Capacity is small and fixed
 * (SGL_OVERLAY_MAX_STATS); further distinct names are ignored. */
#define SGL_OVERLAY_MAX_STATS 6
void sgl_overlay_set_stat(const char *name, long value);

/* Draw the HUD into `fb`. Call once per frame at present time: after all
 * rendering (and after any frame digest -- this writes pixels and would
 * otherwise change the hash), and before the scanout copy. */
void sgl_overlay_draw(sgl_framebuffer *fb);

#ifdef __cplusplus
}
#endif

#endif /* SOFTGLES2_SGL_OVERLAY_H */
