/*
 * dOS lock-free job scheduler — implementation  (riscv/smp/sched.c)
 * ================================================================
 *
 * See sched.h for the interface and riscv/docs/smp/02-scheduler.md for the full
 * design. Summary of the concurrency model (all per 01-scalable-threading.md §3,
 * §6; RVWMO weak ordering, GCC __atomic builtins, no MMU/no D-cache flush so
 * coherence is purely an ordering problem solved with acquire/release fences):
 *
 *   QUEUE  : a power-of-two ring `q.jobs[]` indexed by two monotonic counters.
 *            `head` = #jobs PRODUCED (advanced by the single producer = the
 *            submitter, so NO CAS on enqueue). `tail` = #slots CLAIMED by
 *            consumers (advanced by `__atomic_fetch_add`, one claim per worker,
 *            which load-balances for free). A slot mine is valid iff mine < head.
 *
 *   PUBLISH: producer writes the job payload, then a RELEASE store to `head`
 *            publishes it. Each worker does an ACQUIRE load of `head` before it
 *            trusts a slot's payload — this is the publish/consume edge.
 *
 *   BARRIER: an atomic `done` counter, incremented (ACQ_REL) by each worker as it
 *            finishes a job. The submitter waits until done == #submitted, then
 *            issues an ACQUIRE fence so it observes every job's writes (e.g. the
 *            result-array slices) before reading them.
 *
 *   WAKE   : workers `wfi`-park when the ring is empty; the submitter sends an
 *            SBI IPI to wake them after publishing (NO tight spin). On finishing
 *            a batch, a worker IPIs the submitter so its own barrier `wfi`
 *            returns. The IPI is delivered as an SSIP handled by
 *            sched_trap_vector (sched_secondary.S), which clears it and `sret`s,
 *            so the `wfi` simply falls through to re-check.
 *
 *   N=1    : with zero workers `sched_submit` runs every job INLINE on the
 *            submitter — no ring, no atomics, no barrier, no WFI. Identical
 *            cost to a plain function call and a correct result.
 *
 * RENDERER EXTENSION POINT: hart_worker_main()'s `switch (j->kind)` already has a
 * JOB_TILE arm. The tiled softgles2 fan-out (01-scalable-threading.md §5) drops
 * in by making JOB_TILE call rasterize_tile(j->minx..j->maxy) instead of the
 * generic callable — no change to enqueue/dequeue/barrier.
 */
#include <stdint.h>
#include "sched.h"
#include "sched_sbi.h"
#include "boot/runtime.h"

/* ------------------------------------------------------------------ ring --- */

typedef struct {
    sched_job_t   jobs[SCHED_QUEUE_SIZE];
    volatile unsigned long head;   /* #produced; single producer advances    */
    volatile unsigned long tail;   /* #claimed;  consumers fetch-add          */
    volatile unsigned long done;   /* #completed; barrier counter             */
} sched_queue_t;

static sched_queue_t q;

/* Scheduler config, set once by sched_init() before workers start. */
static unsigned long g_submitter_hartid;
static int           g_worker_harts[SMP_MAX_HARTS];
static int           g_n_workers;
static volatile int  g_running;        /* 0 => workers should drain and exit  */

/* Per-hart "jobs executed" counter — proves work distribution. Each hart writes
 * only its own slot, so no atomic is needed on the write; the reader (submitter,
 * after the barrier) uses an acquire fence. */
static volatile uint64_t g_hart_jobs[SMP_MAX_HARTS];

/* ------------------------------------------------------- wake primitives --- */

/* Bitmask of all worker harts, for a single broadcast IPI (base 0). */
static unsigned long g_worker_mask;

static void wake_all_workers(void)
{
    if (g_worker_mask)
        sbi_send_ipi(g_worker_mask, 0);
}

static void wake_submitter(void)
{
    sbi_send_ipi(1UL << g_submitter_hartid, 0);
}

/* Enable supervisor software interrupts on the calling hart so an IPI actually
 * pulls it out of wfi. (Workers also do this in sched_secondary.S; the submitter
 * calls this from C before it waits on a barrier.) */
static inline void enable_ssie(void)
{
    asm volatile("csrs sie, %0" :: "r"(1UL << 1) : "memory"); /* SIE.SSIE */
}
static inline void clear_ssip(void)
{
    asm volatile("csrc sip, %0" :: "r"(1UL << 1) : "memory"); /* SIP.SSIP */
}

/* --------------------------------------------------------------- public --- */

void sched_init(unsigned long submitter_hartid,
                const int *worker_harts, int n_workers)
{
    q.head = q.tail = q.done = 0;
    g_submitter_hartid = submitter_hartid;
    g_n_workers = n_workers;
    g_worker_mask = 0;
    for (int i = 0; i < SMP_MAX_HARTS; ++i)
        g_hart_jobs[i] = 0;
    for (int i = 0; i < n_workers && i < SMP_MAX_HARTS; ++i) {
        g_worker_harts[i] = worker_harts[i];
        g_worker_mask |= 1UL << worker_harts[i];
    }
    g_running = 1;
    /* Publish the config + zeroed ring before any worker observes g_running. */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    /* The submitter must be able to take the workers' completion IPI. */
    enable_ssie();
}

int sched_is_inline(void)  { return g_n_workers == 0; }
int sched_num_workers(void){ return g_n_workers; }

/* Run one job to completion (shared by inline submit and the worker loop). */
static inline void run_job(const sched_job_t *j, unsigned long hartid)
{
    switch (j->kind) {
    case JOB_TILE:
        /* EXTENSION POINT: the renderer will call rasterize_tile(j->minx..maxy)
         * here. For the test the tile job is just a callable that fills its
         * rectangular slice, so it shares the JOB_FN path. */
    case JOB_FN:
    default:
        if (j->fn)
            j->fn(j->arg);
        break;
    }
    /* Per-hart execution tally (this hart owns its slot; no atomic needed). */
    g_hart_jobs[hartid]++;
}

void sched_submit(const sched_job_t *jobs, int n)
{
    if (n <= 0)
        return;

    /* ---- N=1 / inline: run everything here, no ring, no atomics, no WFI. ---- */
    if (g_n_workers == 0) {
        for (int i = 0; i < n; ++i)
            run_job(&jobs[i], g_submitter_hartid);
        return;
    }

    /* ---- Pool mode: publish the batch, then barrier-wait. ---- */
    unsigned long base = q.head;          /* single producer: no CAS on head  */
    for (int i = 0; i < n; ++i)
        q.jobs[(base + i) & (SCHED_QUEUE_SIZE - 1)] = jobs[i];

    /* RELEASE the payload, then publish by advancing head (release). Workers
     * acquire-load head before trusting any slot. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&q.head, base + (unsigned long)n, __ATOMIC_RELEASE);

    /* Wake the (possibly wfi-parked) workers. */
    wake_all_workers();

    /* Barrier: wait until all n jobs in THIS batch have completed. `done` is
     * monotonic; the target is the new head value. The submitter parks in `wfi`
     * (workers are guaranteed runnable and will IPI us), with a bounded
     * rdtime deadline as a safety net so a stuck worker fails instead of
     * hanging QEMU. */
    unsigned long target = base + (unsigned long)n;
    uint64_t deadline = timer_ticks() + 10UL * TIMEBASE_HZ; /* 10 s budget */
    for (;;) {
        unsigned long d = __atomic_load_n(&q.done, __ATOMIC_ACQUIRE);
        if (d >= target)
            break;
        if (timer_ticks() >= deadline)
            break;                         /* safety net; caller verifies     */
        /* Re-check under the wfi: a worker's completion IPI (SSIP) wakes us; if
         * an IPI already landed we fall straight through. Clear SSIP first so we
         * don't immediately re-wake on a stale pending bit, then wfi. */
        clear_ssip();
        if (__atomic_load_n(&q.done, __ATOMIC_ACQUIRE) >= target)
            break;
        asm volatile("wfi");
    }

    /* Observe all framebuffer/result writes the workers published. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void hart_worker_main(unsigned long hartid)
{
    /* Ensure config is visible (paired with sched_init's release fence). */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    while (__atomic_load_n(&g_running, __ATOMIC_ACQUIRE)) {
        unsigned long h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
        unsigned long t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);

        if (t < h) {
            /* Claim slot `t` with a CAS, NOT a fetch-add. A plain
             * `fetch_add(&tail,1)` advances tail even when the worker loses the
             * race for the last slot (mine >= h): that over-increment is never
             * undone, so tail drifts PERMANENTLY past head. One batch still works
             * (valid claims == n, so `done` hits target), but every later batch
             * starts with tail > head and skips that many jobs -> `done` never
             * reaches target -> submitter + all workers wfi -> virtual time
             * freezes (rdtime stops too, so the deadline never fires). CAS only
             * advances tail when it actually claims a valid slot, so tail and head
             * stay in lock-step across unbounded batches. */
            if (!__atomic_compare_exchange_n(&q.tail, &t, t + 1,
                                             1 /*weak*/,
                                             __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                /* Another worker advanced tail; `t` now holds the fresh value.
                 * Re-loop (re-reads head/tail) — do NOT wfi (work may remain). */
                continue;
            }
            /* Won slot `t`. ACQUIRE the published payload before reading it. */
            unsigned long mine = t;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            sched_job_t *j = &q.jobs[mine & (SCHED_QUEUE_SIZE - 1)];
            run_job(j, hartid);

            /* Completion: bump the barrier counter (release so the submitter's
             * acquire load+fence observes our writes), then nudge the submitter
             * so its barrier wfi returns promptly. */
            __atomic_fetch_add(&q.done, 1, __ATOMIC_ACQ_REL);
            wake_submitter();
        } else {
            /* Ring empty: park in wfi until the submitter IPIs us (NO tight
             * spin — TCG is round-robin). Clear our SSIP first so a prior wake
             * doesn't make wfi a no-op forever, then re-check the ring once more
             * (a job may have been published in the window) before sleeping. */
            clear_ssip();
            if (!__atomic_load_n(&g_running, __ATOMIC_ACQUIRE))
                break;
            h = __atomic_load_n(&q.head, __ATOMIC_ACQUIRE);
            t = __atomic_load_n(&q.tail, __ATOMIC_ACQUIRE);
            if (t < h)
                continue;                  /* work appeared; don't sleep      */
            asm volatile("wfi");
        }
    }
}

void sched_shutdown(void)
{
    __atomic_store_n(&g_running, 0, __ATOMIC_RELEASE);
    /* Wake any wfi-parked workers so they observe !g_running and exit. */
    wake_all_workers();
}

uint64_t sched_hart_jobs(unsigned long hartid)
{
    if (hartid >= SMP_MAX_HARTS)
        return 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return g_hart_jobs[hartid];
}

uint64_t sched_total_jobs(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t sum = 0;
    for (int i = 0; i < SMP_MAX_HARTS; ++i)
        sum += g_hart_jobs[i];
    return sum;
}
