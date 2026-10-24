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

typedef struct sg_frame_chunk {
    struct sg_frame_chunk *next;
    size_t cap, cursor;
    uintptr_t _align_pad;       /* data starts on a 16-byte boundary */
    unsigned char data[];
} sg_frame_chunk;
static sg_frame_chunk *g_chunks;

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
    /* Reserve exhausted: use reusable overflow chunks. The old fallback called
     * malloc for every request and never freed it, growing Doom's heap by
     * 2-15 MiB every frame. Chunks persist at the peak frame size and their
     * cursors reset at present, just like the primary arena. */
    for (sg_frame_chunk *c=g_chunks;c;c=c->next) {
        size_t base=(c->cursor+15u)&~(size_t)15u;
        if (base+n<=c->cap) { c->cursor=base+n; return c->data+base; }
    }
    size_t cap=16u*1024u*1024u;
    if (cap<n) cap=(n+15u)&~(size_t)15u;
    if (cap > (size_t)-1 - sizeof(sg_frame_chunk)) return 0;
    sg_frame_chunk *c=(sg_frame_chunk *)malloc(sizeof(*c)+cap);
    if (!c) return 0;
    c->next=g_chunks; c->cap=cap; c->cursor=n; g_chunks=c;
    g_overflow += cap;
    return c->data;
}

void sg_frame_reset(void)
{
    g_cursor = 0;
    for (sg_frame_chunk *c=g_chunks;c;c=c->next) c->cursor=0;
}

size_t sg_frame_highwater(void) { return g_high; }
size_t sg_frame_overflow(void)  { return g_overflow; }
