/*
 * fb_2048_bench.c -- bare-metal RVV framebuffer bandwidth/present benchmark.
 *
 * Measures the non-shader frame operations that matter for a software
 * swapchain:
 *   - materialised color+depth clear through sgl_clear()
 *   - color present/copy through sgl_present_copy()
 *   - O(1) software double-buffer storage swap
 */
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"

#define FB_W 2048
#define FB_H 2048
#define FB_PIXELS ((uint64_t)FB_W * (uint64_t)FB_H)

#ifndef FB_CLEAR_ITERS
#define FB_CLEAR_ITERS 6
#endif
#ifndef FB_COPY_ITERS
#define FB_COPY_ITERS 8
#endif
#ifndef FB_PRESENT_ITERS
#define FB_PRESENT_ITERS 200000
#endif
#ifndef FB_SWAP_ITERS
#define FB_SWAP_ITERS 200000
#endif

static uint32_t ptr_low7(const void *p)
{
    return (uint32_t)((uintptr_t)p & 127u);
}

static uint32_t ns_per_pixel(uint64_t ticks, uint64_t pixels)
{
    uint64_t ns = ticks * 1000000000ull / TIMEBASE_HZ;
    return (uint32_t)(ns / (pixels ? pixels : 1ull));
}

static uint32_t ms_total(uint64_t ticks)
{
    uint64_t ns = ticks * 1000000000ull / TIMEBASE_HZ;
    return (uint32_t)(ns / 1000000ull);
}

int kmain(void)
{
    printf("\n==== softgles2 framebuffer 2048 bench (RVV) ====\n");
    sgl_framebuffer *front = sgl_fb_create(FB_W, FB_H);
    sgl_framebuffer *back = sgl_fb_create(FB_W, FB_H);
    sgl_framebuffer *nodepth = sgl_fb_create_ex(FB_W, FB_H, SGL_FB_COLOR | SGL_FB_DEPTH);
    if (!front || !back || !nodepth) {
        printf("[fb] alloc failed front=%p back=%p nodepth=%p\n", front, back, nodepth);
        sbi_shutdown();
        return 1;
    }

    printf("[fb] align front color=%u depth=%u stencil=%u  back color=%u depth=%u stencil=%u\n",
           ptr_low7(front->color), ptr_low7(front->depth), ptr_low7(front->stencil),
           ptr_low7(back->color), ptr_low7(back->depth), ptr_low7(back->stencil));

    uint64_t t0 = timer_ticks();
    for (int i = 0; i < FB_CLEAR_ITERS; ++i)
        sgl_clear(back, 0xFF102030u + (uint32_t)i, 1.0f);
    uint64_t t1 = timer_ticks();

    uint32_t clear_ns_px = ns_per_pixel(t1 - t0, FB_PIXELS * (uint64_t)FB_CLEAR_ITERS);
    printf("[fb] clear color+depth %d x %ux%u: %u ms total, %u ns/pixel\n",
           FB_CLEAR_ITERS, FB_W, FB_H, ms_total(t1 - t0), clear_ns_px);

    t0 = timer_ticks();
    for (int i = 0; i < FB_CLEAR_ITERS; ++i)
        sgl_clear(nodepth, 0xFF405060u + (uint32_t)i, 1.0f);
    t1 = timer_ticks();

    uint32_t clear_cd_ns_px = ns_per_pixel(t1 - t0, FB_PIXELS * (uint64_t)FB_CLEAR_ITERS);
    printf("[fb] clear color+depth(no stencil) %d x %ux%u: %u ms total, %u ns/pixel\n",
           FB_CLEAR_ITERS, FB_W, FB_H, ms_total(t1 - t0), clear_cd_ns_px);

    t0 = timer_ticks();
    for (int i = 0; i < FB_COPY_ITERS; ++i)
        sgl_present_copy(front, back);
    t1 = timer_ticks();

    uint32_t copy_ns_px = ns_per_pixel(t1 - t0, FB_PIXELS * (uint64_t)FB_COPY_ITERS);
    printf("[fb] present copy rgba32 %d x %ux%u: %u ms total, %u ns/pixel\n",
           FB_COPY_ITERS, FB_W, FB_H, ms_total(t1 - t0), copy_ns_px);

    t0 = timer_ticks();
    for (int i = 0; i < FB_PRESENT_ITERS; ++i)
        sgl_present(front, back, SGL_PRESENT_AUTO);
    t1 = timer_ticks();

    uint64_t present_ns = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;
    printf("[fb] present auto %d presents: %u ms total, %u ns/present\n",
           FB_PRESENT_ITERS, ms_total(t1 - t0), (uint32_t)(present_ns / FB_PRESENT_ITERS));

    t0 = timer_ticks();
    for (int i = 0; i < FB_SWAP_ITERS; ++i)
        sgl_fb_swap_storage(front, back);
    t1 = timer_ticks();

    uint64_t swap_ns = (t1 - t0) * 1000000000ull / TIMEBASE_HZ;
    printf("[fb] swap storage %d swaps: %u ms total, %u ns/swap\n",
           FB_SWAP_ITERS, ms_total(t1 - t0), (uint32_t)(swap_ns / FB_SWAP_ITERS));

    printf("[fb] checksum front=0x%08X back=0x%08X\n", sgl_fnv1a(front), sgl_fnv1a(back));
    sgl_fb_destroy(front);
    sgl_fb_destroy(back);
    sgl_fb_destroy(nodepth);
    printf("[fb] done.\n");
    sbi_shutdown();
    return 0;
}
