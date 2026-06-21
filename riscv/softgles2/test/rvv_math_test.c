/*
 * rvv_math_test.c — standalone bare-metal QEMU gate for sg_math.h RVV kernels.
 *
 * Header-only dependent (does NOT link glsl.c): exercises the four RVV math
 * kernels directly and compares each against a scalar reference, printing MAX
 * error (relative for rsqrt, absolute for sincos) and ns/element for SIMT vs
 * scalar over the SBI serial console, like simt_validate.c.
 *
 *   rsqrt    : vs 1.0f/__builtin_sqrtf(x)           -> max RELATIVE error
 *              fast (1 NR) ASSERT < 5e-3, accurate (2 NR) ASSERT < 5e-6.
 *              Edge cases: tiny 1e-6, 0.5, 1, 2, 100, 1e6; call-site zero-mask
 *              idiom verified to give exactly 0 for x=0 and x=-1.
 *   sincos   : vs __builtin_sinf/__builtin_cosf, angle in RADIANS.
 *              Covers [-4*PI,4*PI] + a 4096-sample fine sweep + the integer-phase
 *              quadrant points 0/8192/16384/24576 (a=phase*2*PI/32768) and the
 *              exact radian quadrant points 0,PI/2,PI,3PI/2,2PI. ASSERT < 2e-5.
 *   dot4     : vs scalar contracted-FMA dot, ASSERT bit-exact (bitmiss==0).
 *   mat4xvec4: vs scalar fmaf rows in same j-order, ASSERT bit-exact.
 *
 * lean printf has NO %f: all errors are printed scaled to integers as "e-9"
 * (i.e. (unsigned)(err*1e9f)), exactly like simt_validate.c prints e-6.
 */
#include "libc/libc.h"
#include "boot/runtime.h"
#include <riscv_vector.h>
#include "sg_align.h"
#include "sg_math.h"

#define M_PI_F   3.14159265358979323846f
#define TWO_PI_F 6.28318530717958647692f
#define LANES    SGL_SIMT_LANES

static float bench_vx[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_vy[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_vz[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_vw[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_r0[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_r1[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_r2[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_r3[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_a0[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_a1[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_a2[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_a3[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_b0[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_b1[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_b2[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);
static float bench_b3[SGL_SIMT_LANES] SG_ALIGNED_VAR(SG_RVV_ALIGN);

/* ----- scalar references (freestanding: no libm link) -----
 * sin/cos: an INDEPENDENT double-precision Cephes-style oracle (separate code
 * path and higher precision than the f32 kernel under test). Range-reduce to
 * [-PI/4,PI/4] then a deg-13/deg-12 double minimax — accurate to ~1e-12, far
 * tighter than the 2e-5 test bound, so it is a fair oracle for the f32 kernel.
 * Uses only +,-,*,/ and hardware double fma/round (no libcalls). */
static inline float ref_rsqrt(float x) { return 1.0f / __builtin_sqrtf(x); }

#define REF_PI_2 1.5707963267948966192313216916398
#define REF_2_PI 0.63661977236758134307553505349006

/* round double to nearest integer (ties handled adequately for this range)
 * via a sign-correct truncation; |a*2/PI| stays small over the tested range. */
static inline double ref_round(double v)
{
    double t = (double)(long long)(v + (v >= 0.0 ? 0.5 : -0.5));
    return t;
}

static void ref_sincos(float xf, float *so, float *co)
{
    double a = (double)xf;
    double k = ref_round(a * REF_2_PI);
    double r = a - k * REF_PI_2;          /* r in [-PI/4, PI/4] */
    double r2 = r * r;
    /* sin(r) double minimax (odd, through r^13) */
    double s = r * (1.0
        + r2 * (-1.6666666666666666e-01
        + r2 * ( 8.3333333333333333e-03
        + r2 * (-1.9841269841269841e-04
        + r2 * ( 2.7557319223985891e-06
        + r2 * (-2.5052108385441719e-08
        + r2 * ( 1.6059043836821615e-10)))))));
    /* cos(r) double minimax (even, through r^12) */
    double c = 1.0
        + r2 * (-5.0000000000000000e-01
        + r2 * ( 4.1666666666666667e-02
        + r2 * (-1.3888888888888889e-03
        + r2 * ( 2.4801587301587302e-05
        + r2 * (-2.7557319223985891e-07
        + r2 * ( 2.0876756987868099e-09))))));
    long q = (long)k & 3;
    double sin_v, cos_v;
    switch (q) {
        case 0:  sin_v =  s; cos_v =  c; break;
        case 1:  sin_v =  c; cos_v = -s; break;
        case 2:  sin_v = -s; cos_v = -c; break;
        default: sin_v = -c; cos_v =  s; break;
    }
    *so = (float)sin_v;
    *co = (float)cos_v;
}
static inline float ref_sin(float x) { float s, c; ref_sincos(x, &s, &c); return s; }
static inline float ref_cos(float x) { float s, c; ref_sincos(x, &s, &c); return c; }

/* deterministic xorshift PRNG in [-1,1) for dot4/mat4 random inputs */
static unsigned g_seed = 2463534242u;
static float rnd(void)
{
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return (float)((int)(g_seed >> 9)) / (float)(1 << 22) - 1.0f;
}

static inline float fabsf_(float x) { return x < 0.0f ? -x : x; }
static inline uint32_t ns_per_elem(uint64_t ticks, uint64_t elems)
{
    return (uint32_t)(ticks * 1000000000ull / TIMEBASE_HZ / elems);
}
static inline int   bits_eq(float a, float b)   /* +0/-0 treated equal */
{
    union { float f; uint32_t u; } A, B; A.f = a; B.f = b;
    if (A.u == B.u) return 1;
    return ((A.u & 0x7fffffffu) == 0) && ((B.u & 0x7fffffffu) == 0);
}

int kmain(void)
{
    printf("\n==== sg_math kernels validation (rva23s64) ====\n");
    int vl = (int)__riscv_vsetvl_e32m1((size_t)LANES);

    /* ===================== RSQRT ===================== */
    /* positive accuracy sweep (avoid 0 / negatives, which the caller masks) */
    static const float xs[] = {
        1e-6f, 1e-4f, 0.01f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f,
        1.5f, 2.0f, 3.0f, 7.0f, 16.0f, 100.0f, 1e4f, 1e6f
    };
    const int NX = (int)(sizeof(xs) / sizeof(xs[0]));
    float rq_fast = 0.0f, rq_acc = 0.0f;
    {
        float in[LANES], of[LANES], oa[LANES];
        for (int base = 0; base < NX; base += vl) {
            int m = NX - base; if (m > vl) m = vl;
            for (int i = 0; i < vl; i++) in[i] = xs[(base + (i % m))];   /* fill lanes */
            vfloat32m1_t x  = __riscv_vle32_v_f32m1(in, (size_t)vl);
            __riscv_vse32_v_f32m1(of, sgl_rvv_rsqrt_fast(x, (size_t)vl), (size_t)vl);
            __riscv_vse32_v_f32m1(oa, sgl_rvv_rsqrt_accurate(x, (size_t)vl), (size_t)vl);
            for (int i = 0; i < m; i++) {
                float ref = ref_rsqrt(in[i]);
                float ef = fabsf_(of[i] - ref) / ref;
                float ea = fabsf_(oa[i] - ref) / ref;
                if (ef > rq_fast) rq_fast = ef;
                if (ea > rq_acc)  rq_acc  = ea;
            }
        }
    }
    /* call-site zero-mask idiom: x<=0 -> 0 (BI_INVERSESQRT pattern) */
    int rsqrt_mask_ok = 1;
    {
        float in[LANES], out[LANES];
        for (int i = 0; i < vl; i++) in[i] = (i == 0) ? 0.0f : (i == 1 ? -1.0f : (float)(i));
        vfloat32m1_t x   = __riscv_vle32_v_f32m1(in, (size_t)vl);
        vbool32_t   pos  = __riscv_vmfgt_vf_f32m1_b32(x, 0.0f, (size_t)vl);
        vfloat32m1_t r   = sgl_rvv_rsqrt_accurate(x, (size_t)vl);
        r = __riscv_vmerge_vvm_f32m1(__riscv_vfmv_v_f_f32m1(0.0f, (size_t)vl), r, pos, (size_t)vl);
        __riscv_vse32_v_f32m1(out, r, (size_t)vl);
        if (out[0] != 0.0f) rsqrt_mask_ok = 0;   /* x=0  -> 0 */
        if (out[1] != 0.0f) rsqrt_mask_ok = 0;   /* x=-1 -> 0 */
    }

    /* ===================== SINCOS ===================== */
    float sc_sin = 0.0f, sc_cos = 0.0f;
    {
        /* fine sweep over [-4*PI, 4*PI] */
        const int NS = 4096;
        const float lo = -4.0f * M_PI_F, hi = 4.0f * M_PI_F;
        float in[LANES], osi[LANES], oco[LANES];
        for (int base = 0; base < NS; base += vl) {
            for (int i = 0; i < vl; i++) {
                int idx = base + i; if (idx >= NS) idx = NS - 1;
                in[i] = lo + (hi - lo) * (float)idx / (float)(NS - 1);
            }
            vfloat32m1_t a = __riscv_vle32_v_f32m1(in, (size_t)vl);
            vfloat32m1_t s, c; sgl_rvv_sincos_f(&s, &c, a, (size_t)vl);
            __riscv_vse32_v_f32m1(osi, s, (size_t)vl);
            __riscv_vse32_v_f32m1(oco, c, (size_t)vl);
            int m = NS - base; if (m > vl) m = vl;
            for (int i = 0; i < m; i++) {
                float es = fabsf_(osi[i] - ref_sin(in[i]));
                float ec = fabsf_(oco[i] - ref_cos(in[i]));
                if (es > sc_sin) sc_sin = es;
                if (ec > sc_cos) sc_cos = ec;
            }
        }
    }
    /* exact radian quadrant points + integer-phase quadrant points.
     * HW VLEN gives VL=4 here, so each angle of interest is placed across all
     * VL lanes (broadcast) and verified per-lane; we never index beyond vl. */
    int sc_quad_ok = 1;
    {
        /* integer phase 0/8192/16384/24576 over a 32768-step full turn ->
         * a = phase*(2*PI/32768). MANDATORY radian conversion (not phase/8192). */
        const float conv = TWO_PI_F / 32768.0f;
        float qangles[9] = {
            0.0f * conv, 8192.0f * conv, 16384.0f * conv, 24576.0f * conv,
            0.0f, M_PI_F * 0.5f, M_PI_F, M_PI_F * 1.5f, TWO_PI_F
        };
        float in[LANES], osi[LANES], oco[LANES];
        for (int qi = 0; qi < 9; qi++) {
            for (int i = 0; i < vl; i++) in[i] = qangles[qi];   /* broadcast across lanes */
            vfloat32m1_t a = __riscv_vle32_v_f32m1(in, (size_t)vl);
            vfloat32m1_t s, c; sgl_rvv_sincos_f(&s, &c, a, (size_t)vl);
            __riscv_vse32_v_f32m1(osi, s, (size_t)vl);
            __riscv_vse32_v_f32m1(oco, c, (size_t)vl);
            for (int i = 0; i < vl; i++) {
                float es = fabsf_(osi[i] - ref_sin(qangles[qi]));
                float ec = fabsf_(oco[i] - ref_cos(qangles[qi]));
                if (es > sc_sin) sc_sin = es;
                if (ec > sc_cos) sc_cos = ec;
                /* exact-point structural checks */
                if (qi == 0) {                       /* angle 0 */
                    if (!bits_eq(osi[i], 0.0f)) sc_quad_ok = 0;   /* sin==0.0f exactly */
                    if (!bits_eq(oco[i], 1.0f)) sc_quad_ok = 0;   /* cos==1.0f exactly */
                }
                if (qi == 5) {                       /* PI/2 */
                    if (fabsf_(osi[i] - 1.0f) >= 2e-5f) sc_quad_ok = 0;
                    if (fabsf_(oco[i])        >= 2e-5f) sc_quad_ok = 0;
                }
            }
        }
    }

    /* ===================== DOT4 (bit-exact) ===================== */
    uint32_t dot_bitmiss = 0;
    {
        float a0[LANES], a1[LANES], a2[LANES], a3[LANES];
        float b0[LANES], b1[LANES], b2[LANES], b3[LANES], out[LANES];
        for (int it = 0; it < 256; it++) {
            for (int i = 0; i < vl; i++) {
                a0[i]=rnd(); a1[i]=rnd(); a2[i]=rnd(); a3[i]=rnd();
                b0[i]=rnd(); b1[i]=rnd(); b2[i]=rnd(); b3[i]=rnd();
            }
            vfloat32m1_t acc = sgl_rvv_dot4(a0,a1,a2,a3, b0,b1,b2,b3, (size_t)vl);
            __riscv_vse32_v_f32m1(out, acc, (size_t)vl);
            for (int i = 0; i < vl; i++) {
                float s = a0[i]*b0[i];
                s = __builtin_fmaf(a1[i], b1[i], s);
                s = __builtin_fmaf(a2[i], b2[i], s);
                s = __builtin_fmaf(a3[i], b3[i], s);
                if (!bits_eq(out[i], s)) dot_bitmiss++;
            }
        }
    }

    /* ===================== MAT4xVEC4 (bit-exact) ===================== */
    uint32_t mat_bitmiss = 0;
    {
        float vx[LANES], vy[LANES], vz[LANES], vw[LANES];
        float r0[LANES], r1[LANES], r2[LANES], r3[LANES];
        for (int it = 0; it < 256; it++) {
            float M[16]; for (int j = 0; j < 16; j++) M[j] = rnd();   /* per-lane-identical */
            for (int i = 0; i < vl; i++) { vx[i]=rnd(); vy[i]=rnd(); vz[i]=rnd(); vw[i]=rnd(); }
            vfloat32m1_t x=__riscv_vle32_v_f32m1(vx,(size_t)vl), y=__riscv_vle32_v_f32m1(vy,(size_t)vl),
                         z=__riscv_vle32_v_f32m1(vz,(size_t)vl), w=__riscv_vle32_v_f32m1(vw,(size_t)vl);
            vfloat32m1_t o0,o1,o2,o3;
            sgl_rvv_mat4xvec4(&o0,&o1,&o2,&o3, M, x,y,z,w, (size_t)vl);
            __riscv_vse32_v_f32m1(r0,o0,(size_t)vl); __riscv_vse32_v_f32m1(r1,o1,(size_t)vl);
            __riscv_vse32_v_f32m1(r2,o2,(size_t)vl); __riscv_vse32_v_f32m1(r3,o3,(size_t)vl);
            for (int i = 0; i < vl; i++) {
                float v[4] = { vx[i], vy[i], vz[i], vw[i] };
                float *rr[4] = { r0, r1, r2, r3 };
                for (int row = 0; row < 4; row++) {
                    float s = 0.0f;
                    for (int j = 0; j < 4; j++) s = __builtin_fmaf(M[j*4+row], v[j], s);
                    /* first term is mul not fma in the kernel: r=M[0]*x then macc.
                     * Match exactly: redo with the kernel's mul-first order. */
                    float sk = M[0*4+row]*v[0];
                    sk = __builtin_fmaf(M[1*4+row], v[1], sk);
                    sk = __builtin_fmaf(M[2*4+row], v[2], sk);
                    sk = __builtin_fmaf(M[3*4+row], v[3], sk);
                    (void)s;
                    if (!bits_eq(rr[row][i], sk)) mat_bitmiss++;
                }
            }
        }
    }

    /* ===================== TIMING: ns/element SIMT vs scalar ===================== */
    /* rsqrt accurate */
    uint32_t rsqrt_simt_ns = 0, rsqrt_scal_ns = 0;
    {
        const int BATCHES = 20000;
        float in[LANES], out[LANES];
        for (int i = 0; i < vl; i++) in[i] = 0.5f + (float)i;
        vfloat32m1_t x = __riscv_vle32_v_f32m1(in, (size_t)vl);
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            vfloat32m1_t r = sgl_rvv_rsqrt_accurate(x, (size_t)vl);
            __riscv_vse32_v_f32m1(out, r, (size_t)vl);
            __asm__ volatile("" : : "r"(out[0]) : "memory");
        }
        uint64_t t1 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl; i++) { out[i] = ref_rsqrt(in[i]); __asm__ volatile("" : : "r"(out[i]) : "memory"); }
        uint64_t t2 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl;
        rsqrt_simt_ns = ns_per_elem(t1 - t0, elems);
        rsqrt_scal_ns = ns_per_elem(t2 - t1, elems);
    }
    /* sincos */
    uint32_t sc_simt_ns = 0, sc_scal_ns = 0;
    {
        const int BATCHES = 20000;
        float in[LANES], os[LANES], oc[LANES];
        for (int i = 0; i < vl; i++) in[i] = -3.0f + 0.37f * (float)i;
        vfloat32m1_t a = __riscv_vle32_v_f32m1(in, (size_t)vl);
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            vfloat32m1_t s, c; sgl_rvv_sincos_f(&s, &c, a, (size_t)vl);
            __riscv_vse32_v_f32m1(os, s, (size_t)vl); __riscv_vse32_v_f32m1(oc, c, (size_t)vl);
            __asm__ volatile("" : : "r"(os[0]), "r"(oc[0]) : "memory");
        }
        uint64_t t1 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl; i++) { os[i]=ref_sin(in[i]); oc[i]=ref_cos(in[i]); __asm__ volatile("" : : "r"(os[i]), "r"(oc[i]) : "memory"); }
        uint64_t t2 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl;
        sc_simt_ns = ns_per_elem(t1 - t0, elems);
        sc_scal_ns = ns_per_elem(t2 - t1, elems);
    }
    uint32_t dot_simt_ns = 0, dot_scal_ns = 0;
    {
        const int BATCHES = 20000;
        for (int i = 0; i < vl; i++) {
            bench_a0[i]=0.1f+(float)i; bench_a1[i]=0.2f+(float)i; bench_a2[i]=0.3f+(float)i; bench_a3[i]=0.4f+(float)i;
            bench_b0[i]=0.5f-(float)i*0.01f; bench_b1[i]=0.6f-(float)i*0.01f; bench_b2[i]=0.7f-(float)i*0.01f; bench_b3[i]=0.8f-(float)i*0.01f;
        }
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            vfloat32m1_t r = sgl_rvv_dot4(bench_a0,bench_a1,bench_a2,bench_a3, bench_b0,bench_b1,bench_b2,bench_b3, (size_t)vl);
            __riscv_vse32_v_f32m1(bench_r0, r, (size_t)vl);
            __asm__ volatile("" : : "r"(bench_r0[0]) : "memory");
        }
        uint64_t t1 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl; i++) { float s=bench_a0[i]*bench_b0[i]; s=__builtin_fmaf(bench_a1[i],bench_b1[i],s); s=__builtin_fmaf(bench_a2[i],bench_b2[i],s); bench_r0[i]=__builtin_fmaf(bench_a3[i],bench_b3[i],s); __asm__ volatile("" : : "r"(bench_r0[i]) : "memory"); }
        uint64_t t2 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl;
        dot_simt_ns = ns_per_elem(t1 - t0, elems);
        dot_scal_ns = ns_per_elem(t2 - t1, elems);
    }
    uint32_t mat4_simt_ns = 0, mat4_scal_ns = 0, mat4_m2_ns = 0, mat4_m2_scal_ns = 0;
    {
        const int BATCHES = 20000;
        float M[16];
        for (int j = 0; j < 16; j++) M[j] = 0.05f + 0.03f * (float)j;
        for (int i = 0; i < vl; i++) { bench_vx[i]=0.1f+(float)i; bench_vy[i]=0.2f+(float)i; bench_vz[i]=0.3f+(float)i; bench_vw[i]=0.4f+(float)i; }
        vfloat32m1_t x=__riscv_vle32_v_f32m1(bench_vx,(size_t)vl), y=__riscv_vle32_v_f32m1(bench_vy,(size_t)vl), z=__riscv_vle32_v_f32m1(bench_vz,(size_t)vl), w=__riscv_vle32_v_f32m1(bench_vw,(size_t)vl);
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            vfloat32m1_t o0,o1,o2,o3; sgl_rvv_mat4xvec4(&o0,&o1,&o2,&o3, M, x,y,z,w, (size_t)vl);
            __riscv_vse32_v_f32m1(bench_r0,o0,(size_t)vl); __riscv_vse32_v_f32m1(bench_r1,o1,(size_t)vl); __riscv_vse32_v_f32m1(bench_r2,o2,(size_t)vl); __riscv_vse32_v_f32m1(bench_r3,o3,(size_t)vl);
            __asm__ volatile("" : : "r"(bench_r0[0]), "r"(bench_r1[0]), "r"(bench_r2[0]), "r"(bench_r3[0]) : "memory");
        }
        uint64_t t1 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl; i++) { float v[4]={bench_vx[i],bench_vy[i],bench_vz[i],bench_vw[i]}; for (int row=0; row<4; row++) { float s=M[row]*v[0]; s=__builtin_fmaf(M[4+row],v[1],s); s=__builtin_fmaf(M[8+row],v[2],s); bench_r0[i]=__builtin_fmaf(M[12+row],v[3],s); } __asm__ volatile("" : : "r"(bench_r0[i]) : "memory"); }
        uint64_t t2 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl;
        mat4_simt_ns = ns_per_elem(t1 - t0, elems);
        mat4_scal_ns = ns_per_elem(t2 - t1, elems);
    }
    uint32_t mat3_simt_ns = 0, mat3_scal_ns = 0, mat3_m2_ns = 0, mat3_m2_scal_ns = 0;
    {
        const int BATCHES = 20000;
        float M[9];
        for (int j = 0; j < 9; j++) M[j] = 0.07f + 0.02f * (float)j;
        for (int i = 0; i < vl; i++) { bench_vx[i]=0.3f+(float)i; bench_vy[i]=0.4f+(float)i; bench_vz[i]=0.5f+(float)i; }
        vfloat32m1_t x=__riscv_vle32_v_f32m1(bench_vx,(size_t)vl), y=__riscv_vle32_v_f32m1(bench_vy,(size_t)vl), z=__riscv_vle32_v_f32m1(bench_vz,(size_t)vl);
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            vfloat32m1_t o0,o1,o2; sgl_rvv_mat3xvec3_m1(&o0,&o1,&o2, M, x,y,z, (size_t)vl);
            __riscv_vse32_v_f32m1(bench_r0,o0,(size_t)vl); __riscv_vse32_v_f32m1(bench_r1,o1,(size_t)vl); __riscv_vse32_v_f32m1(bench_r2,o2,(size_t)vl);
            __asm__ volatile("" : : "r"(bench_r0[0]), "r"(bench_r1[0]), "r"(bench_r2[0]) : "memory");
        }
        uint64_t t1 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl; i++) { float v[3]={bench_vx[i],bench_vy[i],bench_vz[i]}; for (int row=0; row<3; row++) { float s=M[row]*v[0]; s=__builtin_fmaf(M[3+row],v[1],s); bench_r0[i]=__builtin_fmaf(M[6+row],v[2],s); } __asm__ volatile("" : : "r"(bench_r0[i]) : "memory"); }
        uint64_t t2 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl;
        mat3_simt_ns = ns_per_elem(t1 - t0, elems);
        mat3_scal_ns = ns_per_elem(t2 - t1, elems);
    }
    {
        const int BATCHES = 20000;
        int vl2 = (int)__riscv_vsetvl_e32m2((size_t)SGL_SIMT_LANES);
        float M4[16], M3[9];
        for (int j = 0; j < 16; j++) M4[j] = 0.05f + 0.03f * (float)j;
        for (int j = 0; j < 9; j++) M3[j] = 0.07f + 0.02f * (float)j;
        for (int i = 0; i < vl2; i++) {
            bench_vx[i]=0.1f+(float)i;
            bench_vy[i]=0.2f+(float)i;
            bench_vz[i]=0.3f+(float)i;
            bench_vw[i]=0.4f+(float)i;
        }
        vfloat32m2_t x=__riscv_vle32_v_f32m2(bench_vx,(size_t)vl2), y=__riscv_vle32_v_f32m2(bench_vy,(size_t)vl2);
        vfloat32m2_t z=__riscv_vle32_v_f32m2(bench_vz,(size_t)vl2), w=__riscv_vle32_v_f32m2(bench_vw,(size_t)vl2);
        uint64_t t0 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            sgl_rvv_mat4xvec4_m2(&bench_r0, &bench_r1, &bench_r2, &bench_r3, M4, x, y, z, w, (size_t)vl2);
            __asm__ volatile("" : : "r"(bench_r0[0]), "r"(bench_r1[0]), "r"(bench_r2[0]), "r"(bench_r3[0]) : "memory");
        }
        uint64_t t1 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl2; i++) {
                float v[4]={bench_vx[i],bench_vy[i],bench_vz[i],bench_vw[i]};
                for (int row=0; row<4; row++) {
                    float s=M4[row]*v[0];
                    s=__builtin_fmaf(M4[4+row],v[1],s);
                    s=__builtin_fmaf(M4[8+row],v[2],s);
                    bench_r0[i]=__builtin_fmaf(M4[12+row],v[3],s);
                }
                __asm__ volatile("" : : "r"(bench_r0[i]) : "memory");
            }
        uint64_t t2 = timer_ticks();
        for (int b = 0; b < BATCHES; b++) {
            sgl_rvv_mat3xvec3_m2(&bench_r0, &bench_r1, &bench_r2, M3, x, y, z, (size_t)vl2);
            __asm__ volatile("" : : "r"(bench_r0[0]), "r"(bench_r1[0]), "r"(bench_r2[0]) : "memory");
        }
        uint64_t t3 = timer_ticks();
        for (int b = 0; b < BATCHES; b++)
            for (int i = 0; i < vl2; i++) {
                float v[3]={bench_vx[i],bench_vy[i],bench_vz[i]};
                for (int row=0; row<3; row++) {
                    float s=M3[row]*v[0];
                    s=__builtin_fmaf(M3[3+row],v[1],s);
                    bench_r0[i]=__builtin_fmaf(M3[6+row],v[2],s);
                }
                __asm__ volatile("" : : "r"(bench_r0[i]) : "memory");
            }
        uint64_t t4 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl2;
        mat4_m2_ns = ns_per_elem(t1 - t0, elems);
        mat4_m2_scal_ns = ns_per_elem(t2 - t1, elems);
        mat3_m2_ns = ns_per_elem(t3 - t2, elems);
        mat3_m2_scal_ns = ns_per_elem(t4 - t3, elems);
    }
    uint32_t norm_simt_ns = 0, norm_scal_ns = 0, dist_simt_ns = 0, dist_scal_ns = 0;
    uint32_t refl_simt_ns = 0, refl_scal_ns = 0, cross_simt_ns = 0, cross_scal_ns = 0, mix_simt_ns = 0, mix_scal_ns = 0;
    {
        const int BATCHES = 20000;
        int vl2 = (int)__riscv_vsetvl_e32m2((size_t)LANES);
        float a[4][SGL_SIMT_LANES], b[4][SGL_SIMT_LANES], c[4][SGL_SIMT_LANES], d[SGL_SIMT_LANES];
        for (int r = 0; r < 4; r++) for (int i = 0; i < vl2; i++) { a[r][i]=0.25f+(float)r+(float)i*0.125f; b[r][i]=0.75f-(float)r*0.1f+(float)i*0.0625f; c[r][i]=0.0f; }
        for (int i = 0; i < vl2; i++) d[i] = 0.2f + (float)i * 0.03f;
        uint64_t t0 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) { sgl_rvv_normalize3_v2(a, c, (size_t)vl2); __asm__ volatile("" : : "r"(c[0][0]) : "memory"); }
        uint64_t t1 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) for (int i = 0; i < vl2; i++) { float s=a[0][i]*a[0][i]; s=__builtin_fmaf(a[1][i],a[1][i],s); s=__builtin_fmaf(a[2][i],a[2][i],s); float inv=ref_rsqrt(s); c[0][i]=a[0][i]*inv; c[1][i]=a[1][i]*inv; c[2][i]=a[2][i]*inv; __asm__ volatile("" : : "r"(c[0][i]) : "memory"); }
        uint64_t t2 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) { vfloat32m2_t r = sgl_rvv_distance2_3_v2(a, b, (size_t)vl2); __riscv_vse32_v_f32m2(c[0], r, (size_t)vl2); __asm__ volatile("" : : "r"(c[0][0]) : "memory"); }
        uint64_t t3 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) for (int i = 0; i < vl2; i++) { float dx=a[0][i]-b[0][i], dy=a[1][i]-b[1][i], dz=a[2][i]-b[2][i]; c[0][i]=__builtin_fmaf(dz,dz,__builtin_fmaf(dy,dy,dx*dx)); __asm__ volatile("" : : "r"(c[0][i]) : "memory"); }
        uint64_t t4 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) { sgl_rvv_reflect3_v2(a, b, c, (size_t)vl2); __asm__ volatile("" : : "r"(c[0][0]) : "memory"); }
        uint64_t t5 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) for (int i = 0; i < vl2; i++) { float dot=a[0][i]*b[0][i]; dot=__builtin_fmaf(a[1][i],b[1][i],dot); dot=__builtin_fmaf(a[2][i],b[2][i],dot); c[0][i]=a[0][i]-2.0f*dot*b[0][i]; c[1][i]=a[1][i]-2.0f*dot*b[1][i]; c[2][i]=a[2][i]-2.0f*dot*b[2][i]; __asm__ volatile("" : : "r"(c[0][i]) : "memory"); }
        uint64_t t6 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) { sgl_rvv_cross3_v2(a, b, c, (size_t)vl2); __asm__ volatile("" : : "r"(c[0][0]) : "memory"); }
        uint64_t t7 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) for (int i = 0; i < vl2; i++) { c[0][i]=a[1][i]*b[2][i]-a[2][i]*b[1][i]; c[1][i]=a[2][i]*b[0][i]-a[0][i]*b[2][i]; c[2][i]=a[0][i]*b[1][i]-a[1][i]*b[0][i]; __asm__ volatile("" : : "r"(c[0][i]) : "memory"); }
        uint64_t t8 = timer_ticks();
        vfloat32m2_t ma=__riscv_vle32_v_f32m2(a[0],(size_t)vl2), mb=__riscv_vle32_v_f32m2(b[0],(size_t)vl2), md=__riscv_vle32_v_f32m2(d,(size_t)vl2);
        for (int x = 0; x < BATCHES; x++) { vfloat32m2_t r = sgl_rvv_lerp_m2(ma, mb, md, (size_t)vl2); __riscv_vse32_v_f32m2(c[0], r, (size_t)vl2); __asm__ volatile("" : : "r"(c[0][0]) : "memory"); }
        uint64_t t9 = timer_ticks();
        for (int x = 0; x < BATCHES; x++) for (int i = 0; i < vl2; i++) { c[0][i]=a[0][i]+(b[0][i]-a[0][i])*d[i]; __asm__ volatile("" : : "r"(c[0][i]) : "memory"); }
        uint64_t t10 = timer_ticks();
        uint64_t elems = (uint64_t)BATCHES * vl2;
        norm_simt_ns = ns_per_elem(t1 - t0, elems); norm_scal_ns = ns_per_elem(t2 - t1, elems);
        dist_simt_ns = ns_per_elem(t3 - t2, elems); dist_scal_ns = ns_per_elem(t4 - t3, elems);
        refl_simt_ns = ns_per_elem(t5 - t4, elems); refl_scal_ns = ns_per_elem(t6 - t5, elems);
        cross_simt_ns = ns_per_elem(t7 - t6, elems); cross_scal_ns = ns_per_elem(t8 - t7, elems);
        mix_simt_ns = ns_per_elem(t9 - t8, elems); mix_scal_ns = ns_per_elem(t10 - t9, elems);
    }

    /* ===================== verdicts ===================== */
    int rsqrt_ok  = (rq_fast < 5e-3f) && (rq_acc < 5e-6f) && rsqrt_mask_ok;
    int sincos_ok = (sc_sin < 2e-5f) && (sc_cos < 2e-5f) && sc_quad_ok;
    int dot_ok    = (dot_bitmiss == 0);
    int mat_ok    = (mat_bitmiss == 0);
    int all_ok    = rsqrt_ok && sincos_ok && dot_ok && mat_ok;

    printf("[rvvmath] rsqrt   fast=%u e-9  acc=%u e-9  maskOK=%u   SIMT=%u ns/el  scalar=%u ns/el  %s\n",
           (unsigned)(rq_fast * 1e9f), (unsigned)(rq_acc * 1e9f), (unsigned)rsqrt_mask_ok,
           rsqrt_simt_ns, rsqrt_scal_ns, rsqrt_ok ? "OK" : "*** FAIL ***");
    printf("[rvvmath] sincos  sin=%u e-9  cos=%u e-9  quadOK=%u   SIMT=%u ns/el  scalar=%u ns/el  %s\n",
           (unsigned)(sc_sin * 1e9f), (unsigned)(sc_cos * 1e9f), (unsigned)sc_quad_ok,
           sc_simt_ns, sc_scal_ns, sincos_ok ? "OK" : "*** FAIL ***");
    printf("[rvvmath] dot4    bitmiss=%u   %s\n", (unsigned)dot_bitmiss, dot_ok ? "OK(bit-exact)" : "*** FAIL ***");
    printf("[rvvmath] mat4    bitmiss=%u   %s\n", (unsigned)mat_bitmiss, mat_ok ? "OK(bit-exact)" : "*** FAIL ***");
    printf("[rvvbench] dot4       SIMT=%u ns/el  scalar=%u ns/el\n", dot_simt_ns, dot_scal_ns);
    printf("[rvvbench] mat4xvec4  SIMT=%u ns/el  scalar=%u ns/el\n", mat4_simt_ns, mat4_scal_ns);
    printf("[rvvbench] mat3xvec3  SIMT=%u ns/el  scalar=%u ns/el\n", mat3_simt_ns, mat3_scal_ns);
    printf("[rvvbench] mat4xvec4_m2 SIMT=%u ns/el  scalar=%u ns/el\n", mat4_m2_ns, mat4_m2_scal_ns);
    printf("[rvvbench] mat3xvec3_m2 SIMT=%u ns/el  scalar=%u ns/el\n", mat3_m2_ns, mat3_m2_scal_ns);
    printf("[rvvbench] normalize3 SIMT=%u ns/el  scalar=%u ns/el\n", norm_simt_ns, norm_scal_ns);
    printf("[rvvbench] distance2  SIMT=%u ns/el  scalar=%u ns/el\n", dist_simt_ns, dist_scal_ns);
    printf("[rvvbench] reflect3   SIMT=%u ns/el  scalar=%u ns/el\n", refl_simt_ns, refl_scal_ns);
    printf("[rvvbench] cross3     SIMT=%u ns/el  scalar=%u ns/el\n", cross_simt_ns, cross_scal_ns);
    printf("[rvvbench] mix        SIMT=%u ns/el  scalar=%u ns/el\n", mix_simt_ns, mix_scal_ns);
    printf("[rvvmath] summary rsqrt fast=%u e-9 acc=%u e-9  sincos sin=%u e-9 cos=%u e-9  dot4 bitmiss=%u  mat4 bitmiss=%u  %s\n",
           (unsigned)(rq_fast * 1e9f), (unsigned)(rq_acc * 1e9f),
           (unsigned)(sc_sin * 1e9f), (unsigned)(sc_cos * 1e9f),
           (unsigned)dot_bitmiss, (unsigned)mat_bitmiss, all_ok ? "OK" : "*** FAIL ***");
    printf("[rvvmath] done.\n");
    sbi_shutdown();
    return 0;
}
