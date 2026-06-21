/*
 * dOS lock-free job scheduler — public interface  (riscv/smp/sched.h)
 * ===================================================================
 *
 * STEP 1 of dОС thread support (see riscv/docs/smp/02-scheduler.md). This is a
 * standalone module built into the `sched_test` payload under riscv/smp/. It
 * implements the Tier-1 design from riscv/docs/smp/01-scalable-threading.md §3:
 *
 *   - a single-producer / multi-consumer (SPMC) lock-free ring of {fn,arg} jobs:
 *       * the SUBMITTER (boot hart) is the single producer — `head` is advanced
 *         by exactly one writer, so enqueue needs NO CAS;
 *       * WORKER harts are the consumers — each claims a slot with one
 *         `__atomic_fetch_add(&tail,1)` (the claim load-balances for free);
 *   - a frame BARRIER built on an atomic completion counter: the submitter waits
 *     until every submitted job has run, then observes all their writes;
 *   - a persistent per-hart WORKER LOOP that pulls jobs and WFI-idles (NOT a
 *     tight spin — TCG is round-robin) when the queue is empty.
 *
 * GRACEFUL N=1 DEGRADATION is first-class: with no worker harts, `sched_submit`
 * runs each job INLINE on the submitter and the barrier is a no-op. The result
 * is identical and correct.
 *
 * EXTENSION POINT for the future tiled renderer: a job carries a `kind` tag and
 * a tile-style payload variant (JOB_TILE) alongside the generic {fn,arg}
 * (JOB_FN). The worker loop already switches on `kind`, so wiring the
 * softgles2 raster fan-out (01-scalable-threading.md §5) is purely adding a
 * `JOB_TILE` case that calls `rasterize_tile()` — no scheduler change.
 */
#ifndef DHEWM3_RV_SMP_SCHED_H
#define DHEWM3_RV_SMP_SCHED_H

#include <stdint.h>

#ifndef SMP_MAX_HARTS
#define SMP_MAX_HARTS 8
#endif

/* Power-of-two ring capacity (index masked with SCHED_QUEUE_SIZE-1). Sized for
 * many small jobs; the test only submits a handful per batch. */
#define SCHED_QUEUE_SIZE 4096

/*
 * Job kinds. JOB_FN is the generic {fn,arg} callable. JOB_TILE is the
 * tile-style variant reserved for the renderer fan-out (the worker loop already
 * dispatches on it; the test exercises it with a tiny tile-fill function so the
 * extension point is live, not just declared).
 */
typedef enum {
    JOB_FN   = 0,   /* generic callable: fn(arg)                              */
    JOB_TILE = 1    /* tile-style variant: fn(arg) over a rectangular region  */
} job_kind_t;

/* A job is a small {fn,arg} struct (+ tile bounds for the tile variant). */
typedef struct sched_job {
    job_kind_t kind;
    void     (*fn)(void *arg);   /* work function                            */
    void      *arg;              /* opaque argument                          */
    /* Tile-style variant payload (only meaningful for JOB_TILE; the renderer
     * will read these in its rasterize_tile()). Kept inline so the queue stays
     * a flat array of POD entries with no extra indirection. */
    int        minx, miny, maxx, maxy;
} sched_job_t;

/*
 * Bring the scheduler up. `submitter_hartid` is the boot hart (the single
 * producer). `worker_harts[0..n_workers-1]` are the hartids that will run the
 * worker loop. With n_workers == 0 the scheduler runs in INLINE mode (N=1).
 *
 * Must be called by the submitter before any worker enters hart_worker_main().
 */
void sched_init(unsigned long submitter_hartid,
                const int *worker_harts, int n_workers);

/* True when there are no worker harts (N=1): jobs run inline on the submitter. */
int  sched_is_inline(void);

/* Number of worker harts the scheduler was initialised with. */
int  sched_num_workers(void);

/*
 * Submit a batch of `n` jobs and BARRIER-wait until all of them have completed.
 *
 *   - INLINE mode (no workers): each job runs immediately on the calling
 *     (submitter) hart; returns when the last one finishes.
 *   - POOL mode: all `n` jobs are published to the ring (single-producer, no
 *     CAS), workers are woken (IPI where wired; otherwise their relaxed poll
 *     picks them up), and the submitter WFI/relax-polls the atomic completion
 *     counter until it reaches `n`, then issues an ACQUIRE fence so it observes
 *     every job's writes.
 *
 * The jobs[] array must stay live until this returns (it does — the caller
 * blocks here). Returns when all `n` jobs are done.
 */
void sched_submit(const sched_job_t *jobs, int n);

/*
 * The persistent worker loop. Entered by each worker hart (via secondary.S ->
 * secondary_worker -> here). Pulls jobs from the ring, executes them, bumps the
 * completion counter, and WFI-idles when the ring is empty. Never returns while
 * the scheduler is running; returns only after sched_shutdown() is signalled so
 * the worker can sbi_hart_stop() cleanly.
 */
void hart_worker_main(unsigned long hartid);

/* Tell all workers to drain and exit hart_worker_main() (so they can stop). */
void sched_shutdown(void);

/* Per-hart counter: how many jobs hartid executed (proves work distribution).
 * Indexed by hartid; the submitter's own inline/fanned executions count too. */
uint64_t sched_hart_jobs(unsigned long hartid);

/* Total jobs completed across all harts (sum of the per-hart counters). */
uint64_t sched_total_jobs(void);

#endif /* DHEWM3_RV_SMP_SCHED_H */
