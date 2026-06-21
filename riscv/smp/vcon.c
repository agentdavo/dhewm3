/*
 * dOS metrics channel — virtio-console (TX-only) driver  (riscv/smp/vcon.c)
 * ========================================================================
 *
 * Self-contained bare-metal virtio-MMIO driver for a single virtio-console
 * (VIRTIO_ID_CONSOLE = 3), TX path only. The transport (register map, status
 * handshake, VIRTIO_F_VERSION_1 negotiation, split virtqueue, QUEUE_NOTIFY,
 * used-ring poll) mirrors riscv/engine/dos_virtio_blk.c; reproduced here inline
 * so this directory stays self-contained (no engine includes). See vcon.h.
 *
 * Polling only — no PLIC/IRQ. Rings are identity-mapped (PA==VA on bare metal).
 * Memory ordering via __sync_synchronize().
 */
#include "vcon.h"
#include <stdint.h>

/* ---- virt MMIO window (same as virtio-blk) ---- */
#define VIRT_MMIO_BASE     0x10001000UL
#define VIRT_MMIO_STRIDE   0x1000UL
#define VIRT_MMIO_SLOTS    8
#define VIRTIO_ID_CONSOLE  3

/* ---- virtio-MMIO transport registers (modern / version 2) ---- */
#define VIRTIO_MMIO_MAGIC_VALUE          0x000  /* RO == 0x74726976 */
#define VIRTIO_MMIO_VERSION              0x004  /* RO == 2          */
#define VIRTIO_MMIO_DEVICE_ID            0x008  /* RO == 3 for console */
#define VIRTIO_MMIO_DEVICE_FEATURES      0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL  0x014
#define VIRTIO_MMIO_DRIVER_FEATURES      0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL  0x024
#define VIRTIO_MMIO_QUEUE_SEL            0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX        0x034
#define VIRTIO_MMIO_QUEUE_NUM            0x038
#define VIRTIO_MMIO_QUEUE_READY          0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY         0x050
#define VIRTIO_MMIO_STATUS               0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW       0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH      0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW      0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH     0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW       0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH      0x0a4

#define VIRTIO_CONFIG_S_ACKNOWLEDGE  1
#define VIRTIO_CONFIG_S_DRIVER       2
#define VIRTIO_CONFIG_S_DRIVER_OK    4
#define VIRTIO_CONFIG_S_FEATURES_OK  8
#define VIRTIO_CONFIG_S_FAILED       0x80
#define VIRTIO_F_VERSION_1   32     /* bank 1, bit 0 — MUST accept */

/* port-0 transmitq is virtqueue index 1 (receiveq = 0). */
#define VCON_TX_QUEUE  1

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[]; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[]; };

static inline uint32_t mmio_rd(uintptr_t base, uint32_t off)
{ return *(volatile uint32_t *)(base + off); }
static inline void mmio_wr(uintptr_t base, uint32_t off, uint32_t v)
{ *(volatile uint32_t *)(base + off) = v; }

/* ---- driver state ---- */
#define VCON_QSZ 8u
static struct virtq_desc  g_desc[VCON_QSZ]  __attribute__((aligned(4096)));
static uint8_t            g_avail_buf[64]    __attribute__((aligned(4096)));
static uint8_t            g_used_buf[256]    __attribute__((aligned(4096)));

static uintptr_t          g_base;        /* console slot MMIO base, 0 = down */
static struct virtq_avail *g_avail;
static struct virtq_used  *g_used;
static uint16_t           g_qsz;
static uint16_t           g_last_used;
static uint16_t           g_free_head;

static uintptr_t vcon_scan_slots(void)
{
    for (int i = 0; i < VIRT_MMIO_SLOTS; ++i) {
        uintptr_t base = VIRT_MMIO_BASE + (uintptr_t)i * VIRT_MMIO_STRIDE;
        if (mmio_rd(base, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976UL) continue;
        if (mmio_rd(base, VIRTIO_MMIO_VERSION)     != 2)            continue;
        if (mmio_rd(base, VIRTIO_MMIO_DEVICE_ID)   == VIRTIO_ID_CONSOLE)
            return base;
    }
    return 0;
}

/* Set up the TX virtqueue (index 1). */
static int vq_setup_tx(void)
{
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_SEL, VCON_TX_QUEUE);
    if (mmio_rd(g_base, VIRTIO_MMIO_QUEUE_READY) != 0) return -1;
    uint32_t num_max = mmio_rd(g_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0) return -2;
    g_qsz = (num_max < VCON_QSZ) ? (uint16_t)num_max : (uint16_t)VCON_QSZ;

    g_avail = (struct virtq_avail *)g_avail_buf;
    g_used  = (struct virtq_used  *)g_used_buf;
    for (unsigned i = 0; i < sizeof g_desc; ++i)     ((uint8_t *)g_desc)[i] = 0;
    for (unsigned i = 0; i < sizeof g_avail_buf; ++i) g_avail_buf[i] = 0;
    for (unsigned i = 0; i < sizeof g_used_buf; ++i)  g_used_buf[i]  = 0;

    for (uint16_t i = 0; i < g_qsz; ++i) g_desc[i].next = (uint16_t)(i + 1);
    g_free_head = 0;
    g_last_used = 0;

    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NUM, g_qsz);
    uint64_t pa = (uint64_t)(uintptr_t)g_desc;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_DESC_LOW,  (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)g_avail;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_AVAIL_LOW,  (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)g_used;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_USED_LOW,  (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(pa >> 32));

    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

int vcon_init(void)
{
    if (g_base) return 0;

    g_base = vcon_scan_slots();
    if (!g_base) return -1;                 /* no virtio-console (METRICS off) */

    mmio_wr(g_base, VIRTIO_MMIO_STATUS, 0); /* reset */
    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* Accept ONLY VERSION_1. In particular do NOT take VIRTIO_CONSOLE_F_MULTIPORT
     * (bank 0) so QEMU treats this as a simple single console port whose
     * transmitq flows without the control-queue port-open handshake. */
    mmio_wr(g_base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    (void)mmio_rd(g_base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_wr(g_base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    (void)mmio_rd(g_base, VIRTIO_MMIO_DEVICE_FEATURES);

    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES, 1u << (VIRTIO_F_VERSION_1 - 32));

    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK);
    uint32_t status = mmio_rd(g_base, VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        mmio_wr(g_base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
        g_base = 0;
        return -2;
    }

    if (vq_setup_tx() != 0) {
        mmio_wr(g_base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
        g_base = 0;
        return -3;
    }

    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
    return 0;
}

int vcon_ok(void) { return g_base != 0; }

void vcon_write(const void *buf, size_t len)
{
    if (!g_base || !buf || len == 0) return;

    /* depth-1 synchronous: one device-readable descriptor. */
    uint16_t d = g_free_head;
    g_free_head = g_desc[d].next;

    g_desc[d].addr  = (uint64_t)(uintptr_t)buf;
    g_desc[d].len   = (uint32_t)len;
    g_desc[d].flags = 0;                 /* device READS (guest -> host) */
    g_desc[d].next  = 0;

    g_avail->ring[g_avail->idx % g_qsz] = d;
    __sync_synchronize();
    g_avail->idx += 1;
    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NOTIFY, VCON_TX_QUEUE);

    while (g_last_used == g_used->idx)   /* poll TX completion */
        __sync_synchronize();
    __sync_synchronize();
    g_last_used += 1;

    g_desc[d].next = g_free_head;        /* recycle */
    g_free_head = d;
}

void vcon_puts(const char *s)
{
    size_t n = 0;
    while (s[n]) ++n;
    vcon_write(s, n);
}
