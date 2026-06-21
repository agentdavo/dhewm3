#include "engine/dos_thread.h"

#include <stddef.h>
#include <stdint.h>

#include "boot/runtime.h"
#include "libc/libc.h"

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

static dos_thread_slot_t slots[DOS_MAX_THREADS];
static unsigned char stacks[DOS_MAX_THREADS][DOS_THREAD_STACK_SIZE] __attribute__((aligned(16)));
static unsigned char tls_blocks[DOS_MAX_THREADS][DOS_THREAD_TLS_SIZE] __attribute__((aligned(16)));
static volatile int slot_lock;
static volatile int threads_inited;

extern char __tls_base[];
extern char __tbss_start[];
extern char __tbss_end[];

static void dos_pause(void)
{
    __asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
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

    unsigned present = 0;
    for (unsigned long hart = 0; hart < DOS_MAX_HARTS; ++hart) {
        unsigned long state = 0;
        if (sbi_hart_get_status(hart, &state) == 0 || hart == boot_hartid)
            ++present;
    }
    printf("[dos-thread] boot hart %lu; HSM reports %u hart(s) present (scan max %u)\n",
           boot_hartid, present, DOS_MAX_HARTS);

    dos_lock(&slot_lock);
    for (unsigned i = 0; i < DOS_MAX_THREADS; ++i) {
        slots[i].stack_top = (uintptr_t)&stacks[i][DOS_THREAD_STACK_SIZE];
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

    for (unsigned long hart = 0; hart < DOS_MAX_HARTS; ++hart) {
        if (hart == boot_hartid) {
            continue;
        }

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
