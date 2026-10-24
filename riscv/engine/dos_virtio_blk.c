/*
 * dos_virtio_blk.c — bare-metal virtio-blk (virtio-MMIO) read driver.
 *
 * All constants/offsets verified against QEMU 11.0.1 sources (virtio_mmio.h /
 * virtio_config.h / virtio_ids.h / virtio_blk.h / hw/block/virtio-blk.c /
 * hw/riscv/virt.c). See riscv/docs/fs/10-virtio-blk.md.
 *
 * The MMIO transport (register map, status handshake, VIRTIO_F_VERSION_1
 * negotiation, split virtqueue desc/avail/used, QUEUE_NOTIFY, completion wait)
 * mirrors riscv/audio/virtio-snd/dos_virtio_snd.c; that driver left vq_setup /
 * vq_submit / reap as TODO skeletons, so the working virtqueue machinery is
 * implemented here in full.
 *
 * Completion interrupts use the dOS IRQ layer over QEMU virt PLIC or
 * APLIC/IMSIC. Rings are identity-mapped (PA==VA on bare metal, no MMU
 * programmed in start.S). Memory ordering via __sync_synchronize().
 */
#include "engine/dos_virtio_blk.h"
#include "engine/dos_disk_layout.h"
#include "boot/irq.h"
#include "boot/runtime.h"
#include "boot/virtio_mmio.h"
#include <string.h>

/* ------------------------------------------------------------------ *
 *  virt MMIO window (doc 10 §0)                                       *
 * ------------------------------------------------------------------ */
#define VIRT_MMIO_BASE     0x10001000UL
#define VIRT_MMIO_STRIDE   0x1000UL
#define VIRT_MMIO_SLOTS    8
#define VIRTIO_ID_BLOCK    2

/* Bounded park slice for the completion wait. Long enough that a healthy read
 * is woken by its interrupt rather than by this timer, short enough that a lost
 * interrupt costs latency instead of the machine. */
#define VBLK_WAIT_SLICE_US 2000

/* ------------------------------------------------------------------ *
 *  virtio-MMIO transport registers (modern / version 2, doc 10 §2)   *
 * ------------------------------------------------------------------ */
#define VIRTIO_MMIO_MAGIC_VALUE          0x000  /* RO == 0x74726976 */
#define VIRTIO_MMIO_VERSION              0x004  /* RO == 2          */
#define VIRTIO_MMIO_DEVICE_ID            0x008  /* RO == 2 for blk  */
#define VIRTIO_MMIO_VENDOR_ID            0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES      0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL  0x014
#define VIRTIO_MMIO_DRIVER_FEATURES      0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL  0x024
#define VIRTIO_MMIO_QUEUE_SEL            0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX        0x034
#define VIRTIO_MMIO_QUEUE_NUM            0x038
#define VIRTIO_MMIO_QUEUE_READY          0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY         0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS     0x060
#define VIRTIO_MMIO_INTERRUPT_ACK        0x064
#define VIRTIO_MMIO_STATUS               0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW       0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH      0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW      0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH     0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW       0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH      0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION    0x0fc
#define VIRTIO_MMIO_CONFIG               0x100

/* status bits (virtio_config.h) */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE  1
#define VIRTIO_CONFIG_S_DRIVER       2
#define VIRTIO_CONFIG_S_DRIVER_OK    4
#define VIRTIO_CONFIG_S_FEATURES_OK  8
#define VIRTIO_CONFIG_S_FAILED       0x80

#define VIRTIO_F_VERSION_1   32   /* bank 1, bit 0 — MUST accept */

/* ------------------------------------------------------------------ *
 *  split virtqueue layout (doc 10 §3)                                 *
 * ------------------------------------------------------------------ */
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];       /* qsz entries */
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];   /* qsz entries */
};

typedef struct {
    uint16_t            qsz;
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint16_t            free_head;
    uint16_t            last_used;
    uint16_t            num_free;
} virtq_t;

/* ------------------------------------------------------------------ *
 *  virtio-blk request protocol (doc 10 §5)                            *
 * ------------------------------------------------------------------ */
#define VIRTIO_BLK_T_IN        0    /* read */
#define VIRTIO_BLK_T_OUT       1    /* write */
#define VIRTIO_BLK_T_FLUSH     4    /* flush (durability barrier) */
#define VIRTIO_BLK_S_OK        0    /* status byte: success */
#define VIRTIO_BLK_SECTOR_SIZE 512u

/* feature bits (bank 0, virtio_blk.h) */
#define VIRTIO_BLK_F_RO        5    /* device is read-only */
#define VIRTIO_BLK_F_FLUSH     9    /* device supports FLUSH */

struct virtio_blk_req {        /* == virtio_blk_outhdr, 16 bytes, LE */
    uint32_t type;             /* VIRTIO_BLK_T_IN                     */
    uint32_t reserved;         /* QEMU field name is `ioprio`; set 0  */
    uint64_t sector;           /* start LBA, in 512-byte units        */
};

/* layout sanity — fail the build if a struct drifts. */
_Static_assert(sizeof(struct virtio_blk_req)  == 16, "blk_req/outhdr must be 16B");
_Static_assert(sizeof(struct virtq_desc)      == 16, "virtq_desc must be 16B");
_Static_assert(sizeof(struct virtq_used_elem) ==  8, "used_elem must be 8B");

/* ------------------------------------------------------------------ *
 *  MMIO accessors                                                     *
 * ------------------------------------------------------------------ */
/* Now fenced -- see boot/virtio_mmio.h. `volatile` alone was a compiler barrier
 * only, so e.g. QUEUE_SEL-then-QUEUE_NUM_MAX could architecturally be reordered. */
static inline uint32_t mmio_rd(uintptr_t base, uint32_t off)
{
    return virtio_mmio_rd(base, off);
}
static inline void mmio_wr(uintptr_t base, uint32_t off, uint32_t v)
{
    virtio_mmio_wr(base, off, v);
}

/* ------------------------------------------------------------------ *
 *  driver state                                                      *
 * ------------------------------------------------------------------ */
#define VBLK_QSZ 8u   /* power of two; depth-1 synchronous needs ~3 descs/chain */

/* Rings live in .bss, page-aligned (identity-mapped: VA==PA). A single page
 * comfortably holds desc(8*16=128) + avail(4+2*8=20) + used(4+8*8=68). */
static struct virtq_desc  g_desc[VBLK_QSZ]  __attribute__((aligned(4096)));
static uint8_t            g_avail_buf[64]    __attribute__((aligned(4096)));
static uint8_t            g_used_buf[256]    __attribute__((aligned(4096)));

static uintptr_t g_base;          /* chosen blk slot MMIO base, 0 = unprobed */
static uint32_t  g_irq_source;    /* QEMU virt interrupt source: slot index + 1 */
static volatile uint32_t g_irq_seen;
static int       g_irq_routed;
static virtq_t   g_q;
static uint64_t  g_capacity;      /* sectors */
static int       g_readonly;      /* device advertised VIRTIO_BLK_F_RO */
static int       g_has_flush;     /* device advertised+negotiated F_FLUSH */

static struct virtio_blk_req g_hdr   __attribute__((aligned(16)));
static volatile uint8_t      g_status __attribute__((aligned(64)));
static vblk_stats            g_stats;

void vblk_stats_snapshot(vblk_stats *out)
{
    if (out) *out = g_stats;
}

void vblk_stats_reset(void)
{
    memset(&g_stats, 0, sizeof g_stats);
}

static int vblk_irq_handler(uint32_t source, void *opaque)
{
    uint32_t status;

    (void)opaque;
    status = mmio_rd(g_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (status)
        mmio_wr(g_base, VIRTIO_MMIO_INTERRUPT_ACK, status);

    os_irq_disable(source);
    g_irq_seen = 1;
    return 1;
}

static void vblk_irq_init(void)
{
    uint32_t status;

    status = mmio_rd(g_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (status)
        mmio_wr(g_base, VIRTIO_MMIO_INTERRUPT_ACK, status);
    g_irq_seen = 0;
    g_irq_routed = (os_irq_route(g_irq_source, vblk_irq_handler, NULL) == 0);
}

static void vblk_wait_used(void)
{
    if (!g_irq_routed) {
        while (g_q.last_used == g_q.used->idx)
            __sync_synchronize();
        __sync_synchronize();
        return;
    }

    os_irq_enable(g_irq_source);
    while (g_q.last_used == g_q.used->idx) {
        /* Park with a DEADLINE, never bare. A bare `wfi` here depended on the
         * completion interrupt actually arriving: if it is ever lost, masked or
         * mis-routed the hart halts forever with no diagnostic, mid-asset-load.
         * The loop condition is the used ring, not the interrupt -- the device
         * updates used->idx regardless -- so waking on a timer turns a permanent
         * hang into a poll that still completes. The interrupt remains the
         * expected wakeup; the deadline is only the net. */
        bm_wait_until(timer_ticks() + timer_ticks_from_us(VBLK_WAIT_SLICE_US));
        __sync_synchronize();
    }
    __sync_synchronize();
}

/* ================================================================== *
 *  bring-up handshake (doc 10 §4)                                     *
 * ================================================================== */

/* Scan 8 MMIO slots; return base of the slot whose DeviceID==2, else 0. */
static uintptr_t vblk_scan_slots(void)
{
    int i;
    for (i = 0; i < VIRT_MMIO_SLOTS; ++i) {
        uintptr_t base = VIRT_MMIO_BASE + (uintptr_t)i * VIRT_MMIO_STRIDE;
        if (mmio_rd(base, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976UL) continue;
        if (mmio_rd(base, VIRTIO_MMIO_VERSION)     != 2)            continue;
        if (mmio_rd(base, VIRTIO_MMIO_DEVICE_ID)   == VIRTIO_ID_BLOCK)
            return base;
    }
    return 0;
}

/* Allocate (from .bss) + zero + register the single split virtqueue (index 0). */
static int vq_setup(virtq_t *vq)
{
    uint32_t num_max;
    uint16_t qsz, i;
    uint64_t pa;

    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_SEL, 0);
    if (mmio_rd(g_base, VIRTIO_MMIO_QUEUE_READY) != 0) return -1; /* already live */
    num_max = mmio_rd(g_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0) return -2;                                  /* queue absent */
    qsz = (num_max < VBLK_QSZ) ? (uint16_t)num_max : VBLK_QSZ;

    vq->qsz   = qsz;
    vq->desc  = g_desc;
    vq->avail = (struct virtq_avail *)g_avail_buf;
    vq->used  = (struct virtq_used  *)g_used_buf;

    memset(g_desc,      0, sizeof g_desc);
    memset(g_avail_buf, 0, sizeof g_avail_buf);
    memset(g_used_buf,  0, sizeof g_used_buf);

    /* thread the descriptor free-list through desc[i].next */
    for (i = 0; i < qsz; ++i)
        vq->desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->num_free  = qsz;
    vq->last_used = 0;

    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NUM, qsz);

    pa = (uint64_t)(uintptr_t)vq->desc;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_DESC_LOW,  (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)vq->avail;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_AVAIL_LOW,  (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)vq->used;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_USED_LOW,  (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(pa >> 32));

    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

int vblk_init(void)
{
    uint32_t status, cap_lo, cap_hi, dev0, drv0;

    if (g_base) return 0;                       /* already up */

    g_base = vblk_scan_slots();
    if (!g_base) return -1;                     /* no blk device */
    g_irq_source = (uint32_t)(((g_base - VIRT_MMIO_BASE) / VIRT_MMIO_STRIDE) + 1u);

    mmio_wr(g_base, VIRTIO_MMIO_STATUS, 0);                          /* reset */
    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* read device features (banked). bank 0 carries F_RO(5)/F_FLUSH(9); bank 1
     * carries VERSION_1(32). Negotiate F_FLUSH (for durable writes) iff offered,
     * and record F_RO so we never issue writes to a read-only device. */
    mmio_wr(g_base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    dev0 = mmio_rd(g_base, VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_wr(g_base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    (void)mmio_rd(g_base, VIRTIO_MMIO_DEVICE_FEATURES);

    g_readonly  = (dev0 >> VIRTIO_BLK_F_RO)    & 1u;
    g_has_flush = (dev0 >> VIRTIO_BLK_F_FLUSH) & 1u;
    drv0        = g_has_flush ? (1u << VIRTIO_BLK_F_FLUSH) : 0u;

    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES, drv0);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES, 1u << (VIRTIO_F_VERSION_1 - 32));

    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK);
    status = mmio_rd(g_base, VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
        mmio_wr(g_base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
        g_base = 0;
        return -2;                              /* device rejected features */
    }

    if (vq_setup(&g_q) != 0) {
        mmio_wr(g_base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
        g_base = 0;
        return -3;
    }

    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);

    /* capacity (config +0, u64, 512-B sectors) */
    cap_lo = mmio_rd(g_base, VIRTIO_MMIO_CONFIG + 0);
    cap_hi = mmio_rd(g_base, VIRTIO_MMIO_CONFIG + 4);
    g_capacity = ((uint64_t)cap_hi << 32) | cap_lo;
    vblk_irq_init();
    return 0;
}

uint64_t vblk_capacity_sectors(void) { return g_capacity; }

static uint64_t g_volume_lba;
static int g_volume_selected;
uint64_t vblk_volume_start(void) { return g_volume_lba; }

int vblk_select_dosfs(void)
{
    uint8_t sector[512] __attribute__((aligned(64)));
    uint64_t start = 0, size = 0;
    if (g_volume_selected) return 0;
    if (vblk_read_blocks(0, 1, sector)) return -1;
    int rc = dos_disk_volume(sector, g_capacity, &start, &size);
    if (rc) return rc;
    if (start && (vblk_read_blocks(start, 1, sector) || disk_le32(sector) != 0x53464f44u))
        return -7;
    g_volume_lba = start;
    g_capacity = size;
    g_volume_selected = 1;
    return 0;
}

/* ================================================================== *
 *  descriptor allocation (depth-1 synchronous: trivial free-list)     *
 * ================================================================== */
static uint16_t vq_alloc_desc(virtq_t *vq)
{
    uint16_t d = vq->free_head;
    vq->free_head = vq->desc[d].next;
    vq->num_free--;
    return d;
}
static void vq_free_desc(virtq_t *vq, uint16_t d)
{
    vq->desc[d].next = vq->free_head;
    vq->free_head = d;
    vq->num_free++;
}

/* ================================================================== *
 *  synchronous read (doc 10 §5.3)                                     *
 * ================================================================== */
int vblk_read_blocks(uint64_t lba, uint32_t count, void *buf)
{
    uint16_t h0, h1, h2;
    uint64_t t0;
    int ok;

    if (!g_base || count == 0 || buf == 0) return -1;
    if (count > UINT32_MAX / VIRTIO_BLK_SECTOR_SIZE || lba >= g_capacity || count > g_capacity - lba) return -1;
    if (g_q.num_free < 3) return -1;            /* depth-1: should never happen */
    t0 = timer_ticks();

    /* 1. build the request header */
    g_hdr.type     = VIRTIO_BLK_T_IN;
    g_hdr.reserved = 0;
    g_hdr.sector   = g_volume_lba + lba;
    g_status       = 0xff;                       /* sentinel; device overwrites */

    /* 2. allocate a 3-desc chain (header RO | data WO | status WO) */
    h0 = vq_alloc_desc(&g_q);
    h1 = vq_alloc_desc(&g_q);
    h2 = vq_alloc_desc(&g_q);

    g_q.desc[h0].addr  = (uint64_t)(uintptr_t)&g_hdr;
    g_q.desc[h0].len   = (uint32_t)sizeof g_hdr;          /* 16 */
    g_q.desc[h0].flags = VIRTQ_DESC_F_NEXT;
    g_q.desc[h0].next  = h1;

    g_q.desc[h1].addr  = (uint64_t)(uintptr_t)buf;
    g_q.desc[h1].len   = count * VIRTIO_BLK_SECTOR_SIZE;
    g_q.desc[h1].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    g_q.desc[h1].next  = h2;

    g_q.desc[h2].addr  = (uint64_t)(uintptr_t)&g_status;
    g_q.desc[h2].len   = 1;
    g_q.desc[h2].flags = VIRTQ_DESC_F_WRITE;
    g_q.desc[h2].next  = 0;

    /* 3. publish head h0 in the avail ring + kick */
    g_q.avail->ring[g_q.avail->idx % g_q.qsz] = h0;
    __sync_synchronize();
    g_q.avail->idx += 1;
    /* `fence w,o`, not `fence rw,rw`: the doorbell is an I/O write and the ring
     * updates above are MEMORY writes, and rw,rw does not order memory-before-IO.
     * Without this the device may read a stale avail->idx and see no work. */
    virtio_wmb();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);          /* queue index 0 */

    /* 4. wait for the used ring completion (depth-1: it is ours) */
    vblk_wait_used();
    g_q.last_used += 1;

    /* 5. recycle descriptors */
    vq_free_desc(&g_q, h2);
    vq_free_desc(&g_q, h1);
    vq_free_desc(&g_q, h0);
    os_irq_enable(g_irq_source);

    ok = (g_status == VIRTIO_BLK_S_OK);
    g_stats.read_calls++;
    g_stats.read_sectors += count;
    g_stats.read_ticks += timer_ticks() - t0;
    return ok ? 0 : -2;
}

/* dosfs_read_blocks_fn adapter: (sector, buf, count). */
int vblk_read(uint64_t sector, void *buf, uint32_t count)
{
    return vblk_read_blocks(sector, count, buf);
}

/* ================================================================== *
 *  synchronous write (mirrors vblk_read_blocks; data desc is device-  *
 *  READABLE so it carries NO F_WRITE — the device reads our payload)  *
 * ================================================================== */
int vblk_write_blocks(uint64_t lba, uint32_t count, const void *buf)
{
    uint16_t h0, h1, h2;
    uint64_t t0;
    int ok;

    if (!g_base || count == 0 || buf == 0) return -1;
    if (count > UINT32_MAX / VIRTIO_BLK_SECTOR_SIZE || lba >= g_capacity || count > g_capacity - lba) return -1;
    if (g_readonly) return -1;                  /* never write a read-only device */
    if (g_q.num_free < 3) return -1;            /* depth-1: should never happen */
    t0 = timer_ticks();

    g_hdr.type     = VIRTIO_BLK_T_OUT;
    g_hdr.reserved = 0;
    g_hdr.sector   = g_volume_lba + lba;
    g_status       = 0xff;                       /* sentinel; device overwrites */

    h0 = vq_alloc_desc(&g_q);
    h1 = vq_alloc_desc(&g_q);
    h2 = vq_alloc_desc(&g_q);

    g_q.desc[h0].addr  = (uint64_t)(uintptr_t)&g_hdr;
    g_q.desc[h0].len   = (uint32_t)sizeof g_hdr;
    g_q.desc[h0].flags = VIRTQ_DESC_F_NEXT;
    g_q.desc[h0].next  = h1;

    g_q.desc[h1].addr  = (uint64_t)(uintptr_t)buf;
    g_q.desc[h1].len   = count * VIRTIO_BLK_SECTOR_SIZE;
    g_q.desc[h1].flags = VIRTQ_DESC_F_NEXT;      /* device reads payload: no F_WRITE */
    g_q.desc[h1].next  = h2;

    g_q.desc[h2].addr  = (uint64_t)(uintptr_t)&g_status;
    g_q.desc[h2].len   = 1;
    g_q.desc[h2].flags = VIRTQ_DESC_F_WRITE;
    g_q.desc[h2].next  = 0;

    g_q.avail->ring[g_q.avail->idx % g_q.qsz] = h0;
    __sync_synchronize();
    g_q.avail->idx += 1;
    /* `fence w,o`, not `fence rw,rw`: the doorbell is an I/O write and the ring
     * updates above are MEMORY writes, and rw,rw does not order memory-before-IO.
     * Without this the device may read a stale avail->idx and see no work. */
    virtio_wmb();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    vblk_wait_used();
    g_q.last_used += 1;

    vq_free_desc(&g_q, h2);
    vq_free_desc(&g_q, h1);
    vq_free_desc(&g_q, h0);
    os_irq_enable(g_irq_source);

    ok = (g_status == VIRTIO_BLK_S_OK);
    g_stats.write_calls++;
    g_stats.write_sectors += count;
    g_stats.write_ticks += timer_ticks() - t0;
    return ok ? 0 : -2;
}

/* VIRTIO_BLK_T_FLUSH — a 2-desc chain (header RO | status WO), no data desc. */
int vblk_flush(void)
{
    uint16_t h0, h1;
    uint64_t t0;
    int ok;

    if (!g_base || g_readonly) return -1;
    if (!g_has_flush) return 0;                  /* no flush feature: best-effort no-op */
    if (g_q.num_free < 2) return -1;
    t0 = timer_ticks();

    g_hdr.type     = VIRTIO_BLK_T_FLUSH;
    g_hdr.reserved = 0;
    g_hdr.sector   = 0;                          /* reserved for flush */
    g_status       = 0xff;

    h0 = vq_alloc_desc(&g_q);
    h1 = vq_alloc_desc(&g_q);

    g_q.desc[h0].addr  = (uint64_t)(uintptr_t)&g_hdr;
    g_q.desc[h0].len   = (uint32_t)sizeof g_hdr;
    g_q.desc[h0].flags = VIRTQ_DESC_F_NEXT;
    g_q.desc[h0].next  = h1;

    g_q.desc[h1].addr  = (uint64_t)(uintptr_t)&g_status;
    g_q.desc[h1].len   = 1;
    g_q.desc[h1].flags = VIRTQ_DESC_F_WRITE;
    g_q.desc[h1].next  = 0;

    g_q.avail->ring[g_q.avail->idx % g_q.qsz] = h0;
    __sync_synchronize();
    g_q.avail->idx += 1;
    /* `fence w,o`, not `fence rw,rw`: the doorbell is an I/O write and the ring
     * updates above are MEMORY writes, and rw,rw does not order memory-before-IO.
     * Without this the device may read a stale avail->idx and see no work. */
    virtio_wmb();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    vblk_wait_used();
    g_q.last_used += 1;

    vq_free_desc(&g_q, h1);
    vq_free_desc(&g_q, h0);
    os_irq_enable(g_irq_source);

    ok = (g_status == VIRTIO_BLK_S_OK);
    g_stats.flush_calls++;
    g_stats.flush_ticks += timer_ticks() - t0;
    return ok ? 0 : -2;
}

int vblk_can_write(void) { return g_base && !g_readonly; }
