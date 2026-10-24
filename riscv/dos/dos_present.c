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

#include "sgl.h"
#include "sg_fb.h"        /* sgl_framebuffer, sgl_fb_create */
#include "sgl_overlay.h" /* sgl_overlay_draw (on-screen stats HUD) */
#include "bridge.h"     /* sgl_bridge_init */
#include "dos_virtio_gpu.h"
#include "video-dos/dos_gl.h"

/* The single GL surface for the dOS window (dhewm3 only ever has one). */
static sgl_framebuffer *g_fb;
static sgl_framebuffer *g_sw_fb;   /* separate backing for the software blit path */

/* ---- supersampled anti-aliasing (SSAA) -----------------------------------
 * The engine renders at render-res = output-res * ssaa (set at build time via
 * BAREMETAL_SSAA -> BM_FB_WIDTH/HEIGHT already carry the supersampled size, and
 * BM_SSAA carries the factor). g_fb is the supersampled render target; at
 * present we box-downsample it (ssaa x ssaa average) into g_scanout, which is
 * the virtio-gpu scanout at the real display resolution. With ssaa==1 the old
 * behaviour is kept exactly: g_fb's colour buffer IS the scanout (external
 * bind), and present is copy-free.
 *
 * There is no MSAA in this rasteriser, so SSAA is the only way to resolve the
 * high-contrast texture aliasing (e.g. the stratified rock) at native 1:1. */
static int       g_ssaa = 0;        /* 0 = not yet set; resolved in create_context */
static uint32_t *g_scanout;         /* display-res scanout (ssaa>1 only) */
static int       g_ow, g_oh;        /* output (display) resolution */

/* Runtime override of the SSAA factor (bm_ssaa cvar). Must be called before
 * dos_gl_create_context to take effect; clamped to [1,4]. */
void dos_gl_set_ssaa(int s)
{
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    g_ssaa = s;
}

static int dos_ssaa_factor(void)
{
    if (g_ssaa > 0) return g_ssaa;      /* explicit runtime override wins */
#if defined(BM_SSAA)
    return (BM_SSAA) >= 1 ? (BM_SSAA) : 1;
#else
    return 1;
#endif
}

#if !defined(SGL_BAREMETAL)
#define DOS_PRESENT_TRACE(...) do { if (getenv("DOS_TRACE")) fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define DOS_PRESENT_TRACE(...) ((void)0)
#endif

void *dos_gl_create_context(int w, int h)
{
    int ssaa = dos_ssaa_factor();
    g_ssaa = ssaa;                 /* resolve compile default into the runtime flag */
    if (w <= 0) w = 800;
    if (h <= 0) h = 600;
    if (!g_fb) {
        g_fb = sgl_fb_create(w, h);   /* (w,h) = supersampled render resolution */
        if (!g_fb) {
            return NULL;
        }
        if (ssaa > 1) {
            /* Decoupled: the render fb keeps its own colour buffer; the scanout
             * is a separate display-res surface we downsample into at present. */
            g_ow = w / ssaa;
            g_oh = h / ssaa;
            g_scanout = dvgpu_scanout_rgba(g_ow, g_oh);
            DOS_PRESENT_TRACE("dos-present: SSAA %dx render %dx%d -> scanout %dx%d\n",
                              ssaa, w, h, g_ow, g_oh);
        } else {
            /* ssaa==1: bind the scanout directly as the fb colour (copy-free). */
            uint32_t *scanout = dvgpu_scanout_rgba(w, h);
            if (scanout) {
                sgl_fb_bind_external_color(g_fb, scanout);
            }
            g_ow = w; g_oh = h;
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
extern void sg_frame_reset(void);      /* rewind the per-frame scratch arena */
extern int sgl_prof_enabled;
extern uint64_t sgl_prof_present;
extern uint64_t sgl_prof_now(void);
extern void sgl_prof_frame_report(void);

/* ---- present-time gamma / brightness --------------------------------------
 * Doom 3's bare-metal GL path uses r_gammaInShader: dhewm3 injects brightness +
 * POW into ARB fragment programs, gl4es uploads program.env[21], and softgles2
 * executes it. A second full-frame gamma pass at scanout is therefore redundant
 * for the lit world and costs several ms/frame under QEMU TCG. Keep this as an
 * off-by-default fallback for fixed-function/UI comparisons via bm_presentGamma. */
int   dos_present_gamma_enable = 0;
float dos_present_gamma_brightness = 2.00f;   /* fallback pre-gamma multiply */
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

static const uint8_t *dos_present_gamma_lut(void)
{
    if (!dos_present_gamma_enable) return NULL;
    if (!s_gamma_lut_ready || s_gamma_lut_bri != dos_present_gamma_brightness) {
        dos_present_build_gamma_lut();
    }
    return s_gamma_lut;
}

/* Per-frame framebuffer digest, printed when BM_FB_HASH=1.
 *
 * This is the FrameBin regression gate. FrameBin replays per-tile draw refs in
 * submission order specifically so that deferred rendering is PIXEL-IDENTICAL to
 * immediate mode -- so any difference is a bug, not a tradeoff, and the way to
 * find one is to render the same frame both ways and compare. Screendump-based
 * comparison needs a GPU-enabled run and host-side PPM diffing; a digest printed
 * on the serial log works in the ordinary headless profiling run and diffs with
 * `diff`.
 *
 * FNV-1a over the whole colour buffer. Not cryptographic -- it only has to make
 * an accidental collision between two genuinely different frames implausible. */
static int dos_fb_hash_enabled(void)
{
#if defined(BM_FB_HASH)
    return BM_FB_HASH;
#else
    return 0;
#endif
}

static void dos_fb_hash_report(const sgl_framebuffer *fb)
{
    static unsigned long frame_no;
    const uint32_t *px = fb->color;
    size_t n = (size_t)fb->w * (size_t)fb->h;
    uint64_t h = 1469598103934665603ull;   /* FNV-1a offset basis */
    size_t i;

    if (!px) return;
    for (i = 0; i < n; ++i) {
        h ^= (uint64_t)px[i];
        h *= 1099511628211ull;
    }
    printf("[fbhash] frame=%lu %dx%d h=%016llx\n",
           frame_no++, fb->w, fb->h, (unsigned long long)h);
}

/* Deterministic virtual clock: advance one frame length per present. Weak so
 * the softgles2 standalone tests, which have no engine, still link. See
 * BM_DETERMINISTIC_CLOCK in riscv/engine/sys_baremetal.cpp. */
extern void BM_VClockTick(void) __attribute__((weak));
extern void BM_BootPresented(void) __attribute__((weak));

int dos_gl_present(void *ctx)
{
    sgl_framebuffer *fb = ctx ? (sgl_framebuffer *)ctx : g_fb;
    if (!fb) {
        return -1;
    }
    if (BM_VClockTick) BM_VClockTick();
    /* Frame boundary: ensure any deferred draws have rasterised into fb before
     * we read its pixels for display. */
    unsigned long long pf_t0 = sgl_prof_now();
    sg_framebin_flush();
    /* After the flush, before the scanout copy: this must digest exactly what
     * FrameBin produced. */
    if (dos_fb_hash_enabled()) dos_fb_hash_report(fb);
    (void)sgl_diag_present_report(fb, 0);   /* optional per-frame diagnostics (off by default) */
    /* Stats HUD. Strictly AFTER the digest above: it writes pixels, so drawing
     * it first would change every [fbhash] and silently invalidate the FrameBin
     * / VS-dedup pixel-identity gates. Before the scanout copy, so it reaches
     * the display. Reads the sg_prof counters, which sgl_prof_frame_report()
     * resets below -- hence also before that. No-op unless enabled. */
    sgl_overlay_draw(fb);
    /* Resolve SSAA (if any) then hand the display-res buffer to the scanout.
     * Optional fallback gamma is fused into the virtio copy. With ssaa==1 and
     * the direct scanout framebuffer and fallback disabled, this is copy-free. */
    int present_result;
    if (g_ssaa > 1 && g_scanout) {
        int ssaa = g_ssaa;
        sgl_downsample_rgba_box(fb->color, fb->w, fb->h, g_scanout, g_ow, g_oh, ssaa);
        present_result = dvgpu_present_rgba_gamma(g_scanout, g_ow, g_oh, dos_present_gamma_lut());
    } else {
        present_result = dvgpu_present_rgba_gamma(fb->color, fb->w, fb->h, dos_present_gamma_lut());
    }
    if (present_result == 0 && BM_BootPresented) BM_BootPresented();
    if (sgl_prof_enabled) {
        sgl_prof_present += sgl_prof_now() - pf_t0;   /* flush + blit time */
        sgl_prof_frame_report();
#if BM_SHIM_PROFILE
        { extern void shim_profile_report(void); shim_profile_report(); }
#endif
    }
    { extern void sgl_pareto_report(void); sgl_pareto_report(); }
    /* Rewind the per-frame bump arena. sg_draw.c allocates every draw's
     * projected verts / varyings from it and documents them as "reclaimed at
     * present", but the only caller of sg_frame_reset() was sgl_present(), which
     * this engine path never uses -- so the arena only ever grew. It shows up in
     * [rprof-heap] as committed climbing 191MB -> 670MB over 66 frames (~7MB per
     * frame); past the 128MiB reserve every frame then mallocs a fresh >=16MiB
     * overflow chunk that is never freed. Must run after sg_framebin_flush()
     * above, which still reads the arena. */
    sg_frame_reset();
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
