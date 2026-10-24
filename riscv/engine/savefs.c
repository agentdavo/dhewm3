/*
 * savefs.c — append-only WHOLE-FILE journal that persists the dos_ramfs working
 * set to the write-region appended after the read-only dosfs pack on the one
 * virtio-blk image. dos_ramfs stays the live read/write tier; savefs only
 * snapshots a file on the close of a writable fd (PUT), tombstones it on unlink
 * (DELETE), and replays the log into ramfs at boot.
 *
 * Crash-safety: append-only, the header sector is the commit point and is written
 * (and flushed) AFTER its payload, so a power loss can only damage the single
 * tail record — the previous committed version of every file is intact. Replay
 * scans seq-order and stops at the first sector that is not a valid record
 * (stop-at-first-bad); a torn tail is simply ignored.
 *
 * No in-place compaction in this MVP: when the region fills, commits fail-closed
 * (the file stays live in RAM for the session). See riscv/docs/fs/13-savefs.md.
 */
#include "engine/savefs.h"
#include "engine/savefs_format.h"
#include "engine/dos_virtio_blk.h"
#include "engine/dos_ramfs.h"
#include "boot/dma.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ state */
static int      g_active;
static int      g_replaying;
static uint64_t g_region_lba;        /* LBA of region sector 0 (the jhdr)        */
static uint32_t g_region_sectors;    /* region length, sectors (incl. the jhdr)  */
static uint64_t g_head_lba;          /* next free LBA (append cursor)            */
static uint64_t g_seq;               /* next record seq (first committed == 1)   */

/* sector scratch — static + aligned so it is a valid identity-mapped DMA target
 * (PA==VA on bare metal), mirroring dosfs.c's g_bounce. */
static uint8_t  g_secbuf[SAVEFS_SECTOR] __attribute__((aligned(64)));  /* scan/read */
static uint8_t  g_hdrbuf[SAVEFS_SECTOR] __attribute__((aligned(64)));  /* hdr build */

/* Cache-line-isolated allocation for buffers the BLOCK DEVICE writes into.
 *
 * These payloads were plain malloc() blocks. This allocator keeps a 16-byte
 * in-band header immediately before the returned pointer, so the first cache
 * line of every DMA target also held allocator metadata -- and on a
 * non-coherent SoC the invalidate that makes the device's data visible discards
 * that header, corrupting the heap. Over-allocate and hand back a DMA_LINE
 * aligned, line-rounded interior block instead, stashing the original pointer
 * just below it (outside the DMA region, in the preceding line). */
static void *savefs_dma_alloc(size_t n)
{
    size_t total = DMA_ROUND(n) + DMA_LINE * 2u;
    void *raw = malloc(total);
    uintptr_t p;
    if (!raw) return 0;
    p = ((uintptr_t)raw + DMA_LINE * 2u - 1u) & ~(uintptr_t)(DMA_LINE - 1u);
    ((void **)p)[-1] = raw;          /* lives in the line BEFORE the buffer */
    return (void *)p;
}

static void savefs_dma_free(void *p)
{
    if (p) free(((void **)p)[-1]);
}

/* ------------------------------------------------------------------ crc32 *
 * Standard zlib/IEEE CRC-32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF, reflected)
 * — byte-identical to Python's binascii.crc32 used by mkdosfs.py/dumpsavefs.py. */
static uint32_t g_crc_table[256];
static int      g_crc_ready;

static void crc32_init(void)
{
    uint32_t i, k, c;
    if (g_crc_ready) return;
    for (i = 0; i < 256; ++i) {
        c = i;
        for (k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc32_buf(const void *data, uint64_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    uint64_t i;
    for (i = 0; i < len; ++i)
        c = g_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ mount */
int savefs_mount(const uint8_t *super_sector)
{
    savefs_region_desc desc;
    savefs_jhdr jhdr;
    uint32_t want, got;

    g_active = 0;
    g_replaying = 0;
    g_head_lba = 0;
    g_seq = 1;
    crc32_init();

    if (!super_sector) return -1;
    memcpy(&desc, super_sector + 48, sizeof desc);
    if (desc.region_magic != SAVEFS_REGION_MAGIC) return -1;   /* legacy image */
    if (desc.version       != SAVEFS_VERSION)      return -2;
    if (desc.region_sectors < 2)                   return -3;

    g_region_lba     = desc.region_lba;
    g_region_sectors = desc.region_sectors;

    /* validate the journal header at region sector 0 */
    if (vblk_read(g_region_lba, g_secbuf, 1) != 0) return -4;
    memcpy(&jhdr, g_secbuf, sizeof jhdr);
    if (jhdr.magic       != SAVEFS_JHDR_MAGIC) return -5;
    if (jhdr.version     != SAVEFS_VERSION)    return -6;
    if (jhdr.sector_size != SAVEFS_SECTOR)     return -7;
    want = jhdr.hdr_crc;
    memset(g_secbuf + 12, 0, 4);               /* hdr_crc lives at offset 12 */
    got = crc32_buf(g_secbuf, SAVEFS_SECTOR);
    if (got != want) return -8;

    g_head_lba = g_region_lba + 1;             /* provisional; replay finds the head */
    g_seq      = 1;
    g_active   = 1;
    return 0;
}

int savefs_active(void) { return g_active; }

uint64_t savefs_free_bytes(void)
{
    uint64_t end;
    if (!g_active) return 0;
    end = g_region_lba + g_region_sectors;
    if (g_head_lba >= end) return 0;
    return (end - g_head_lba) * SAVEFS_SECTOR;
}

uint64_t savefs_scratch_lba(void)
{
    return g_active ? (g_region_lba + g_region_sectors - 1) : 0;
}

/* ------------------------------------------------------------------ replay */
int savefs_replay(void)
{
    uint64_t end = g_region_lba + g_region_sectors;
    uint64_t lba = g_region_lba + 1;
    uint64_t seq = 1;
    int applied = 0;
    int ramfs_full = 0;

    if (!g_active) return 0;
    g_replaying = 1;

    for (;;) {
        savefs_rec rec;
        uint32_t want, got;

        if (lba >= end) break;
        if (vblk_read(lba, g_secbuf, 1) != 0) break;
        memcpy(&rec, g_secbuf, sizeof rec);
        if (rec.magic != SAVEFS_REC_MAGIC) break;          /* end-of-log */
        want = rec.hdr_crc;
        memset(g_secbuf + 4, 0, 4);                         /* hdr_crc at offset 4 */
        got = crc32_buf(g_secbuf, SAVEFS_SECTOR);
        if (got != want)                 break;             /* torn header */
        if (rec.seq != seq)              break;             /* sequence gap */
        if (rec.path_len >= SAVEFS_PATH_MAX) break;
        rec.path[SAVEFS_PATH_MAX - 1] = 0;

        if (rec.op == SAVEFS_OP_PUT) {
            uint64_t dsec = ((uint64_t)rec.data_len + SAVEFS_SECTOR - 1) / SAVEFS_SECTOR;
            uint8_t *payload = 0;
            if (lba + 1 + dsec > end) break;                /* truncated tail */
            if (rec.data_len > 0) {
                payload = (uint8_t *)savefs_dma_alloc((size_t)(dsec * SAVEFS_SECTOR));
                if (!payload) break;                        /* OOM: stop cleanly */
                if (vblk_read(lba + 1, payload, (uint32_t)dsec) != 0) { savefs_dma_free(payload); break; }
                got = crc32_buf(payload, rec.data_len);
                if (got != rec.data_crc) { savefs_dma_free(payload); break; }   /* torn payload */
            }
            {
                dos_ramfs_file rf;
                if (dos_ramfs_open(rec.path, O_CREAT | O_TRUNC | O_WRONLY, &rf) == 0) {
                    if (rec.data_len > 0)
                        (void)dos_ramfs_write(&rf, payload, rec.data_len);
                    dos_ramfs_close(&rf);
                    applied++;
                } else {
                    ramfs_full = 1;                         /* cap hit; keep scanning */
                }
            }
            if (payload) savefs_dma_free(payload);
            lba += 1 + dsec;
            seq++;
        } else if (rec.op == SAVEFS_OP_DELETE) {
            (void)dos_ramfs_unlink(rec.path);
            applied++;
            lba += 1;
            seq++;
        } else {
            break;                                          /* unknown op */
        }
    }

    g_head_lba = lba;
    g_seq      = seq;
    g_replaying = 0;
    if (ramfs_full)
        printf("savefs: WARNING ramfs full during replay; some files not restored\n");
    return applied;
}

/* ------------------------------------------------------------------ commit */
int savefs_commit_file(const char *path)
{
    dos_ramfs_file rf;
    uint64_t size = 0, dsec, need, end;
    uint32_t mt;
    uint8_t *payload = 0;
    savefs_rec *rec;
    size_t plen;

    if (!g_active || g_replaying || !path) return -1;
    if (dos_ramfs_open(path, O_RDONLY, &rf) != 0) return -1;
    if (dos_ramfs_fstat(&rf, &size, &mt) != 0) { dos_ramfs_close(&rf); return -1; }

    dsec = (size + SAVEFS_SECTOR - 1) / SAVEFS_SECTOR;
    need = 1 + dsec;                                        /* header + payload */
    end  = g_region_lba + g_region_sectors;
    if (g_head_lba + need > end) { dos_ramfs_close(&rf); return -1; }   /* full: fail-closed */

    if (size > 0) {
        payload = (uint8_t *)savefs_dma_alloc((size_t)(dsec * SAVEFS_SECTOR));
        if (!payload) { dos_ramfs_close(&rf); return -1; }
        memset(payload, 0, (size_t)(dsec * SAVEFS_SECTOR));
        if (dos_ramfs_read(&rf, payload, size) != (int64_t)size) {
            savefs_dma_free(payload); dos_ramfs_close(&rf); return -1;
        }
    }
    dos_ramfs_close(&rf);

    /* build the record header (a full 512B sector) */
    plen = strlen(path);
    if (plen >= SAVEFS_PATH_MAX) plen = SAVEFS_PATH_MAX - 1;
    memset(g_hdrbuf, 0, SAVEFS_SECTOR);
    rec = (savefs_rec *)g_hdrbuf;
    rec->magic    = SAVEFS_REC_MAGIC;
    rec->hdr_crc  = 0;
    rec->seq      = g_seq;
    rec->op       = SAVEFS_OP_PUT;
    rec->data_len = (uint32_t)size;
    rec->data_crc = (size > 0) ? crc32_buf(payload, size) : 0;
    rec->path_len = (uint32_t)plen;
    memcpy(rec->path, path, plen);
    rec->path[plen] = 0;
    rec->hdr_crc  = crc32_buf(g_hdrbuf, SAVEFS_SECTOR);

    /* commit ordering: payload -> FLUSH -> header -> FLUSH (header is the barrier) */
    if (size > 0) {
        if (vblk_write_blocks(g_head_lba + 1, (uint32_t)dsec, payload) != 0) { savefs_dma_free(payload); return -1; }
        if (vblk_flush() < 0) { savefs_dma_free(payload); return -1; }
    }
    if (payload) savefs_dma_free(payload);
    if (vblk_write_blocks(g_head_lba, 1, g_hdrbuf) != 0) return -1;
    if (vblk_flush() < 0) return -1;

    g_head_lba += need;
    g_seq++;
    return 0;
}

int savefs_commit_delete(const char *path)
{
    uint64_t end;
    savefs_rec *rec;
    size_t plen;

    if (!g_active || g_replaying || !path) return -1;
    end = g_region_lba + g_region_sectors;
    if (g_head_lba + 1 > end) return -1;                   /* full: fail-closed */

    plen = strlen(path);
    if (plen >= SAVEFS_PATH_MAX) plen = SAVEFS_PATH_MAX - 1;
    memset(g_hdrbuf, 0, SAVEFS_SECTOR);
    rec = (savefs_rec *)g_hdrbuf;
    rec->magic    = SAVEFS_REC_MAGIC;
    rec->hdr_crc  = 0;
    rec->seq      = g_seq;
    rec->op       = SAVEFS_OP_DELETE;
    rec->data_len = 0;
    rec->data_crc = 0;
    rec->path_len = (uint32_t)plen;
    memcpy(rec->path, path, plen);
    rec->path[plen] = 0;
    rec->hdr_crc  = crc32_buf(g_hdrbuf, SAVEFS_SECTOR);

    if (vblk_write_blocks(g_head_lba, 1, g_hdrbuf) != 0) return -1;
    if (vblk_flush() < 0) return -1;

    g_head_lba += 1;
    g_seq++;
    return 0;
}
