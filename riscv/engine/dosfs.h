/*
 * dosfs.h — flat read-only "dosfs" reader API (Phase 6).
 *
 * Sits between the virtio-blk driver (a sector read callback) below and the
 * newlib syscall bottom-end + Sys_* hooks above. Pure C, freestanding, no malloc
 * on the hot path: the superblock + directory are pinned in .bss after init, and
 * an open file is just a cursor over a contiguous extent.
 *
 * See riscv/docs/fs/12-dosfs.md.
 */
#ifndef DOSFS_H
#define DOSFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read `count` 512-byte sectors starting at LBA `sector` into `buf`
 * (count*512 bytes). Returns 0 on success, <0 on error. Supplied by the
 * virtio-blk driver (&vblk_read). */
typedef int (*dosfs_read_blocks_fn)(uint64_t sector, void *buf, uint32_t count);

/* An open file: a cursor over an extent — no allocation. */
typedef struct dosfs_file {
    uint64_t base_off;   /* dirent.offset (absolute, sector-aligned) */
    uint64_t size;       /* dirent.size (exact bytes)                */
    uint64_t pos;        /* current read cursor in [0, size]         */
    int      used;       /* slot in use                              */
} dosfs_file;

/* Read superblock (sector 0) + directory into static RAM and validate magic.
 * Returns 0 ok, <0 on bad magic/version/sector_size/too-many-entries/read err.
 * Call once at boot before fileSystem->Init(). */
int  dosfs_init(dosfs_read_blocks_fn rb);

/* True after a successful dosfs_init. */
int  dosfs_mounted(void);

/* Number of directory entries (files) in the mounted image. */
int  dosfs_entry_count(void);

/* Look up an image-relative path (any case; a scratch copy is lowercased).
 * On success fills *out and returns 0; returns <0 if not found. No I/O. */
int  dosfs_open(const char *path, dosfs_file *out);

/* Random read: copies up to `len` bytes from `pos` into `dst`, clamped to file
 * size. Returns bytes copied (0 at EOF), <0 on device error. Does NOT advance
 * f->pos. Handles sub-sector head/tail via an internal bounce buffer. */
int64_t dosfs_pread(const dosfs_file *f, uint64_t pos, void *dst, uint64_t len);

/* Read at f->pos and advance it. */
int64_t dosfs_read(dosfs_file *f, void *dst, uint64_t len);

/* Cursor seek (whence: 0=SET,1=CUR,2=END), clamped to [0,size]; returns new
 * pos or <0 on bad whence. */
int64_t dosfs_seek(dosfs_file *f, int64_t off, int whence);

/* File size by path without opening. Returns size, or <0 if not found. */
int64_t dosfs_size(const char *path);

/* Directory enumeration for Sys_ListFiles. For each direct child of `dir`
 * (image-relative, no trailing slash, "" or "base"), invoke cb(basename,is_dir).
 * `ext` filters files: NULL/"" = all files, "/" = directories only,
 * ".pk4" = that extension only (case-insensitive). Returns the number of
 * matches, or -1 if `dir` itself does not exist (mirrors opendir failing). */
typedef void (*dosfs_list_cb)(const char *basename, int is_dir, void *user);
int  dosfs_list(const char *dir, const char *ext, dosfs_list_cb cb, void *user);

/* The single image timestamp (superblock build_epoch). */
uint32_t dosfs_build_epoch(void);

typedef struct dosfs_stats {
	uint64_t pread_calls;
	uint64_t requested_bytes;
	uint64_t served_bytes;
	uint64_t head_bounce_reads;
	uint64_t tail_bounce_reads;
	uint64_t direct_read_calls;
	uint64_t direct_sectors;
	uint64_t cache_hits;      /* sectors served from the block cache */
	uint64_t cache_misses;    /* sectors fetched from the device into the cache */
} dosfs_stats;

void dosfs_stats_snapshot(dosfs_stats *out);
void dosfs_stats_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DOSFS_H */
