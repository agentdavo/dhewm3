/*
 * dos_virtio_input.h — bare-metal virtio-input (virtio-MMIO) RX driver surface.
 *
 * Scans the virt MMIO bus for virtio-input devices (DeviceID==18), handshakes,
 * sets up the single event virtqueue (index 0) pre-populated with device-writable
 * 8-byte buffers, and serves Linux evdev events by polling the used ring. Up to
 * VINPUT_MAX_DEV devices (e.g. a virtio-keyboard + a virtio-tablet on separate
 * MMIO slots). Polled, no IRQ; identity-mapped DMA (PA==VA). Self-contained: it
 * carries its own copy of the MMIO transport (mirrors dos_virtio_blk.c) so it
 * links into libSDL3 alongside the "dos" video driver.
 *
 * See riscv/docs/input/00-virtio-input-plan.md.
 */
#ifndef DOS_VIRTIO_INPUT_H
#define DOS_VIRTIO_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One Linux evdev event, exactly as virtio-input delivers it (8 bytes, LE). */
struct dos_input_event {
    uint16_t type;    /* EV_SYN=0, EV_KEY=1, EV_REL=2, EV_ABS=3              */
    uint16_t code;    /* KEY_ / BTN_ (EV_KEY), REL_ (EV_REL), ABS_ (EV_ABS) */
    uint32_t value;   /* key: 1=down 0=up 2=repeat; rel: delta; abs: absolute */
};

/* Scan + bring up every virtio-input device. Returns the count found (0 = none).
 * Idempotent (returns the cached count after the first call). */
int vinput_init(void);

/* Poll across all devices; on an event, fill *out, re-post the buffer, return 1.
 * Returns 0 when no event is pending. */
int vinput_poll_event(struct dos_input_event *out);

#ifdef __cplusplus
}
#endif
#endif /* DOS_VIRTIO_INPUT_H */
