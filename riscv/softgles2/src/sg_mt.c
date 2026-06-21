#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#endif

#include "sgl.h"
#include "sg_mt_internal.h"
#include "sg_mt_sched.h"
#include "sg_mt_tiles.h"
#include "sg_mt_prim.h"
#include "sg_diag.h"
#include "sg_prof.h"

/* MT engagement stats (read by the [rprof] line + the mt_overlay unit test). */
volatile unsigned long sgl_dbg_mt_engaged = 0;
volatile unsigned long sgl_dbg_mt_cover_admit = 0;

static int sg_mt_div_up_int(int a, int b)
{
    return (a + b - 1) / b;
}

static int sg_mt_layout_tiles(const sg_mt_tile_layout *l, int fbw, int fbh)
{
    int w = l->w > 0 ? l->w : 1;
    int h = l->h > 0 ? l->h : 1;
    int tx = sg_mt_div_up_int(fbw > 0 ? fbw : 0, w);
    int ty = sg_mt_div_up_int(fbh > 0 ? fbh : 0, h);
    if (tx < 1) tx = 1;
    if (ty < 1) ty = 1;
    return tx * ty;
}

/* Candidate filter for tile layouts explored during `sg_mt_layout_choose`.
 * - prefers increased tile count over the base layout
 * - enforces a floor on tile area so we do not oversubdivide into tiny strips
 * - caps at `max_tiles` so we do not over-index when primitive count is tiny.
 */
static void sg_mt_layout_select_candidate(const sg_mt_tile_layout *cand,
                                         int fbw, int fbh, int base_tiles,
                                         int max_tiles, int min_area,
                                         sg_mt_tile_layout *best)
{
    sg_mt_tile_layout c = *cand;

    if (c.w < 1) c.w = 1;
    if (c.h < 1) c.h = 1;
    if (c.w > fbw && fbw > 0) c.w = fbw;
    if (c.h > fbh && fbh > 0) c.h = fbh;

    int tiles = sg_mt_layout_tiles(&c, fbw, fbh);
    if (tiles <= base_tiles) return;
    if (tiles > max_tiles) return;
    if (c.w * c.h < min_area) return;
    *best = c;
}

sg_mt_layout_policy sg_mt_layout_policy_default(void)
{
    sg_mt_layout_policy p;
    p.split_prim_threshold = SGL_MT_TILE_SPLIT_PRIMS;
    p.split_area_div = 2;
    p.min_active_tiles = SGL_MT_MIN_ACTIVE_TILES;
    return p;
}

sg_mt_tile_layout sg_mt_layout_choose(int nprim, int fbw, int fbh,
                                     const sg_mt_layout_policy *policy)
{
    sg_mt_layout_policy p = policy ? *policy : sg_mt_layout_policy_default();
    if (p.split_prim_threshold < 0) p.split_prim_threshold = 0;
    if (p.split_area_div < 1) p.split_area_div = 1;
    if (p.min_active_tiles < 1) p.min_active_tiles = 1;

    sg_mt_tile_layout layout = { SGL_MT_TILE_W, SGL_MT_TILE_H };
    sg_mt_tile_layout split = layout;
    int base_tiles = sg_mt_layout_tiles(&layout, fbw, fbh);
    int split_ok = nprim >= p.split_prim_threshold &&
                   base_tiles > p.min_active_tiles &&
                   fbw > (layout.w * 2) && fbh > (layout.h * 2);
    int min_area = (layout.w * layout.h) / p.split_area_div;
    if (min_area < 1) min_area = 1;

    int max_tiles = nprim > base_tiles ? nprim : base_tiles;

    if (split_ok && fbw > layout.w && fbh > layout.h) {
        if (fbw >= fbh * 2) {
            split.w = layout.w / 2;
            split.h = layout.h * 2;
        } else if (fbh >= fbw * 2 && layout.h > 1) {
            split.w = layout.w * 2;
            split.h = layout.h / 2;
        } else {
            split.w = layout.w / 2;
            split.h = layout.h * 2;
        }
        sg_mt_layout_select_candidate(&split, fbw, fbh, base_tiles, max_tiles, min_area, &layout);

        if (split_ok && fbw > layout.w * 2 && layout.w > 1) {
            split = (sg_mt_tile_layout){ layout.w / 2, layout.h };
            sg_mt_layout_select_candidate(&split, fbw, fbh, base_tiles, max_tiles, min_area, &layout);
        }

        if (split_ok && fbh > layout.h * 2 && layout.h > 1) {
            split = (sg_mt_tile_layout){ layout.w, layout.h / 2 };
            sg_mt_layout_select_candidate(&split, fbw, fbh, base_tiles, max_tiles, min_area, &layout);
        }
    }

    if (layout.w < 1) layout.w = 1;
    if (layout.h < 1) layout.h = 1;
    if (layout.w > fbw && fbw > 0) layout.w = fbw;
    if (layout.h > fbh && fbh > 0) layout.h = fbh;
    return layout;
}

int sg_mt_validate_job(const sg_prog_job *j, int total_tiles, int bin_refs)
{
    if (!j || !j->draw || !j->pv || !j->prims) return 0;
    if (!j->tile_offsets) return 0;
    if (bin_refs > 0 && !j->tile_refs) return 0;
    if (!j->layout.w || !j->layout.h) return 0;
    if (total_tiles < 1) return 0;
    if (j->tiles_x < 1 || j->tiles_y < 1) return 0;
    if (j->tiles_x * j->tiles_y != total_tiles) return 0;
    if (j->active_count < 0) return 0;
    if (j->active_count > total_tiles) return 0;
    if (j->active_tiles) {
        if (j->active_count == 0) return 0;
        int prev_tile = -1;
        for (int i = 0; i < j->active_count; ++i) {
            if (j->active_tiles[i] < 0 || j->active_tiles[i] >= total_tiles) return 0;
            if (j->active_tiles[i] == prev_tile) return 0;
            if (j->active_tiles[i] < prev_tile) return 0;
            prev_tile = j->active_tiles[i];
        }
    } else if (j->active_count != 0) {
        return 0;
    }
    if (j->tile_offsets[0] != 0) return 0;
    for (int t = 0; t < total_tiles; ++t) {
        int a = j->tile_offsets[t];
        int b = j->tile_offsets[t + 1];
        if (a < 0 || b < a || b > bin_refs) return 0;
    }
    if (j->tile_offsets[total_tiles] != bin_refs) return 0;
    return 1;
}

int sg_mt_prog_dispatch(const sg_draw_packet *draw, GLenum mode, GLsizei count,
                        sgl_pvert *pv, int fbw, int fbh, int enabled)
{
#if defined(SGL_BAREMETAL)
    if (!enabled || !draw || !pv || count < 3 || fbh < 2 * SGL_MT_MAXHARTS)
        return 0;
    /* Work proxy for the cheap early-out: many vertices (the 3D world) OR a
     * large covered screen area (low-vertex fullscreen blits — the gl4es 2D
     * overlay is a 4-vertex GL_TRIANGLE_FAN that fills the screen and would
     * otherwise be rejected here and shaded single-hart, idling 3 of 4 harts on
     * the most-rendered surface). Coverage = projected-vertex screen bbox; the
     * real per-tile work is re-checked by sg_mt_sched_should_mt() after binning. */
    int cover_admit = 0;
    if (count < SGL_MT_MIN_VERTS) {
        float minx = pv[0].x, maxx = pv[0].x, miny = pv[0].y, maxy = pv[0].y;
        for (GLsizei i = 1; i < count; ++i) {
            float x = pv[i].x, y = pv[i].y;
            if (x < minx) minx = x; else if (x > maxx) maxx = x;
            if (y < miny) miny = y; else if (y > maxy) maxy = y;
        }
        float cw = maxx - minx, ch = maxy - miny;
        if (cw < 0.f) cw = 0.f;
        if (ch < 0.f) ch = 0.f;
        if ((long)cw * (long)ch < SGL_MT_MIN_COVER_PX)
            return 0;
        cover_admit = 1;   /* low-vertex/high-coverage draw the fix newly admits */
    }

    uint64_t pf_bin0 = sgl_prof_now();   /* Lever 1: serial binning sub-timer */
    int maxprim = (mode == GL_TRIANGLES) ? (int)(count / 3) :
                  (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) ? (int)(count - 2) : 0;
    sg_prim_desc *prims = maxprim > 0 ? (sg_prim_desc *)malloc(sizeof(sg_prim_desc) * (size_t)maxprim) : 0;
    int *tile_offsets = 0, *tile_refs = 0;
    int nprim = prims ? sg_mt_prim_build(mode, count, prims) : 0;
    if (!prims || nprim <= 0) {
        free(prims);
        return 0;
    }

    for (int i = 0; i < nprim; ++i) {
        float za = pv[prims[i].a].z;
        float zb = pv[prims[i].b].z;
        float zc = pv[prims[i].c].z;
        prims[i].zmin = (za < zb) ? (za < zc ? za : zc) : (zb < zc ? zb : zc);
    }

    sg_mt_layout_policy policy = sg_mt_layout_policy_default();
    sg_mt_tile_layout layout = sg_mt_layout_choose(nprim, fbw, fbh, &policy);
    int tiles_x = (fbw + layout.w - 1) / layout.w;
    int tiles_y = (fbh + layout.h - 1) / layout.h;
    if (tiles_x < 1) tiles_x = 1;
    if (tiles_y < 1) tiles_y = 1;
    int bin_refs = (nprim > 0) ? sg_mt_tiles_build(
        pv, prims, nprim, &draw->state, fbw, fbh,
        tiles_x, tiles_y, &layout, draw->cull_face, draw->cull_mode, draw->front_ccw,
        &tile_offsets, &tile_refs
    ) : -1;

    if (bin_refs < 0) {
        free(prims);
        return 0;
    }
    SGL_DIAG_ADD(sgl_dbg_tile_refs, (unsigned long)bin_refs);

    if (bin_refs == 0) {
        free(tile_offsets);
        free(tile_refs);
        free(prims);
        return 1;
    }

    int ntiles = tiles_x * tiles_y;
    int *active_tiles = (int *)malloc(sizeof(int) * (size_t)ntiles);
    int active_count = ntiles;
    if (active_tiles) {
        active_count = 0;
        for (int t = 0; t < ntiles; ++t) {
            if (tile_offsets[t] < tile_offsets[t + 1]) {
                active_tiles[active_count++] = t;
            }
        }
    }

    if (!sg_mt_sched_should_mt(nprim, bin_refs, active_count, draw->fs_cost, draw->fs_simt_ok, &draw->state,
                               layout.w * layout.h)) {
        free(active_tiles);
        free(tile_offsets);
        free(tile_refs);
        free(prims);
        return 0;
    }

    /* Shared immutable payload for all harts. Ownership of backing arrays remains
     * with this dispatch stack frame; each tile worker reads them only.
     */
    sg_prog_job job = {
        draw, pv, prims, nprim, tile_offsets, tile_refs,
        active_tiles, active_count, tiles_x, tiles_y, fbh, fbw, layout
    };
    if (!sg_mt_validate_job(&job, ntiles, bin_refs)) {
        free(active_tiles);
        free(tile_offsets);
        free(tile_refs);
        free(prims);
        return 0;
    }

    /* engagement stats (always on; per-draw, negligible cost): every draw that
     * actually fans across harts, and the subset newly admitted by the coverage
     * proxy (the fullscreen blits that used to be shaded single-hart). */
    sgl_dbg_mt_engaged++;
    if (cover_admit) sgl_dbg_mt_cover_admit++;
    if (sgl_prof_enabled) sgl_prof_mt_bin += sgl_prof_now() - pf_bin0;

    /* scheduler context owns only mutable handoff state (tile cursor). */
    {
        sg_mt_sched_ctx sctx = { &job, 0 };
        sg_mt_sched_run(sg_mt_sched_tiles, &sctx);
    }

    if (active_tiles) free(active_tiles);
    free(tile_offsets);
    free(tile_refs);
    free(prims);
    return 1;
#else
    (void)draw; (void)mode; (void)count; (void)pv;
    (void)fbw; (void)fbh; (void)enabled;
    return 0;
#endif
}
