/*
 * dOS milestone: boot bare metal (no OS) and bring up SDL3.
 * Proves the dOS link/runtime surface (newlib over SBI + gettimeofday/select)
 * is enough for SDL_Init(SDL_INIT_VIDEO). Uses SDL's built-in dummy driver for
 * now; the custom "dos" video driver (GL->gl4es, present->softgles2) is next.
 */
#include <stdio.h>
#include "boot/runtime.h"
#define SDL_MAIN_HANDLED            /* we provide our own entry (kmain) */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int kmain(void)
{
    printf("\n========================================\n");
    printf("  dOS (doomOS): SDL3 on bare-metal RISC-V\n");
    printf("========================================\n");

    int v = SDL_GetVersion();
    printf("[dos] SDL %d.%d.%d\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));

    printf("[dos] video drivers compiled in:");
    for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) printf(" %s", SDL_GetVideoDriver(i));
    printf("\n");

    SDL_SetMainReady();               /* we are not using SDL_main */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");   /* until the dos driver lands */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("[dos] SDL_Init(VIDEO) FAILED: %s\n", SDL_GetError());
        return 1;
    }
    printf("[dos] SDL_Init(VIDEO) ok; current driver = \"%s\"\n", SDL_GetCurrentVideoDriver());
    printf("[dos] SDL_GetTicks = %llu ms\n", (unsigned long long)SDL_GetTicks());

    SDL_Window *w = SDL_CreateWindow("dOS", 800, 600, 0);
    printf("[dos] SDL_CreateWindow(800x600) = %p%s%s\n", (void *)w,
           w ? "" : " : ", w ? "" : SDL_GetError());

    if (w) SDL_DestroyWindow(w);
    SDL_Quit();
    printf("[ok] dOS brought up SDL3; powering off.\n");
    return 0;
}
