#include "engine/dos_thread.h"

#include <stddef.h>
#include <stdint.h>

#include "boot/runtime.h"
#include "boot/fdt.h"
#include "boot/irq.h"
#include "boot/bm_pause.h"
#include "libc/libc.h"
#include <string.h>

#define DOS_MAX_THREADS        8u   /* softgles tile workers (Lever 4): up to 7
                                     * workers + main hart = 8-way raster when
                                     * QEMU -smp provides 8 harts. 256KB stack +
                                     * 16KB TLS each. */
#define DOS_MAX_HARTS          8u
#define DOS_THREAD_STACK_SIZE  (256u * 1024u)
#define DOS_THREAD_TLS_SIZE    (16u * 1024u)

enum {
    DOS_THREAD_FREE = 0,
    DOS_THREAD_STARTING,
    DOS_THREAD_RUNNING,
    DOS_THREAD_DONE
};

typedef struct dos_thread_slot_s {
    uintptr_t stack_top; /* offset 0: loaded by dos_hart_entry.S */
    uintptr_t tls_base;  /* offset 8: loaded by dos_hart_entry.S */
    dos_thread_fn fn;
    void *arg;
    const char *name;
    volatile int state;
    volatile int result;
    unsigned long hartid;
} dos_thread_slot_t;

/* Worker stacks used to be a bare stacks[N][256K]: contiguous, no guard, so an
 * overflow in worker i walked silently into worker i-1's live stack and
 * corrupted it with no fault and no diagnostic. dhewm3 uses _alloca heavily,
 * which is exactly the pattern that overflows without touching a guard page.
 *
 * There is no MMU here to give us a guard page (and turning on paging to get one
 * would be the wrong trade -- see the audit's C1), so each stack instead carries
 * a poisoned red zone below its low end. Overflow is then detectable rather than
 * silent: dos_thread_join() and the profiler tick check the poison. Costs one
 * page per thread and nothing at runtime. */
#define DOS_STACK_REDZONE      4096u
#define DOS_REDZONE_POISON     0xA5u

typedef struct dos_stack_s {
    unsigned char redzone[DOS_STACK_REDZONE];   /* below the low end: grows into this */
    unsigned char stack[DOS_THREAD_STACK_SIZE];
} dos_stack_t;

static dos_thread_slot_t slots[DOS_MAX_THREADS];
static dos_stack_t stacks[DOS_MAX_THREADS] __attribute__((aligned(16)));
static unsigned char tls_blocks[DOS_MAX_THREADS][DOS_THREAD_TLS_SIZE] __attribute__((aligned(64)));

static void dos_redzone_paint(unsigned i)
{
    unsigned char *rz = stacks[i].redzone;
    for (unsigned j = 0; j < DOS_STACK_REDZONE; ++j)
        rz[j] = DOS_REDZONE_POISON;
}

/* 0 = intact. Checks the whole zone: a large _alloca can skip over the first
 * bytes entirely and land deeper in, so sampling only the top would miss it. */
static int dos_redzone_check(unsigned i)
{
    const unsigned char *rz = stacks[i].redzone;
    for (unsigned j = 0; j < DOS_STACK_REDZONE; ++j)
        if (rz[j] != DOS_REDZONE_POISON)
            return (int)(DOS_STACK_REDZONE - j);   /* bytes past the low end */
    return 0;
}

int dos_thread_stack_overflowed(void)
{
    for (unsigned i = 0; i < DOS_MAX_THREADS; ++i) {
        int over = dos_redzone_check(i);
        if (over) {
            printf("[dos-thread] STACK OVERFLOW: worker %u ran %d bytes past its "
                   "%u KB stack (name=%s)\n",
                   i, over, DOS_THREAD_STACK_SIZE / 1024u,
                   slots[i].name ? slots[i].name : "?");
            return 1;
        }
    }
    return 0;
}
static volatile int slot_lock;
static volatile int threads_inited;

extern char __tls_base[];
extern char __tbss_start[];
extern char __tbss_end[];

/* Hartids that are NOT the boot hart and that HSM will start, in DTB order.
 *
 * Hart discovery used to be `for (hart = 0; hart < 8; ++hart)`, i.e. it assumed
 * hartids are dense and zero-based. Real platforms are neither: QEMU itself
 * gives each socket its own base_hartid, and a management core sitting at hartid
 * 0 with the application harts above it is a common SoC layout. Enumerating
 * /cpus from the DTB costs one walk at init and removes the assumption. */
static unsigned long g_harts[DOS_MAX_HARTS];
static unsigned      g_nharts;

static void dos_discover_harts(void)
{
    int cpu;

    g_nharts = 0;
    for (cpu = fdt_find_compatible(FDT_INVALID, "riscv");
         cpu != FDT_INVALID && g_nharts < DOS_MAX_HARTS;
         cpu = fdt_find_compatible(cpu, "riscv")) {
        uint64_t reg = 0;
        uint32_t len;
        const char *status = (const char *)fdt_getprop(cpu, "status", &len);
        unsigned long state = 0;

        if (fdt_reg(cpu, 0, &reg, NULL) != 0)
            continue;
        if (status && len && !strcmp(status, "disabled"))
            continue;
        if ((unsigned long)reg == boot_hartid)
            continue;
        if (sbi_hart_get_status((unsigned long)reg, &state) != 0)
            continue;               /* firmware will not manage this hart */
        g_harts[g_nharts++] = (unsigned long)reg;
    }

    /* No usable DTB (or a tree with no cpu nodes): fall back to the old dense
     * scan so this can never be a regression on a machine that used to work. */
    if (g_nharts == 0) {
        for (unsigned long hart = 0; hart < DOS_MAX_HARTS && g_nharts < DOS_MAX_HARTS; ++hart) {
            unsigned long state = 0;
            if (hart == boot_hartid) continue;
            if (sbi_hart_get_status(hart, &state) == 0)
                g_harts[g_nharts++] = hart;
        }
    }
}

static void dos_pause(void)
{
    bm_pause();
}

static void dos_lock(volatile int *lock)
{
    while (__sync_lock_test_and_set(lock, 1) != 0) {
        dos_pause();
    }
    __sync_synchronize();
}

static void dos_unlock(volatile int *lock)
{
    __sync_synchronize();
    __sync_lock_release(lock);
}

static uintptr_t dos_tls_image_size(void)
{
    return (uintptr_t)(__tbss_end - __tls_base);
}

static void dos_prepare_tls(dos_thread_slot_t *slot)
{
    uintptr_t tdata_size = (uintptr_t)(__tbss_start - __tls_base);
    uintptr_t tbss_size = (uintptr_t)(__tbss_end - __tbss_start);
    unsigned char *dst = (unsigned char *)slot->tls_base;

    for (uintptr_t i = 0; i < tdata_size; ++i) {
        dst[i] = (unsigned char)__tls_base[i];
    }
    for (uintptr_t i = 0; i < tbss_size; ++i) {
        dst[tdata_size + i] = 0;
    }
}

void dos_threads_init(void)
{
    if (dos_tls_image_size() > DOS_THREAD_TLS_SIZE) {
        panic("dOS thread TLS block too small");
    }
    if (__atomic_exchange_n(&threads_inited, 1, __ATOMIC_ACQ_REL)) {
        return;
    }

    dos_discover_harts();
    printf("[dos-thread] boot hart %lu; %u startable hart(s) discovered (cap %u)\n",
           boot_hartid, g_nharts, DOS_MAX_HARTS);

    dos_lock(&slot_lock);
    for (unsigned i = 0; i < DOS_MAX_THREADS; ++i) {
        dos_redzone_paint(i);
        slots[i].stack_top = (uintptr_t)&stacks[i].stack[DOS_THREAD_STACK_SIZE];
        slots[i].tls_base = (uintptr_t)&tls_blocks[i][0];
        slots[i].fn = 0;
        slots[i].arg = 0;
        slots[i].name = 0;
        slots[i].state = DOS_THREAD_FREE;
        slots[i].result = 0;
        slots[i].hartid = (unsigned long)-1;
    }
    dos_unlock(&slot_lock);
}

unsigned long dos_thread_self_id(void)
{
    unsigned long hartid;
    __asm__ __volatile__("csrr %0, sscratch" : "=r"(hartid));
    return hartid;
}

int dos_thread_is_worker(void)
{
    return dos_thread_self_id() != boot_hartid;
}

int dos_thread_is_handle(const void *handle)
{
    const dos_thread_slot_t *slot = (const dos_thread_slot_t *)handle;
    return slot >= &slots[0] && slot < &slots[DOS_MAX_THREADS];
}

unsigned long dos_thread_hartid(const void *handle)
{
    const dos_thread_slot_t *slot = (const dos_thread_slot_t *)handle;
    if (!dos_thread_is_handle(handle)) {
        return 0;
    }
    return slot->hartid;
}

void *dos_thread_create(dos_thread_fn fn, void *arg, const char *name)
{
    if (!fn) {
        return 0;
    }

    dos_lock(&slot_lock);

    dos_thread_slot_t *slot = 0;
    for (unsigned i = 0; i < DOS_MAX_THREADS; ++i) {
        if (slots[i].state == DOS_THREAD_FREE) {
            slot = &slots[i];
            break;
        }
    }

    if (!slot) {
        dos_unlock(&slot_lock);
        return 0;
    }

    slot->fn = fn;
    slot->arg = arg;
    slot->name = name;
    slot->result = 0;
    dos_prepare_tls(slot);
    slot->state = DOS_THREAD_STARTING;

    /* Publish the TLS image, stack_top and tls_base BEFORE the target hart can
     * read them. sbi_ecall's "memory" clobber is only a *compiler* barrier -- it
     * does not emit a fence, so without this the stores above may still be in
     * this hart's store buffer when the started hart executes `ld sp, 0(a1)` /
     * `ld tp, 8(a1)` in dos_hart_entry.S. A stale sp or tp there faults on a hart
     * whose stvec is not yet installed (dos_hart_entry.S sets it only after the
     * loads), which presents as a silent hang rather than a panic -- matching the
     * intermittent "boot sometimes hangs after OpenSBI, just re-run" symptom.
     * Timing-dependent, so this is a strong candidate rather than a proven fix;
     * it is correct regardless. */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    for (unsigned h = 0; h < g_nharts; ++h) {
        unsigned long hart = g_harts[h];

        slot->hartid = hart;
        long err = sbi_hart_start(hart, (unsigned long)(uintptr_t)dos_hart_entry,
                                  (unsigned long)(uintptr_t)slot);
        if (err == 0) {
            dos_unlock(&slot_lock);
            return slot;
        }
    }

    slot->fn = 0;
    slot->arg = 0;
    slot->name = 0;
    slot->hartid = (unsigned long)-1;
    slot->state = DOS_THREAD_FREE;
    dos_unlock(&slot_lock);
    return 0;
}

void dos_thread_join(void *handle)
{
    dos_thread_slot_t *slot = (dos_thread_slot_t *)handle;
    if (!dos_thread_is_handle(handle)) {
        return;
    }

    while (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != DOS_THREAD_DONE) {
        dos_pause();
    }

    /* Cheapest place to notice an overflow: the worker has just finished, so its
     * deepest excursion has already happened. */
    {
        unsigned idx = (unsigned)(slot - &slots[0]);
        int over = dos_redzone_check(idx);
        if (over)
            printf("[dos-thread] STACK OVERFLOW: worker %u ran %d bytes past its "
                   "%u KB stack (name=%s)\n", idx, over,
                   DOS_THREAD_STACK_SIZE / 1024u, slot->name ? slot->name : "?");
    }

    dos_lock(&slot_lock);
    slot->fn = 0;
    slot->arg = 0;
    slot->name = 0;
    slot->hartid = (unsigned long)-1;
    slot->state = DOS_THREAD_FREE;
    dos_unlock(&slot_lock);
}

void dos_thread_entry_c(void *opaque)
{
    dos_thread_slot_t *slot = (dos_thread_slot_t *)opaque;

    /* Per-hart interrupt enable. os_irq_init_from_dtb() early-returns once the
     * backend has been chosen, so the sie/sstatus writes (and the IMSIC delivery
     * CSRs, which are per-hart state) previously only ever happened on the boot
     * hart. Without this a worker can never take an interrupt, which in
     * particular means it can never be woken from `wfi`. */
    os_irq_init_local();
    if (!dos_thread_is_handle(slot) || !slot->fn) {
        sbi_hart_stop();
        for (;;) {
            __asm__ __volatile__("wfi");
        }
    }

    __atomic_store_n(&slot->state, DOS_THREAD_RUNNING, __ATOMIC_RELEASE);
    int result = slot->fn(slot->arg);
    slot->result = result;
    __atomic_store_n(&slot->state, DOS_THREAD_DONE, __ATOMIC_RELEASE);

    sbi_hart_stop();
    for (;;) {
        __asm__ __volatile__("wfi");
    }
}
