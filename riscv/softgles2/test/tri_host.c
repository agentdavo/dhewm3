/* Host (x86) golden-image generator for the softgles2 rasteriser. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sgl.h"

static void scene(sgl_framebuffer *fb)
{
    sgl_clear(fb, 0xFF402010u /* 0xAABBGGRR: dark blue */, 1.0f);
    sgl_vertex tri[3] = {
        {  0.0f,  0.8f, 0.0f, 1.0f,  1, 0, 0, 1 },   /* top    red   */
        { -0.8f, -0.8f, 0.0f, 1.0f,  0, 1, 0, 1 },   /* bottom-left  green */
        {  0.8f, -0.8f, 0.0f, 1.0f,  0, 0, 1, 1 },   /* bottom-right blue  */
    };
    sgl_draw_triangles(fb, tri, 3);
}

int main(void)
{
    const int W = 192, H = 96;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { fprintf(stderr, "fb alloc failed\n"); return 1; }

    scene(fb);

    printf("rvv=%d checksum=0x%08X\n", sgl_rvv_enabled(), sgl_fnv1a(fb));
    sgl_dump_ascii(fb, 64, 24);

    FILE *f = fopen("build/tri.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; ++i) {
            uint32_t c = fb->color[i];
            unsigned char px[3] = { (unsigned char)(c & 0xFF),
                                    (unsigned char)((c >> 8) & 0xFF),
                                    (unsigned char)((c >> 16) & 0xFF) };
            fwrite(px, 1, 3, f);
        }
        fclose(f);
        printf("wrote build/tri.ppm  center=0x%08X corner=0x%08X\n",
               sgl_pixel(fb, W / 2, H / 2), sgl_pixel(fb, 1, 1));
    }

    sgl_fb_destroy(fb);
    return 0;
}
