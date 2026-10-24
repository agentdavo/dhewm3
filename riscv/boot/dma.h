/*
 * DMA buffer placement rules for device-visible memory.
 *
 * The bug this exists to prevent is about WHERE a buffer lives, not about
 * missing cache maintenance -- and no amount of cache maintenance fixes it.
 *
 * Drivers here declared device-writable response structs as ordinary STACK
 * LOCALS and programmed them into descriptors with VIRTQ_DESC_F_WRITE. On a
 * non-coherent SoC, invalidating the cache line holding such a response also
 * destroys whatever else shares that line -- which, on a stack, is live saved
 * registers and return addresses. Other instances: DMAing into malloc'd blocks
 * whose first cache line carries the 16-byte in-band heap header, and packing
 * several independent device-writable descriptors into one 64-byte line so the
 * device's write to one destroys another.
 *
 * So device-visible buffers must be:
 *   - statically or heap-allocated, never on a stack;
 *   - aligned to DMA_LINE and rounded UP to a multiple of DMA_LINE, so a buffer
 *     never shares a cache line with anything else;
 *   - not overlapping any allocator metadata.
 *
 * DMA_LINE is 64 because that is the common RISC-V cache-block size and QEMU
 * virt's Zicboz block size, but it is deliberately NOT asserted from
 * Zic64b -- treat it as a conservative floor, not a discovered fact.
 */
#ifndef DHEWM3_RV_DMA_H
#define DHEWM3_RV_DMA_H

#define DMA_LINE 64u

#define DMA_ALIGNED __attribute__((aligned(DMA_LINE)))

/* Round a size up so the object occupies whole cache lines. */
#define DMA_ROUND(n) (((n) + (DMA_LINE - 1u)) & ~(DMA_LINE - 1u))

/* Declare a statically allocated, line-isolated DMA buffer. */
#define DMA_BUFFER(name, bytes) \
    static unsigned char name[DMA_ROUND(bytes)] DMA_ALIGNED

#endif /* DHEWM3_RV_DMA_H */
