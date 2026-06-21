/*
 * dos_virtio_blk.h — bare-metal virtio-blk (virtio-MMIO) read driver surface.
 *
 * Discovers QEMU's virtio-blk-device on the virt MMIO bus (DeviceID==2),
 * hand-shakes, sets up the single request virtqueue (index 0), and serves
 * synchronous 512-byte sector I/O. Completions are IRQ-backed on QEMU virt's
 * PLIC or APLIC/IMSIC, with the caller still observing a blocking API. See
 * riscv/docs/fs/10-virtio-blk.md.
 *
 * Adapted from the virtio-snd MMIO transport (riscv/audio/virtio-snd) — same
 * register map + status handshake + split virtqueue, only the DeviceID, the
 * (single) queue, and the request format differ.
 */
#ifndef DOS_VIRTIO_BLK_H
#define DOS_VIRTIO_BLK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scan the 8 virt MMIO slots for DeviceID==2, run the virtio handshake,
 * negotiate VIRTIO_F_VERSION_1, set up request queue 0, DRIVER_OK, read
 * capacity. Returns 0 on success, <0 on no-device/handshake failure.
 * Idempotent after first success. */
int      vblk_init(void);

/* Device capacity in 512-byte sectors (0 if not initialised). */
uint64_t vblk_capacity_sectors(void);

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf`
 * (buf must hold count*512 bytes). Returns 0 on success, <0 on error.
 * Synchronous: submits one 3-descriptor chain and blocks until the used ring
 * completion interrupt arrives. */
int      vblk_read_blocks(uint64_t lba, uint32_t count, void *buf);

/* Adapter matching the dosfs_read_blocks_fn signature (sector, buf, count). */
int      vblk_read(uint64_t sector, void *buf, uint32_t count);

/* Write `count` 512-byte sectors from `buf` (device READS the payload, so the
 * data descriptor carries no F_WRITE) starting at LBA `lba`. Returns 0 ok, <0 on
 * error / read-only device. Used by savefs for the appended write-region only —
 * the read-only dosfs pack occupies the low LBAs and is never written. */
int      vblk_write_blocks(uint64_t lba, uint32_t count, const void *buf);

/* Issue VIRTIO_BLK_T_FLUSH — a durability barrier mapping to qemu_fdatasync on
 * the backing image, so a completed flush survives QEMU exit. Returns 0 on
 * success (or if the device advertises no FLUSH feature), <0 on error. */
int      vblk_flush(void);

/* True if a writable (non-read-only) block device is initialised. */
int      vblk_can_write(void);

typedef struct vblk_stats {
	uint64_t read_calls;
	uint64_t read_sectors;
	uint64_t read_ticks;
	uint64_t write_calls;
	uint64_t write_sectors;
	uint64_t write_ticks;
	uint64_t flush_calls;
	uint64_t flush_ticks;
} vblk_stats;

void     vblk_stats_snapshot(vblk_stats *out);
void     vblk_stats_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DOS_VIRTIO_BLK_H */
