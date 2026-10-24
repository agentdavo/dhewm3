/*
 * test_transcendentals.c  --  host accuracy gate for softgles2's GLSL ES
 * transcendental builtins (sin/cos/tan/exp/log/exp2/log2/radians/degrees).
 *
 * These functions have no libm in the bare-metal engine; the scalar tree-walk
 * (glsl.c) uses the deterministic kernels replicated verbatim below, and the
 * RVV SIMT path uses their lane-parallel m2 replicas. This standalone test
 * proves the SCALAR math (the correctness reference both lanes track) against
 * libm without needing qemu.
 *
 *   build:  gcc -O2 -o test_transcendentals test_transcendentals.c -lm
 *   run:    ./test_transcendentals    (exit 0 = pass)
 *
 * The exp2/log2 kernels are LOW-degree (4th) minimax polys chosen for TCG
 * speed, so their tolerance is coarser than an IEEE libm; the bounds below are
 * the "accuracy convention of the existing pow/exp2/log2" the task refers to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ---- verbatim from sg_math.h (scalar kernels) ---- */
static inline float sgl_math_floor_f(float x)
{
    int i = (int)x;
    return (x < 0.f && (float)i != x) ? (float)(i - 1) : (float)i;
}
static inline float sgl_math_log2_f(float x)
{
    union { float f; unsigned u; } v;
    v.f = x;
    float e = (float)((int)((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;
    float m = v.f;
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f + (0.4471888f - 0.0566510f * m) * m) * m) * m);
}
static inline float sgl_math_exp2_f(float x)
{
    float xi = sgl_math_floor_f(x);
    float xf = x - xi;
    float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * (0.0555041f + xf * 0.0096181f)));
    union { float f; unsigned u; } v;
    v.f = p;
    v.u += (unsigned)((int)xi << 23);
    return v.f;
}

/* ---- verbatim from glsl.c (scalar transcendental derivations) ---- */
#define G_LOG2E  1.4426950408889634f
#define G_LN2    0.6931471805599453f
#define G_DEG2R  0.017453292519943295f
#define G_R2DEG  57.29577951308232f
#define G_PI_2         1.5707963267948966f
#define G_TWO_OVER_PI  0.63661977236758134f
#define G_SC_S1 (-0.16666667f)
#define G_SC_S2  0.0083333310f
#define G_SC_S3 (-0.00019840874f)
#define G_SC_C1 (-0.5f)
#define G_SC_C2  0.041666418f
#define G_SC_C3 (-0.0013888397f)
static inline float g_exp2(float x) { return sgl_math_exp2_f(x); }
/* accurate log2 (artanh series) as used by the GLSL builtin in glsl.c */
static inline float g_log2(float x) {
    union { float f; unsigned u; } v; v.f = x;
    float e = (float)((int)((v.u >> 23) & 0xFF) - 127);
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u; float m = v.f;
    float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
    float s = 0.11111111f;
    s = 0.14285715f + t2 * s;
    s = 0.2f + t2 * s;
    s = 0.33333334f + t2 * s;
    s = 1.0f + t2 * s;
    return e + (t * s) * 2.8853900817779268f;
}
static inline float g_exp(float x)  { return sgl_math_exp2_f(x * G_LOG2E); }
static inline float g_log(float x)  { return g_log2(x) * G_LN2; }
static inline float g_radians(float x){ return x * G_DEG2R; }
static inline float g_degrees(float x){ return x * G_R2DEG; }
static inline void g_sincos(float a, float *so, float *co)
{
    float t  = a * G_TWO_OVER_PI;
    int   ki = (int)(t + (t >= 0.f ? 0.5f : -0.5f));
    float kf = (float)ki;
    float r  = a - kf * G_PI_2;
    float r2 = r * r;
    float cosv = 1.f + r2 * (G_SC_C1 + r2 * (G_SC_C2 + r2 * G_SC_C3));
    float sinv = r + r * r2 * (G_SC_S1 + r2 * (G_SC_S2 + r2 * G_SC_S3));
    int kq = ki & 3, b0 = kq & 1, b2 = kq & 2;
    int swap = b0 != 0, sinneg = b2 != 0, cosneg = (((b0 << 1) ^ b2) != 0);
    float s_sel = swap ? cosv : sinv;
    float c_sel = swap ? sinv : cosv;
    *so = sinneg ? -s_sel : s_sel;
    *co = cosneg ? -c_sel : c_sel;
}
static inline float g_sin(float x){ float s,c; g_sincos(x,&s,&c); return s; }
static inline float g_cos(float x){ float s,c; g_sincos(x,&s,&c); return c; }
static inline float g_tan(float x){ float s,c; g_sincos(x,&s,&c); return c!=0.f ? s/c : 0.f; }

/* ---- harness ---- */
static int g_fail = 0;
static double relerr(double got, double ref)
{
    double a = fabs(ref);
    return fabs(got - ref) / (a > 1e-6 ? a : 1e-6);
}

static void check_abs(const char *name, float (*mine)(float), double (*ref)(double),
                      double lo, double hi, int steps, double tol)
{
    double maxe = 0.0; double at = lo;
    for (int i = 0; i <= steps; i++) {
        double x = lo + (hi - lo) * i / steps;
        double g = mine((float)x), r = ref(x);
        double e = fabs(g - r);
        if (e > maxe) { maxe = e; at = x; }
    }
    int ok = maxe <= tol;
    printf("  %-9s abs  max=%.3e @x=%.4f   tol=%.1e  %s\n",
           name, maxe, at, tol, ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;
}

static void check_rel(const char *name, float (*mine)(float), double (*ref)(double),
                      double lo, double hi, int steps, double tol)
{
    double maxe = 0.0; double at = lo;
    for (int i = 0; i <= steps; i++) {
        double x = lo + (hi - lo) * i / steps;
        double g = mine((float)x), r = ref(x);
        double e = relerr(g, r);
        if (e > maxe) { maxe = e; at = x; }
    }
    int ok = maxe <= tol;
    printf("  %-9s rel  max=%.3e @x=%.4f   tol=%.1e  %s\n",
           name, maxe, at, tol, ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;
}

static void check_tan(double lo, double hi, int steps, double tol)
{
    double maxe = 0.0; double at = lo;
    for (int i = 0; i <= steps; i++) {
        double x = lo + (hi - lo) * i / steps;
        if (fabs(cos(x)) < 0.15) continue;
        double g = g_tan((float)x), r = tan(x);
        double e = relerr(g, r);
        if (e > maxe) { maxe = e; at = x; }
    }
    int ok = maxe <= tol;
    printf("  %-9s rel  max=%.3e @x=%.4f   tol=%.1e  %s  (|cos|>=0.15)\n",
           "tan", maxe, at, tol, ok ? "PASS" : "FAIL");
    if (!ok) g_fail = 1;
}

static double libm_exp2(double x){ return exp2(x); }
static double libm_log2(double x){ return log2(x); }
static double libm_exp (double x){ return exp(x); }
static double libm_log (double x){ return log(x); }
static double libm_sin (double x){ return sin(x); }
static double libm_cos (double x){ return cos(x); }
static double libm_rad (double x){ return x * M_PI / 180.0; }
static double libm_deg (double x){ return x * 180.0 / M_PI; }

int main(void)
{
    const double PI = 3.14159265358979323846;
    printf("softgles2 transcendental accuracy vs libm (float32 kernels)\n");

    check_abs("sin", g_sin, libm_sin, -4*PI, 4*PI, 200000, 5e-4);
    check_abs("cos", g_cos, libm_cos, -4*PI, 4*PI, 200000, 5e-4);
    check_tan(-3*PI, 3*PI, 200000, 5e-3);

    check_rel("exp2", g_exp2, libm_exp2, -30.0, 30.0, 200000, 5e-3);
    check_rel("exp",  g_exp,  libm_exp,  -20.0, 20.0, 200000, 5e-3);

    check_abs("log2", g_log2, libm_log2, 1e-4, 1e4, 200000, 1e-4);
    check_abs("log",  g_log,  libm_log,  1e-4, 1e4, 200000, 1e-4);

    check_rel("radians", g_radians, libm_rad, -1080.0, 1080.0, 100000, 1e-6);
    check_rel("degrees", g_degrees, libm_deg, -1080.0, 1080.0, 100000, 1e-6);

    printf("%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
    return g_fail;
}
