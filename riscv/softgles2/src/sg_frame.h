/* Per-frame bump arena for transient draw scratch (pv/vb/idx/...).
 *
 * Today the renderer mallocs vertex/index/varying scratch per draw and free()
 * is a no-op (malloc.c), so every draw permanently leaks heap for the whole
 * run. This arena hands out that scratch from a reserve that is reclaimed
 * wholesale at frame present (sg_frame_reset), so per-frame scratch stops
 * leaking. It is also the substrate the FrameBin deferred renderer needs: in
 * immediate mode scratch is consumed within the draw, but the same allocations
 * can outlive the draw until the frame flush.
 *
 * Single-producer: only the main hart records/allocates (worker harts just read
 * during the tile flush), so no locking is needed. If the reserve is exhausted
 * (or unavailable), allocation falls back to global malloc so it never fails —
 * matching the pre-arena (leaky) behaviour, surfaced via the high-water counter.
 */
#ifndef SOFTGLES2_SG_FRAME_H
#define SOFTGLES2_SG_FRAME_H

#include <stddef.h>

void  *sg_frame_alloc(size_t n);   /* 16-byte aligned; never returns 0 unless OOM */
void   sg_frame_reset(void);       /* reclaim the whole frame's scratch (at present) */
size_t sg_frame_highwater(void);   /* peak arena bytes used in a single frame */
size_t sg_frame_overflow(void);    /* reusable overflow-chunk capacity */

#endif /* SOFTGLES2_SG_FRAME_H */
