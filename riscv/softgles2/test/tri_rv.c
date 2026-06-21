/* Bare-metal RISC-V entry: render a triangle with softgles2 (RVV path) and
 * show it over the serial console as ASCII + a checksum. */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "sgl.h"

int kmain(void)
{
    printf("\n== softgles2 RVV triangle test (bare metal) ==\n");
    printf("[sgl] rasteriser: %s\n", sgl_rvv_enabled() ? "RVV (V extension)" : "scalar");

    const int W = 192, H = 96;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { printf("[sgl] framebuffer alloc failed\n"); return 1; }

    sgl_clear(fb, 0xFF402010u, 1.0f);
    sgl_vertex tri[3] = {
        {  0.0f,  0.8f, 0.0f, 1.0f,  1, 0, 0, 1 },
        { -0.8f, -0.8f, 0.0f, 1.0f,  0, 1, 0, 1 },
        {  0.8f, -0.8f, 0.0f, 1.0f,  0, 0, 1, 1 },
    };

    uint64_t t0 = timer_ticks();
    sgl_draw_triangles(fb, tri, 3);
    uint64_t t1 = timer_ticks();

    printf("[sgl] drew triangle in %lu ticks\n", (unsigned long)(t1 - t0));
    printf("[sgl] checksum=0x%08X  center=0x%08X  corner=0x%08X\n",
           sgl_fnv1a(fb), sgl_pixel(fb, W / 2, H / 2), sgl_pixel(fb, 1, 1));
    sgl_dump_ascii(fb, 64, 24);

    printf("[ok] render complete; powering off.\n");
    sgl_fb_destroy(fb);
    return 0;
}
