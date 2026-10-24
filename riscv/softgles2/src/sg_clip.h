/* Generic near-plane clipping in homogeneous clip space.
 *
 * GLES2 has no user clip planes, but the near plane is mandatory: a vertex with
 * clip-space z + w < 0 is behind the near plane (and, for w <= 0, behind the
 * camera). After the perspective divide such a vertex projects to a mirrored /
 * infinite screen position, so the triangle must be clipped BEFORE the divide.
 *
 * This is deliberately pipeline-agnostic (no GL state, no RVV, no Doom3
 * specifics) so it can be reused by any software GLES2 backend.
 */
#ifndef SOFTGLES2_SG_CLIP_H
#define SOFTGLES2_SG_CLIP_H

#include "sg_core.h"

/* Near-plane clip of one triangle. Keeps the half-space z + w >= 0.
 * Sutherland-Hodgman against the single near plane turns a triangle into a
 * convex polygon of 0, 3, or 4 vertices (fan: 0-1-2 and, if 4, 0-2-3).
 *
 *   in_pos[i]  clip-space xyzw of input vertex i (i = 0..2)
 *   in_vary[i] nvary RAW (not perspective-divided) varying floats for vertex i
 *   nvary      varying float count (0 .. SGL_MAX_VARY)
 *   out_pos[j] clip-space xyzw of output vertex j
 *   out_vary   flat [4 * nvary]; output vertex j's varyings at out_vary + j*nvary
 *
 * Position and varyings are interpolated LINEARLY in clip space at each cut
 * (perspective correction stays the rasteriser's job, via invw). Returns the
 * output vertex count: 0 (fully behind), 3, or 4. */
int sgl_clip_near_tri(const float in_pos[3][4], const float *const in_vary[3],
                      int nvary, float out_pos[4][4], float *out_vary);

/* Max output vertices from the guard-band frustum clip (triangle vs 5 planes). */
#define SGL_CLIP_MAXV 8

/* Guard-band frustum clip: near plane + the four side planes at |x|,|y| <= guard*w
 * (guard >= 1 leaves all in-viewport geometry untouched). Bounds the projected
 * screen coordinates of grazing triangles (tiny positive w) so the rasteriser
 * does not lose edge-function precision and paint garbage. Output is a convex
 * polygon of 0 or 3..SGL_CLIP_MAXV vertices (consume as a fan). Same linear
 * clip-space interpolation contract as sgl_clip_near_tri. */
int sgl_clip_frustum_tri(const float in_pos[3][4], const float *const in_vary[3],
                         int nvary, float out_pos[SGL_CLIP_MAXV][4], float *out_vary,
                         float guard);

/* True if a clip-space vertex is behind the near plane (z + w < 0), i.e. the
 * triangle that contains it needs clipping. */
static inline int sgl_clip_vert_behind_near(const float clip[4])
{
    return (clip[2] + clip[3]) < 0.f;
}

#endif /* SOFTGLES2_SG_CLIP_H */
