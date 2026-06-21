#if !defined(SGL_BAREMETAL)
#  include <stddef.h>
#endif

#include "sg_mt_internal.h"
#include "sg_mt_raster.h"
#include "sg_raster.h"
#include "sgl.h"

void sg_mt_emit_triangle_prog(const sgl_pvert *a, const sgl_pvert *b, const sgl_pvert *c,
                             const sg_draw_packet *draw)
{
#if defined(SGL_BAREMETAL)
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    int front = draw->front_ccw ? (area < 0.f) : (area > 0.f);
    if (draw->cull_face) {
        int cull = (draw->cull_mode == GL_FRONT_AND_BACK) ? 1 : (draw->cull_mode == GL_BACK) ? !front : front;
        if (cull) return;
    }
    sgl_drawstate face_st = draw->state;
    face_st.stencil_frontface = front;
    sgl_raster_tri_prog(
        (sgl_framebuffer *)draw->fb, a, b, c, draw->nvary,
        &face_st, draw->fs_scalar, draw->fs_simt, draw->frag_ctx);
#else
    (void)a; (void)b; (void)c; (void)draw;
#endif
}

void sg_mt_tile_emit(const sg_prog_job *j, int tx, int ty)
{
#if defined(SGL_BAREMETAL)
    sgl_drawstate st = j->draw->state;
    int tile_w = j->layout.w > 0 ? j->layout.w : SGL_MT_TILE_W;
    int tile_h = j->layout.h > 0 ? j->layout.h : SGL_MT_TILE_H;
    st.clip_x0 = tx * tile_w;
    st.clip_y0 = ty * tile_h;
    st.clip_x1 = st.clip_x0 + tile_w;
    st.clip_y1 = st.clip_y0 + tile_h;
    if (st.clip_x1 > j->fbw) st.clip_x1 = j->fbw;
    if (st.clip_y1 > j->fbh) st.clip_y1 = j->fbh;

    sg_draw_packet tile_draw = *j->draw;
    tile_draw.state = st;
    int tile = ty * j->tiles_x + tx;
    int begin = j->tile_offsets[tile];
    int end = j->tile_offsets[tile + 1];
    for (int i = begin; i < end; ++i) {
        const sg_prim_desc *p = &j->prims[j->tile_refs[i]];
        sg_mt_emit_triangle_prog(&j->pv[p->a], &j->pv[p->b], &j->pv[p->c], &tile_draw);
    }
#else
    (void)j; (void)tx; (void)ty;
#endif
}
