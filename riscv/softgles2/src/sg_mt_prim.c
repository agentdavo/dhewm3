#include "sgl.h"
#include "sg_mt_prim.h"
#include "sg_mt_internal.h"

#if defined(SGL_BAREMETAL)
static int ifloor_local(float x)
{
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}
#endif

int sg_mt_prim_build(GLenum mode, GLsizei count, sg_prim_desc *prims)
{
    int n = 0;
    if (!prims) return 0;

    if (mode == GL_TRIANGLES) {
        for (GLsizei i = 0; i + 2 < count; i += 3)
            prims[n++] = (sg_prim_desc){ i, i + 1, i + 2, 0.f };
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (GLsizei i = 0; i + 2 < count; ++i) {
            if (i & 1) prims[n++] = (sg_prim_desc){ i + 1, i, i + 2, 0.f };
            else       prims[n++] = (sg_prim_desc){ i, i + 1, i + 2, 0.f };
        }
    } else if (mode == GL_TRIANGLE_FAN) {
        for (GLsizei i = 1; i + 1 < count; ++i)
            prims[n++] = (sg_prim_desc){ 0, i, i + 1, 0.f };
    }

    return n;
}

sg_tile_range sg_mt_prim_tiles(const sgl_pvert *pv, const sg_prim_desc *p,
                              const sgl_drawstate *st, int fbw, int fbh,
                              int tiles_x, int tiles_y, const sg_mt_tile_layout *layout,
                              int cull_face,
                              int cull_mode, int front_ccw)
{
#if defined(SGL_BAREMETAL)
    sg_tile_range r = { 1, 0, 0, 0 };
    const sgl_pvert *a = &pv[p->a], *b = &pv[p->b], *c = &pv[p->c];
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0.f) return r;
    int front = front_ccw ? (area < 0.f) : (area > 0.f);
    if (cull_face) {
        int cull = (cull_mode == GL_FRONT_AND_BACK) ? 1 : (cull_mode == GL_BACK) ? !front : front;
        if (cull) return r;
    }

    float minxf = a->x, maxxf = a->x;
    float minyf = a->y, maxyf = a->y;
    if (b->x < minxf) minxf = b->x;
    if (c->x < minxf) minxf = c->x;
    if (b->x > maxxf) maxxf = b->x;
    if (c->x > maxxf) maxxf = c->x;
    if (b->y < minyf) minyf = b->y;
    if (c->y < minyf) minyf = c->y;
    if (b->y > maxyf) maxyf = b->y;
    if (c->y > maxyf) maxyf = c->y;
    int minx = ifloor_local(minxf), maxx = ifloor_local(maxxf);
    int miny = ifloor_local(minyf), maxy = ifloor_local(maxyf);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > fbw - 1) maxx = fbw - 1;
    if (maxy > fbh - 1) maxy = fbh - 1;
    if (st->scissor_on) {
        if (minx < st->sx0) minx = st->sx0;
        if (miny < st->sy0) miny = st->sy0;
        if (maxx > st->sx1) maxx = st->sx1;
        if (maxy > st->sy1) maxy = st->sy1;
    }
    if (minx > maxx || miny > maxy) return r;

    int tile_w = layout && layout->w > 0 ? layout->w : SGL_MT_TILE_W;
    int tile_h = layout && layout->h > 0 ? layout->h : SGL_MT_TILE_H;
    r.tx0 = minx / tile_w;
    r.ty0 = miny / tile_h;
    r.tx1 = maxx / tile_w;
    r.ty1 = maxy / tile_h;
    if (r.tx0 < 0) r.tx0 = 0;
    if (r.ty0 < 0) r.ty0 = 0;
    if (r.tx1 >= tiles_x) r.tx1 = tiles_x - 1;
    if (r.ty1 >= tiles_y) r.ty1 = tiles_y - 1;
    return r;
#else
    (void)pv; (void)p; (void)st; (void)fbw; (void)fbh;
    (void)tiles_x; (void)tiles_y; (void)layout; (void)cull_face; (void)cull_mode; (void)front_ccw;
    return (sg_tile_range){ 1, 0, 0, 0 };
#endif
}
