/*
 * sg_vs_range.c — per-worker vertex range for the parallel vertex stage.
 *
 * Deliberately its own translation unit with NO GL, RVV or softgles2 headers, so
 * the host contract test (test/vsmt_ranges.c) links the REAL function rather
 * than a transcription of it -- a copied formula would only ever test the copy.
 */
#ifndef SGL_SIMT_LANES
#define SGL_SIMT_LANES 16
#endif

/* Vertex range [*lo,*hi) owned by `worker` of `nworkers`. Returns 0 when this
 * worker has nothing to do.
 *
 * Split out of sg_vs_mt_work so it can be tested directly. The parallel vertex
 * path is claimed "bit-exact by construction" because each vertex depends only
 * on its own attributes and the uniforms and the ranges are disjoint -- so the
 * whole claim rests on this arithmetic covering [0,count) exactly once. Rounding
 * `per` UP to a SIMT-lane multiple makes that non-obvious: it can push later
 * workers past the end (they take no range), which is correct but is exactly
 * where an off-by-one would hide. See test/vsmt_ranges.c. */
int sg_vs_mt_range(int count, int worker, int nworkers, int *lo, int *hi)
{
    int per, l, h;

    if (count <= 0 || nworkers <= 0 || worker < 0 || worker >= nworkers)
        return 0;
    /* Chunk on a SIMT-lane multiple so each hart's inner loop still runs full
     * batches; the tail hart takes the remainder. */
    per = (count + nworkers - 1) / nworkers;
    per = (per + SGL_SIMT_LANES - 1) & ~(SGL_SIMT_LANES - 1);
    l = worker * per;
    if (l >= count) return 0;
    h = l + per;
    if (h > count) h = count;
    *lo = l; *hi = h;
    return 1;
}

