/*
 * dos_ramfs.h - small writable RAM filesystem for dOS engine config/save paths.
 *
 * This is a runtime overlay, not persistent storage. It gives the engine a real
 * writable fs_savepath/fs_configpath contract while /base remains backed by the
 * read-only dosfs asset image.
 */
#ifndef DOS_RAMFS_H
#define DOS_RAMFS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dos_ramfs_file {
    int      index;
    uint64_t pos;
    int      append;
    int      readable;
    int      writable;
    int      used;
} dos_ramfs_file;

typedef void (*dos_ramfs_list_cb)(const char *basename, int is_dir, void *user);

int     dos_ramfs_mkdir(const char *path);
int     dos_ramfs_open(const char *path, int flags, dos_ramfs_file *out);
int64_t dos_ramfs_read(dos_ramfs_file *f, void *dst, uint64_t len);
int64_t dos_ramfs_write(dos_ramfs_file *f, const void *src, uint64_t len);
int64_t dos_ramfs_seek(dos_ramfs_file *f, int64_t off, int whence);
int     dos_ramfs_close(dos_ramfs_file *f);
int     dos_ramfs_fstat(const dos_ramfs_file *f, uint64_t *size, uint32_t *mtime);
int     dos_ramfs_unlink(const char *path);
int     dos_ramfs_rename(const char *old_path, const char *new_path);
int     dos_ramfs_stat(const char *path, uint64_t *size, int *is_dir, uint32_t *mtime);
int     dos_ramfs_list(const char *dir, const char *ext, dos_ramfs_list_cb cb, void *user);
uint32_t dos_ramfs_epoch(void);

/* The normalized key of an open file (g_files[f->index].path), or NULL if the fd
 * is not a live file. Lets savefs map a closing fd back to its path without
 * exposing the node table. */
const char *dos_ramfs_path(const dos_ramfs_file *f);

#ifdef __cplusplus
}
#endif

#endif /* DOS_RAMFS_H */
