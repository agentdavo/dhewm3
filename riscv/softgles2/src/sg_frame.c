#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#endif
#include "sg_frame.h"

/* Reserve size. Lazily malloc'd once (never freed — the bump allocator can't
 * reclaim it anyway), then bump-allocated within and reset per frame. Sized to
 * hold one 800x600 frame's vertex/index scratch with headroom; tune via the
 * high-water / overflow counters. */
#ifndef SG_FRAME_RESERVE_BYTES
#define SG_FRAME_RESERVE_BYTES (128u * 1024u * 1024u)
#endif

static unsigned char *g_pool;
static size_t g_cap;
static size_t g_cursor;
static size_t g_high;
static size_t g_overflow;
static int    g_init_failed;

static void ensure_pool(void)
{
    if (g_pool || g_init_failed) return;
    g_pool = (unsigned char *)malloc(SG_FRAME_RESERVE_BYTES);
    if (g_pool) g_cap = SG_FRAME_RESERVE_BYTES;
    else        g_init_failed = 1;   /* no reserve -> always fall back to malloc */
}

void *sg_frame_alloc(size_t n)
{
    if (n == 0) n = 1;
    n = (n + 15u) & ~(size_t)15u;          /* 16-byte align, like malloc */
    ensure_pool();
    if (g_pool) {
        size_t base = (g_cursor + 15u) & ~(size_t)15u;
        if (base + n <= g_cap) {
            g_cursor = base + n;
            if (g_cursor > g_high) g_high = g_cursor;
            return g_pool + base;
        }
    }
    /* Reserve exhausted or unavailable: leak via global malloc (matches the
     * pre-arena behaviour). Counted so the reserve size can be retuned. */
    g_overflow += n;
    return malloc(n);
}

void sg_frame_reset(void)
{
    g_cursor = 0;
}

size_t sg_frame_highwater(void) { return g_high; }
size_t sg_frame_overflow(void)  { return g_overflow; }
