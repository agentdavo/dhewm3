#if defined(SGL_BAREMETAL)
#  include "libc/libc.h"
#else
#  include <stdlib.h>
#  include <string.h>
#endif
#include "sgl.h"
#include "sg_mt_internal.h"
#include "sg_mt_prim.h"
#include "sg_mt_tiles.h"

#if defined(SGL_BAREMETAL)
static void sg_mt_tile_sort_front_to_back(int *refs, int n, const sg_prim_desc *prims)
{
    int *a = refs;
    for (int i = n / 2 - 1; i >= 0; --i) {
        int p = a[i];
        float pz = prims[p].zmin;
        for (int child; (child = 2 * i + 1) < n; ) {
            int best = child;
            float bestz = prims[a[child]].zmin;
            if (child + 1 < n && prims[a[child + 1]].zmin < bestz) {
                best = child + 1;
                bestz = prims[a[best]].zmin;
            }
            if (!(bestz < pz)) break;
            a[i] = a[best];
            i = best;
        }
        a[i] = p;
    }
    for (int end = n - 1; end > 0; --end) {
        int tmp = a[end];
        a[end] = a[0];
        float pz = prims[tmp].zmin;
        int i = 0;
        for (int child; (child = 2 * i + 1) < end; ) {
            int best = child;
            float bestz = prims[a[child]].zmin;
            if (child + 1 < end && prims[a[child + 1]].zmin < bestz) {
                best = child + 1;
                bestz = prims[a[best]].zmin;
            }
            if (!(bestz < pz)) break;
            a[i] = a[best];
            i = best;
        }
        a[i] = tmp;
    }
}
#endif

int sg_mt_tiles_build(const sgl_pvert *pv, const sg_prim_desc *prims, int nprim,
                     const sgl_drawstate *st, int fbw, int fbh,
                     int tiles_x, int tiles_y, const sg_mt_tile_layout *layout,
                     int cull_face, int cull_mode, int front_ccw,
                     int **offsets_out, int **refs_out)
{
#if defined(SGL_BAREMETAL)
    int ntiles = tiles_x * tiles_y;
    int *counts = (int *)malloc(sizeof(int) * (size_t)ntiles);
    int *offsets = (int *)malloc(sizeof(int) * (size_t)(ntiles + 1));
    sg_tile_range *ranges = (sg_tile_range *)malloc(sizeof(sg_tile_range) * (size_t)nprim);
    if (!counts || !offsets || !ranges) {
        free(counts);
        free(offsets);
        free(ranges);
        return -1;
    }
    memset(counts, 0, sizeof(int) * (size_t)ntiles);

    for (int i = 0; i < nprim; ++i) {
        sg_tile_range r = sg_mt_prim_tiles(pv, &prims[i], st, fbw, fbh, tiles_x, tiles_y, layout,
                                          cull_face, cull_mode, front_ccw);
        ranges[i] = r;
        if (r.tx0 > r.tx1) continue;
        for (int ty = r.ty0; ty <= r.ty1; ++ty)
            for (int tx = r.tx0; tx <= r.tx1; ++tx)
                counts[ty * tiles_x + tx]++;
    }

    int total = 0;
    for (int t = 0; t < ntiles; ++t) {
        offsets[t] = total;
        total += counts[t];
    }
    offsets[ntiles] = total;
    if (total == 0) {
        free(counts);
        free(ranges);
        *offsets_out = offsets;
        *refs_out = 0;
        return 0;
    }

    int *refs = (int *)malloc(sizeof(int) * (size_t)total);
    int *write = (int *)malloc(sizeof(int) * (size_t)ntiles);
    if (!refs || !write) {
        free(counts);
        free(offsets);
        free(ranges);
        free(refs);
        free(write);
        return -1;
    }
    memcpy(write, offsets, sizeof(int) * (size_t)ntiles);

    for (int i = 0; i < nprim; ++i) {
        sg_tile_range r = ranges[i];
        if (r.tx0 > r.tx1) continue;
        for (int ty = r.ty0; ty <= r.ty1; ++ty) {
            for (int tx = r.tx0; tx <= r.tx1; ++tx) {
                int tile = ty * tiles_x + tx;
                refs[write[tile]++] = i;
            }
        }
    }

    for (int t = 0; t < ntiles; ++t) {
        int b = offsets[t], e = offsets[t + 1];
        if (e - b > 1)
            sg_mt_tile_sort_front_to_back(refs + b, e - b, prims);
    }

    free(counts);
    free(ranges);
    free(write);
    *offsets_out = offsets;
    *refs_out = refs;
    return total;
#else
    (void)pv; (void)prims; (void)nprim; (void)st; (void)fbw; (void)fbh;
    (void)tiles_x; (void)tiles_y; (void)layout; (void)cull_face; (void)cull_mode; (void)front_ccw;
    *offsets_out = 0; *refs_out = 0;
    return 0;
#endif
}
