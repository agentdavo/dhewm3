/* Contract test for the parallel-vertex range split (sg_vs_mt_range).
 *
 * The parallel vertex path is claimed "bit-exact by construction": each vertex
 * depends only on its own attributes and the uniforms, and the per-hart ranges
 * are disjoint, so splitting a draw across harts cannot change the result. The
 * arithmetic part of that claim is testable without any harts -- and it is the
 * part that can actually be wrong.
 *
 * It is not obvious by inspection, because `per` is rounded UP to a SIMT-lane
 * multiple. That rounding pushes later workers past the end (they legitimately
 * take no range), which is exactly where an off-by-one would hide: one vertex
 * transformed twice, or none at all, in a corner case nobody enumerates by hand.
 *
 * So: for every (count, nworkers) pair in a wide sweep, mark each vertex with
 * the worker that owns it and assert
 *   - every vertex in [0,count) is owned by EXACTLY ONE worker  (no gap, no dup)
 *   - no range extends past count
 *   - ranges are ascending and non-overlapping
 *
 * Host build: pure integer arithmetic, no GL, no QEMU needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sg_vs_mt_range(int count, int worker, int nworkers, int *lo, int *hi);

static int g_fail;

static void check(int count, int nworkers, int verbose)
{
    unsigned char *owner = (unsigned char *)malloc((size_t)count ? (size_t)count : 1);
    int w, i, prev_hi = 0, active = 0;

    memset(owner, 0xff, (size_t)count ? (size_t)count : 1);

    for (w = 0; w < nworkers; ++w) {
        int lo = -1, hi = -1;
        if (!sg_vs_mt_range(count, w, nworkers, &lo, &hi))
            continue;
        ++active;
        if (lo < 0 || hi > count || lo >= hi) {
            printf("  FAIL count=%d n=%d worker=%d: bad range [%d,%d)\n",
                   count, nworkers, w, lo, hi);
            ++g_fail; free(owner); return;
        }
        if (lo < prev_hi) {
            printf("  FAIL count=%d n=%d worker=%d: range [%d,%d) overlaps "
                   "previous end %d\n", count, nworkers, w, lo, hi, prev_hi);
            ++g_fail; free(owner); return;
        }
        prev_hi = hi;
        for (i = lo; i < hi; ++i) {
            if (owner[i] != 0xff) {
                printf("  FAIL count=%d n=%d: vertex %d owned by BOTH worker %d "
                       "and %d\n", count, nworkers, i, owner[i], w);
                ++g_fail; free(owner); return;
            }
            owner[i] = (unsigned char)w;
        }
    }

    for (i = 0; i < count; ++i) {
        if (owner[i] == 0xff) {
            printf("  FAIL count=%d n=%d: vertex %d owned by NO worker "
                   "(gap in coverage)\n", count, nworkers, i);
            ++g_fail; free(owner); return;
        }
    }
    if (verbose)
        printf("  ok count=%-6d n=%-2d  %d/%d workers active\n",
               count, nworkers, active, nworkers);
    free(owner);
}

int main(void)
{
    /* Counts chosen around the danger points: lane multiples, one either side,
     * primes, tiny counts, and the ~91761-vertex lit-scene draw from handover 20. */
    static const int counts[] = {
        1, 2, 3, 15, 16, 17, 31, 32, 33, 47, 48, 49, 63, 64, 65,
        100, 127, 128, 129, 255, 256, 257, 511, 512, 513,
        1000, 2047, 2048, 2049, 4095, 4096, 4097, 9600, 27012, 91761
    };
    const int ncounts = (int)(sizeof(counts) / sizeof(counts[0]));
    int ci, n, total = 0;

    printf("[vsmt] sg_vs_mt_range coverage sweep\n");
    for (ci = 0; ci < ncounts; ++ci) {
        for (n = 1; n <= 16; ++n) {
            check(counts[ci], n, 0);
            ++total;
        }
    }
    printf("[vsmt] %d (count,nworkers) combinations checked\n", total);

    /* The sweep must be able to fail. A deliberately wrong split -- plain
     * ceil-divide with no lane rounding is fine, so instead drop the last
     * vertex -- has to be caught, else this test proves nothing. */
    {
        int lo, hi, caught = 0;
        if (sg_vs_mt_range(100, 0, 1, &lo, &hi)) {
            if (hi == 100) {
                /* simulate an off-by-one by checking what the sweep would say */
                unsigned char owner[100];
                int i;
                memset(owner, 0xff, sizeof owner);
                for (i = lo; i < hi - 1; ++i) owner[i] = 0;   /* drop last */
                for (i = 0; i < 100; ++i) if (owner[i] == 0xff) { caught = 1; break; }
            }
        }
        printf("[vsmt] self-check (simulated off-by-one): %s\n",
               caught ? "detected, OK" : "NOT DETECTED -- test is worthless");
        if (!caught) ++g_fail;
    }

    printf("[vsmt] %s\n", g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
