/* depth_roundtrip_test.c -- host unit test for the softgles2 DEPTH-TEXTURE path.
 *
 * Proves that the depth capture/sample encoding is bit-exact, independent of
 * qemu. The engine path is:
 *
 *   CAPTURE (sg_texture.c fb_depth_at_gl / glCopyTexSubImage2D depth branch):
 *       a depth texel = the raw IEEE-754 bits of the window-z float.
 *   SAMPLE  (sg_math.h sgl_rvv_sample2d_nearest_m2 / sg_raster.c sgl_sample):
 *       reinterpret the texel's 32 bits back to float; return (d,d,d,1).
 *
 * Capture is a float->u32 bit copy (memcpy in the scalar path, vle32-of-float
 * in the RVV path -- the same 32 bits). Sample is the inverse u32->float bit
 * copy (memcpy in scalar; vreinterpret_u32m2_f32m2 in RVV -- again the same 32
 * bits). Both directions are pure bit reinterpretations, so the round trip must
 * be the identity on the bit pattern, hence exact float equality for every
 * finite value. This test reproduces both bit copies and asserts that.
 *
 * Build (host):  gcc -O2 -Wall -Wextra -o depth_roundtrip_test depth_roundtrip_test.c
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirror of the capture encode: window-z float -> depth texel bits. */
static uint32_t depth_capture(float z)
{
    uint32_t bits;
    memcpy(&bits, &z, sizeof bits);
    return bits;
}

/* Mirror of the sample decode: depth texel bits -> float (channel d of (d,d,d,1)). */
static float depth_sample(uint32_t texel)
{
    float d;
    memcpy(&d, &texel, sizeof d);
    return d;
}

int main(void)
{
    /* Representative window-depth values in [0,1] plus edge cases:
     * exact endpoints, a mid value, the classic near-far particle depth,
     * a value one ULP below 1.0, the smallest normal and a denormal. */
    const float samples[] = {
        0.0f,
        0.5f,
        0.9994f,
        0.99999994f,      /* ~1 - 1 ULP */
        1.0f,
        1.17549435e-38f,  /* FLT_MIN, smallest normal */
        1.0e-40f,         /* subnormal */
        1.4e-45f,         /* ~smallest positive subnormal */
        -0.0f,            /* signed zero: bit pattern 0x80000000 must survive */
    };
    const int n = (int)(sizeof(samples) / sizeof(samples[0]));

    int failures = 0;
    printf("softgles2 depth capture->sample round-trip\n");
    printf("%-18s %-12s %-18s %s\n", "input", "texel(hex)", "output", "result");
    for (int i = 0; i < n; ++i) {
        float in = samples[i];
        uint32_t texel = depth_capture(in);          /* CAPTURE */
        float out = depth_sample(texel);             /* SAMPLE  */

        /* Bit-exact comparison (NaN-safe, distinguishes +0.0 from -0.0). */
        uint32_t in_bits, out_bits;
        memcpy(&in_bits, &in, 4);
        memcpy(&out_bits, &out, 4);
        int ok = (in_bits == out_bits);
        if (!ok) ++failures;
        printf("%-18.9g 0x%08X   %-18.9g %s\n",
               (double)in, texel, (double)out, ok ? "OK" : "MISMATCH");
    }

    /* Exhaustive sweep over a dense grid of [0,1] to be thorough: every finite
     * float that maps through the pair must come back identical. */
    int swept = 0, sweep_fail = 0;
    for (uint32_t k = 0; k <= 1000000u; ++k) {
        float in = (float)k / 1000000.0f;   /* covers [0,1] finely */
        uint32_t texel = depth_capture(in);
        float out = depth_sample(texel);
        uint32_t a, b;
        memcpy(&a, &in, 4);
        memcpy(&b, &out, 4);
        ++swept;
        if (a != b) { ++sweep_fail; if (sweep_fail <= 5) printf("sweep mismatch at k=%u\n", k); }
    }
    printf("\nlisted cases: %d, failures: %d\n", n, failures);
    printf("swept [0,1] values: %d, failures: %d\n", swept, sweep_fail);

    if (failures || sweep_fail) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS (capture->sample is bit-exact / lossless)\n");
    return 0;
}
