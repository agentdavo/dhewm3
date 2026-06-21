/*
 * dos_virtio_snd.c — bare-metal virtio-sound (virtio-MMIO) playback driver
 * SKELETON (doc 12). All constants/offsets verified against QEMU 11.0.1 sources
 * (virtio_mmio.h / virtio_config.h / virtio_snd.h / hw/audio/virtio-snd.c /
 * hw/riscv/virt.c). This file is COMPILE-ORIENTED scaffolding: the register map,
 * structs, and call flow are laid out; the TODO'd bodies are the bring-up work
 * (build order step 5 in 00-audio.md §4, checklist in §7).
 *
 * Polling only — no PLIC/IRQ. Rings are identity-mapped (PA==VA on bare metal),
 * page-aligned from the heap. Memory ordering via __sync_synchronize().
 */
#include "dos_virtio_snd.h"
#include <string.h>

/* ------------------------------------------------------------------ *
 *  virt MMIO window (doc 12 §0)                                       *
 * ------------------------------------------------------------------ */
#define VIRT_MMIO_BASE     0x10001000UL
#define VIRT_MMIO_STRIDE   0x1000UL
#define VIRT_MMIO_SLOTS    8
#define VIRTIO_ID_SOUND    25

/* ------------------------------------------------------------------ *
 *  virtio-MMIO transport registers (modern / version 2, doc 12 §1)   *
 * ------------------------------------------------------------------ */
#define VIRTIO_MMIO_MAGIC_VALUE          0x000  /* RO == 0x74726976 */
#define VIRTIO_MMIO_VERSION              0x004  /* RO == 2          */
#define VIRTIO_MMIO_DEVICE_ID            0x008  /* RO == 25 for snd */
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

#define VIRTIO_F_VERSION_1   32   /* bank 1, bit 0 */

/* ------------------------------------------------------------------ *
 *  split virtqueue layout (doc 12 §2)                                 *
 * ------------------------------------------------------------------ */
#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2
#define VIRTQ_DESC_F_INDIRECT 4

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

/* snd virtqueue indices (doc 12 §4) */
enum {
    VIRTIO_SND_VQ_CONTROL = 0,
    VIRTIO_SND_VQ_EVENT   = 1,
    VIRTIO_SND_VQ_TX      = 2,
    VIRTIO_SND_VQ_RX      = 3
};

/* ------------------------------------------------------------------ *
 *  virtio-snd protocol (doc 12 §5)                                    *
 * ------------------------------------------------------------------ */
struct virtio_snd_hdr     { uint32_t code; };
struct virtio_snd_pcm_hdr { struct virtio_snd_hdr hdr; uint32_t stream_id; };

enum {  /* request codes */
    VIRTIO_SND_R_PCM_INFO       = 0x0100,
    VIRTIO_SND_R_PCM_SET_PARAMS = 0x0101,
    VIRTIO_SND_R_PCM_PREPARE    = 0x0102,
    VIRTIO_SND_R_PCM_RELEASE    = 0x0103,
    VIRTIO_SND_R_PCM_START      = 0x0104,
    VIRTIO_SND_R_PCM_STOP       = 0x0105
};
enum {  /* status codes */
    VIRTIO_SND_S_OK       = 0x8000,
    VIRTIO_SND_S_BAD_MSG  = 0x8001,
    VIRTIO_SND_S_NOT_SUPP = 0x8002,
    VIRTIO_SND_S_IO_ERR   = 0x8003
};

enum { VIRTIO_SND_PCM_RATE_44100 = 6, VIRTIO_SND_PCM_RATE_48000 = 7 };

struct virtio_snd_config {       /* device config @ 0x100, 16 bytes */
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
    uint32_t controls;
};

struct virtio_snd_pcm_set_params {
    struct virtio_snd_pcm_hdr hdr;     /* code=SET_PARAMS, stream_id */
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  padding;
};

struct virtio_snd_pcm_xfer   { uint32_t stream_id; };            /* TX header */
struct virtio_snd_pcm_status { uint32_t status; uint32_t latency_bytes; };

/* layout sanity (doc 12 §8) — fail the build if a struct drifts. */
_Static_assert(sizeof(struct virtio_snd_hdr) == 4,             "snd_hdr");
_Static_assert(sizeof(struct virtio_snd_pcm_hdr) == 8,         "snd_pcm_hdr");
_Static_assert(sizeof(struct virtio_snd_pcm_set_params) == 24, "set_params");
_Static_assert(sizeof(struct virtio_snd_pcm_xfer) == 4,        "pcm_xfer");
_Static_assert(sizeof(struct virtio_snd_pcm_status) == 8,      "pcm_status");
_Static_assert(sizeof(struct virtio_snd_config) == 16,         "snd_config");

/* ------------------------------------------------------------------ *
 *  MMIO accessors (doc 12 §1)                                         *
 * ------------------------------------------------------------------ */
static inline uint32_t mmio_rd(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}
static inline void mmio_wr(uintptr_t base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* ------------------------------------------------------------------ *
 *  driver state                                                      *
 * ------------------------------------------------------------------ */
static uintptr_t g_base;          /* chosen snd slot MMIO base, 0 = unprobed */
static virtq_t   g_ctrlq;
static virtq_t   g_txq;
static uint32_t  g_streams;
static vsnd_stream_params g_params;
static int16_t  *g_tx_scratch;    /* period_frames*channels staging */

#define VSND_CTRL_QSZ 16u
#define VSND_TX_QSZ   64u
#define VSND_TX_BUFS  8u
#define VSND_MAX_PERIOD_FRAMES 4096u
#define VSND_MAX_CHANNELS 2u
#define VSND_MAX_SAMPLES (VSND_MAX_PERIOD_FRAMES * VSND_MAX_CHANNELS)

static struct virtq_desc  g_ctrl_desc[VSND_CTRL_QSZ] __attribute__((aligned(4096)));
static uint8_t            g_ctrl_avail_buf[128]      __attribute__((aligned(4096)));
static uint8_t            g_ctrl_used_buf[256]       __attribute__((aligned(4096)));

static struct virtq_desc  g_tx_desc[VSND_TX_QSZ]     __attribute__((aligned(4096)));
static uint8_t            g_tx_avail_buf[256]        __attribute__((aligned(4096)));
static uint8_t            g_tx_used_buf[1024]        __attribute__((aligned(4096)));

static int16_t            g_tx_scratch_buf[VSND_MAX_SAMPLES] __attribute__((aligned(64)));

typedef struct {
    int in_use;
    uint16_t head, d_hdr, d_pcm, d_status;
    struct virtio_snd_pcm_xfer xfer;
    struct virtio_snd_pcm_status status;
    int16_t pcm[VSND_MAX_SAMPLES];
    uint32_t frames;
} tx_slot_t;

static tx_slot_t g_tx_slots[VSND_TX_BUFS] __attribute__((aligned(64)));
static uint32_t  g_period_frames;
static uint32_t  g_channels;

/* ================================================================== *
 *  bring-up handshake (doc 12 §3) — SKELETON                          *
 * ================================================================== */

/* Scan 8 MMIO slots; return base of the slot whose DeviceID==25, else 0. */
static uintptr_t vsnd_scan_slots(void)
{
    int i;
    for (i = 0; i < VIRT_MMIO_SLOTS; ++i) {
        uintptr_t base = VIRT_MMIO_BASE + (uintptr_t)i * VIRT_MMIO_STRIDE;
        if (mmio_rd(base, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976UL) continue;
        if (mmio_rd(base, VIRTIO_MMIO_VERSION)     != 2)            continue;
        if (mmio_rd(base, VIRTIO_MMIO_DEVICE_ID)   == VIRTIO_ID_SOUND)
            return base;
    }
    return 0;
}

/* Allocate + zero + register one split virtqueue (doc 12 §4). */
static int vq_setup(virtq_t *vq, int q_index, uint16_t want_size)
{
    uint32_t num_max;
    uint16_t qsz, i;
    uint64_t pa;

    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_SEL, (uint32_t)q_index);
    if (mmio_rd(g_base, VIRTIO_MMIO_QUEUE_READY) != 0) return -1;
    num_max = mmio_rd(g_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0) return -2;
    qsz = (num_max < want_size) ? (uint16_t)num_max : want_size;

    vq->qsz = qsz;
    if (q_index == VIRTIO_SND_VQ_CONTROL) {
        vq->desc  = g_ctrl_desc;
        vq->avail = (struct virtq_avail *)g_ctrl_avail_buf;
        vq->used  = (struct virtq_used  *)g_ctrl_used_buf;
        memset(g_ctrl_desc, 0, sizeof g_ctrl_desc);
        memset(g_ctrl_avail_buf, 0, sizeof g_ctrl_avail_buf);
        memset(g_ctrl_used_buf, 0, sizeof g_ctrl_used_buf);
    } else if (q_index == VIRTIO_SND_VQ_TX) {
        vq->desc  = g_tx_desc;
        vq->avail = (struct virtq_avail *)g_tx_avail_buf;
        vq->used  = (struct virtq_used  *)g_tx_used_buf;
        memset(g_tx_desc, 0, sizeof g_tx_desc);
        memset(g_tx_avail_buf, 0, sizeof g_tx_avail_buf);
        memset(g_tx_used_buf, 0, sizeof g_tx_used_buf);
    } else {
        return -3;
    }

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

int vsnd_probe(void)
{
    uint32_t status;

    if (g_base) return 0;                       /* already up */

    g_base = vsnd_scan_slots();
    if (!g_base) return -1;                     /* no snd device */

    /* handshake (doc 12 §3) */
    mmio_wr(g_base, VIRTIO_MMIO_STATUS, 0);                          /* reset */
    mmio_wr(g_base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* accept ONLY VIRTIO_F_VERSION_1 (bank 1, bit 0) */
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_wr(g_base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
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

    /* read config: need streams >= 1 (doc 12 §1 config space) */
    g_streams = mmio_rd(g_base, VIRTIO_MMIO_CONFIG + 4 /* offsetof streams */);
    if (g_streams < 1) { g_base = 0; return -3; }

    /* set up control + TX queues (event/rx skipped) */
    if (vq_setup(&g_ctrlq, VIRTIO_SND_VQ_CONTROL, 16) != 0) return -4;
    if (vq_setup(&g_txq,   VIRTIO_SND_VQ_TX,      64) != 0) return -5;

    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
    return 0;
}

/* ================================================================== *
 *  virtqueue submit / reap (doc 12 §4) — SKELETON                     *
 * ================================================================== */

/* Submit a 1-2 desc chain on queue q (head index h) and kick. */
static void vq_submit(virtq_t *vq, int q_index, uint16_t h)
{
    vq->avail->ring[vq->avail->idx % vq->qsz] = h;
    __sync_synchronize();
    vq->avail->idx += 1;
    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NOTIFY, (uint32_t)q_index);
}

static uint16_t vq_alloc_desc(virtq_t *vq)
{
    uint16_t d;
    if (vq->num_free == 0) return 0xffffu;
    d = vq->free_head;
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

static tx_slot_t *tx_slot_by_head(uint16_t head)
{
    unsigned i;
    for (i = 0; i < VSND_TX_BUFS; ++i)
        if (g_tx_slots[i].in_use && g_tx_slots[i].head == head)
            return &g_tx_slots[i];
    return 0;
}

static void tx_reap(void)
{
    while (g_txq.last_used != g_txq.used->idx) {
        struct virtq_used_elem *e = &g_txq.used->ring[g_txq.last_used % g_txq.qsz];
        tx_slot_t *s;
        __sync_synchronize();
        s = tx_slot_by_head((uint16_t)e->id);
        if (s) {
            vq_free_desc(&g_txq, s->d_status);
            vq_free_desc(&g_txq, s->d_pcm);
            vq_free_desc(&g_txq, s->d_hdr);
            s->in_use = 0;
        }
        g_txq.last_used++;
    }
}

/* Issue a control request (read-only req desc + write-only resp desc) and poll
 * for completion; return the resp status code, or -1 on timeout/error. */
static int ctrl_request(const void *req, size_t req_len,
                        void *resp, size_t resp_len)
{
    uint16_t h, h2;
    unsigned spins = 0;

    if (g_ctrlq.num_free < 2) return -1;
    h  = vq_alloc_desc(&g_ctrlq);
    h2 = vq_alloc_desc(&g_ctrlq);
    if (h == 0xffffu || h2 == 0xffffu) return -1;

    memset(resp, 0, resp_len);
    g_ctrlq.desc[h].addr  = (uint64_t)(uintptr_t)req;
    g_ctrlq.desc[h].len   = (uint32_t)req_len;
    g_ctrlq.desc[h].flags = VIRTQ_DESC_F_NEXT;
    g_ctrlq.desc[h].next  = h2;

    g_ctrlq.desc[h2].addr  = (uint64_t)(uintptr_t)resp;
    g_ctrlq.desc[h2].len   = (uint32_t)resp_len;
    g_ctrlq.desc[h2].flags = VIRTQ_DESC_F_WRITE;
    g_ctrlq.desc[h2].next  = 0;

    vq_submit(&g_ctrlq, VIRTIO_SND_VQ_CONTROL, h);

    while (g_ctrlq.last_used == g_ctrlq.used->idx) {
        __sync_synchronize();
        if (++spins > 100000000u) {
            vq_free_desc(&g_ctrlq, h2);
            vq_free_desc(&g_ctrlq, h);
            return -1;
        }
    }
    __sync_synchronize();
    g_ctrlq.last_used++;

    vq_free_desc(&g_ctrlq, h2);
    vq_free_desc(&g_ctrlq, h);
    return (int)((struct virtio_snd_hdr *)resp)->code;
}

/* ================================================================== *
 *  stream lifecycle (doc 12 §5)                                       *
 * ================================================================== */
int vsnd_open_stream(vsnd_dir dir, const vsnd_stream_params *p)
{
    struct virtio_snd_pcm_set_params sp;
    struct virtio_snd_hdr            resp;
    struct virtio_snd_pcm_hdr        prep;
    int rc;

    if (!g_base) return -1;
    if (dir != VSND_DIR_OUTPUT) return -2;     /* output stream 0 only */
    g_params = *p;
    if (p->channels == 0 || p->channels > VSND_MAX_CHANNELS ||
        p->period_frames == 0 || p->period_frames > VSND_MAX_PERIOD_FRAMES)
        return -5;

    g_channels = p->channels;
    g_period_frames = p->period_frames;
    g_tx_scratch = g_tx_scratch_buf;
    memset(g_tx_slots, 0, sizeof g_tx_slots);

    /* PCM_SET_PARAMS (doc 12 §5.2): mirror device defaults 8192/2048. */
    memset(&sp, 0, sizeof sp);
    sp.hdr.hdr.code  = VIRTIO_SND_R_PCM_SET_PARAMS;
    sp.hdr.stream_id = 0;
    sp.period_bytes  = p->period_frames * p->channels * 2u;
    sp.buffer_bytes  = sp.period_bytes * VSND_TX_BUFS;
    sp.features      = 0;
    sp.channels      = p->channels;
    sp.format        = (uint8_t)p->fmt;                       /* S16 = 5 */
    sp.rate          = (p->rate == 44100) ? VIRTIO_SND_PCM_RATE_44100
                                          : VIRTIO_SND_PCM_RATE_48000;
    rc = ctrl_request(&sp, sizeof sp, &resp, sizeof resp);
    if (rc != VIRTIO_SND_S_OK) return -3;

    /* PCM_PREPARE (doc 12 §5.3) */
    memset(&prep, 0, sizeof prep);
    prep.hdr.code  = VIRTIO_SND_R_PCM_PREPARE;
    prep.stream_id = 0;
    rc = ctrl_request(&prep, sizeof prep, &resp, sizeof resp);
    if (rc != VIRTIO_SND_S_OK) return -4;

    return 0;
}

static int vsnd_stream_ctl(vsnd_dir dir, uint32_t code)
{
    struct virtio_snd_pcm_hdr req;
    struct virtio_snd_hdr     resp;
    if (!g_base) return -1;
    if (dir != VSND_DIR_OUTPUT) return -2;
    memset(&req, 0, sizeof req);
    req.hdr.code  = code;
    req.stream_id = 0;
    return (ctrl_request(&req, sizeof req, &resp, sizeof resp) == VIRTIO_SND_S_OK)
               ? 0 : -3;
}

int vsnd_stream_start(vsnd_dir dir) { return vsnd_stream_ctl(dir, VIRTIO_SND_R_PCM_START); }
int vsnd_stream_stop (vsnd_dir dir) { return vsnd_stream_ctl(dir, VIRTIO_SND_R_PCM_STOP);  }

/* ================================================================== *
 *  PCM I/O (TX, doc 12 §6) — SKELETON                                 *
 * ================================================================== */
int16_t *vsnd_tx_scratch(void) { return g_tx_scratch; }

int vsnd_tx_avail_frames(void)
{
    unsigned free_slots = 0, i;
    if (!g_base || g_period_frames == 0) return 0;
    tx_reap();
    for (i = 0; i < VSND_TX_BUFS; ++i)
        if (!g_tx_slots[i].in_use)
            free_slots++;
    if (g_txq.num_free / 3u < free_slots)
        free_slots = g_txq.num_free / 3u;
    return (int)(free_slots * g_period_frames);
}

int vsnd_tx(const int16_t *pcm, uint32_t frames)
{
    uint32_t accepted = 0;

    if (!g_base || !pcm || g_period_frames == 0 || g_channels == 0) return -1;
    tx_reap();

    while (frames > 0 && g_txq.num_free >= 3) {
        tx_slot_t *s = 0;
        uint32_t todo = (frames < g_period_frames) ? frames : g_period_frames;
        unsigned i;
        for (i = 0; i < VSND_TX_BUFS; ++i) {
            if (!g_tx_slots[i].in_use) { s = &g_tx_slots[i]; break; }
        }
        if (!s) break;

        memcpy(s->pcm, pcm + (size_t)accepted * g_channels,
               (size_t)todo * g_channels * 2u);
        s->xfer.stream_id = 0;
        s->status.status = 0xffffffffu;
        s->status.latency_bytes = 0;
        s->frames = todo;

        s->d_hdr = vq_alloc_desc(&g_txq);
        s->d_pcm = vq_alloc_desc(&g_txq);
        s->d_status = vq_alloc_desc(&g_txq);
        if (s->d_hdr == 0xffffu || s->d_pcm == 0xffffu || s->d_status == 0xffffu)
            break;

        s->head = s->d_hdr;
        s->in_use = 1;

        g_txq.desc[s->d_hdr].addr  = (uint64_t)(uintptr_t)&s->xfer;
        g_txq.desc[s->d_hdr].len   = (uint32_t)sizeof s->xfer;
        g_txq.desc[s->d_hdr].flags = VIRTQ_DESC_F_NEXT;
        g_txq.desc[s->d_hdr].next  = s->d_pcm;

        g_txq.desc[s->d_pcm].addr  = (uint64_t)(uintptr_t)s->pcm;
        g_txq.desc[s->d_pcm].len   = todo * g_channels * 2u;
        g_txq.desc[s->d_pcm].flags = VIRTQ_DESC_F_NEXT;
        g_txq.desc[s->d_pcm].next  = s->d_status;

        g_txq.desc[s->d_status].addr  = (uint64_t)(uintptr_t)&s->status;
        g_txq.desc[s->d_status].len   = (uint32_t)sizeof s->status;
        g_txq.desc[s->d_status].flags = VIRTQ_DESC_F_WRITE;
        g_txq.desc[s->d_status].next  = 0;

        vq_submit(&g_txq, VIRTIO_SND_VQ_TX, s->head);
        accepted += todo;
        frames -= todo;
    }

    return (int)accepted;
}

void vsnd_tx_drain(void)
{
    unsigned spins = 0;
    for (;;) {
        unsigned busy = 0, i;
        tx_reap();
        for (i = 0; i < VSND_TX_BUFS; ++i)
            if (g_tx_slots[i].in_use)
                busy++;
        if (!busy) return;
        if (++spins > 100000000u) return;
        __sync_synchronize();
    }
}

void vsnd_close(void)
{
    if (!g_base) return;
    vsnd_stream_ctl(VSND_DIR_OUTPUT, VIRTIO_SND_R_PCM_RELEASE);
    /* queues left live for re-open; full teardown is optional on power-off. */
}
