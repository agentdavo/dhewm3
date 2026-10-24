#include "texture_jobs.h"
#include <stdint.h>

/* Small scheduler ABI: keep renderer implementation types out of the engine. */
extern int sg_mt_sched_busy(void);
extern void sg_mt_sched_run(void (*fn)(void *, int, int), void *ctx);

int bm_texture_parallel = 1;
int bm_texture_validate = 0;

struct row_job {
    bm_texture_rows_fn fn;
    void *ctx;
    int rows;
};

static void texture_worker(void *opaque, int worker, int workers)
{
    const struct row_job *job = opaque;
    int first = (int)((int64_t)job->rows * worker / workers);
    int end = (int)((int64_t)job->rows * (worker + 1) / workers);
    if (first < end) job->fn(job->ctx, first, end);
}

void BM_TextureRows(int rows, int width, bm_texture_rows_fn fn, void *ctx)
{
    if (rows <= 0 || width <= 0) return;
    /* Tiny mips cannot amortize the IPI/join; nested dispatch is forbidden. */
    if (!bm_texture_parallel || rows < 4 ||
        (int64_t)rows * width < 16384 || sg_mt_sched_busy()) {
        fn(ctx, 0, rows);
        return;
    }
    struct row_job job = { fn, ctx, rows };
    sg_mt_sched_run(texture_worker, &job);
}
