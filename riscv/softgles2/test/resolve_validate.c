/* SSAA resolve oracle and same-process timing at the Doom 3 output size. */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sg_fb.h"

static uint32_t src[1280 * 960], expected[640 * 480 + 1], actual[640 * 480 + 1];
static void reference(const uint32_t *in, int sw, int sh,
                      uint32_t *out, int dw, int dh, int scale)
{
    for (int y = 0; y < dh; ++y) for (int x = 0; x < dw; ++x) {
        uint32_t result = 0;
        for (int ch = 0; ch < 4; ++ch) {
            unsigned sum = 0;
            for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx) {
                int sx = x * scale + dx, sy = y * scale + dy;
                if (sx >= sw) sx = sw - 1;
                if (sy >= sh) sy = sh - 1;
                sum += (in[sy * sw + sx] >> (ch * 8)) & 255;
            }
            unsigned n = scale * scale;
            result |= ((sum + n / 2) / n) << (ch * 8);
        }
        out[y * dw + x] = result;
    }
}

/* Previous production implementation, retained only as the timing baseline. */
/* Box-downsample an s*s block average from a render-res RGBA8 buffer (src,
 * sw*sh, little-endian 0xAABBGGRR) into an output-res buffer (dst, dw*dh).
 * dw*s must equal the covered width; edge blocks are clamped to src bounds. */
static void __attribute__((noinline, noclone)) legacy_resolve(const uint32_t *src, int sw, int sh,
                                uint32_t *dst, int dw, int dh, int s)
{
    const uint32_t inv = (uint32_t)(s * s);
    int oy;
    for (oy = 0; oy < dh; ++oy) {
        int by0 = oy * s;
        int ox;
        for (ox = 0; ox < dw; ++ox) {
            int bx0 = ox * s;
            uint32_t r = 0, g = 0, b = 0, a = 0;
            int jy;
            for (jy = 0; jy < s; ++jy) {
                int sy = by0 + jy;
                const uint32_t *row;
                int jx;
                if (sy >= sh) sy = sh - 1;
                row = src + (size_t)sy * sw;
                for (jx = 0; jx < s; ++jx) {
                    int sx = bx0 + jx;
                    uint32_t px;
                    if (sx >= sw) sx = sw - 1;
                    px = row[sx];
                    r += (px      ) & 0xffu;
                    g += (px >>  8) & 0xffu;
                    b += (px >> 16) & 0xffu;
                    a += (px >> 24) & 0xffu;
                }
            }
            r = (r + inv / 2) / inv;
            g = (g + inv / 2) / inv;
            b = (b + inv / 2) / inv;
            a = (a + inv / 2) / inv;
            dst[(size_t)oy * dw + ox] =
                r | (g << 8) | (b << 16) | (a << 24);
        }
    }
}


int kmain(void)
{
    uint32_t rng = 0x12345678;
    for (unsigned i = 0; i < sizeof(src) / sizeof(src[0]); ++i) {
        rng = rng * 1664525u + 1013904223u;
        src[i] = (i % 17 == 0) ? 0xffffffffu : (i % 19 == 0 ? 0 : rng);
    }
    const int widths[] = {1, 3, 17, 31, 33, 65, 127};
    int cases = 0;
    for (int scale = 1; scale <= 4; ++scale) for (int w = 0; w < 7; ++w)
        for (int edge = 0; edge < 2; ++edge) {
            int dw = widths[w], dh = 5, sw = dw * scale - edge, sh = dh * scale - edge;
            if (sw < 1) sw = 1;
            actual[dw * dh] = 0xabcddcba;
            reference(src, sw, sh, expected, dw, dh, scale);
            sgl_downsample_rgba_box(src, sw, sh, actual, dw, dh, scale);
            for (int i = 0; i < dw * dh; ++i) if (expected[i] != actual[i]) {
                printf("[resolve] FAIL scale=%d width=%d edge=%d pixel=%d\n", scale, dw, edge, i);
                return 1;
            }
            if (actual[dw * dh] != 0xabcddcba) { puts("[resolve] FAIL canary"); return 1; }
            ++cases;
        }
    reference(src, 1280, 960, expected, 640, 480, 2);
    sgl_downsample_rgba_box(src, 1280, 960, actual, 640, 480, 2);
    for (int i = 0; i < 640 * 480; ++i) if (expected[i] != actual[i]) {
        puts("[resolve] FAIL full frame"); return 1;
    }
    printf("[resolve] PASS %d edge/tail cases + 640x480, all RGBA bytes exact\n", cases);
    /* Keep dimensions dynamic as in dos_present; do not let IPA specialize the
     * old nested loops to a constant scale and erase their runtime divisions. */
    volatile int bw = 640, bh = 480, bs = 2;
    for (int round = 0; round < 3; ++round) {
        int dw = bw, dh = bh, scale = bs;
        uint64_t a = timer_ticks();
        legacy_resolve(src, dw * scale, dh * scale, expected, dw, dh, scale);
        uint64_t b = timer_ticks();
        sgl_downsample_rgba_box(src, dw * scale, dh * scale, actual, dw, dh, scale);
        uint64_t c = timer_ticks();
        printf("[resolve] round=%d legacy_ticks=%lu rvv_ticks=%lu\n",
            round, (unsigned long)(b-a), (unsigned long)(c-b));
    }
    return 0;
}
