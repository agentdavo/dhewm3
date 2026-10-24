/* Compare gamma against libm, not another copy of the renderer approximation.
 * Dense ramps include exponent boundaries that previously caused posterization. */
#include "sg_math.h"
#include <math.h>
#include <stdio.h>

#ifdef SGL_BAREMETAL
#define main kmain
/* Standalone boot runtime has no newlib reentrancy object. */
int *__errno(void) { static int value; return &value; }
#endif
int main(void) {
    const float exponents[] = {1.f, 1.f/1.1f, 0.5f, 2.2f, 16.f};
    int failures = 0;
    float worst = 0.f;
    for (unsigned e = 0; e < sizeof(exponents)/sizeof(exponents[0]); ++e) {
        float previous = 0.f;
        for (int i = 1; i <= 65536;) {
            float x[128], actual[128];
            int n = 65537-i; if (n > 128) n = 128;
#if defined(__riscv_v_intrinsic)
            n = (int)__riscv_vsetvl_e32m2(n);
#endif
            for (int j=0;j<n;++j) x[j]=(float)(i+j)/65536.f;
#if defined(__riscv_v_intrinsic)
            __riscv_vse32_v_f32m2(actual, sgl_rvv_pow_sf_m2(
                __riscv_vle32_v_f32m2(x,n),exponents[e],n),n);
#else
            for (int j=0;j<n;++j) actual[j]=sgl_math_pow_f(x[j],exponents[e]);
#endif
            for (int j=0;j<n;++j) {
                float error=fabsf(actual[j]-powf(x[j],exponents[e]));
                if (error>worst) worst=error;
                /* Less than 1/100 of an 8-bit colour step; tiny rounding
                 * fluctuations at exponent boundaries are permitted. */
                if (!isfinite(actual[j]) || error>0.000039f || actual[j]+0.000003f<previous) ++failures;
                previous=actual[j];
            }
            i+=n;
        }
    }
    printf("[colour-math] %s failures=%d max_error_millionths=%d\n",
        failures?"FAIL":"PASS", failures, (int)(worst*1000000.f+0.5f));
    return failures!=0;
}
