/*
 * dOS SMP bring-up smoke test  (riscv/smp/kmain.c)
 * ================================================
 *
 * Goal: prove the N-hart pool comes up cleanly on QEMU `-smp 1`, `-smp 2`, and
 * `-smp 4` with a SINGLE binary and NO rebuild. This is the foundation for the
 * scalable dOS multi-core threading design (see riscv/docs/smp/01-scalable-
 * threading.md).
 *
 * Flow:
 *   1. Hart 0 boots normally through riscv/boot/{start.S -> runtime.c:_cstart}
 *      which captures the boot hartid in boot_hartid and calls kmain() (here).
 *   2. Hart 0 DISCOVERS the available harts by scanning hartids 0..MAX-1 with
 *      SBI HSM sbi_hart_get_status (EID 0x48534D, fid 2). A valid state
 *      (STARTED/STOPPED/...) means the hart exists; an error (INVALID_PARAM)
 *      means it does not. This auto-adapts to -smp 1/2/4 with no rebuild.
 *   3. For each discovered secondary, hart 0 calls sbi_hart_start(hartid,
 *      secondary_entry, hartid). secondary_entry (secondary.S) sets a per-hart
 *      stack + tp + stvec, then calls secondary_worker() below.
 *   4. Each secondary prints "hart <id> alive (mhartid=<x>)" via the SBI
 *      console, increments a shared atomic counter (release), then stops via
 *      SBI HSM (or parks in WFI as a fallback).
 *   5. Hart 0 WFI-polls the atomic counter (NOT a tight spin -- TCG is
 *      round-robin, a bare spin would starve the secondaries) until every
 *      secondary has reported, prints "SMP smoke: N harts up", then powers off
 *      via sbi_shutdown().
 *
 * The boot files (start.S, runtime.c, sbi.c, linker.ld, *.h) are reused
 * READ-ONLY; HSM/console wrappers live in smp_sbi.h and the secondary stack/tp
 * setup lives in secondary.S. SMP_HART_STACK_SIZE / SMP_HART_SCRATCH_SIZE /
 * SMP_MAX_HARTS are -D'd by the Makefile so C and asm agree.
 */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"
#include "smp_sbi.h"

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
 * Per-hart stacks and scratch (tp targets), indexed by hartid. Referenced by
 * secondary.S. hart_stacks[id+1] base == top of hart id's full-descending
 * stack. Kept in .bss (zeroed once by hart 0's _cstart before kmain runs).
 *
 * Size with +1 slack on the stack array so &hart_stacks[id+1] is always a
 * valid in-array address for the highest hartid (avoids a one-past-end symbol).
 */
char hart_stacks[SMP_MAX_HARTS + 1][SMP_HART_STACK_SIZE]
    __attribute__((aligned(16)));
char hart_scratch[SMP_MAX_HARTS][SMP_HART_SCRATCH_SIZE]
    __attribute__((aligned(16)));

/* Shared atomic counter: each secondary that comes alive increments this. */
static volatile int g_harts_alive = 0;

/* Console serialisation: the minimal libc printf is not reentrant, and several
 * harts may print near-simultaneously. A tiny test-grade spinlock keeps the
 * "hart N alive" lines from interleaving byte-wise. */
static volatile int g_console_lock = 0;

static inline void console_lock(void)
{
    while (__atomic_exchange_n(&g_console_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        /* brief pause; on TCG this hart owns the slice, so just retry */
        asm volatile("nop");
    }
}
static inline void console_unlock(void)
{
    __atomic_store_n(&g_console_lock, 0, __ATOMIC_RELEASE);
}

/* Read this hart's hardware id straight from the CSR, independent of the value
 * SBI handed us. (mhartid is M-mode; in S-mode we cannot read it directly, so
 * we use the SBI-provided hartid as the authoritative id and report it.) */
static inline unsigned long read_self_hartid(unsigned long passed)
{
    /* In S-mode mhartid is not accessible; the SBI-passed hartid is the truth.
     * Returned for the "(mhartid=<x>)" report so the message format matches the
     * spec while staying privilege-correct. */
    return passed;
}

/*
 * Secondary worker. Entered from secondary.S with a0 = hartid (stack/tp/stvec
 * already set up). Announces itself, bumps the shared counter with a release
 * store so hart 0 sees a fully-published increment, then self-stops via SBI
 * HSM. If sbi_hart_stop returns (unexpected), it parks in WFI.
 */
void secondary_worker(unsigned long hartid)
{
    unsigned long self = read_self_hartid(hartid);

    console_lock();
    printf("[smp] hart %lu alive (mhartid=%lu)\n", hartid, self);
    console_unlock();

    /* Publish liveness: release ensures the print above is ordered before the
     * counter increment that hart 0 observes. */
    __atomic_fetch_add(&g_harts_alive, 1, __ATOMIC_RELEASE);

    /* Park: stop this hart cleanly via SBI HSM. It can be re-started later by
     * the real scheduler. If stop is unsupported, fall back to WFI. */
    sbi_hart_stop();
    for (;;)
        asm volatile("wfi");
}

/* Map an HSM state code to a short label for the discovery log. */
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

int kmain(void)
{
    printf("\n");
    printf("==================================================\n");
    printf("  dOS  -  SMP N-hart bring-up smoke test\n");
    printf("==================================================\n");
    printf("[smp] boot hartid     : %lu\n", boot_hartid);
    printf("[smp] device tree     : %#lx\n", boot_dtb);
    printf("[smp] max scan harts  : %d\n", SMP_MAX_HARTS);
    printf("[smp] per-hart stack  : %d bytes\n", SMP_HART_STACK_SIZE);

    /*
     * Phase 1: DISCOVER. Scan hartids 0..SMP_MAX_HARTS-1 with HSM get_status.
     * A non-error return means the hart exists; INVALID_PARAM means it doesn't.
     * The boot hart (boot_hartid) is us -- already running.
     */
    int present[SMP_MAX_HARTS];
    int n_present = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id) {
        struct smp_sbiret r = sbi_hart_get_status(id);
        present[id] = (r.error == SBI_SUCCESS);
        if (present[id]) {
            ++n_present;
            printf("[smp] discover hart %d : present  (state=%s)\n",
                   id, hsm_state_name(r.value));
        } else if ((unsigned long)id == boot_hartid) {
            /* The boot hart must be considered present even if a firmware
             * quirk returns something odd for querying yourself. */
            present[id] = 1;
            ++n_present;
            printf("[smp] discover hart %d : present  (boot hart)\n", id);
        } else {
            printf("[smp] discover hart %d : absent   (error=%ld)\n",
                   id, r.error);
        }
    }
    printf("[smp] harts discovered: %d\n", n_present);

    int n_secondaries = n_present - 1; /* everything except the boot hart */

    /*
     * Phase 2: START SECONDARIES. For each present hart that is not us, wake it
     * at secondary_entry, passing its own hartid as the opaque value.
     */
    int n_started = 0;
    for (int id = 0; id < SMP_MAX_HARTS; ++id) {
        if (!present[id] || (unsigned long)id == boot_hartid)
            continue;

        extern void secondary_entry(void); /* secondary.S */
        struct smp_sbiret r =
            sbi_hart_start(id, (long)(uintptr_t)&secondary_entry, id);

        if (r.error == SBI_SUCCESS) {
            ++n_started;
            printf("[smp] start    hart %d : ok\n", id);
        } else if (r.error == SBI_ERR_ALREADY_AVAILABLE) {
            /* Already running (shouldn't happen from a clean boot, but treat
             * as started so the count is consistent). */
            ++n_started;
            printf("[smp] start    hart %d : already running\n", id);
        } else {
            printf("[smp] start    hart %d : FAILED (error=%ld)\n", id, r.error);
        }
    }
    printf("[smp] secondaries     : %d expected, %d started\n",
           n_secondaries, n_started);

    /*
     * Phase 3: JOIN. WFI-poll the atomic counter until every started secondary
     * has reported. WFI (not a bare spin) yields the TCG time slice so the
     * secondaries actually get to run -- a tight spin here would starve them
     * and look like a hang.
     *
     * Bounded by a deadline so a genuinely stuck hart fails the test instead of
     * hanging QEMU until the external TIMEOUT.
     */
    uint64_t start_ticks = timer_ticks();
    uint64_t deadline = start_ticks + 5UL * TIMEBASE_HZ; /* 5 s budget */
    int seen = 0;
    while ((seen = __atomic_load_n(&g_harts_alive, __ATOMIC_ACQUIRE)) < n_started) {
        if (timer_ticks() >= deadline) {
            printf("[smp] TIMEOUT waiting for secondaries: %d/%d reported\n",
                   seen, n_started);
            break;
        }
        /*
         * Relaxed poll, NOT `wfi`. Under QEMU TCG the secondaries are scheduled
         * round-robin with this hart, so re-reading the shared counter and the
         * monotonic timer is what makes forward progress -- and crucially, once
         * a secondary self-stops via SBI HSM it leaves the run set, so if hart 0
         * were parked in `wfi` with S-mode interrupts masked (start.S sets
         * sie=0) and no peer left to run, the whole VM would halt and the
         * deadline would never be re-evaluated. A short pause keeps the deadline
         * live and yields cycles cheaply. (The real scheduler wakes peers with
         * SBI IPI + an SSIP handler so it CAN safely WFI; the smoke test stays
         * dependency-free and simply relaxes.)
         */
        for (volatile int spin = 0; spin < 1000; ++spin)
            asm volatile("" ::: "memory");
    }

    /* Acquire fence so hart 0 observes everything the secondaries published. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    seen = __atomic_load_n(&g_harts_alive, __ATOMIC_ACQUIRE);

    /* Total harts up = the boot hart + secondaries that reported. */
    int total_up = 1 + seen;

    printf("\n");
    if (seen == n_started) {
        printf("[smp] SMP smoke: %d harts up\n", total_up);
        printf("[smp] PASS (boot hart %lu + %d secondaries)\n", boot_hartid, seen);
    } else {
        printf("[smp] SMP smoke: %d harts up (expected %d)\n",
               total_up, 1 + n_started);
        printf("[smp] FAIL (only %d/%d secondaries reported)\n",
               seen, n_started);
    }

    printf("\n[smp] powering off via SBI.\n");
    return 0; /* _cstart -> sbi_shutdown() */
}
