#include "sg_clip.h"

/* Sutherland-Hodgman against the single near plane (keep z + w >= 0).
 *
 * Walk the triangle's edges (0->1->2->0). For each edge, emit the start vertex
 * if it is inside, then emit the intersection if the edge crosses the plane.
 * A triangle clipped by one plane yields 0, 3, or 4 output vertices:
 *   all inside      -> 3 (unchanged)
 *   all outside     -> 0
 *   1 inside,2 out  -> 3 (1 original + 2 intersections)
 *   2 inside,1 out  -> 4 (2 originals + 2 intersections) */
int sgl_clip_near_tri(const float in_pos[3][4], const float *const in_vary[3],
                      int nvary, float out_pos[4][4], float *out_vary)
{
    float d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = in_pos[i][2] + in_pos[i][3];   /* signed distance to near plane: z + w */

    int n = 0;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        int in_i = d[i] >= 0.f;
        int in_j = d[j] >= 0.f;

        if (in_i) {
            for (int c = 0; c < 4; ++c) out_pos[n][c] = in_pos[i][c];
            for (int k = 0; k < nvary; ++k) out_vary[n * nvary + k] = in_vary[i][k];
            ++n;
        }
        if (in_i != in_j) {
            /* Edge crosses the plane. d[i] - d[j] != 0 because the signs differ.
             * t in [0,1] is the parametric crossing along i->j. */
            float t = d[i] / (d[i] - d[j]);
            for (int c = 0; c < 4; ++c)
                out_pos[n][c] = in_pos[i][c] + t * (in_pos[j][c] - in_pos[i][c]);
            for (int k = 0; k < nvary; ++k)
                out_vary[n * nvary + k] = in_vary[i][k] + t * (in_vary[j][k] - in_vary[i][k]);
            ++n;
            if (n >= 4) break;   /* convex result of one plane is at most a quad */
        }
    }
    return n;
}
