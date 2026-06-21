/* Vertex stage helpers for the programmable path. */
#ifndef SOFTGLES2_SG_VERTEX_H
#define SOFTGLES2_SG_VERTEX_H

#include "sgl_gl.h"
#include "sg_core.h"
#include "glsl.h"

/* Query how many SIMD lanes the current VS SIMT split policy would pick for a
 * given shader/register pressure point. This is useful for offline policy
 * analysis and should return 1 when the shader is not SIMT-vectorisable.
 */
int sg_vertex_simt_chunk_lanes_for(const glsl_program *gp,
                                  int attr_slots, int used_vary);
int sg_vertex_simt_pressure_score(const glsl_program *gp,
                                  int attr_slots, int used_vary);

typedef void (*sg_vertex_fetch_fn)(void *ctx, int loc, int vi, float *out, int outn, const float *def);

/* Explicit lane-group split policy for SIMT vertex transform.
 * A policy can request quarter-width (m1-ish occupancy), half-width (m1/m2 mix),
 * or full-width (default) vector groups based on shader-side pressure.
 */
typedef struct {
    int quarter_attr_slots;  /* > this attr-slot count -> quarter width */
    int quarter_vary_slots;  /* > this varying-float count -> quarter width */
    int quarter_lanes;       /* lanes to use for quarter width path */
    int half_attr_slots;     /* > this attr-slot count -> half width */
    int half_vary_slots;     /* > this varying-float count -> half width */
    int half_lanes;          /* lanes to use for half width path */
    int full_lanes;          /* lanes for full-width default path */
} sg_vertex_simt_lmul_policy;

sg_vertex_simt_lmul_policy sg_vertex_simt_lmul_policy_default(void);
sg_vertex_simt_lmul_policy sg_vertex_simt_lmul_policy_get(void);
void sg_vertex_simt_lmul_policy_set(const sg_vertex_simt_lmul_policy *policy);

void sg_vertex_prog_cached(GLsizei count, const int *idx, glsl_program *gp,
                          int vstride, sgl_pvert *pv, float *vb,
                          int mvp_set, const float *mvp,
                          int vp_x, int vp_y, int vp_w, int vp_h,
                          float depth_near, float depth_far,
                          sg_vertex_fetch_fn fetch, void *fetch_ctx);

#endif /* SOFTGLES2_SG_VERTEX_H */
