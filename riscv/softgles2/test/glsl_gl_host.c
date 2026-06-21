/* Host driver: render the GLSL-shader scene, write build/glsl.ppm. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sgl.h"
#include "sgl_gl.h"
#include "glsl_scene.h"

int main(void)
{
    const int W = 192, H = 96;
    sgl_framebuffer *fb = sgl_fb_create(W, H);
    if (!fb) { fprintf(stderr, "fb alloc failed\n"); return 1; }

    sglMakeCurrent(fb);
    glViewport(0, 0, W, H);
    glsl_scene();

    printf("GLSL host: renderer=\"%s\"\n", (const char *)glGetString(GL_RENDERER));
    printf("checksum=0x%08X glGetError=0x%04X\n", sgl_fnv1a(fb), glGetError());
    sgl_dump_ascii(fb, 64, 24);

    FILE *f = fopen("build/glsl.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; ++i) {
            uint32_t c = fb->color[i];
            unsigned char p[3] = { (unsigned char)(c & 0xFF), (unsigned char)((c >> 8) & 0xFF), (unsigned char)((c >> 16) & 0xFF) };
            fwrite(p, 1, 3, f);
        }
        fclose(f);
        printf("wrote build/glsl.ppm\n");
    }
    sgl_fb_destroy(fb);
    return 0;
}
