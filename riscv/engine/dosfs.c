/*
 * dosfs.c — flat read-only "dosfs" reader.
 *
 * Parses the superblock + directory (doc 12) into static RAM, then resolves
 * paths, reads byte ranges, and lists directories. The block layer below is a
 * sector read callback (vblk_read); this layer owns the sub-sector head/tail
 * bounce so the engine's _read / minizip see arbitrary byte ranges.
 *
 * See riscv/docs/fs/12-dosfs.md.
 */
#include "engine/dosfs.h"
#include "engine/dosfs_format.h"
#include <string.h>

#define DOSFS_MAX_ENTRIES 256u            /* 256 * 128B = 32 KiB BSS directory */
#define DOSFS_DIR_SECTORS \
    ((DOSFS_MAX_ENTRIES * sizeof(dosfs_dirent) + DOSFS_SECTOR - 1) / DOSFS_SECTOR)

static dosfs_read_blocks_fn g_rb;
static int                  g_mounted;
static uint32_t             g_count;
static uint32_t             g_epoch;

static dosfs_super  g_super __attribute__((aligned(64)));
static dosfs_dirent g_dir[DOSFS_MAX_ENTRIES] __attribute__((aligned(64)));

/* one-sector scratch for sub-sector head/tail copies (depth-1 synchronous). */
static uint8_t g_bounce[DOSFS_SECTOR] __attribute__((aligned(64)));
static dosfs_stats g_stats;

/* ------------------------------------------------------------------ *
 *  Block (sector) cache.                                              *
 *  The disk is read-only to dosfs, so cached sectors never go stale.  *
 *  Direct-mapped by LBA; serves the sub-sector head/tail bounce reads *
 *  and any re-read region from RAM instead of a virtio round-trip     *
 *  (each of which is ~0.5 ms depth-1-synchronous). Cold misses are    *
 *  coalesced into one large g_rb so big sequential reads stay batched.*/
#define DOSFS_CACHE_SECTORS  32768u    /* 32768 * 512 = 16 MiB */
#define DOSFS_MAX_CHUNK_SECTORS 1024u  /* max sectors per virtio request (was 128) */
#define DOSFS_READAHEAD      128u      /* prefetch window (64 KiB) on a cold miss */
#define DOSFS_CACHE_INVALID  (~(uint64_t)0)

static uint8_t  g_cache_data[DOSFS_CACHE_SECTORS][DOSFS_SECTOR] __attribute__((aligned(64)));
static uint64_t g_cache_tag[DOSFS_CACHE_SECTORS];
static int      g_cache_ready;

static void cache_init(void)
{
    uint32_t i;
    for (i = 0; i < DOSFS_CACHE_SECTORS; ++i) g_cache_tag[i] = DOSFS_CACHE_INVALID;
    g_cache_ready = 1;
}

/* Read-ahead scratch (separate from the caller's buf, which is only `count`). */
static uint8_t g_ra_buf[DOSFS_READAHEAD][DOSFS_SECTOR] __attribute__((aligned(64)));

/* Prefetch the window [lba, lba+count) into the cache (not into any caller buf).
 * Reads only the contiguous *uncached* runs, in big g_rb calls. A g_rb failure
 * (e.g. past the disk end) is benign -- prefetch is best-effort, just stop. The
 * point: a pk4 file is read sequentially in ~2-sector minizip chunks, each a
 * cold miss = a tiny virtio request; pulling a 64 KiB window on the first miss
 * turns the next ~64 of those into cache hits, collapsing the request count. */
static void cache_prefetch(uint64_t lba, uint32_t count)
{
    uint32_t i = 0;
    while (i < count) {
        uint64_t s = lba + i;
        if (g_cache_tag[(uint32_t)(s % DOSFS_CACHE_SECTORS)] == s) { i++; continue; }
        uint32_t j = i;
        while (j < count && (j - i) < DOSFS_READAHEAD) {
            uint64_t sj = lba + j;
            if (g_cache_tag[(uint32_t)(sj % DOSFS_CACHE_SECTORS)] == sj) break;
            j++;
        }
        uint32_t run = j - i;
        if (g_rb(s, g_ra_buf[0], run) != 0) return;      /* past end / error: stop */
        g_stats.cache_misses += run;
        for (uint32_t k = 0; k < run; ++k) {
            uint64_t sk    = s + k;
            uint32_t slotk = (uint32_t)(sk % DOSFS_CACHE_SECTORS);
            memcpy(g_cache_data[slotk], g_ra_buf[k], DOSFS_SECTOR);
            g_cache_tag[slotk] = sk;
        }
        i = j;
    }
}

/* Read `count` sectors at `lba` into `buf` through the block cache. Every
 * sector touched is cached; contiguous misses are read in one g_rb call. On a
 * cold (missing) read the next DOSFS_READAHEAD sectors are prefetched, so the
 * sequential pk4 access pattern collapses from many tiny reads to few big ones. */
static int cached_read(uint64_t lba, void *buf, uint32_t count)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t i = 0;
    int had_miss = 0;

    if (!g_cache_ready) cache_init();

    while (i < count) {
        uint64_t s    = lba + i;
        uint32_t slot = (uint32_t)(s % DOSFS_CACHE_SECTORS);
        if (g_cache_tag[slot] == s) {                    /* hit */
            memcpy(out + (uint64_t)i * DOSFS_SECTOR, g_cache_data[slot], DOSFS_SECTOR);
            g_stats.cache_hits++;
            i++;
            continue;
        }
        /* miss: gather the maximal contiguous run of misses and read it once */
        had_miss = 1;
        uint32_t j = i;
        while (j < count && (j - i) < DOSFS_MAX_CHUNK_SECTORS) {
            uint64_t sj    = lba + j;
            uint32_t slotj = (uint32_t)(sj % DOSFS_CACHE_SECTORS);
            if (g_cache_tag[slotj] == sj) break;         /* stop the run at a hit */
            j++;
        }
        uint32_t run = j - i;
        if (g_rb(s, out + (uint64_t)i * DOSFS_SECTOR, run) != 0) return -1;
        g_stats.cache_misses += run;
        for (uint32_t k = 0; k < run; ++k) {             /* populate the cache */
            uint64_t sk    = s + k;
            uint32_t slotk = (uint32_t)(sk % DOSFS_CACHE_SECTORS);
            memcpy(g_cache_data[slotk], out + (uint64_t)(i + k) * DOSFS_SECTOR, DOSFS_SECTOR);
            g_cache_tag[slotk] = sk;
        }
        i = j;
    }
    /* Sequential cold read -> pull the next window so the following minizip
     * reads hit the cache instead of issuing more tiny virtio requests. */
    if (had_miss) cache_prefetch(lba + count, DOSFS_READAHEAD);
    return 0;
}

void dosfs_stats_snapshot(dosfs_stats *out)
{
    if (out) *out = g_stats;
}

void dosfs_stats_reset(void)
{
    memset(&g_stats, 0, sizeof g_stats);
}

/* ------------------------------------------------------------------ */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void path_lower_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (src && src[0] == '/') src++;          /* strip leading slash */
    for (; src && *src && i + 1 < cap; ++src, ++i) dst[i] = lc(*src);
    dst[i] = 0;
}

/* ------------------------------------------------------------------ */
int dosfs_init(dosfs_read_blocks_fn rb)
{
    uint32_t span, want;

    g_mounted = 0;
    if (!rb) return -1;
    g_rb = rb;

    if (g_rb(0, &g_super, 1) != 0) return -2;            /* read superblock */
    if (g_super.magic       != DOSFS_MAGIC)   return -3;
    if (g_super.version     != DOSFS_VERSION) return -4;
    if (g_super.sector_size != DOSFS_SECTOR)  return -5;
    if (g_super.entry_count > DOSFS_MAX_ENTRIES) return -6;
    if (g_super.dir_off != DOSFS_SECTOR)      return -7; /* directory @ sector 1 */

    g_count = g_super.entry_count;
    g_epoch = g_super.build_epoch;

    want = (uint32_t)((g_count * sizeof(dosfs_dirent) + DOSFS_SECTOR - 1) / DOSFS_SECTOR);
    span = (uint32_t)DOSFS_DIR_SECTORS;
    if (want > span) return -8;
    if (want > 0) {
        if (g_rb(g_super.dir_off / DOSFS_SECTOR, g_dir, want) != 0) return -9;
    }

    g_mounted = 1;
    return 0;
}

int      dosfs_mounted(void)     { return g_mounted; }
int      dosfs_entry_count(void) { return (int)g_count; }
uint32_t dosfs_build_epoch(void) { return g_epoch; }

/* ------------------------------------------------------------------ */
static const dosfs_dirent *find_path(const char *path)
{
    char key[DOSFS_PATH_MAX];
    uint32_t i;
    path_lower_copy(key, path, sizeof key);
    for (i = 0; i < g_count; ++i) {
        if (strcmp(g_dir[i].path, key) == 0) return &g_dir[i];
    }
    return 0;
}

int dosfs_open(const char *path, dosfs_file *out)
{
    const dosfs_dirent *e;
    if (!g_mounted || !path || !out) return -1;
    e = find_path(path);
    if (!e) return -2;
    out->base_off = e->offset;
    out->size     = e->size;
    out->pos      = 0;
    out->used     = 1;
    return 0;
}

int64_t dosfs_size(const char *path)
{
    const dosfs_dirent *e;
    if (!g_mounted) return -1;
    e = find_path(path);
    return e ? (int64_t)e->size : -1;
}

/* ------------------------------------------------------------------ *
 *  pread — translate (base_off+pos) to whole-sector reads, bounce the *
 *  partial head and tail sectors through g_bounce.                     *
 * ------------------------------------------------------------------ */
int64_t dosfs_pread(const dosfs_file *f, uint64_t pos, void *dst, uint64_t len)
{
    uint8_t *out = (uint8_t *)dst;
    uint64_t abs, remaining;
    uint64_t copied = 0;
    uint64_t requested = len;

    if (!g_mounted || !f || !f->used) return -1;
    if (pos >= f->size) return 0;                       /* EOF */
    if (len > f->size - pos) len = f->size - pos;       /* clamp */
    if (len == 0) return 0;
    g_stats.pread_calls++;
    g_stats.requested_bytes += requested;

    abs       = f->base_off + pos;                      /* absolute image byte */
    remaining = len;

    /* --- head: partial first sector --- */
    {
        uint64_t head = abs % DOSFS_SECTOR;
        if (head != 0) {
            uint64_t lba   = abs / DOSFS_SECTOR;
            uint64_t avail = DOSFS_SECTOR - head;
            uint64_t n     = (remaining < avail) ? remaining : avail;
            if (cached_read(lba, g_bounce, 1) != 0) return -2;
            g_stats.head_bounce_reads++;
            memcpy(out, g_bounce + head, (size_t)n);
            out += n; abs += n; remaining -= n; copied += n;
        }
    }

    /* --- middle: whole aligned sectors, read straight into dst --- */
    if (remaining >= DOSFS_SECTOR) {
        uint64_t whole = remaining / DOSFS_SECTOR;
        uint64_t lba   = abs / DOSFS_SECTOR;            /* abs is sector-aligned here */
        /* Chunk to DOSFS_MAX_CHUNK_SECTORS per virtio request; the block cache
         * coalesces cold runs and serves repeats/bounces from RAM. */
        while (whole > 0) {
            uint32_t chunk = (whole > DOSFS_MAX_CHUNK_SECTORS) ? DOSFS_MAX_CHUNK_SECTORS : (uint32_t)whole;
            if (cached_read(lba, out, chunk) != 0) return -2;
            g_stats.direct_read_calls++;
            g_stats.direct_sectors += chunk;
            out += (uint64_t)chunk * DOSFS_SECTOR;
            abs += (uint64_t)chunk * DOSFS_SECTOR;
            remaining -= (uint64_t)chunk * DOSFS_SECTOR;
            copied    += (uint64_t)chunk * DOSFS_SECTOR;
            lba       += chunk;
            whole     -= chunk;
        }
    }

    /* --- tail: partial last sector --- */
    if (remaining > 0) {
        uint64_t lba = abs / DOSFS_SECTOR;              /* abs sector-aligned here */
        if (cached_read(lba, g_bounce, 1) != 0) return -2;
        g_stats.tail_bounce_reads++;
        memcpy(out, g_bounce, (size_t)remaining);
        copied += remaining;
    }

    g_stats.served_bytes += copied;
    return (int64_t)copied;
}

int64_t dosfs_read(dosfs_file *f, void *dst, uint64_t len)
{
    int64_t n;
    if (!f || !f->used) return -1;
    n = dosfs_pread(f, f->pos, dst, len);
    if (n > 0) f->pos += (uint64_t)n;
    return n;
}

int64_t dosfs_seek(dosfs_file *f, int64_t off, int whence)
{
    int64_t base;
    if (!f || !f->used) return -1;
    switch (whence) {
        case 0: base = 0;                  break;   /* SEEK_SET */
        case 1: base = (int64_t)f->pos;    break;   /* SEEK_CUR */
        case 2: base = (int64_t)f->size;   break;   /* SEEK_END */
        default: return -1;
    }
    base += off;
    if (base < 0) base = 0;
    if (base > (int64_t)f->size) base = (int64_t)f->size;
    f->pos = (uint64_t)base;
    return base;
}

/* ------------------------------------------------------------------ *
 *  directory listing (drives Sys_ListFiles)                           *
 * ------------------------------------------------------------------ */
static int icmp_ext(const char *name, const char *ext)
{
    /* ext is e.g. "pk4" (no dot); match the part of name after its last '.' */
    const char *dot = 0, *p;
    size_t i;
    for (p = name; *p; ++p) if (*p == '.') dot = p;
    if (!dot) return 0;
    dot++;                                          /* skip the '.' */
    for (i = 0; ext[i] && dot[i]; ++i)
        if (lc(dot[i]) != lc(ext[i])) return 0;
    return ext[i] == 0 && dot[i] == 0;
}

int dosfs_list(const char *dir, const char *ext, dosfs_list_cb cb, void *user)
{
    char base[DOSFS_PATH_MAX];
    char prev_subdir[DOSFS_PATH_MAX];
    size_t blen;
    uint32_t i;
    int matches = 0;
    int dir_exists = 0;
    int dironly = 0;
    int allfiles = 0;

    if (!g_mounted) return -1;

    /* normalise dir: strip leading '/', lowercase */
    path_lower_copy(base, dir ? dir : "", sizeof base);
    /* strip a trailing '/' if present */
    blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') base[--blen] = 0;

    if (!ext)            allfiles = 1;
    else if (ext[0] == 0) allfiles = 1;
    else if (ext[0] == '/' && ext[1] == 0) dironly = 1;
    else if (ext[0] == '.') ext++;                  /* engine passes ".pk4"; want "pk4" */

    prev_subdir[0] = 0;

    /* root ("") always exists; "base" exists if any entry has that prefix */
    if (blen == 0) dir_exists = 1;

    for (i = 0; i < g_count; ++i) {
        const char *p = g_dir[i].path;
        const char *rest;

        if (blen == 0) {
            rest = p;                               /* whole path is relative to root */
        } else {
            if (strncmp(p, base, blen) != 0)  continue;
            if (p[blen] != '/')               continue;
            rest = p + blen + 1;
            dir_exists = 1;
        }
        if (!*rest) continue;

        /* find first '/' in rest -> subdirectory child, else direct file */
        {
            const char *slash = strchr(rest, '/');
            if (slash) {
                /* subdirectory child of `dir`; emit (deduped) if listing dirs */
                size_t sl = (size_t)(slash - rest);
                if (sl + 1 >= sizeof prev_subdir) continue;
                /* dedupe consecutive same subdir (dir is sorted ascending) */
                if (strncmp(prev_subdir, rest, sl) == 0 && prev_subdir[sl] == 0)
                    continue;
                memcpy(prev_subdir, rest, sl);
                prev_subdir[sl] = 0;
                dir_exists = 1;
                if (dironly || allfiles) {
                    /* allfiles excludes dirs in posix semantics; only dironly emits */
                    if (dironly) { cb(prev_subdir, 1, user); matches++; }
                }
            } else {
                /* direct file child */
                if (dironly) continue;
                if (allfiles || icmp_ext(rest, ext)) {
                    cb(rest, 0, user);
                    matches++;
                }
            }
        }
    }

    if (!dir_exists) return -1;
    return matches;
}
