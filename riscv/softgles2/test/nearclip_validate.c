/* Host unit test for the generic near-plane clipper (sgl_clip_near_tri).
 *
 * Pure clip-space math, no GL/RVV/QEMU — the fast correctness gate for C3.
 * Verifies vertex counts, that intersections land exactly on the near plane
 * (z + w == 0), and that varyings interpolate linearly at the cuts. */
#include <stdio.h>
#include <math.h>
#include "sg_clip.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)
#define NEARF(a, b) (fabsf((a) - (b)) < 1e-4f)

/* Two varyings per vertex: [0] = a per-vertex ramp (to check interpolation),
 * [1] = a constant passthrough. */
static void run_case(const char *name, const float pos[3][4], const float vary[3][2],
                     int expect_n)
{
    const float *vp[3] = { vary[0], vary[1], vary[2] };
    float out_pos[4][4];
    float out_vary[4 * 2];
    int n = sgl_clip_near_tri(pos, vp, 2, out_pos, out_vary);

    printf("[nearclip] %-10s n=%d (expect %d)\n", name, n, expect_n);
    CHECK(n == expect_n, name);

    /* Every output vertex must be inside the kept half-space (z + w >= -eps). */
    for (int i = 0; i < n; ++i) {
        float zpw = out_pos[i][2] + out_pos[i][3];
        CHECK(zpw >= -1e-4f, "output vertex inside near half-space");
    }
}

int main(void)
{
    /* Case A: all three in front of the near plane -> unchanged triangle. */
    {
        float pos[3][4] = { { 0.f, 0.5f, 0.5f, 1.f },
                            { -0.5f, -0.5f, 0.5f, 1.f },
                            { 0.5f, -0.5f, 0.5f, 1.f } };
        float vary[3][2] = { { 0.f, 7.f }, { 10.f, 7.f }, { 20.f, 7.f } };
        const float *vp[3] = { vary[0], vary[1], vary[2] };
        float op[4][4], ov[8];
        int n = sgl_clip_near_tri(pos, vp, 2, op, ov);
        printf("[nearclip] %-10s n=%d (expect 3)\n", "all_front", n);
        CHECK(n == 3, "all_front count");
        for (int i = 0; i < 3; ++i) {
            for (int c = 0; c < 4; ++c) CHECK(NEARF(op[i][c], pos[i][c]), "all_front pos passthrough");
            CHECK(NEARF(ov[i * 2 + 0], vary[i][0]), "all_front vary passthrough");
            CHECK(NEARF(ov[i * 2 + 1], 7.f), "all_front const vary");
        }
    }

    /* Case B: one vertex behind near (v2) -> quad (4 verts). Intersections at
     * t=0.6 (edge 1->2) and t=0.4 (edge 2->0); ramp -> 16 and 12. */
    {
        float pos[3][4] = { { 0.f, 0.5f, 0.5f, 1.f },     /* d = 1.5 in  */
                            { -0.5f, -0.5f, 0.5f, 1.f },   /* d = 1.5 in  */
                            { 0.5f, -0.5f, -2.f, 1.f } };  /* d = -1  out */
        float vary[3][2] = { { 0.f, 7.f }, { 10.f, 7.f }, { 20.f, 7.f } };
        const float *vp[3] = { vary[0], vary[1], vary[2] };
        float op[4][4], ov[8];
        int n = sgl_clip_near_tri(pos, vp, 2, op, ov);
        printf("[nearclip] %-10s n=%d (expect 4)\n", "one_behind", n);
        CHECK(n == 4, "one_behind count");
        /* out order: v0, v1, I12, I20 */
        CHECK(NEARF(op[0][1], 0.5f), "one_behind v0");
        CHECK(NEARF(op[1][1], -0.5f), "one_behind v1");
        CHECK(NEARF(op[2][2] + op[2][3], 0.f), "I12 on near plane");
        CHECK(NEARF(op[3][2] + op[3][3], 0.f), "I20 on near plane");
        CHECK(NEARF(ov[2 * 2 + 0], 16.f), "I12 ramp interpolation");
        CHECK(NEARF(ov[3 * 2 + 0], 12.f), "I20 ramp interpolation");
        CHECK(NEARF(ov[2 * 2 + 1], 7.f) && NEARF(ov[3 * 2 + 1], 7.f), "const vary at cuts");
    }

    /* Case C: two vertices behind near (v1, v2) -> triangle (3 verts). */
    {
        float pos[3][4] = { { 0.f, 0.5f, 0.5f, 1.f },     /* d = 1.5 in  */
                            { -0.5f, -0.5f, -2.f, 1.f },   /* d = -1  out */
                            { 0.5f, -0.5f, -2.f, 1.f } };  /* d = -1  out */
        float vary[3][2] = { { 0.f, 7.f }, { 10.f, 7.f }, { 20.f, 7.f } };
        run_case("two_behind", pos, vary, 3);
    }

    /* Case D: all three behind near -> fully clipped (0 verts). */
    {
        float pos[3][4] = { { 0.f, 0.5f, -2.f, 1.f },
                            { -0.5f, -0.5f, -2.f, 1.f },
                            { 0.5f, -0.5f, -2.f, 1.f } };
        float vary[3][2] = { { 0.f, 7.f }, { 10.f, 7.f }, { 20.f, 7.f } };
        run_case("all_behind", pos, vary, 0);
    }

    /* Case E: vertex behind the camera (w < 0) -> must still clip cleanly and
     * the kept intersections must have z + w == 0 with w > 0. */
    {
        float pos[3][4] = { { 0.f, 0.5f, 1.f, 2.f },      /* d = 3   in  */
                            { -0.5f, -0.5f, 1.f, 2.f },    /* d = 3   in  */
                            { 0.5f, -0.5f, 1.f, -1.f } };  /* d = 0?  w<0 */
        /* v2: z+w = 1 + (-1) = 0 -> exactly on plane (treated as inside). Nudge
         * it behind: */
        pos[2][2] = -2.f;   /* z+w = -2 + (-1) = -3 out, w < 0 */
        float vary[3][2] = { { 0.f, 7.f }, { 10.f, 7.f }, { 20.f, 7.f } };
        const float *vp[3] = { vary[0], vary[1], vary[2] };
        float op[4][4], ov[8];
        int n = sgl_clip_near_tri(pos, vp, 2, op, ov);
        printf("[nearclip] %-10s n=%d (expect 4)\n", "w_negative", n);
        CHECK(n == 4, "w_negative count");
        for (int i = 0; i < n; ++i) {
            float zpw = op[i][2] + op[i][3];
            CHECK(zpw >= -1e-4f, "w_negative output inside half-space");
        }
    }

    if (fails == 0) printf("[nearclip] ALL PASS\n");
    else            printf("[nearclip] %d CHECK(S) FAILED\n", fails);
    return fails ? 1 : 0;
}
