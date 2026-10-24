/*
 * dОС "top" — perf monitor demo  (riscv/smp/top_kmain.c)
 * =====================================================
 *
 * Standalone payload that runs the dОС hart pool under a SUSTAINED workload while
 * a 1 Hz monitor on the boot hart emits a metrics snapshot every second over the
 * virtio-console metrics channel (vcon.c) — mirrored to uart0 (prefixed "[top]")
 * so it is also visible in the boot log. The host viewer riscv/scripts/dostop.py
 * renders the stream like top.
 *
 * Workload model: each worker hart runs a PERSISTENT busy loop (the steady-state
 * model the future tiled-renderer fan-out will use — workers stay hot, no
 * per-batch park/wake), accumulating a per-hart "work-units" counter. The boot
 * hart also does a slice of work between samples, then once per second snapshots
 * the per-hart counters and emits:
 *
 *   MET <sec> <n_harts> <total_units_per_s> <0> <heap_kb> <total_units> <h0_ups> <h1_ups> ...
 *
 * Per-hart units/sec is the utilization proxy: with -smp 1 only the boot hart is
 * busy; with -smp 4 all four harts accumulate — exactly the multi-core scaling
 * this demo shows. N=1 degrades to the boot hart doing all the work.
 *
 * Reuses the boot runtime + libc + sched_secondary.S (worker trampoline: per-hart
 * sp/tp/stvec) READ-ONLY. (The scheduler's per-batch barrier path is exercised by
 * sched_test; this demo deliberately keeps workers hot to measure sustained
 * utilization without relying on park/wake.)
 */
#include <stdint.h>
#include "libc/libc.h"
#include "smp_timer.h"   /* NOT boot/runtime.h -- see that header */
#include "smp_sbi.h"
#include "sched.h"     /* job kinds / SMP sizing (shared header) */
#include "vcon.h"

#ifndef SMP_MAX_HARTS
#define SMP_MAX_HARTS 8
#endif
#ifndef SMP_HART_STACK_SIZE
#define SMP_HART_STACK_SIZE (64 * 1024)
#endif
#ifndef SMP_HART_SCRATCH_SIZE
#define SMP_HART_SCRATCH_SIZE 256
#endif

/* Per-hart stacks + scratch (tp targets) referenced by sched_secondary.S — same
 * layout/contract as sched_kmain.c. In .bss, zeroed once by hart 0's _cstart. */
char hart_stacks[SMP_MAX_HARTS + 1][SMP_HART_STACK_SIZE] __attribute__((aligned(16)));
char hart_scratch[SMP_MAX_HARTS][SMP_HART_SCRATCH_SIZE]  __attribute__((aligned(16)));

#define RUN_SECONDS 6

/* Per-hart "work units" accumulators (each hart writes only its own slot) and a
 * shared stop flag. volatile + release/acquire fences for cross-hart visibility
 * (no MMU/D-cache: ordering is the only concern). */
static volatile uint64_t g_units[SMP_MAX_HARTS];
static volatile int      g_stop;

/* One unit of work: a fixed chunk of arithmetic. Sized so a hart completes a few
 * thousand units/second under QEMU TCG — enough resolution for the 1 Hz monitor
 * without starving the round-robin scheduler. */
static void do_work_unit(unsigned long hartid)
{
    volatile uint32_t acc = (uint32_t)hartid + 1u;
    for (int i = 0; i < 20000; ++i)
        acc = acc * 1664525u + 1013904223u;   /* LCG churn */
    (void)acc;
}

/* ------------------------------------------------- tracked heap (metric) --- */
static uint64_t g_alloc_bytes;
static void *track_malloc(size_t n) { void *p = malloc(n); if (p) g_alloc_bytes += n; return p; }

/* --------------------------------------------- tiny unsigned formatter ---- */
static void put_u(char *buf, int *pos, uint64_t v)
{
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) buf[(*pos)++] = tmp[--n];
}
static void put_s(char *buf, int *pos, const char *s) { while (*s) buf[(*pos)++] = *s++; }

/* -------------------------------------------------------- worker entry ---- */
/* Entered from sched_secondary.S (a0 = hartid): a persistent busy loop. Returns
 * when the boot hart sets g_stop, after which sched_secondary.S sbi_hart_stop()s. */
void secondary_worker(unsigned long hartid)
{
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        do_work_unit(hartid);
        g_units[hartid]++;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }
}

/* ------------------------------------------------------------------ main --- */
int kmain(void)
{
    printf("\n=================================================\n");
    printf("  dОС  -  perf monitor ('top') demo\n");
    printf("=================================================\n");
    printf("[top] boot hartid : %lu\n", boot_hartid);

    /* Discover the hart pool (HSM get_status scan). */
    int present[SMP_MAX_HARTS], n_present = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id) {
        struct smp_sbiret r = sbi_hart_get_status(id);
        present[id] = (r.error == SBI_SUCCESS) || ((unsigned long)id == boot_hartid);
        if (present[id]) ++n_present;
    }
    int workers[SMP_MAX_HARTS], n_workers = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id)
        if (present[id] && (unsigned long)id != boot_hartid)
            workers[n_workers++] = id;
    printf("[top] harts: %d present, %d worker(s)%s\n", n_present, n_workers,
           n_workers == 0 ? "  (N=1: boot hart only)" : "");

    /* Start each worker on the persistent busy loop (sched_secondary.S sets up
     * per-hart sp/tp/stvec then calls secondary_worker). */
    extern void sched_secondary_entry(void);
    for (int i = 0; i < n_workers; ++i)
        sbi_hart_start(workers[i], (long)(uintptr_t)&sched_secondary_entry, workers[i]);

    /* Metrics channel. */
    int vc = vcon_init();
    printf("[top] metrics channel: %s\n",
           vc == 0 ? "virtio-console (-> metrics.log)"
                   : "NONE (no virtio-console; uart0 mirror only)");

    /* A working-set allocation so the heap metric is non-trivial + real. */
    (void)track_malloc(256 * 1024);

    static const char hdr[] =
        "# dostop1 fields: sec n_harts units_per_s backlog heap_kb total_units perhart_ups...\n";
    if (vcon_ok()) vcon_write(hdr, sizeof hdr - 1);
    printf("[top] %s", hdr);

    /* ---- sustained workload (boot hart) + 1 Hz monitor ---- */
    uint64_t start = timer_ms();
    uint64_t next_tick = start + 1000;
    uint64_t prev_units[SMP_MAX_HARTS] = {0};
    uint64_t prev_total = 0;
    int sec = 0;

    while (timer_ms() - start < (uint64_t)RUN_SECONDS * 1000) {
        do_work_unit(boot_hartid);          /* boot hart pulls its weight too */
        g_units[boot_hartid]++;

        if (timer_ms() >= next_tick) {
            ++sec;
            (void)track_malloc(4 * 1024);   /* show a small alloc rate */

            uint64_t total = 0, per[SMP_MAX_HARTS];
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            for (int id = 0; id < SMP_MAX_HARTS; ++id) { per[id] = g_units[id]; total += per[id]; }
            uint64_t ups = total - prev_total;

            char line[256]; int p = 0;
            put_s(line, &p, "MET ");
            put_u(line, &p, (uint64_t)sec);        put_s(line, &p, " ");
            put_u(line, &p, (uint64_t)n_present);  put_s(line, &p, " ");
            put_u(line, &p, ups);                  put_s(line, &p, " ");
            put_u(line, &p, 0);                    put_s(line, &p, " ");  /* backlog n/a */
            put_u(line, &p, g_alloc_bytes / 1024); put_s(line, &p, " ");
            put_u(line, &p, total);
            for (int id = 0; id < n_present && id < SMP_MAX_HARTS; ++id) {
                put_s(line, &p, " ");
                put_u(line, &p, per[id] - prev_units[id]);
                prev_units[id] = per[id];
            }
            line[p++] = '\n';

            if (vcon_ok()) vcon_write(line, (size_t)p);
            line[p] = 0;
            printf("[top] %s", line);              /* uart0 mirror */

            prev_total = total;
            next_tick += 1000;
        }
    }

    /* Stop workers (they exit secondary_worker -> sched_secondary.S hart_stop). */
    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);

    uint64_t total = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id) total += g_units[id];
    printf("\n[top] done: %lu work-units over %d s across %d hart(s), heap %lu KB, %s.\n",
           (unsigned long)total, RUN_SECONDS, n_present,
           (unsigned long)(g_alloc_bytes / 1024),
           vcon_ok() ? "metrics on virtio-console" : "uart0 only");
    printf("[top] powering off via SBI.\n");
    return 0;
}
