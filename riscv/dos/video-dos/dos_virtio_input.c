/*
 * dos_virtio_input.c — bare-metal virtio-input (virtio-MMIO) RX driver.
 *
 * The MMIO transport (register map, status handshake, VIRTIO_F_VERSION_1
 * negotiation, split virtqueue desc/avail/used) mirrors riscv/engine/
 * dos_virtio_blk.c verbatim; only the device protocol differs:
 *   - DeviceID 18 (VIRTIO_ID_INPUT), 0..VINPUT_MAX_DEV devices across the 8 slots
 *     (keyboard + tablet/mouse land on separate slots, order non-deterministic);
 *   - ONE RX event virtqueue (index 0), pre-filled with device-writable 8-byte
 *     buffers; the device writes a `struct virtio_input_event` and completes it on
 *     the used ring; after reading, the buffer is re-posted.
 *
 * Polling only — no PLIC/IRQ. Rings are identity-mapped (PA==VA on bare metal).
 * Memory ordering via __sync_synchronize(). See docs/input/00-virtio-input-plan.md.
 */
#include "dos_virtio_input.h"
#include <string.h>

/* ------------------------------------------------------------------ MMIO */
#define VIRT_MMIO_BASE    0x10001000UL
#define VIRT_MMIO_STRIDE  0x1000UL
#define VIRT_MMIO_SLOTS   8
#define VIRTIO_ID_INPUT   18

#define VIRTIO_MMIO_MAGIC_VALUE         0x000  /* RO == 0x74726976 */
#define VIRTIO_MMIO_VERSION             0x004  /* RO == 2          */
#define VIRTIO_MMIO_DEVICE_ID           0x008  /* RO == 18 for input */
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW     0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH    0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW      0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH     0x0a4

#define VIRTIO_CONFIG_S_ACKNOWLEDGE  1
#define VIRTIO_CONFIG_S_DRIVER       2
#define VIRTIO_CONFIG_S_DRIVER_OK    4
#define VIRTIO_CONFIG_S_FEATURES_OK  8
#define VIRTIO_CONFIG_S_FAILED       0x80
#define VIRTIO_F_VERSION_1           32

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; };
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[]; };
struct virtq_used_elem { uint32_t id; uint32_t len; };
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[]; };

static inline uint32_t mmio_rd(uintptr_t b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void     mmio_wr(uintptr_t b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

/* ------------------------------------------------------------------ state */
#define VINPUT_MAX_DEV 4u
#define VINPUT_QSZ     64u    /* event buffers per device */

/* per-device ring memory — each component on its own page (identity-mapped DMA;
 * a page comfortably holds desc(64*16=1024) / avail(132) / used(516)). */
static uint8_t g_desc_mem [VINPUT_MAX_DEV][4096] __attribute__((aligned(4096)));
static uint8_t g_avail_mem[VINPUT_MAX_DEV][4096] __attribute__((aligned(4096)));
static uint8_t g_used_mem [VINPUT_MAX_DEV][4096] __attribute__((aligned(4096)));
static struct dos_input_event g_evbuf[VINPUT_MAX_DEV][VINPUT_QSZ] __attribute__((aligned(64)));

typedef struct {
    uintptr_t           base;
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint16_t            qsz;
    uint16_t            last_used;
} vinput_dev;

static vinput_dev g_dev[VINPUT_MAX_DEV];
static int        g_ndev = -1;   /* -1 = not yet scanned */

/* ------------------------------------------------------------------ setup */
static int vinput_setup_one(uintptr_t base, int d)
{
    vinput_dev *v = &g_dev[d];
    uint32_t status, num_max, i;
    uint16_t qsz;
    uint64_t pa;

    mmio_wr(base, VIRTIO_MMIO_STATUS, 0);                  /* reset */
    __sync_synchronize();
    mmio_wr(base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmio_wr(base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* accept ONLY VERSION_1 (input carries no feature bits we need) */
    mmio_wr(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    (void)mmio_rd(base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_wr(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    (void)mmio_rd(base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_wr(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_wr(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    mmio_wr(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_wr(base, VIRTIO_MMIO_DRIVER_FEATURES, 1u << (VIRTIO_F_VERSION_1 - 32));

    mmio_wr(base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER | VIRTIO_CONFIG_S_FEATURES_OK);
    status = mmio_rd(base, VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        mmio_wr(base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    /* event queue 0 */
    mmio_wr(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    if (mmio_rd(base, VIRTIO_MMIO_QUEUE_READY) != 0) return -1;
    num_max = mmio_rd(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0) return -1;
    qsz = (num_max < VINPUT_QSZ) ? (uint16_t)num_max : (uint16_t)VINPUT_QSZ;

    v->base  = base;
    v->desc  = (struct virtq_desc  *)g_desc_mem[d];
    v->avail = (struct virtq_avail *)g_avail_mem[d];
    v->used  = (struct virtq_used  *)g_used_mem[d];
    v->qsz   = qsz;
    v->last_used = 0;
    memset(g_desc_mem[d],  0, sizeof g_desc_mem[d]);
    memset(g_avail_mem[d], 0, sizeof g_avail_mem[d]);
    memset(g_used_mem[d],  0, sizeof g_used_mem[d]);

    /* one device-writable buffer per descriptor; post them all */
    for (i = 0; i < qsz; ++i) {
        v->desc[i].addr  = (uint64_t)(uintptr_t)&g_evbuf[d][i];
        v->desc[i].len   = (uint32_t)sizeof(struct dos_input_event);
        v->desc[i].flags = VIRTQ_DESC_F_WRITE;             /* device WRITES events */
        v->desc[i].next  = 0;
        v->avail->ring[i] = (uint16_t)i;
    }

    mmio_wr(base, VIRTIO_MMIO_QUEUE_NUM, qsz);
    pa = (uint64_t)(uintptr_t)v->desc;
    mmio_wr(base, VIRTIO_MMIO_QUEUE_DESC_LOW,  (uint32_t)pa);
    mmio_wr(base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)v->avail;
    mmio_wr(base, VIRTIO_MMIO_QUEUE_AVAIL_LOW,  (uint32_t)pa);
    mmio_wr(base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)v->used;
    mmio_wr(base, VIRTIO_MMIO_QUEUE_USED_LOW,  (uint32_t)pa);
    mmio_wr(base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(pa >> 32));

    __sync_synchronize();
    v->avail->idx = qsz;                                   /* all buffers available */
    __sync_synchronize();
    mmio_wr(base, VIRTIO_MMIO_QUEUE_READY, 1);
    mmio_wr(base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
    __sync_synchronize();
    mmio_wr(base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);            /* buffers available */
    return 0;
}

int vinput_init(void)
{
    int i;
    if (g_ndev >= 0) return g_ndev;                        /* already scanned */
    g_ndev = 0;
    for (i = 0; i < VIRT_MMIO_SLOTS && g_ndev < (int)VINPUT_MAX_DEV; ++i) {
        uintptr_t base = VIRT_MMIO_BASE + (uintptr_t)i * VIRT_MMIO_STRIDE;
        if (mmio_rd(base, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976UL) continue;
        if (mmio_rd(base, VIRTIO_MMIO_VERSION)     != 2)            continue;
        if (mmio_rd(base, VIRTIO_MMIO_DEVICE_ID)   != VIRTIO_ID_INPUT) continue;
        if (vinput_setup_one(base, g_ndev) == 0) g_ndev++;
    }
    return g_ndev;
}

/* ------------------------------------------------------------------ poll */
int vinput_poll_event(struct dos_input_event *out)
{
    int i;
    if (g_ndev <= 0 || !out) return 0;
    for (i = 0; i < g_ndev; ++i) {
        vinput_dev *v = &g_dev[i];
        uint16_t used_idx, slot, id;

        __sync_synchronize();
        used_idx = v->used->idx;
        if (v->last_used == used_idx) continue;            /* nothing for this dev */
        __sync_synchronize();                              /* ring entry now visible */

        slot = (uint16_t)(v->last_used % v->qsz);
        id   = (uint16_t)v->used->ring[slot].id;
        v->last_used++;
        if (id >= v->qsz) continue;                        /* defensive: bad id */

        *out = g_evbuf[i][id];

        /* re-post the consumed buffer */
        v->avail->ring[v->avail->idx % v->qsz] = id;
        __sync_synchronize();
        v->avail->idx++;
        __sync_synchronize();
        mmio_wr(v->base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
        return 1;
    }
    return 0;
}
