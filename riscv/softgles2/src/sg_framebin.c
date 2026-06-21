/* FrameBin — frame-scoped sort-middle deferred tiled rasteriser (see header). */
#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#endif

#include "sg_framebin.h"
#include "sg_frame.h"
#include "sg_frag.h"
#include "sg_raster.h"
#include "sg_mt_internal.h"
#include "sg_mt_prim.h"
#include "sg_mt_sched.h"

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#  define SG_FRAMEBIN_RVV 1
#else
#  define SG_FRAMEBIN_RVV 0
#endif

int sgl_framebin_enabled = 0;

#if defined(SGL_BAREMETAL)

/* One recorded command: a programmable draw or a clear. fc/state/ustore are
 * copied by value so workers shade with this command's own state at flush. */
typedef struct {
    unsigned char    is_clear;
    sgl_framebuffer *fb;
    /* draw */
    const sgl_pvert *pv;
    int              nvary, front_ccw, cull_face;
    GLenum           cull_mode;
    sgl_drawstate    state;
    sg_frag_ctx      fc;            /* fc.ustore -> snapshot below */
    sgl_frag_fn      fs_scalar;
    sgl_frag_simt_fn fs_simt;
    /* clear */
    GLbitfield       clear_mask;
    uint32_t         clear_color;
    float            clear_depth;
    unsigned char    clear_stencil;
} fb_cmd;

/* One bin item: a clear (a<0, covers every tile) or a triangle of a draw's pv.
 * range is the inclusive tile span this item touches (tx0>tx1 == culled). */
typedef struct {
    int          cmd;
    int          a, b, c;
    sg_tile_range range;
} fb_item;

static fb_cmd  *g_cmds;  static int g_ncmd,  g_capcmd;
static fb_item *g_items; static int g_nitem, g_capitem;

static void *fb_grow(void *old, int *cap, int need, size_t elem)
{
    if (need <= *cap) return old;
    int nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    void *p = sg_frame_alloc((size_t)nc * elem);
    if (p && old && *cap) memcpy(p, old, (size_t)(*cap) * elem);
    *cap = nc;
    return p;
}

static void fb_record_reset(void)
{
    /* Arena memory is reclaimed at present; drop stale pointers/caps so the next
     * frame re-allocates fresh after sg_frame_reset(). */
    g_cmds = 0; g_ncmd = 0; g_capcmd = 0;
    g_items = 0; g_nitem = 0; g_capitem = 0;
}

static fb_cmd *fb_new_cmd(void)
{
    g_cmds = (fb_cmd *)fb_grow(g_cmds, &g_capcmd, g_ncmd + 1, sizeof(fb_cmd));
    if (!g_cmds) return 0;
    fb_cmd *c = &g_cmds[g_ncmd++];
    memset(c, 0, sizeof(*c));
    return c;
}

static fb_item *fb_new_item(void)
{
    g_items = (fb_item *)fb_grow(g_items, &g_capitem, g_nitem + 1, sizeof(fb_item));
    if (!g_items) return 0;
    return &g_items[g_nitem++];
}

int sg_framebin_record_draw(const sg_draw_packet *draw, GLenum mode, GLsizei count,
                            const sgl_pvert *pv, glsl_program *gp)
{
    fb_cmd *c = fb_new_cmd();
    if (!c) return 0;
    int ci = g_ncmd - 1;
    c->is_clear  = 0;
    c->fb        = (sgl_framebuffer *)draw->fb;
    c->pv        = pv;
    c->nvary     = draw->nvary;
    c->front_ccw = draw->front_ccw;
    c->cull_face = draw->cull_face;
    c->cull_mode = draw->cull_mode;
    c->state     = draw->state;
    c->fc        = *(const sg_frag_ctx *)draw->frag_ctx;   /* value copy */
    c->fs_scalar = draw->fs_scalar;
    c->fs_simt   = draw->fs_simt;
    /* Snapshot the program's uniform store so this command shades with its own
     * uniforms even though the program object is rewritten by later draws. */
    if (gp) {
        float *snap = (float *)sg_frame_alloc(sizeof(float) * GLSL_USTORE_FLOATS);
        if (snap) {
            memcpy(snap, glsl_ustore(gp), sizeof(float) * GLSL_USTORE_FLOATS);
            c->fc.ustore = snap;
        }
    }

    /* Assemble primitives (indices into pv) and append one item per triangle. */
    int maxprim = (mode == GL_TRIANGLES)      ? (int)(count / 3) :
                  (mode == GL_TRIANGLE_STRIP ||
                   mode == GL_TRIANGLE_FAN)   ? (int)(count - 2) : 0;
    if (maxprim <= 0) return 1;
    sg_prim_desc *prims = (sg_prim_desc *)sg_frame_alloc(sizeof(sg_prim_desc) * (size_t)maxprim);
    if (!prims) return 1;
    int nprim = sg_mt_prim_build(mode, count, prims);
    for (int i = 0; i < nprim; ++i) {
        fb_item *it = fb_new_item();
        if (!it) break;
        it->cmd = ci;
        it->a = prims[i].a; it->b = prims[i].b; it->c = prims[i].c;
        it->range.tx0 = 0; it->range.ty0 = 0; it->range.tx1 = -1; it->range.ty1 = 0; /* set at flush */
    }
    return 1;
}

void sg_framebin_record_clear(sgl_framebuffer *fb, GLbitfield mask,
                              uint32_t color, float depth, unsigned char stencil)
{
    fb_cmd *c = fb_new_cmd();
    if (!c) return;
    int ci = g_ncmd - 1;
    c->is_clear      = 1;
    c->fb            = fb;
    c->clear_mask    = mask;
    c->clear_color   = color;
    c->clear_depth   = depth;
    c->clear_stencil = stencil;
    fb_item *it = fb_new_item();
    if (!it) return;
    it->cmd = ci;
    it->a = -1;   /* clear marker; range = whole grid (set at flush) */
}

/* ---- flush: bin once, drain across harts ---- */

typedef struct {
    const fb_cmd  *cmds;
    const fb_item *items;
    const int     *offsets;     /* [ntiles+1] */
    const int     *refs;        /* item indices, submission order within a tile */
    const int     *active;      /* compact active-tile list */
    int            active_count;
    int            tiles_x, tiles_y, fbw, fbh;
    sg_mt_tile_layout layout;
} fb_job;

typedef struct { const fb_job *job; volatile int tile_next; } fb_sched_ctx;

static void fb_clear_rect(const fb_cmd *c, int x0, int y0, int x1, int y1)
{
    sgl_framebuffer *fb = c->fb;
    if (!fb) return;
    int span = x1 - x0;
    if (span <= 0) return;
#if SG_FRAMEBIN_RVV
    /* Item 9 (RVV audit): splat the clear constants into vector registers ONCE,
     * not once per scanline+strip. The store value is invariant across the whole
     * rect, so a splat built at vlmax is reused for every (smaller-vl) tail store
     * (vse uses its own vl). color(8)+depth(8)+stencil(8)=24 vregs <= 32, no spill. */
    const int do_c = (c->clear_mask & GL_COLOR_BUFFER_BIT) && fb->color;
    const int do_d = (c->clear_mask & GL_DEPTH_BUFFER_BIT) && fb->depth;
    const int do_s = (c->clear_mask & GL_STENCIL_BUFFER_BIT) && fb->stencil;
    vuint32m8_t  cv = __riscv_vmv_v_x_u32m8(c->clear_color, __riscv_vsetvlmax_e32m8());
    vfloat32m8_t dv = __riscv_vfmv_v_f_f32m8(c->clear_depth, __riscv_vsetvlmax_e32m8());
    vuint8m8_t   sv = __riscv_vmv_v_x_u8m8(c->clear_stencil, __riscv_vsetvlmax_e8m8());
    for (int y = y0; y < y1; ++y) {
        size_t row = (size_t)y * (size_t)fb->w + (size_t)x0;
        if (do_c)
            for (size_t i = 0; i < (size_t)span; ) { size_t vl = __riscv_vsetvl_e32m8((size_t)span - i);
                __riscv_vse32_v_u32m8(&fb->color[row + i], cv, vl); i += vl; }
        if (do_d)
            for (size_t i = 0; i < (size_t)span; ) { size_t vl = __riscv_vsetvl_e32m8((size_t)span - i);
                __riscv_vse32_v_f32m8(&fb->depth[row + i], dv, vl); i += vl; }
        if (do_s)
            for (size_t i = 0; i < (size_t)span; ) { size_t vl = __riscv_vsetvl_e8m8((size_t)span - i);
                __riscv_vse8_v_u8m8(&fb->stencil[row + i], sv, vl); i += vl; }
    }
#else
    for (int y = y0; y < y1; ++y) {
        size_t row = (size_t)y * (size_t)fb->w + (size_t)x0;
        if ((c->clear_mask & GL_COLOR_BUFFER_BIT) && fb->color)
            for (int x = 0; x < span; ++x) fb->color[row + x] = c->clear_color;
        if ((c->clear_mask & GL_DEPTH_BUFFER_BIT) && fb->depth)
            for (int x = 0; x < span; ++x) fb->depth[row + x] = c->clear_depth;
        if ((c->clear_mask & GL_STENCIL_BUFFER_BIT) && fb->stencil)
            for (int x = 0; x < span; ++x) fb->stencil[row + x] = c->clear_stencil;
    }
#endif
}

static void fb_tile_emit(const fb_job *j, int tile)
{
    int tx = tile % j->tiles_x, ty = tile / j->tiles_x;
    int cx0 = tx * j->layout.w, cy0 = ty * j->layout.h;
    int cx1 = cx0 + j->layout.w, cy1 = cy0 + j->layout.h;
    if (cx1 > j->fbw) cx1 = j->fbw;
    if (cy1 > j->fbh) cy1 = j->fbh;
    int begin = j->offsets[tile], end = j->offsets[tile + 1];
    for (int r = begin; r < end; ++r) {
        const fb_item *it = &j->items[j->refs[r]];
        const fb_cmd  *c  = &j->cmds[it->cmd];
        if (c->is_clear) { fb_clear_rect(c, cx0, cy0, cx1, cy1); continue; }
        const sgl_pvert *a = &c->pv[it->a], *b = &c->pv[it->b], *cc = &c->pv[it->c];
        float area = (b->x - a->x) * (cc->y - a->y) - (b->y - a->y) * (cc->x - a->x);
        int front = c->front_ccw ? (area < 0.f) : (area > 0.f);
        if (c->cull_face) {
            int cull = (c->cull_mode == GL_FRONT_AND_BACK) ? 1 : (c->cull_mode == GL_BACK) ? !front : front;
            if (cull) continue;
        }
        sgl_drawstate st = c->state;
        st.stencil_frontface = front;
        st.clip_x0 = cx0; st.clip_y0 = cy0; st.clip_x1 = cx1; st.clip_y1 = cy1;
        sgl_raster_tri_prog(c->fb, a, b, cc, c->nvary, &st,
                            c->fs_scalar, c->fs_simt, (void *)&c->fc);
    }
}

static void fb_work(void *vctx, int worker, int nworkers)
{
    (void)worker; (void)nworkers;
    fb_sched_ctx *sc = (fb_sched_ctx *)vctx;
    const fb_job *j = sc->job;
    /* batch-grab the tile cursor (see sg_mt_sched.c): RELAXED block claims cut
     * the cursor cache-line bounce. Disjoint tiles -> order-independent. */
    for (;;) {
        int base = __atomic_fetch_add(&sc->tile_next, SG_MT_GRAB, __ATOMIC_RELAXED);
        if (base >= j->active_count) break;
        int end = base + SG_MT_GRAB; if (end > j->active_count) end = j->active_count;
        for (int t = base; t < end; ++t)
            fb_tile_emit(j, j->active[t]);
    }
}

/* Fallback when commands span multiple framebuffers (e.g. mid-frame FBO use):
 * replay items in submission order on the main hart, full-screen (no tile clip),
 * which is exactly immediate mode. Correct, just not parallel. */
static void fb_replay_inorder(void)
{
    for (int i = 0; i < g_nitem; ++i) {
        const fb_item *it = &g_items[i];
        const fb_cmd  *c  = &g_cmds[it->cmd];
        if (c->is_clear) { fb_clear_rect(c, 0, 0, c->fb ? c->fb->w : 0, c->fb ? c->fb->h : 0); continue; }
        const sgl_pvert *a = &c->pv[it->a], *b = &c->pv[it->b], *cc = &c->pv[it->c];
        float area = (b->x - a->x) * (cc->y - a->y) - (b->y - a->y) * (cc->x - a->x);
        int front = c->front_ccw ? (area < 0.f) : (area > 0.f);
        if (c->cull_face) { int cull = (c->cull_mode == GL_FRONT_AND_BACK) ? 1 : (c->cull_mode == GL_BACK) ? !front : front; if (cull) continue; }
        sgl_drawstate st = c->state;
        st.stencil_frontface = front;
        st.clip_x0 = st.clip_y0 = st.clip_x1 = st.clip_y1 = 0;  /* no tile clip */
        sgl_raster_tri_prog(c->fb, a, b, cc, c->nvary, &st, c->fs_scalar, c->fs_simt, (void *)&c->fc);
    }
}

void sg_framebin_flush(void)
{
    if (g_nitem <= 0) { fb_record_reset(); return; }

    /* All commands must target one framebuffer for the single tile grid. */
    sgl_framebuffer *fb = 0;
    int mixed = 0;
    for (int i = 0; i < g_ncmd; ++i) {
        if (!g_cmds[i].fb) continue;
        if (!fb) fb = g_cmds[i].fb;
        else if (g_cmds[i].fb != fb) { mixed = 1; break; }
    }
    if (mixed || !fb) { fb_replay_inorder(); fb_record_reset(); return; }

    int fbw = fb->w, fbh = fb->h;
    sg_mt_tile_layout layout = { SGL_MT_TILE_W, SGL_MT_TILE_H };
    int tiles_x = (fbw + layout.w - 1) / layout.w;
    int tiles_y = (fbh + layout.h - 1) / layout.h;
    if (tiles_x < 1) tiles_x = 1;
    if (tiles_y < 1) tiles_y = 1;
    int ntiles = tiles_x * tiles_y;

    /* Per-item tile range (clears cover the whole grid). */
    for (int i = 0; i < g_nitem; ++i) {
        fb_item *it = &g_items[i];
        const fb_cmd *c = &g_cmds[it->cmd];
        if (it->a < 0) { it->range.tx0 = 0; it->range.ty0 = 0; it->range.tx1 = tiles_x - 1; it->range.ty1 = tiles_y - 1; continue; }
        sg_prim_desc desc = { it->a, it->b, it->c, 0.f };
        it->range = sg_mt_prim_tiles(c->pv, &desc, &c->state, fbw, fbh,
                                     tiles_x, tiles_y, &layout,
                                     c->cull_face, c->cull_mode, c->front_ccw);
    }

    int *counts  = (int *)sg_frame_alloc(sizeof(int) * (size_t)ntiles);
    int *offsets = (int *)sg_frame_alloc(sizeof(int) * (size_t)(ntiles + 1));
    if (!counts || !offsets) { fb_replay_inorder(); fb_record_reset(); return; }
    memset(counts, 0, sizeof(int) * (size_t)ntiles);

    for (int i = 0; i < g_nitem; ++i) {
        sg_tile_range r = g_items[i].range;
        if (r.tx0 > r.tx1) continue;
        for (int ty = r.ty0; ty <= r.ty1; ++ty)
            for (int tx = r.tx0; tx <= r.tx1; ++tx) counts[ty * tiles_x + tx]++;
    }
    int total = 0;
    for (int t = 0; t < ntiles; ++t) { offsets[t] = total; total += counts[t]; }
    offsets[ntiles] = total;
    if (total == 0) { fb_record_reset(); return; }

    int *refs  = (int *)sg_frame_alloc(sizeof(int) * (size_t)total);
    int *write = (int *)sg_frame_alloc(sizeof(int) * (size_t)ntiles);
    if (!refs || !write) { fb_replay_inorder(); fb_record_reset(); return; }
    memcpy(write, offsets, sizeof(int) * (size_t)ntiles);
    /* Scatter in item (submission) order -> each tile's refs stay submission-ordered. */
    for (int i = 0; i < g_nitem; ++i) {
        sg_tile_range r = g_items[i].range;
        if (r.tx0 > r.tx1) continue;
        for (int ty = r.ty0; ty <= r.ty1; ++ty)
            for (int tx = r.tx0; tx <= r.tx1; ++tx) refs[write[ty * tiles_x + tx]++] = i;
    }

    int *active = (int *)sg_frame_alloc(sizeof(int) * (size_t)ntiles);
    int active_count = 0;
    for (int t = 0; t < ntiles; ++t)
        if (offsets[t] < offsets[t + 1] && active) active[active_count++] = t;

    fb_job job = { g_cmds, g_items, offsets, refs, active, active_count,
                   tiles_x, tiles_y, fbw, fbh, layout };
    fb_sched_ctx ctx = { &job, 0 };
    sg_mt_sched_run(fb_work, &ctx);

    fb_record_reset();
}

#else  /* host build: deferral disabled, immediate path only */
int  sg_framebin_record_draw(const sg_draw_packet *draw, GLenum mode, GLsizei count,
                             const sgl_pvert *pv, glsl_program *gp)
{ (void)draw;(void)mode;(void)count;(void)pv;(void)gp; return 0; }
void sg_framebin_record_clear(sgl_framebuffer *fb, GLbitfield mask,
                              uint32_t color, float depth, unsigned char stencil)
{ (void)fb;(void)mask;(void)color;(void)depth;(void)stencil; }
void sg_framebin_flush(void) {}
#endif
