#include "dos_virtio_gpu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
#  include <riscv_vector.h>
#endif

#define VIRT_MMIO_BASE     0x10001000UL
#define VIRT_MMIO_STRIDE   0x1000UL
#define VIRT_MMIO_SLOTS    8
#define VIRTIO_ID_GPU      16

#define VIRTIO_MMIO_MAGIC_VALUE          0x000
#define VIRTIO_MMIO_VERSION              0x004
#define VIRTIO_MMIO_DEVICE_ID            0x008
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

#define VIRTIO_F_VERSION_1 32

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VGPU_QSZ 8u
#define VGPU_RESOURCE_ID 1u
#define VGPU_MAX_SCANOUTS 16u
#define VGPU_SUBMIT_SPIN_LIMIT 10000000u
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67u

enum {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO       = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D     = 0x0101,
    VIRTIO_GPU_CMD_SET_SCANOUT            = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH         = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D    = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_RESP_OK_NODATA             = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO       = 0x1101
};

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
};

typedef struct {
    uint16_t qsz;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t free_head;
    uint16_t last_used;
    uint16_t num_free;
} virtq_t;

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
};

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VGPU_MAX_SCANOUTS];
};

struct vgpu_attach_req {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtio_gpu_mem_entry entry;
};

_Static_assert(sizeof(struct virtq_desc) == 16, "virtq_desc");
_Static_assert(sizeof(struct virtq_used_elem) == 8, "virtq_used_elem");
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24, "gpu_ctrl_hdr");
_Static_assert(sizeof(struct virtio_gpu_mem_entry) == 16, "gpu_mem_entry");

static struct virtq_desc g_desc[VGPU_QSZ] __attribute__((aligned(4096)));
static uint8_t g_avail_buf[64] __attribute__((aligned(4096)));
static uint8_t g_used_buf[128] __attribute__((aligned(4096)));

static uintptr_t g_base;
static virtq_t g_q;
static int g_ready;
static int g_width;
static int g_height;
static uint32_t *g_scanout;
static uint32_t g_scanout_bytes;
static int g_reported_no_device;
static int g_disabled;
static uint32_t g_scanout_limit_w;
static uint32_t g_scanout_limit_h;

static inline uint32_t mmio_rd(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_wr(uintptr_t base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static uintptr_t vgpu_scan_slots(void)
{
    for (int i = 0; i < VIRT_MMIO_SLOTS; ++i) {
        uintptr_t base = VIRT_MMIO_BASE + (uintptr_t)i * VIRT_MMIO_STRIDE;
        if (mmio_rd(base, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976UL) continue;
        if (mmio_rd(base, VIRTIO_MMIO_VERSION) != 2) continue;
        if (mmio_rd(base, VIRTIO_MMIO_DEVICE_ID) == VIRTIO_ID_GPU) return base;
    }
    return 0;
}

static int vq_setup(virtq_t *vq)
{
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_SEL, 0);
    if (mmio_rd(g_base, VIRTIO_MMIO_QUEUE_READY) != 0) return -1;

    uint32_t num_max = mmio_rd(g_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (num_max == 0) return -2;
    uint16_t qsz = (num_max < VGPU_QSZ) ? (uint16_t)num_max : VGPU_QSZ;
    if (qsz < 3) return -3;

    memset(g_desc, 0, sizeof(g_desc));
    memset(g_avail_buf, 0, sizeof(g_avail_buf));
    memset(g_used_buf, 0, sizeof(g_used_buf));

    vq->qsz = qsz;
    vq->desc = g_desc;
    vq->avail = (struct virtq_avail *)g_avail_buf;
    vq->used = (struct virtq_used *)g_used_buf;
    vq->free_head = 0;
    vq->last_used = 0;
    vq->num_free = qsz;

    for (uint16_t i = 0; i < qsz; ++i) {
        vq->desc[i].next = (uint16_t)(i + 1);
    }

    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NUM, qsz);

    uint64_t pa = (uint64_t)(uintptr_t)vq->desc;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)vq->avail;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, (uint32_t)(pa >> 32));
    pa = (uint64_t)(uintptr_t)vq->used;
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)pa);
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_USED_HIGH, (uint32_t)(pa >> 32));

    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int vq_alloc_desc(virtq_t *vq)
{
    if (vq->num_free == 0) return -1;
    uint16_t d = vq->free_head;
    vq->free_head = vq->desc[d].next;
    vq->num_free--;
    vq->desc[d].next = 0;
    return d;
}

static void vq_free_desc(virtq_t *vq, uint16_t d)
{
    vq->desc[d].next = vq->free_head;
    vq->free_head = d;
    vq->num_free++;
}

static void vgpu_disable_device(void)
{
    if (g_base) {
        uint32_t status = mmio_rd(g_base, VIRTIO_MMIO_STATUS);
        if (status) {
            mmio_wr(g_base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
            __sync_synchronize();
        }
        mmio_wr(g_base, VIRTIO_MMIO_STATUS, 0);
        __sync_synchronize();
    }
    g_ready = 0;
    g_base = 0;
    g_disabled = 1;
}

static int vgpu_submit(const void *req, uint32_t req_len,
                       void *resp, uint32_t resp_len)
{
    int d_req = vq_alloc_desc(&g_q);
    int d_resp = vq_alloc_desc(&g_q);
    if (d_req < 0 || d_resp < 0) {
        if (d_req >= 0) vq_free_desc(&g_q, (uint16_t)d_req);
        if (d_resp >= 0) vq_free_desc(&g_q, (uint16_t)d_resp);
        return -1;
    }

    memset(resp, 0, resp_len);
    g_q.desc[d_req].addr = (uint64_t)(uintptr_t)req;
    g_q.desc[d_req].len = req_len;
    g_q.desc[d_req].flags = VIRTQ_DESC_F_NEXT;
    g_q.desc[d_req].next = (uint16_t)d_resp;
    g_q.desc[d_resp].addr = (uint64_t)(uintptr_t)resp;
    g_q.desc[d_resp].len = resp_len;
    g_q.desc[d_resp].flags = VIRTQ_DESC_F_WRITE;
    g_q.desc[d_resp].next = 0;

    uint16_t slot = g_q.avail->idx % g_q.qsz;
    g_q.avail->ring[slot] = (uint16_t)d_req;
    __sync_synchronize();
    g_q.avail->idx++;
    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    uint32_t spins = 0;
    while (g_q.used->idx == g_q.last_used && spins < VGPU_SUBMIT_SPIN_LIMIT) {
        __asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
        spins++;
    }
    if (g_q.used->idx == g_q.last_used) {
        vgpu_disable_device();
        vq_free_desc(&g_q, (uint16_t)d_resp);
        vq_free_desc(&g_q, (uint16_t)d_req);
        return -2;
    }
    __sync_synchronize();

    uint16_t used_slot = (uint16_t)(g_q.last_used % g_q.qsz);
    uint32_t used_id = g_q.used->ring[used_slot].id;
    uint32_t used_len = g_q.used->ring[used_slot].len;
    g_q.last_used++;

    vq_free_desc(&g_q, (uint16_t)d_resp);
    vq_free_desc(&g_q, (uint16_t)d_req);
    if (used_id != (uint32_t)d_req || used_len < sizeof(struct virtio_gpu_ctrl_hdr)) {
        vgpu_disable_device();
        return -3;
    }
    return ((struct virtio_gpu_ctrl_hdr *)resp)->type;
}

static int vgpu_cmd_ok(const void *req, uint32_t req_len)
{
    struct virtio_gpu_ctrl_hdr resp;
    int type = vgpu_submit(req, req_len, &resp, sizeof(resp));
    if (type < 0) return type;
    return type == VIRTIO_GPU_RESP_OK_NODATA ? 0 : -type;
}

static struct virtio_gpu_rect vgpu_rect(uint32_t w, uint32_t h)
{
    struct virtio_gpu_rect r;
    r.x = 0;
    r.y = 0;
    r.width = w;
    r.height = h;
    return r;
}

static int vgpu_frame_bytes(int w, int h, uint32_t *bytes_out)
{
    uint64_t bytes;
    if (w <= 0 || h <= 0 || !bytes_out) return -1;
    bytes = (uint64_t)(uint32_t)w * (uint32_t)h * 4u;
    if (bytes == 0 || bytes > 0xffffffffu) return -1;
    *bytes_out = (uint32_t)bytes;
    return 0;
}

static int vgpu_get_display_info(struct virtio_gpu_resp_display_info *info)
{
    struct virtio_gpu_ctrl_hdr req;
    int type;
    if (!info) return -1;

    memset(&req, 0, sizeof(req));
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    type = vgpu_submit(&req, sizeof(req), info, sizeof(*info));
    if (type < 0) return type;
    return type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO ? 0 : -type;
}

static int vgpu_validate_scanout(int w, int h)
{
    struct virtio_gpu_resp_display_info info;
    const struct virtio_gpu_display_one *scanout;
    int rc = vgpu_get_display_info(&info);
    if (rc != 0) return rc;

    scanout = &info.pmodes[0];
    if (!scanout->enabled) return -10;
    if (scanout->r.width == 0 || scanout->r.height == 0) return -11;
    if ((uint32_t)w > scanout->r.width || (uint32_t)h > scanout->r.height) {
        g_scanout_limit_w = scanout->r.width;
        g_scanout_limit_h = scanout->r.height;
        return -12;
    }
    g_scanout_limit_w = scanout->r.width;
    g_scanout_limit_h = scanout->r.height;
    return 0;
}

static int vgpu_ensure_scanout(int w, int h)
{
    uint32_t bytes;
    if (vgpu_frame_bytes(w, h, &bytes) != 0) return -1;
    if (g_scanout && g_scanout_bytes == bytes) {
        return 0;
    }
    free(g_scanout);
    g_scanout = NULL;
    g_scanout_bytes = 0;
    g_scanout = (uint32_t *)malloc(bytes);
    if (!g_scanout) {
        return -1;
    }
    g_scanout_bytes = bytes;
    return 0;
}

static int vgpu_init_resource(int w, int h)
{
    uint32_t bytes;
    int rc;
    if (vgpu_frame_bytes(w, h, &bytes) != 0) return -1;

    struct virtio_gpu_resource_create_2d create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = VGPU_RESOURCE_ID;
    create.format = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
    create.width = (uint32_t)w;
    create.height = (uint32_t)h;
    rc = vgpu_cmd_ok(&create, sizeof(create));
    if (rc != 0) return rc;

    struct vgpu_attach_req attach;
    memset(&attach, 0, sizeof(attach));
    attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.resource_id = VGPU_RESOURCE_ID;
    attach.nr_entries = 1;
    attach.entry.addr = (uint64_t)(uintptr_t)g_scanout;
    attach.entry.length = bytes;
    rc = vgpu_cmd_ok(&attach, sizeof(attach));
    if (rc != 0) return rc;

    struct virtio_gpu_set_scanout scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r = vgpu_rect((uint32_t)w, (uint32_t)h);
    scanout.scanout_id = 0;
    scanout.resource_id = VGPU_RESOURCE_ID;
    rc = vgpu_cmd_ok(&scanout, sizeof(scanout));
    if (rc != 0) return rc;

    return 0;
}

static int vgpu_init(int w, int h)
{
    g_base = vgpu_scan_slots();
    if (!g_base) return -1;

    if (vgpu_ensure_scanout(w, h) != 0) {
        return -2;
    }

    mmio_wr(g_base, VIRTIO_MMIO_STATUS, 0);
    __sync_synchronize();
    mmio_wr(g_base, VIRTIO_MMIO_STATUS, VIRTIO_CONFIG_S_ACKNOWLEDGE);
    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

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
        return -3;
    }

    if (vq_setup(&g_q) != 0) {
        mmio_wr(g_base, VIRTIO_MMIO_STATUS, status | VIRTIO_CONFIG_S_FAILED);
        g_base = 0;
        return -4;
    }

    mmio_wr(g_base, VIRTIO_MMIO_STATUS,
            VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
    __sync_synchronize();

    int scanout_rc = vgpu_validate_scanout(w, h);
    if (scanout_rc != 0) {
        if (scanout_rc == -12) {
            printf("dvgpu: requested %dx%d exceeds scanout0 %ux%u\n",
                   w, h, g_scanout_limit_w, g_scanout_limit_h);
        } else {
            printf("dvgpu: GET_DISPLAY_INFO/scanout0 validation failed rc=%d\n",
                   scanout_rc);
        }
        if (g_base) {
            mmio_wr(g_base, VIRTIO_MMIO_STATUS,
                    mmio_rd(g_base, VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_S_FAILED);
        }
        g_base = 0;
        return -5;
    }

    int resource_rc = vgpu_init_resource(w, h);
    if (resource_rc != 0) {
        printf("dvgpu: resource init failed rc=%d\n", resource_rc);
        if (g_base) {
            mmio_wr(g_base, VIRTIO_MMIO_STATUS,
                    mmio_rd(g_base, VIRTIO_MMIO_STATUS) | VIRTIO_CONFIG_S_FAILED);
        }
        g_base = 0;
        return -6;
    }

    g_ready = 1;
    g_width = w;
    g_height = h;
    printf("dvgpu: scanout %dx%d rgba resource=%u display=%ux%u\n",
           w, h, VGPU_RESOURCE_ID, g_scanout_limit_w, g_scanout_limit_h);
    return 0;
}

int dvgpu_present_rgba(const uint32_t *pixels, int w, int h)
{
    uint32_t bytes;
    if (!pixels || w <= 0 || h <= 0) return -1;
    if (vgpu_frame_bytes(w, h, &bytes) != 0) return -1;
    if (g_disabled) return -2;

    if (!g_ready || g_width != w || g_height != h) {
        g_ready = 0;
        int init_rc = vgpu_init(w, h);
        if (init_rc != 0) {
            if (!g_reported_no_device) {
                printf("dvgpu: no virtio-gpu scanout rc=%d; keeping serial/headless present\n", init_rc);
                g_reported_no_device = 1;
            }
            g_disabled = 1;
            return -2;
        }
    }

#if defined(__riscv_v_intrinsic) && __riscv_v_intrinsic >= 1000000
    /* RVV present blit: fb->color -> GPU scanout, ~1.9 MB/frame at 800x600.
     * Runs on the present (non-IRQ) path. */
    {
        size_t nw = (size_t)bytes / 4u;
        for (size_t i = 0; i < nw; ) {
            size_t vl = __riscv_vsetvl_e32m8(nw - i);
            __riscv_vse32_v_u32m8(&g_scanout[i], __riscv_vle32_v_u32m8(&pixels[i], vl), vl);
            i += vl;
        }
    }
#else
    memcpy(g_scanout, pixels, bytes);
#endif
    __sync_synchronize();

    struct virtio_gpu_transfer_to_host_2d transfer;
    memset(&transfer, 0, sizeof(transfer));
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r = vgpu_rect((uint32_t)w, (uint32_t)h);
    transfer.offset = 0;
    transfer.resource_id = VGPU_RESOURCE_ID;
    int transfer_rc = vgpu_cmd_ok(&transfer, sizeof(transfer));
    if (transfer_rc != 0) {
        printf("dvgpu: TRANSFER_TO_HOST_2D failed rc=%d (%dx%d); disabling virtio-gpu\n",
               transfer_rc, w, h);
        vgpu_disable_device();
        return -3;
    }

    struct virtio_gpu_resource_flush flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r = vgpu_rect((uint32_t)w, (uint32_t)h);
    flush.resource_id = VGPU_RESOURCE_ID;
    int flush_rc = vgpu_cmd_ok(&flush, sizeof(flush));
    if (flush_rc != 0) {
        printf("dvgpu: RESOURCE_FLUSH failed rc=%d (%dx%d); disabling virtio-gpu\n",
               flush_rc, w, h);
        vgpu_disable_device();
        return -4;
    }

    return 0;
}
