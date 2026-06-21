/*
 * dos_present.c — payload-side implementation of the dos_gl.h surface that the
 * SDL3 "dos" video driver calls.
 *
 * Links the SDL3 "dos" driver to gl4es + softgles2 + the dOS framebuffer sink,
 * keeping libSDL3 free of softgles2/gl4es headers. The driver only forwards
 * function pointers here.
 *
 * GL path:   dos_gl_create_context -> sgl_fb_create + sgl_bridge_init (current)
 *            dos_gl_present        -> sg_framebin_flush + dvgpu_present_rgba
 *                                     (virtio-gpu scanout; capture via QEMU
 *                                     monitor screendump — see capture_screendump.sh).
 * SW path:   dos_fb_blit          -> swizzle XRGB8888 into the current fb sink.
 *
 * Naming convention matches dos_gl.h:
 *   DOS_*  = SDL's static video-driver callbacks.
 *   dos_*  = this payload ABI boundary.
 *
 * gl4es_GetProcAddress is provided by libGL-riscv.a directly; we don't redefine
 * it here (the driver references it; the linker resolves it from gl4es).
 */
#include <stdint.h>
#include <stdio.h>      /* printf -> SBI serial (demoflow markers) */
#include <string.h>
#if !defined(SGL_BAREMETAL)
#include <stdlib.h>
#endif

#include "sgl.h"        /* sgl_framebuffer, sgl_fb_create */
#include "bridge.h"     /* sgl_bridge_init */
#include "dos_virtio_gpu.h"
#include "video-dos/dos_gl.h"

/* The single GL surface for the dOS window (dhewm3 only ever has one). */
static sgl_framebuffer *g_fb;
static sgl_framebuffer *g_sw_fb;   /* separate backing for the software blit path */

#if !defined(SGL_BAREMETAL)
#define DOS_PRESENT_TRACE(...) do { if (getenv("DOS_TRACE")) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define DOS_PRESENT_TRACE(...) ((void)0)
#endif

void *dos_gl_create_context(int w, int h)
{
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;
    if (!g_fb) {
        g_fb = sgl_fb_create(w, h);
        if (!g_fb) {
            return NULL;
        }
        DOS_PRESENT_TRACE("dos-present: created GL fb %dx%d -> %p\n", w, h, (void *)g_fb);
    } else {
        DOS_PRESENT_TRACE("dos-present: reuse GL fb %dx%d -> %p\n", g_fb->w, g_fb->h, (void *)g_fb);
    }
    sgl_bridge_init(g_fb);     /* set_getprocaddress + initialize_gl4es; current */
    return (void *)g_fb;       /* opaque, non-NULL context token */
}

int dos_gl_make_current(void *ctx)
{
    (void)ctx;                 /* single surface; gl4es state already current */
    return 0;
}

void dos_gl_destroy_context(void *ctx)
{
    DOS_PRESENT_TRACE("dos-present: destroy GL ctx %p (fb kept alive)\n", ctx);
    (void)ctx;
    /* Leave g_fb alive across the run so the final present can dump it. The
     * payload frees it explicitly (or power-off reclaims all RAM). */
}

extern void sg_framebin_flush(void);   /* drain deferred (FrameBin) draws; no-op if disabled */
extern int sgl_prof_enabled;
extern uint64_t sgl_prof_present;
extern uint64_t sgl_prof_now(void);
extern void sgl_prof_frame_report(void);

/* ---- present-time gamma / brightness --------------------------------------
 * Bare metal has no hardware gamma ramp (GLimp_SetGamma is a no-op) and softgles2
 * does not honor r_gammaInShader, so the Doom 3 world — authored for a ~2.2-gamma
 * display — scans out far too dark (raw mean ~10/255 in dim interiors). Emulate the
 * display gamma ramp here, at the single scanout choke point, so every frame is
 * presented correctly bright. A pre-gamma brightness lifts shadows; the gamma-2.0
 * curve (fsqrt, no libm) compresses highlights so the HUD/GUI don't blow out.
 * dos_present_gamma_enable / _brightness are plain globals so a debugger or a future
 * cvar bridge can retune them; default on. */
int   dos_present_gamma_enable = 1;
float dos_present_gamma_brightness = 2.00f;   /* pre-gamma multiply (lifted for the dark Mars City interiors) */
static uint8_t s_gamma_lut[256];
static int     s_gamma_lut_ready = 0;
static float   s_gamma_lut_bri = 0.0f;

static void dos_present_build_gamma_lut(void)
{
    float bri = dos_present_gamma_brightness;
    int i;
    for (i = 0; i < 256; ++i) {
        float t = (float)i * (1.0f / 255.0f) * bri;
        int v;
        if (t > 1.0f) t = 1.0f;
        v = (int)(__builtin_sqrtf(t) * 255.0f + 0.5f);   /* pow(t, 1/2.0) */
        s_gamma_lut[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    s_gamma_lut_bri = bri;
    s_gamma_lut_ready = 1;
}

/* In-place RGB gamma over the frame just rendered (alpha preserved). Safe in place:
 * Doom 3 clears + fully redraws the framebuffer every frame, and dos_gl_present runs
 * exactly once per SwapBuffers, so gamma'd pixels are discarded on the next clear. */
static void dos_present_apply_gamma(uint32_t *px, int n)
{
    const uint8_t *lut;
    int i;
    if (!dos_present_gamma_enable) return;
    if (!s_gamma_lut_ready || s_gamma_lut_bri != dos_present_gamma_brightness) {
        dos_present_build_gamma_lut();
    }
    lut = s_gamma_lut;
    for (i = 0; i < n; ++i) {
        uint32_t c = px[i];
        px[i] = (c & 0xFF000000u)
              | ((uint32_t)lut[(c >> 16) & 0xFF] << 16)   /* B */
              | ((uint32_t)lut[(c >>  8) & 0xFF] <<  8)   /* G */
              |  (uint32_t)lut[ c        & 0xFF];         /* R */
    }
}

int dos_gl_present(void *ctx)
{
    sgl_framebuffer *fb = ctx ? (sgl_framebuffer *)ctx : g_fb;
    if (!fb) {
        return -1;
    }
    /* Frame boundary: ensure any deferred draws have rasterised into fb before
     * we read its pixels for display. */
    unsigned long long pf_t0 = sgl_prof_now();
    sg_framebin_flush();
    (void)sgl_diag_present_report(fb, 0);   /* optional per-frame diagnostics (off by default) */
    /* Emulate the display gamma ramp (bare metal has none) so the frame is not too
     * dark, then scan it out to the virtio-gpu device. Capture is host-side via the
     * QEMU monitor `screendump` (riscv/scripts/capture_screendump.sh). */
    dos_present_apply_gamma(fb->color, fb->w * fb->h);
    (void)dvgpu_present_rgba(fb->color, fb->w, fb->h);
    if (sgl_prof_enabled) {
        sgl_prof_present += sgl_prof_now() - pf_t0;   /* flush + blit time */
        sgl_prof_frame_report();
    }
    return 0;
}

/* Software (non-GL) blit. The SDL surface is ABGR8888, which is byte-for-byte
 * softgles2's little-endian RGBA8 storage (word 0xAABBGGRR). Honours dirty
 * rects after the first full copy. */
int dos_fb_blit(const void *pixels, int pitch, int w, int h,
                const int *rects, int numrects)
{
    /* The software path has its own backing surface, independent of the GL
     * surface (g_fb), so a non-GL window does not perturb the GL frame. */
    int force_full = 0;
    if (!g_sw_fb || g_sw_fb->w != w || g_sw_fb->h != h) {
        if (g_sw_fb) sgl_fb_destroy(g_sw_fb);
        g_sw_fb = sgl_fb_create_ex(w, h, SGL_FB_COLOR);
        if (!g_sw_fb) {
            return -1;
        }
        DOS_PRESENT_TRACE("dos-present: created SW fb %dx%d -> %p\n", w, h, (void *)g_sw_fb);
        force_full = 1;
    }

    if (!force_full && rects && numrects > 0) {
        sgl_blit_rgba32(g_sw_fb, pixels, pitch, w, h, rects, numrects);
    } else {
        sgl_blit_rgba32(g_sw_fb, pixels, pitch, w, h, NULL, 0);
    }
    (void)dvgpu_present_rgba(g_sw_fb->color, g_sw_fb->w, g_sw_fb->h);
    DOS_PRESENT_TRACE("dos-present: SW blit %dx%d rects=%d\n", w, h, numrects);
    return 0;
}

void dos_fb_get_size(int *w, int *h)
{
    if (w) *w = g_fb ? g_fb->w : 800;
    if (h) *h = g_fb ? g_fb->h : 600;
}
