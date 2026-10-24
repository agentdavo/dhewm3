/* Small SMP-safe coalescing allocator for the DoomOS linker heap.
 *
 * The bring-up bump allocator made free() a no-op, so per-draw gl4es scratch
 * arrays exhausted a 1.8 GiB heap after ~150 frames.  Blocks are address-sorted
 * in a free list, split on allocation and coalesced on free. */
#include "libc/libc.h"

extern char __heap_start[], __heap_end[];

typedef struct heap_block {
    size_t size;                 /* payload bytes */
    struct heap_block *next;     /* free-list link */
    uintptr_t tag;
} heap_block;

#define HEAP_ALIGN 16u
#define HEAP_HDR ((sizeof(heap_block) + HEAP_ALIGN - 1u) & ~(HEAP_ALIGN - 1u))
#define TAG_FREE  ((uintptr_t)0x46524545424c4f43ULL) /* FREEBLOC */
#define TAG_ALLOC ((uintptr_t)0x414c4c4f43424c4bULL) /* ALLOCBLK */

static heap_block *free_list;
static int heap_ready;
static volatile unsigned heap_lock;

static uintptr_t align_up(uintptr_t x, uintptr_t a) { return (x + a - 1u) & ~(a - 1u); }
static uintptr_t align_down(uintptr_t x, uintptr_t a) { return x & ~(a - 1u); }

static void lock_heap(void)
{
    while (__atomic_exchange_n(&heap_lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ __volatile__("nop" ::: "memory");
}
static void unlock_heap(void) { __atomic_store_n(&heap_lock, 0u, __ATOMIC_RELEASE); }

static void init_heap_locked(void)
{
    if (heap_ready) return;
    uintptr_t lo = align_up((uintptr_t)__heap_start, HEAP_ALIGN);
    uintptr_t hi = align_down((uintptr_t)__heap_end, HEAP_ALIGN);
    free_list = (heap_block *)lo;
    free_list->size = hi > lo + HEAP_HDR ? hi - lo - HEAP_HDR : 0;
    free_list->next = 0;
    free_list->tag = TAG_FREE;
    heap_ready = 1;
}

void *malloc(size_t n)
{
    if (!n) n = 1;
    if (n > (size_t)-1 - (HEAP_ALIGN - 1u)) return 0;
    n = (size_t)align_up(n, HEAP_ALIGN);

    lock_heap();
    init_heap_locked();
    heap_block *prev = 0, *b = free_list;
    while (b && b->size < n) { prev = b; b = b->next; }
    if (!b) { unlock_heap(); return 0; }

    if (b->size >= n + HEAP_HDR + HEAP_ALIGN) {
        heap_block *tail = (heap_block *)((char *)b + HEAP_HDR + n);
        tail->size = b->size - n - HEAP_HDR;
        tail->next = b->next;
        tail->tag = TAG_FREE;
        if (prev) prev->next = tail; else free_list = tail;
        b->size = n;
    } else {
        if (prev) prev->next = b->next; else free_list = b->next;
    }
    b->next = 0;
    b->tag = TAG_ALLOC;
    unlock_heap();
    return (char *)b + HEAP_HDR;
}

void free(void *ptr)
{
    if (!ptr) return;
    heap_block *b = (heap_block *)((char *)ptr - HEAP_HDR);
    lock_heap();
    if ((uintptr_t)b < align_up((uintptr_t)__heap_start, HEAP_ALIGN) ||
        (uintptr_t)b >= (uintptr_t)__heap_end || b->tag != TAG_ALLOC) {
        unlock_heap();
        return;                 /* tolerate foreign/double frees at the C ABI edge */
    }
    b->tag = TAG_FREE;
    heap_block *prev = 0, *cur = free_list;
    while (cur && cur < b) { prev = cur; cur = cur->next; }
    b->next = cur;
    if (prev) prev->next = b; else free_list = b;

    if (cur && (char *)b + HEAP_HDR + b->size == (char *)cur) {
        b->size += HEAP_HDR + cur->size;
        b->next = cur->next;
    }
    if (prev && (char *)prev + HEAP_HDR + prev->size == (char *)b) {
        prev->size += HEAP_HDR + b->size;
        prev->next = b->next;
    }
    unlock_heap();
}

void *calloc(size_t nmemb, size_t size)
{
    if (size && nmemb > (size_t)-1 / size) return 0;
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr) return malloc(n);
    if (!n) { free(ptr); return 0; }
    heap_block *b = (heap_block *)((char *)ptr - HEAP_HDR);
    lock_heap();
    size_t old = (b->tag == TAG_ALLOC) ? b->size : 0;
    unlock_heap();
    if (!old) return 0;
    if (n <= old) return ptr;
    void *p = malloc(n);
    if (!p) return 0;
    memcpy(p, ptr, old);
    free(ptr);
    return p;
}
