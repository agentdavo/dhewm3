/*
 * virtio-mmio register accessors with correct RISC-V I/O ordering.
 *
 * These were copy-pasted into five drivers (blk, gpu, input, snd, vcon) as bare
 * `*(volatile uint32_t *)`, and the copies had already drifted. `volatile` is
 * only a COMPILER barrier: it stops GCC reordering the accesses, and says
 * nothing to the hardware. RVWMO permits the CPU to reorder two accesses to the
 * same I/O region unless a fence orders them.
 *
 * The reorder hazards here are real, not theoretical:
 *   - write QUEUE_SEL, then read QUEUE_NUM_MAX  -> may read the wrong queue
 *   - write DEVICE_FEATURES_SEL, then read DEVICE_FEATURES -> may conclude
 *     VERSION_1 is absent and refuse to negotiate
 *
 * NOTE ON WHY THIS CANNOT BE TESTED HERE: QEMU TCG's memory model is far
 * stronger than the architecture permits, so this entire bug class is
 * unreproducible under the emulator the project runs on. Correctness has to come
 * from the ordering rules, not from a passing run.
 *
 * Fence taxonomy (the `i`/`o` bits are the I/O halves of the RVWMO fence):
 *   fence o,o    order this device write after previous device writes
 *   fence i,ir   order previous device reads before subsequent reads
 *   fence w,o    order previous MEMORY writes before a device write -- the
 *                doorbell case. `__sync_synchronize()` is `fence rw,rw`, which
 *                orders memory against memory and does NOT order a prior memory
 *                write before a subsequent I/O write, so descriptor rings
 *                published with only that fence can be seen stale by the device.
 */
#ifndef DHEWM3_RV_VIRTIO_MMIO_H
#define DHEWM3_RV_VIRTIO_MMIO_H

#include <stdint.h>

/* Order everything memory-side before a following device store. Use immediately
 * before ringing a doorbell (QUEUE_NOTIFY) so the descriptors, avail ring and
 * avail->idx the device is about to read are all globally visible first. */
static inline void virtio_wmb(void)
{
    __asm__ __volatile__("fence w,o" ::: "memory");
}

/* Order a device read before subsequent reads. Use after reading a status or
 * index register whose value gates what is read next (e.g. used->idx before the
 * used ring entries it makes valid). */
static inline void virtio_rmb(void)
{
    __asm__ __volatile__("fence i,ir" ::: "memory");
}

/* Full device-side ordering barrier. */
static inline void virtio_mb(void)
{
    __asm__ __volatile__("fence io,io" ::: "memory");
}

static inline uint32_t virtio_mmio_rd(uintptr_t base, uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(base + off);
    __asm__ __volatile__("fence i,ir" ::: "memory");
    return v;
}

static inline void virtio_mmio_wr(uintptr_t base, uint32_t off, uint32_t v)
{
    __asm__ __volatile__("fence o,o" ::: "memory");
    *(volatile uint32_t *)(base + off) = v;
}

#endif /* DHEWM3_RV_VIRTIO_MMIO_H */
