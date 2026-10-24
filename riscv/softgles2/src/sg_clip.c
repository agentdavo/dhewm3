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

/* Guard-band frustum clip: the near plane PLUS the four side planes at a generous
 * guard band |x|,|y| <= guard*w (guard >= 1, so nothing inside the viewport --
 * NDC [-1,1] -- is ever touched). Its job is not viewport culling but NUMERIC
 * safety: a grazing triangle with a vertex whose w is tiny-but-positive is NOT
 * behind the near plane, so the near-only clip passes it through; after the
 * perspective divide that vertex lands tens of thousands of pixels off-screen,
 * and the rasteriser's edge functions then lose all precision and paint garbage.
 * Clipping to the guard band bounds every output screen coord to ~guard*viewport,
 * killing the garbage while leaving all visible geometry bit-identical.
 *
 * Sutherland-Hodgman polygon clip against 5 planes; a triangle grows to at most
 * SGL_CLIP_MAXV vertices, returned as a fan (0-1-2, 0-2-3, ...). Linear clip-space
 * interpolation (perspective correction stays the rasteriser's, via invw). */
static float clip_plane_dist(const float p[4], int plane, float guard)
{
    switch (plane) {
    case 0:  return p[2] + p[3];          /* near:  z + w >= 0 */
    case 1:  return guard * p[3] - p[0];  /* +x:    x <= guard*w */
    case 2:  return guard * p[3] + p[0];  /* -x:    x >= -guard*w */
    case 3:  return guard * p[3] - p[1];  /* +y:    y <= guard*w */
    default: return guard * p[3] + p[1];  /* -y:    y >= -guard*w */
    }
}

int sgl_clip_frustum_tri(const float in_pos[3][4], const float *const in_vary[3],
                         int nvary, float out_pos[SGL_CLIP_MAXV][4], float *out_vary,
                         float guard)
{
    /* two ping-pong polygons: positions + flat varyings */
    float posA[SGL_CLIP_MAXV][4], posB[SGL_CLIP_MAXV][4];
    float varA[SGL_CLIP_MAXV * SGL_MAX_VARY], varB[SGL_CLIP_MAXV * SGL_MAX_VARY];
    if (nvary > SGL_MAX_VARY) nvary = SGL_MAX_VARY;

    int n = 3;
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 4; ++c) posA[i][c] = in_pos[i][c];
        for (int k = 0; k < nvary; ++k) varA[i * nvary + k] = in_vary[i][k];
    }

    float (*sp)[4] = posA, (*dp)[4] = posB;
    float *sv = varA, *dv = varB;

    for (int plane = 0; plane < 5 && n > 0; ++plane) {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            int j = (i + 1) % n;
            float di = clip_plane_dist(sp[i], plane, guard);
            float dj = clip_plane_dist(sp[j], plane, guard);
            int in_i = di >= 0.f, in_j = dj >= 0.f;
            if (in_i && m < SGL_CLIP_MAXV) {
                for (int c = 0; c < 4; ++c) dp[m][c] = sp[i][c];
                for (int k = 0; k < nvary; ++k) dv[m * nvary + k] = sv[i * nvary + k];
                ++m;
            }
            if (in_i != in_j && m < SGL_CLIP_MAXV) {
                float t = di / (di - dj);
                for (int c = 0; c < 4; ++c) dp[m][c] = sp[i][c] + t * (sp[j][c] - sp[i][c]);
                for (int k = 0; k < nvary; ++k)
                    dv[m * nvary + k] = sv[i * nvary + k] + t * (sv[j * nvary + k] - sv[i * nvary + k]);
                ++m;
            }
        }
        n = m;
        float (*tp)[4] = sp; sp = dp; dp = tp;
        float *tv = sv; sv = dv; dv = tv;
    }

    if (n < 3) return 0;
    if (n > SGL_CLIP_MAXV) n = SGL_CLIP_MAXV;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 4; ++c) out_pos[i][c] = sp[i][c];
        for (int k = 0; k < nvary; ++k) out_vary[i * nvary + k] = sv[i * nvary + k];
    }
    return n;
}
