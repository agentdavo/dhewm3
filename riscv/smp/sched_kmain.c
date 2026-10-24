/*
 * dOS scheduler self-test — driver  (riscv/smp/sched_kmain.c)
 * ==========================================================
 *
 * STEP 1 of dОС thread support: a job/scheduler running on the hart pool, as a
 * standalone payload extending the SMP smoke test. See sched.c/sched.h and
 * riscv/docs/smp/02-scheduler.md.
 *
 * Flow (one binary; auto-adapts to -smp 1/2/4 with NO rebuild):
 *   1. Hart 0 (the boot hart / submitter) boots via riscv/boot/{start.S ->
 *      runtime.c:_cstart} -> kmain() here.
 *   2. Discover the hart pool with SBI HSM get_status (same scan as the smoke
 *      test). The boot hart is the submitter; every other present hart is a
 *      worker.
 *   3. sched_init(submitter, workers, n_workers). Start each worker at
 *      sched_secondary_entry (sched_secondary.S), which sets per-hart sp/tp, an
 *      interrupt-aware trap vector, enables SSIE, then calls secondary_worker ->
 *      hart_worker_main(): a PERSISTENT worker loop that pulls jobs and WFI-idles
 *      when the queue is empty.
 *   4. Submit a batch of NUM_JOBS jobs. Each job fills AND sums a DISJOINT slice
 *      of a shared result array (slice i = elements [i*SLICE, (i+1)*SLICE)). The
 *      barrier (sched_submit) blocks until all jobs are done.
 *   5. Hart 0 verifies the WHOLE result array is correct (every element written
 *      with its expected value, and the global sum matches the closed-form
 *      total), and reports how many jobs EACH hart executed (per-hart counters)
 *      to prove the work was distributed across the pool.
 *   6. sched_shutdown() drains the workers (they sbi_hart_stop), then hart 0
 *      powers off.
 *
 * N=1 MUST degrade gracefully: no worker harts => sched runs all jobs INLINE on
 * the boot hart, result still correct (all NUM_JOBS attributed to hart 0).
 *
 * Reuses riscv/boot/{start.S,runtime.c,sbi.c,trap.c,linker.ld} + libc READ-ONLY;
 * sched_secondary.S provides the worker trampoline + a non-fatal SSIP trap
 * vector. The smoke test's secondary.S / kmain.c are left untouched (this is a
 * separate ELF target).
 */
#include <stdint.h>
#include "libc/libc.h"
#include "smp_timer.h"   /* NOT boot/runtime.h -- see that header */
#include "smp_sbi.h"
#include "sched.h"

#ifndef SMP_MAX_HARTS
#define SMP_MAX_HARTS 8
#endif
#ifndef SMP_HART_STACK_SIZE
#define SMP_HART_STACK_SIZE (64 * 1024)
#endif
#ifndef SMP_HART_SCRATCH_SIZE
#define SMP_HART_SCRATCH_SIZE 256
#endif

/*
 * Per-hart stacks + scratch (tp targets), indexed by hartid. Referenced by
 * sched_secondary.S (same layout/contract as the smoke test). hart_stacks[id+1]
 * base == top of hart id's full-descending stack; +1 slack so the top symbol of
 * the highest hartid is in-array. In .bss, zeroed once by hart 0's _cstart.
 */
char hart_stacks[SMP_MAX_HARTS + 1][SMP_HART_STACK_SIZE]
    __attribute__((aligned(16)));
char hart_scratch[SMP_MAX_HARTS][SMP_HART_SCRATCH_SIZE]
    __attribute__((aligned(16)));

/* ----------------------------------------------------------- the workload --- */

/*
 * The test workload: a shared result array partitioned into NUM_JOBS disjoint
 * slices. Job i owns elements [i*SLICE_LEN, (i+1)*SLICE_LEN). Each job FILLS its
 * slice with a deterministic value and SUMS it into a per-job partial, so the
 * boot hart can verify both the array contents and a global sum after the
 * barrier — proving every job ran exactly once with correct ordering/fences.
 */
#define NUM_JOBS   64
#define SLICE_LEN  256
#define ARR_LEN    (NUM_JOBS * SLICE_LEN)   /* 16384 elements */

static uint32_t g_result[ARR_LEN];
static volatile uint64_t g_partial_sum[NUM_JOBS];

/* Value written at array index `idx` (and summed). Chosen so each element is
 * unique across the whole array — a single dropped/duplicated job is detectable
 * both as a wrong element AND as a wrong total. */
static inline uint32_t cell_value(int idx)
{
    return (uint32_t)(idx * 3u + 1u);
}

typedef struct {
    int job_id;     /* 0..NUM_JOBS-1 */
} job_arg_t;

static job_arg_t g_job_args[NUM_JOBS];

/* The job body: fill + sum this job's disjoint slice. No locks — slices never
 * overlap, and each job writes only its own g_partial_sum slot. */
static void slice_job(void *argp)
{
    job_arg_t *a = (job_arg_t *)argp;
    int start = a->job_id * SLICE_LEN;
    int end   = start + SLICE_LEN;
    uint64_t s = 0;
    for (int i = start; i < end; ++i) {
        uint32_t v = cell_value(i);
        g_result[i] = v;
        s += v;
    }
    g_partial_sum[a->job_id] = s;
}

/* -------------------------------------------------------- worker entry --- */

/*
 * Entered from sched_secondary.S (a0 = hartid). The persistent worker loop lives
 * in sched.c; this thin shim just forwards into it. When the scheduler shuts
 * down, hart_worker_main() returns and sched_secondary.S sbi_hart_stop()s.
 */
void secondary_worker(unsigned long hartid)
{
    hart_worker_main(hartid);
}

/* ----------------------------------------------------------- discovery --- */

static const char *hsm_state_name(long s)
{
    switch (s) {
    case SBI_HSM_STATE_STARTED:         return "STARTED";
    case SBI_HSM_STATE_STOPPED:         return "STOPPED";
    case SBI_HSM_STATE_START_PENDING:   return "START_PENDING";
    case SBI_HSM_STATE_STOP_PENDING:    return "STOP_PENDING";
    case SBI_HSM_STATE_SUSPENDED:       return "SUSPENDED";
    case SBI_HSM_STATE_SUSPEND_PENDING: return "SUSPEND_PENDING";
    case SBI_HSM_STATE_RESUME_PENDING:  return "RESUME_PENDING";
    default:                            return "?";
    }
}

/* ----------------------------------------------------------------- main --- */

int kmain(void)
{
    printf("\n");
    printf("==================================================\n");
    printf("  dOS  -  scheduler self-test (STEP 1 thread pool)\n");
    printf("==================================================\n");
    printf("[sched] boot hartid    : %lu\n", boot_hartid);
    printf("[sched] device tree    : %#lx\n", boot_dtb);
    printf("[sched] jobs / batch   : %d\n", NUM_JOBS);
    printf("[sched] slice length   : %d  (array %d u32)\n", SLICE_LEN, ARR_LEN);

    /* ---- Phase 1: DISCOVER the hart pool (HSM get_status scan). ---- */
    int present[SMP_MAX_HARTS];
    int n_present = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id) {
        struct smp_sbiret r = sbi_hart_get_status(id);
        present[id] = (r.error == SBI_SUCCESS);
        if (!present[id] && (unsigned long)id == boot_hartid)
            present[id] = 1;             /* the boot hart is always present  */
        if (present[id]) {
            ++n_present;
            if (r.error == SBI_SUCCESS)
                printf("[sched] discover hart %d: present (state=%s)\n",
                       id, hsm_state_name(r.value));
            else
                printf("[sched] discover hart %d: present (boot hart)\n", id);
        } else {
            printf("[sched] discover hart %d: absent  (error=%ld)\n",
                   id, r.error);
        }
    }

    /* The boot hart is the submitter; every other present hart is a worker. */
    int workers[SMP_MAX_HARTS];
    int n_workers = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id)
        if (present[id] && (unsigned long)id != boot_hartid)
            workers[n_workers++] = id;

    printf("[sched] harts present  : %d\n", n_present);
    printf("[sched] submitter hart : %lu\n", boot_hartid);
    printf("[sched] worker harts   : %d%s\n", n_workers,
           n_workers == 0 ? "  (INLINE mode — N=1 graceful degradation)" : "");

    /* ---- Phase 2: bring the scheduler up + start the worker harts. ---- */
    sched_init(boot_hartid, workers, n_workers);

    extern void sched_secondary_entry(void); /* sched_secondary.S */
    int n_started = 0;
    for (int i = 0; i < n_workers; ++i) {
        int id = workers[i];
        struct smp_sbiret r =
            sbi_hart_start(id, (long)(uintptr_t)&sched_secondary_entry, id);
        if (r.error == SBI_SUCCESS || r.error == SBI_ERR_ALREADY_AVAILABLE) {
            ++n_started;
            printf("[sched] start worker %d : ok\n", id);
        } else {
            printf("[sched] start worker %d : FAILED (error=%ld)\n", id, r.error);
        }
    }
    if (n_workers)
        printf("[sched] workers started: %d/%d\n", n_started, n_workers);

    /* ---- Phase 3: build + submit the job batch, then barrier. ---- */
    for (int i = 0; i < NUM_JOBS; ++i)
        g_job_args[i].job_id = i;

    /* tile-style variant for a couple of jobs so the JOB_TILE extension point is
     * exercised, not just declared (it currently shares the callable path). */
    sched_job_t batch[NUM_JOBS];
    for (int i = 0; i < NUM_JOBS; ++i) {
        batch[i].kind = (i < 4) ? JOB_TILE : JOB_FN;
        batch[i].fn   = slice_job;
        batch[i].arg  = &g_job_args[i];
        batch[i].minx = 0;                 /* tile bounds: 1-D slice mapped to */
        batch[i].maxx = SLICE_LEN;         /* a row for the renderer-style arm */
        batch[i].miny = i;
        batch[i].maxy = i + 1;
    }

    /* Closed-form expected sum of cell_value(0..ARR_LEN-1) = sum(3*i+1). */
    uint64_t expect_sum = 0;
    for (int i = 0; i < ARR_LEN; ++i)
        expect_sum += cell_value(i);

    /* ---- Phase 3+4: submit MANY batches (the sustained per-frame model the
     * renderer fan-out needs) and verify each. Between batches the workers drain
     * the ring and WFI-park, so every sched_submit after the first must WAKE the
     * parked workers — this is the exact path that used to hang at batch 2
     * (tail over-increment -> skipped jobs -> barrier never completes -> all
     * harts wfi -> virtual-time freeze). g_result is cleared before each batch so
     * a skipped job leaves zeros and fails the array check. */
    enum { NUM_BATCHES = 16 };
    int batches_ok = 0;
    uint64_t t0 = timer_ms();
    for (int b = 0; b < NUM_BATCHES; ++b) {
        for (int i = 0; i < ARR_LEN; ++i)
            g_result[i] = 0;                 /* clear so skipped jobs show up */

        sched_submit(batch, NUM_JOBS);       /* publish + barrier (wakes workers) */

        int bok = 1, bad = -1;
        for (int i = 0; i < ARR_LEN; ++i)
            if (g_result[i] != cell_value(i)) { bok = 0; bad = i; break; }
        uint64_t gs = 0;
        for (int i = 0; i < NUM_JOBS; ++i)
            gs += g_partial_sum[i];

        if (bok && gs == expect_sum) {
            ++batches_ok;
            printf("[sched]   batch %2d : OK\n", b);
        } else {
            printf("[sched] batch %2d : FAIL (array=%d bad_idx=%d sum=%lu/%lu)\n",
                   b, bok, bad, (unsigned long)gs, (unsigned long)expect_sum);
            break;
        }
    }
    uint64_t t1 = timer_ms();
    printf("\n[sched] batches completed: %d/%d  (%d jobs/batch, %lu ms total)\n",
           batches_ok, (int)NUM_BATCHES, NUM_JOBS, (unsigned long)(t1 - t0));

    /* Per-hart job distribution (cumulative across all batches): proves the work
     * spread across the pool and that every job of every batch ran exactly once. */
    uint64_t expect_total = (uint64_t)batches_ok * NUM_JOBS;
    printf("[sched] per-hart job distribution (cumulative):\n");
    uint64_t total = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id) {
        uint64_t n = sched_hart_jobs(id);
        if (n == 0 && !present[id])
            continue;
        const char *role = ((unsigned long)id == boot_hartid)
                               ? "submitter" : "worker";
        printf("[sched]   hart %d (%-9s): %lu jobs\n",
               id, role, (unsigned long)n);
        total += n;
    }
    printf("[sched]   total executed     : %lu jobs (want %lu)\n",
           (unsigned long)total, (unsigned long)expect_total);

    /* ---- Phase 5: shut the workers down and report PASS/FAIL. ---- */
    sched_shutdown();

    int pass = (batches_ok == (int)NUM_BATCHES) && (total == expect_total);
    printf("\n");
    if (pass) {
        printf("[sched] SCHED test: PASS  (%d batches x %d jobs, %d worker(s), sustained submit OK)\n",
               (int)NUM_BATCHES, NUM_JOBS, n_workers);
    } else {
        printf("[sched] SCHED test: FAIL  (batches=%d/%d total=%lu/%lu)\n",
               batches_ok, (int)NUM_BATCHES,
               (unsigned long)total, (unsigned long)expect_total);
    }

    printf("\n[sched] powering off via SBI.\n");
    return 0; /* _cstart -> sbi_shutdown() */
}
