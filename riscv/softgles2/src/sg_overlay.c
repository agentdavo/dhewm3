/* softgles2 on-screen statistics overlay (see include/sgl_overlay.h).
 *
 * Writes glyphs directly into the colour buffer at present time. No GL calls,
 * no allocation, no dependency on the rest of the pipeline -- this has to keep
 * working when the pipeline is what is broken.
 */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdio.h>
#  include <string.h>
#endif

#include "sgl_overlay.h"
#include "sg_prof.h"
#include "sg_overlay_font.h"

/* ---- host timebase -------------------------------------------------------
 * Same contract as sg_prof.c: ticks come from rdtime, and the tick->ns scale is
 * read from the device tree rather than assumed, so the numbers stay correct on
 * silicon that does not clock mtime at QEMU virt's 10 MHz. */
extern uint64_t timer_ticks(void);
#if defined(SGL_BAREMETAL)
extern uint64_t timer_ns_from_ticks(uint64_t ticks);
#else
static uint64_t timer_ns_from_ticks(uint64_t t) { (void)t; return 0; }
#endif

/* ---- state --------------------------------------------------------------- */

static int g_enabled;
static int g_scale = 1;
static int g_corner = SGL_OVERLAY_TOP_LEFT;
static int g_prof_was_enabled;   /* restored on disable */

static uint64_t g_last_ticks;
static uint64_t g_frame_ns;      /* EMA-smoothed frame time */
static uint64_t g_min_ns, g_max_ns;
static unsigned g_window;        /* frames since min/max were reset */
static uint64_t g_hud_ns;        /* cost of drawing the overlay itself */

static struct { const char *name; long value; } g_stats[SGL_OVERLAY_MAX_STATS];
static int g_nstats;

/* Shader-program registry totals, owned by glsl.c (populated when
 * glsl_link_dump is on, which sgl_overlay_enable turns on). */
extern void glsl_dbg_workload_totals(int *nprogs, int *nkernel,
                                     unsigned long *frags_kernel,
                                     unsigned long *frags_vm);
extern int glsl_link_dump;

/* Vertex-stage per-path vertex counts (reset each frame by sg_prof). */
extern volatile unsigned long sgl_prof_vs_simt_verts;
extern volatile unsigned long sgl_prof_vs_kernel_verts;
extern volatile unsigned long sgl_prof_vs_scalar_verts;

extern unsigned long dos_heap_committed_bytes(void) __attribute__((weak));
extern unsigned long dos_heap_capacity_bytes(void) __attribute__((weak));

void sgl_overlay_enable(int on)
{
    on = on ? 1 : 0;
    if (on == g_enabled) return;
    if (on) {
        /* Every count the overlay shows is accumulated under sgl_prof_enabled,
         * so the overlay is useless without it. Quiet, because the serial
         * [rprof] block is ~8 lines a frame and would drown the console the
         * moment anyone turned on a *visual* readout. */
        g_prof_was_enabled = sgl_prof_enabled;
        sgl_prof_enabled = 1;
        sgl_prof_quiet = 1;
        glsl_link_dump = 1;      /* per-program kernel/VM fragment attribution */
        g_last_ticks = 0;
        g_min_ns = 0; g_max_ns = 0; g_window = 0;
    } else {
        sgl_prof_enabled = g_prof_was_enabled;
        sgl_prof_quiet = 0;
    }
    g_enabled = on;
}

int sgl_overlay_enabled(void) { return g_enabled; }

void sgl_overlay_set_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    g_scale = scale;
}

void sgl_overlay_set_corner(int corner)
{
    if (corner < 0 || corner > SGL_OVERLAY_BOTTOM_RIGHT) corner = SGL_OVERLAY_TOP_LEFT;
    g_corner = corner;
}

void sgl_overlay_set_stat(const char *name, long value)
{
    if (!name) return;
    for (int i = 0; i < g_nstats; ++i) {
        if (g_stats[i].name == name || !strcmp(g_stats[i].name, name)) {
            g_stats[i].value = value;
            return;
        }
    }
    if (g_nstats >= SGL_OVERLAY_MAX_STATS) return;
    g_stats[g_nstats].name = name;
    g_stats[g_nstats].value = value;
    ++g_nstats;
}

/* ---- pixel plumbing ------------------------------------------------------ */

/* Colour buffer is RGBA8 stored little-endian as 0xAABBGGRR. */
#define RGB(r, g, b) ((uint32_t)0xff000000u | ((uint32_t)(b) << 16) | \
                      ((uint32_t)(g) << 8) | (uint32_t)(r))

/* Darken a block so light-on-dark text stays readable over any scene. Cheap
 * shift-based 25% keep, no blend unit involved. */
static void panel(sgl_framebuffer *fb, int x0, int y0, int w, int h)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > fb->w) w = fb->w - x0;
    if (y0 + h > fb->h) h = fb->h - y0;
    for (int y = 0; y < h; ++y) {
        uint32_t *row = fb->color + (size_t)(y0 + y) * fb->w + x0;
        for (int x = 0; x < w; ++x) {
            uint32_t c = row[x];
            row[x] = 0xff000000u | (((c >> 2) & 0x003f3f3fu));
        }
    }
}

static void glyph(sgl_framebuffer *fb, int px, int py, unsigned char ch, uint32_t col)
{
    if (ch < SGL_FONT_FIRST || ch > SGL_FONT_LAST) ch = '?';
    const unsigned char *bits = sgl_font_bits[ch - SGL_FONT_FIRST];
    const int s = g_scale;
    for (int y = 0; y < SGL_FONT_H; ++y) {
        unsigned b = bits[y];
        if (!b) continue;
        for (int x = 0; x < SGL_FONT_W; ++x) {
            if (!(b & (0x80u >> x))) continue;
            for (int sy = 0; sy < s; ++sy) {
                int fy = py + y * s + sy;
                if (fy < 0 || fy >= fb->h) continue;
                uint32_t *row = fb->color + (size_t)fy * fb->w;
                for (int sx = 0; sx < s; ++sx) {
                    int fx = px + x * s + sx;
                    if (fx >= 0 && fx < fb->w) row[fx] = col;
                }
            }
        }
    }
}

static void text(sgl_framebuffer *fb, int px, int py, const char *s, uint32_t col)
{
    for (; *s; ++s) {
        glyph(fb, px, py, (unsigned char)*s, col);
        px += SGL_FONT_W * g_scale;
    }
}

/* ---- formatting ----------------------------------------------------------
 * The bare-metal printf has no %f, and the profiler's convention is to split
 * fixed-point into two integers rather than pretend otherwise. Same here. */

#define NS_MS_A(ns) (unsigned long)((ns) / 1000000ull)
#define NS_MS_B(ns) (unsigned long)(((ns) / 1000ull) % 1000ull)

/* Thousands/millions with one decimal, so 5-million-fragment frames stay
 * readable in a fixed-width column. */
static void count_str(char *buf, int n, unsigned long v)
{
    if (v >= 1000000ul)
        snprintf(buf, n, "%lu.%luM", v / 1000000ul, (v / 100000ul) % 10ul);
    else if (v >= 10000ul)
        snprintf(buf, n, "%lu.%luk", v / 1000ul, (v / 100ul) % 10ul);
    else
        snprintf(buf, n, "%lu", v);
}

static unsigned long pct(uint64_t part, uint64_t whole)
{
    return whole ? (unsigned long)(part * 100ull / whole) : 0ul;
}

/* ---- the HUD ------------------------------------------------------------- */

#define MAXLINE 64
#define MAXROWS 14

void sgl_overlay_draw(sgl_framebuffer *fb)
{
    if (!g_enabled || !fb || !fb->color) return;

    uint64_t t_begin = timer_ticks();

    /* Frame pacing. Measured present-to-present, which is what "fps" means to
     * anyone reading it -- not the sum of the stage timers, which excludes
     * whatever the application did between frames. */
    if (g_last_ticks) {
        uint64_t dns = timer_ns_from_ticks(t_begin - g_last_ticks);
        /* EMA (7/8 old + 1/8 new) so the number is readable rather than
         * flickering, but min/max below preserve the spikes it hides. */
        g_frame_ns = g_frame_ns ? (g_frame_ns * 7 + dns) / 8 : dns;
        if (!g_min_ns || dns < g_min_ns) g_min_ns = dns;
        if (dns > g_max_ns) g_max_ns = dns;
        if (++g_window >= 120) { g_min_ns = 0; g_max_ns = 0; g_window = 0; }
    }
    g_last_ticks = t_begin;

    char rows[MAXROWS][MAXLINE];
    int  n = 0;
    char a[16], b[16], c[16];

    /* --- frame pacing --- */
    {
        /* Two decimals, not one. Under TCG a frame takes seconds, so fps is well
         * below 1 and a single decimal truncates every real reading to "0.0".
         * 100*1e9/ns keeps 0.01 fps resolution and still cannot overflow: at
         * 1us/frame it is 1e8, far inside unsigned long. */
        unsigned long fps100 = g_frame_ns ? (unsigned long)(100000000000ull / g_frame_ns) : 0;
        snprintf(rows[n++], MAXLINE, "softgles2 %dx%d  hud %lu.%03lums",
                 fb->w, fb->h, NS_MS_A(g_hud_ns), NS_MS_B(g_hud_ns));
        snprintf(rows[n++], MAXLINE, "fps %lu.%02lu  frame %lu.%03lums",
                 fps100 / 100, fps100 % 100, NS_MS_A(g_frame_ns), NS_MS_B(g_frame_ns));
        snprintf(rows[n++], MAXLINE, "  min %lu.%03lu  max %lu.%03lums",
                 NS_MS_A(g_min_ns), NS_MS_B(g_min_ns),
                 NS_MS_A(g_max_ns), NS_MS_B(g_max_ns));
    }

    /* --- geometry --- */
    {
        unsigned long verts = (unsigned long)(sgl_prof_vs_simt_verts +
                                              sgl_prof_vs_kernel_verts +
                                              sgl_prof_vs_scalar_verts);
        count_str(a, sizeof a, (unsigned long)sgl_prof_draws);
        count_str(b, sizeof b, (unsigned long)sgl_prof_tris);
        count_str(c, sizeof c, verts);
        snprintf(rows[n++], MAXLINE, "draws %s  tris %s  verts %s", a, b, c);
    }

    /* --- fragments --- */
    {
        count_str(a, sizeof a, (unsigned long)sgl_prof_frags);
        count_str(b, sizeof b, (unsigned long)sgl_prof_frag_lanes);
        /* occupancy = useful lanes / executed lanes. A low number means the
         * SIMT batches are running mostly-masked-off and the width is wasted. */
        snprintf(rows[n++], MAXLINE, "frags %s lanes %s occ %lu%%",
                 a, b, sgl_prof_frag_lanes ? pct(sgl_prof_frags, sgl_prof_frag_lanes) : 100ul);
    }

    /* --- stage breakdown ---
     * mt is the submitter's view of the parallel section and MUST be in the
     * total; see the long note in sg_prof.c about this exact total being wrong
     * twice before. */
    {
        uint64_t vtx = timer_ns_from_ticks(sgl_prof_vertex);
        uint64_t bin = timer_ns_from_ticks(sgl_prof_mt_bin);
        uint64_t ras = timer_ns_from_ticks(sgl_prof_mt_mainbusy + sgl_prof_raster);
        /* present/total are latched from the PREVIOUS frame: present is not
         * accumulated until after this overlay has drawn, so reading the live
         * accumulator here would always print 0.000. */
        uint64_t pre = timer_ns_from_ticks(sgl_prof_last_present);
        uint64_t tot = timer_ns_from_ticks(sgl_prof_last_total);
        snprintf(rows[n++], MAXLINE, "vtx %lu.%03lu bin %lu.%03lu ras %lu.%03lums",
                 NS_MS_A(vtx), NS_MS_B(vtx), NS_MS_A(bin), NS_MS_B(bin),
                 NS_MS_A(ras), NS_MS_B(ras));
        snprintf(rows[n++], MAXLINE, "present %lu.%03lu  sgl-total %lu.%03lums",
                 NS_MS_A(pre), NS_MS_B(pre), NS_MS_A(tot), NS_MS_B(tot));
    }

    /* --- parallelism --- */
    {
        uint64_t hartsum = 0, hartmax = 0, hartmin = (uint64_t)-1;
        int harts = 0;
        for (int h = 0; h < SGL_PROF_MAXHARTS; ++h) {
            if (!sgl_prof_hart_tiles[h]) continue;
            uint64_t busy = sgl_prof_hart_busy[h];
            hartsum += busy;
            if (busy > hartmax) hartmax = busy;
            if (busy < hartmin) hartmin = busy;
            ++harts;
        }
        if (!harts) hartmin = 0;
        uint64_t wait = timer_ns_from_ticks(sgl_prof_mt_wait);
        /* balance = slowest vs fastest worker. 100% is perfect; a low number
         * means tiles are unevenly sized and harts are idling at the join. */
        snprintf(rows[n++], MAXLINE, "harts %d  bal %lu%%  wait %lu.%03lums",
                 harts, hartmax ? pct(hartmin, hartmax) : 100ul,
                 NS_MS_A(wait), NS_MS_B(wait));

        /* shade = time inside the fragment shader; wrapper = coverage, early-Z,
         * varying interp, blend, store. Which one dominates decides whether
         * shader work or raster overhead is worth optimising. */
        uint64_t shade = sgl_prof_shade;
        snprintf(rows[n++], MAXLINE, "shade %lu%%  wrapper %lu%%",
                 hartsum ? pct(shade, hartsum) : 0ul,
                 hartsum ? 100ul - pct(shade, hartsum) : 0ul);
    }

    /* --- shaders --- */
    {
        int nprogs = 0, nkernel = 0;
        unsigned long fk = 0, fv = 0;
        glsl_dbg_workload_totals(&nprogs, &nkernel, &fk, &fv);
        snprintf(rows[n++], MAXLINE, "programs %d  kernel %d  vm-frags %lu%%",
                 nprogs, nkernel, (fk + fv) ? (unsigned long)(fv * 100ul / (fk + fv)) : 0ul);
    }

    /* --- heap (weak: absent in the standalone softgles2 tests) --- */
    if (dos_heap_committed_bytes && dos_heap_capacity_bytes) {
        unsigned long u = dos_heap_committed_bytes() >> 20;
        unsigned long cap = dos_heap_capacity_bytes() >> 20;
        snprintf(rows[n++], MAXLINE, "heap %luMB / %luMB", u, cap);
    }

    /* --- application-supplied counters --- */
    for (int i = 0; i < g_nstats && n < MAXROWS; ) {
        int used = snprintf(rows[n], MAXLINE, "%s %ld", g_stats[i].name, g_stats[i].value);
        ++i;
        /* pack a second stat onto the line when it fits */
        if (i < g_nstats && used > 0 && used < MAXLINE - 18)
            snprintf(rows[n] + used, MAXLINE - used, "  %s %ld",
                     g_stats[i].name, g_stats[i].value), ++i;
        ++n;
    }

    /* --- blit --- */
    const int pad = 4 * g_scale;
    int cols = 0;
    for (int i = 0; i < n; ++i) {
        int len = (int)strlen(rows[i]);
        if (len > cols) cols = len;
    }
    int bw = cols * SGL_FONT_W * g_scale + pad * 2;
    int bh = n * SGL_FONT_H * g_scale + pad * 2;
    int bx = (g_corner == SGL_OVERLAY_TOP_RIGHT || g_corner == SGL_OVERLAY_BOTTOM_RIGHT)
             ? fb->w - bw - pad : pad;
    int by = (g_corner == SGL_OVERLAY_BOTTOM_LEFT || g_corner == SGL_OVERLAY_BOTTOM_RIGHT)
             ? fb->h - bh - pad : pad;
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;

    panel(fb, bx, by, bw, bh);
    for (int i = 0; i < n; ++i) {
        /* first line is the title, rest are data */
        uint32_t col = (i == 0) ? RGB(255, 208, 64) : RGB(190, 255, 190);
        text(fb, bx + pad, by + pad + i * SGL_FONT_H * g_scale, rows[i], col);
    }

    /* Self-cost, shown on the title line next frame. The overlay runs inside
     * the window sg_prof attributes to `present`, so without this its cost
     * would silently inflate a stage timing it is supposed to be reporting. */
    g_hud_ns = timer_ns_from_ticks(timer_ticks() - t_begin);
}
