/*
 * dOS metrics channel — virtio-console (TX-only) driver  (riscv/smp/vcon.h)
 * ========================================================================
 *
 * A dedicated, second output channel for doomOS perf metrics, separate from
 * uart0 (which carries the SBI console log AND the framebuffer base64 screenshot
 * dumps). QEMU's 'virt' machine has only ONE hardware UART (confirmed in QEMU
 * hw/riscv/virt.c — VIRT_UART0 @ 0x10000000, no uart1), so the equivalent of a
 * "second serial" is a virtio-console (VIRTIO_ID_CONSOLE = 3) over the
 * virtio-MMIO transport we already use for virtio-blk.
 *
 * We only emit OUT (metrics), so this is a TX-only driver: port-0 transmitq is
 * virtqueue index 1 (receiveq=0). We deliberately do NOT negotiate
 * VIRTIO_CONSOLE_F_MULTIPORT, so QEMU's virtio-serial does not require the
 * control-queue port-open handshake: use_multiport() reads the *negotiated*
 * feature (QEMU hw/char/virtio-serial-bus.c), and a console-mode port backed by
 * a chardev is opened at realize (host_connected=true) and "doesn't block guest
 * data transfer" (QEMU hw/char/virtio-console.c virtconsole_realize). So a bare
 * transmitq write reaches the host chardev (e.g. metrics.log).
 *
 * Host wiring (run.sh, gated by METRICS=1):
 *   -chardev file,id=met,path=metrics.log
 *   -device  virtio-serial-device,id=vser0
 *   -device  virtconsole,chardev=met,bus=vser0.0,name=metrics
 */
#ifndef DHEWM3_RV_SMP_VCON_H
#define DHEWM3_RV_SMP_VCON_H

#include <stddef.h>

/* Probe + bring up the virtio-console. Returns 0 on success (channel ready),
 * negative if no virtio-console device is present (e.g. METRICS not enabled) —
 * in which case vcon_write() is a no-op and callers fall back to uart0. */
int  vcon_init(void);

/* True once vcon_init() has succeeded. */
int  vcon_ok(void);

/* Write len bytes to the metrics console (synchronous: posts one TX descriptor,
 * notifies, polls the used ring to completion, recycles). No-op if !vcon_ok().
 * The buffer need only stay valid for the duration of the call. */
void vcon_write(const void *buf, size_t len);

/* Convenience: write a NUL-terminated string. */
void vcon_puts(const char *s);

#endif /* DHEWM3_RV_SMP_VCON_H */
