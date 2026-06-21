/*
 * Bump allocator over the linker-defined heap window [__heap_start,__heap_end).
 *
 * Deliberately minimal for bring-up: allocations are 16-byte aligned, free() is
 * a no-op. A real coalescing allocator (needed before the engine runs in
 * earnest) is tracked in ROADMAP.md, Phase 2.
 */
#include "libc/libc.h"

extern char __heap_start[], __heap_end[];

static char *brk = 0;

static uintptr_t align_up(uintptr_t x, uintptr_t a)
{
    return (x + (a - 1)) & ~(a - 1);
}

void *malloc(size_t n)
{
    if (n == 0) n = 1;

    /* Lock-free bump under SMP: the boot smoke build runs with -smp N, so harts
     * can allocate concurrently. CAS the bump pointer instead of a plain store
     * (free() is a no-op, so there is nothing else to serialise). */
    for (;;) {
        char *old  = __atomic_load_n(&brk, __ATOMIC_RELAXED);
        char *base = old ? old : __heap_start;
        uintptr_t p = align_up((uintptr_t)base, 16);
        if (p + n > (uintptr_t)__heap_end)
            return 0;             /* out of heap */
        char *neu = (char *)(p + n);
        if (__atomic_compare_exchange_n(&brk, &old, neu,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return (void *)p;
        /* another hart advanced brk; retry with the reloaded value */
    }
}

void free(void *ptr)
{
    (void)ptr;                    /* bump allocator: no reclamation yet */
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (size != 0 && total / size != nmemb)
        return 0;                 /* overflow */
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    /* No size tracking yet: allocate fresh and copy n bytes. Adequate for the
     * grow-only use during bring-up; revisited with the real allocator. */
    void *p = malloc(n);
    if (p && ptr) memcpy(p, ptr, n);
    return p;
}
