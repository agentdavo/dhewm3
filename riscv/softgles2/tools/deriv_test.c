/* deriv_test.c -- host unit test for softgles2's ANALYTIC screen-space UV
 * derivatives (the load-bearing input to mip-LOD selection + anisotropy).
 *
 * The rasteriser interpolates a texcoord varying perspective-correctly as
 *
 *     u(X,Y) = (Sum_i w_i * p_i) / (Sum_i w_i * iw_i),
 *
 * where w_i = e_i(X,Y) * invArea are the barycentric weights, e_i are the linear
 * edge functions, iw_i = 1/w of vertex i, and p_i = u_i * iw_i is the
 * premultiplied plane coefficient (exactly the rasteriser's vpa/vpb/vpc). The
 * sampler used to fake d(u)/d(x) from two lanes of one scanline (missing the
 * vertical partial and breaking on vcompress-packed batches). It now uses the
 * closed-form partials computed by sgl_vary_screen_deriv() in sg_core.h.
 *
 * This test builds the SAME plane form the rasteriser builds (sg_raster.c:
 * Ax_i/Ay_i and the row_e_i edge formula), calls the REAL engine helper at
 * several interior points of two triangles (one nearly affine, one strongly
 * foreshortened like a Doom floor), and compares every partial to a central
 * finite difference of the perspective-correct u(X,Y),v(X,Y). Passes when the
 * relative error is below TOL for every partial at every point.
 *
 * Build (host):
 *   gcc -O2 -Wall -Wextra -I../include -I../src -o deriv_test deriv_test.c
 * (from riscv/softgles2/tools). No qemu, no RVV: pure scalar math.
 */
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "sg_core.h"   /* the engine's own sgl_vary_screen_deriv() */

/* Relative-error tolerance. u(X,Y) is a ratio of linears, so the central
 * difference carries an O(h^2) truncation error; with h = 1e-3 px this leaves a
 * comfortable margin below 1e-3 even on the strongly foreshortened triangle. */
#define TOL 1e-3

typedef struct { float x, y, invw, u, v; } vtx;

/* Perspective-correct interpolation of (u,v) at screen (X,Y), mirroring the
 * rasteriser exactly (edge functions -> barycentric weights -> premul / denom). */
static void interp_uv(const vtx *a, const vtx *b, const vtx *c, float invArea,
                      double X, double Y, double *u, double *v)
{
    double e0 = (double)(c->x - b->x) * (Y - b->y) - (double)(c->y - b->y) * (X - b->x);
    double e1 = (double)(a->x - c->x) * (Y - c->y) - (double)(a->y - c->y) * (X - c->x);
    double e2 = (double)(b->x - a->x) * (Y - a->y) - (double)(b->y - a->y) * (X - a->x);
    double w0 = e0 * invArea, w1 = e1 * invArea, w2 = e2 * invArea;
    double denom = w0 * a->invw + w1 * b->invw + w2 * c->invw;
    double nu = w0 * (double)a->u * a->invw + w1 * (double)b->u * b->invw + w2 * (double)c->u * c->invw;
    double nv = w0 * (double)a->v * a->invw + w1 * (double)b->v * b->invw + w2 * (double)c->v * c->invw;
    *u = nu / denom;
    *v = nv / denom;
}

static int check(const char *tag, double analytic, double fd, int *fail)
{
    double denom = fabs(fd) > 1e-6 ? fabs(fd) : 1e-6;
    double rel = fabs(analytic - fd) / denom;
    int ok = rel < TOL;
    printf("    %-6s analytic=% .8f  fd=% .8f  rel=%.2e  %s\n",
           tag, analytic, fd, rel, ok ? "ok" : "FAIL");
    if (!ok) (*fail)++;
    return ok;
}

static void run_tri(const char *name, vtx a, vtx b, vtx c, int *fail)
{
    printf("triangle %s\n", name);
    float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area < 0.f) { vtx t = b; b = c; c = t; area = -area; }   /* rasteriser winding fix */
    float invArea = 1.f / area;

    /* Plane form exactly as sg_raster.c builds it. */
    float Ax[3] = { -(c.y - b.y), -(a.y - c.y), -(b.y - a.y) };
    float Ay[3] = {  (c.x - b.x),  (a.x - c.x),  (b.x - a.x) };
    float iw[3] = { a.invw, b.invw, c.invw };
    float pu[3] = { a.u * a.invw, b.u * b.invw, c.u * c.invw };
    float pv[3] = { a.v * a.invw, b.v * b.invw, c.v * c.invw };

    /* A few interior sample points (barycentric-ish blends of the vertices). */
    const double bc[][3] = {
        { 0.34, 0.33, 0.33 }, { 0.60, 0.25, 0.15 },
        { 0.20, 0.20, 0.60 }, { 0.15, 0.70, 0.15 },
    };
    const double h = 1e-3;   /* central-difference step in pixels */

    for (int s = 0; s < 4; ++s) {
        double X = bc[s][0] * a.x + bc[s][1] * b.x + bc[s][2] * c.x;
        double Y = bc[s][0] * a.y + bc[s][1] * b.y + bc[s][2] * c.y;

        /* Edge values e_i at (X,Y) for the analytic helper. */
        float e[3];
        e[0] = (float)((double)(c.x - b.x) * (Y - b.y) - (double)(c.y - b.y) * (X - b.x));
        e[1] = (float)((double)(a.x - c.x) * (Y - c.y) - (double)(a.y - c.y) * (X - c.x));
        e[2] = (float)((double)(b.x - a.x) * (Y - a.y) - (double)(b.y - a.y) * (X - a.x));

        float dudx, dudy, dvdx, dvdy;
        sgl_vary_screen_deriv(e, Ax, Ay, invArea, iw, pu, &dudx, &dudy);
        sgl_vary_screen_deriv(e, Ax, Ay, invArea, iw, pv, &dvdx, &dvdy);

        /* Central finite differences of the reference interpolation. */
        double up, um, vp, vm;
        interp_uv(&a, &b, &c, invArea, X + h, Y, &up, &vp);
        interp_uv(&a, &b, &c, invArea, X - h, Y, &um, &vm);
        double fd_dudx = (up - um) / (2 * h), fd_dvdx = (vp - vm) / (2 * h);
        interp_uv(&a, &b, &c, invArea, X, Y + h, &up, &vp);
        interp_uv(&a, &b, &c, invArea, X, Y - h, &um, &vm);
        double fd_dudy = (up - um) / (2 * h), fd_dvdy = (vp - vm) / (2 * h);

        printf("  point %d  (X=%.2f Y=%.2f)\n", s, X, Y);
        check("du/dx", dudx, fd_dudx, fail);
        check("du/dy", dudy, fd_dudy, fail);
        check("dv/dx", dvdx, fd_dvdx, fail);
        check("dv/dy", dvdy, fd_dvdy, fail);
    }
}

int main(void)
{
    int fail = 0;
    printf("softgles2 analytic UV-derivative test (tol rel < %.0e)\n\n", (double)TOL);

    /* 1) Nearly affine: all w equal, so u,v are linear -> exact match expected. */
    run_tri("affine",
        (vtx){  40.f,  50.f, 1.0f, 0.0f, 0.0f },
        (vtx){ 300.f,  70.f, 1.0f, 1.0f, 0.0f },
        (vtx){  90.f, 360.f, 1.0f, 0.2f, 1.0f }, &fail);

    /* 2) Strongly foreshortened (Doom floor/ceiling): the far vertex has a large
     *    w, so invw and thus the perspective term vary sharply across the tri --
     *    exactly where the old du/dx-only estimate was worst. */
    run_tri("foreshortened",
        (vtx){  60.f, 420.f, 1.000f,  0.0f, 0.0f },
        (vtx){ 520.f, 430.f, 0.125f,  6.0f, 0.0f },
        (vtx){ 300.f,  40.f, 0.070f,  3.0f, 8.0f }, &fail);

    /* 3) Rotated/skewed with unequal w and non-axis-aligned uv gradients, so
     *    both partials of both varyings are non-trivial. */
    run_tri("skewed",
        (vtx){ 120.f,  90.f, 0.9f,  0.1f, 0.3f },
        (vtx){ 480.f, 180.f, 0.4f,  2.5f, 0.7f },
        (vtx){ 200.f, 400.f, 0.6f,  1.0f, 4.0f }, &fail);

    printf("\n%s (%d failing partials)\n", fail ? "FAILED" : "PASSED", fail);
    return fail ? 1 : 0;
}
