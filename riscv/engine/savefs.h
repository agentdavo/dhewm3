/*
 * savefs.h — persistence layer that sits UNDER dos_ramfs. It mirrors the live
 * ramfs working set into an append-only whole-file journal on the write-region of
 * the one virtio-blk image, and replays it at boot. The engine keeps talking to
 * dos_ramfs through dos_syscalls.c; savefs is wired only at mount and on the
 * close of a writable fd / unlink. See riscv/docs/fs/13-savefs.md.
 */
#ifndef SAVEFS_H
#define SAVEFS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the dosfs superblock's savefs_region_desc + the write-region jhdr, validate
 * magic/version/crc, and provisionally place the append head. Returns 0 if a
 * usable journal is mounted, <0 if absent/invalid (caller then runs in today's
 * volatile-ramfs mode). Idempotent. `super_sector` is the already-read 512-byte
 * dosfs superblock (sector 0); savefs parses reserved[]. */
int  savefs_mount(const uint8_t *super_sector);

/* True once savefs_mount has succeeded and the journal is writable. */
int  savefs_active(void);

/* Replay every valid record (seq order) into dos_ramfs BEFORE common->Init(),
 * re-creating the prior config/saves, and set the true append head. Returns the
 * number of records applied. Self-suppresses re-journalling. */
int  savefs_replay(void);

/* Commit one file's CURRENT full bytes (snapshotted from dos_ramfs) as a PUT
 * record + flush. Called from _close for a writable, dirty fd. Returns 0 on
 * success, <0 on ENOSPC/region-full or I/O error (file stays live in RAM). */
int  savefs_commit_file(const char *path);

/* Append a DELETE tombstone + flush. Called from _unlink. 0 / <0. */
int  savefs_commit_delete(const char *path);

/* Remaining write-region free space in bytes (region end - append head). Feeds
 * Sys_GetDriveFreeSpace so SaveGame's >=25 MiB gate refuses gracefully near full. */
uint64_t savefs_free_bytes(void);

/* Test-only: a scratch sector inside the region that the append log never reaches
 * in the MVP (the last region sector), for the P0 driver write/flush self-test.
 * Returns 0 when no journal is mounted. */
uint64_t savefs_scratch_lba(void);

#ifdef __cplusplus
}
#endif
#endif /* SAVEFS_H */
