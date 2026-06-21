/*
 * dos_ramfs.c - writable in-memory filesystem for dOS save/config paths.
 */
#include "engine/dos_ramfs.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define RAMFS_MAX_FILES 128
#define RAMFS_MAX_DIRS  128
#define RAMFS_PATH_MAX  160

typedef struct {
    char     path[RAMFS_PATH_MAX];
    uint8_t *data;
    uint64_t size;
    uint64_t cap;
    uint32_t mtime;
    int      used;
} ramfs_node;

static ramfs_node g_files[RAMFS_MAX_FILES];
static char       g_dirs[RAMFS_MAX_DIRS][RAMFS_PATH_MAX];
static int        g_dir_used[RAMFS_MAX_DIRS];
static uint32_t   g_epoch = 2;

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int norm_path(const char *src, char *dst, size_t cap)
{
    size_t i = 0;
    if (!src || cap == 0) return -1;
    while (*src == '/') src++;
    while (*src && i + 1 < cap) {
        char c = *src++;
        if (c == '\\') c = '/';
        while (c == '/' && *src == '/') src++;
        dst[i++] = lc(c);
    }
    dst[i] = 0;
    while (i > 0 && dst[i - 1] == '/') dst[--i] = 0;
    if (*src) return -1;
    return 0;
}

static int has_prefix_dir(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 0) return path[0] != 0;
    return strncmp(path, dir, n) == 0 && path[n] == '/' && path[n + 1] != 0;
}

static int find_file(const char *path)
{
    int i;
    for (i = 0; i < RAMFS_MAX_FILES; ++i) {
        if (g_files[i].used && strcmp(g_files[i].path, path) == 0) return i;
    }
    return -1;
}

static int find_dir(const char *path)
{
    int i;
    if (!path[0]) return 0;
    for (i = 0; i < RAMFS_MAX_DIRS; ++i) {
        if (g_dir_used[i] && strcmp(g_dirs[i], path) == 0) return i;
    }
    return -1;
}

static int alloc_file(void)
{
    int i;
    for (i = 0; i < RAMFS_MAX_FILES; ++i) {
        if (!g_files[i].used) return i;
    }
    return -1;
}

static int ensure_dir_norm(const char *path)
{
    int i;
    if (!path[0] || find_dir(path) >= 0) return 0;
    for (i = 0; i < RAMFS_MAX_DIRS; ++i) {
        if (!g_dir_used[i]) {
            strcpy(g_dirs[i], path);
            g_dir_used[i] = 1;
            return 0;
        }
    }
    return -1;
}

static void ensure_parent_dirs(const char *path)
{
    char tmp[RAMFS_PATH_MAX];
    char *p;
    strncpy(tmp, path, sizeof tmp);
    tmp[sizeof(tmp) - 1] = 0;
    for (p = tmp; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            (void)ensure_dir_norm(tmp);
            *p = '/';
        }
    }
}

static int ensure_capacity(ramfs_node *n, uint64_t want)
{
    uint64_t cap;
    uint8_t *p;
    if (want <= n->cap) return 0;
    cap = n->cap ? n->cap : 256;
    while (cap < want) cap *= 2;
    p = (uint8_t *)realloc(n->data, (size_t)cap);
    if (!p) return -1;
    n->data = p;
    n->cap = cap;
    return 0;
}

static int ext_match(const char *name, const char *ext)
{
    const char *dot = 0, *p;
    size_t i;
    if (!ext || !ext[0]) return 1;
    if (ext[0] == '.') ext++;
    for (p = name; *p; ++p) if (*p == '.') dot = p;
    if (!dot) return 0;
    dot++;
    for (i = 0; ext[i] && dot[i]; ++i) {
        if (lc(ext[i]) != lc(dot[i])) return 0;
    }
    return ext[i] == 0 && dot[i] == 0;
}

static int emit_once(char seen[][RAMFS_PATH_MAX], int *nseen, const char *name,
                     int is_dir, dos_ramfs_list_cb cb, void *user)
{
    int i;
    for (i = 0; i < *nseen; ++i) {
        if (strcmp(seen[i], name) == 0) return 0;
    }
    if (*nseen < RAMFS_MAX_FILES + RAMFS_MAX_DIRS) {
        strncpy(seen[*nseen], name, RAMFS_PATH_MAX);
        seen[*nseen][RAMFS_PATH_MAX - 1] = 0;
        (*nseen)++;
    }
    if (cb) cb(name, is_dir, user);
    return 1;
}

int dos_ramfs_mkdir(const char *path)
{
    char key[RAMFS_PATH_MAX];
    if (norm_path(path, key, sizeof key) != 0) return -1;
    ensure_parent_dirs(key);
    return ensure_dir_norm(key);
}

int dos_ramfs_open(const char *path, int flags, dos_ramfs_file *out)
{
    char key[RAMFS_PATH_MAX];
    int idx;
    int acc = flags & O_ACCMODE;
    int writeable = (acc == O_WRONLY || acc == O_RDWR);
    int readable = (acc == O_RDONLY || acc == O_RDWR);

    if (!out || norm_path(path, key, sizeof key) != 0) return -1;
    idx = find_file(key);
    if (idx < 0) {
        if (!(flags & O_CREAT)) return -1;
        idx = alloc_file();
        if (idx < 0) return -1;
        memset(&g_files[idx], 0, sizeof g_files[idx]);
        strcpy(g_files[idx].path, key);
        g_files[idx].used = 1;
        g_files[idx].mtime = g_epoch++;
        ensure_parent_dirs(key);
    }
    if ((flags & O_TRUNC) && writeable) {
        g_files[idx].size = 0;
        g_files[idx].mtime = g_epoch++;
    }

    memset(out, 0, sizeof *out);
    out->index = idx;
    out->pos = (flags & O_APPEND) ? g_files[idx].size : 0;
    out->append = (flags & O_APPEND) != 0;
    out->readable = readable;
    out->writable = writeable;
    out->used = 1;
    return 0;
}

int64_t dos_ramfs_read(dos_ramfs_file *f, void *dst, uint64_t len)
{
    ramfs_node *n;
    if (!f || !f->used || !f->readable || !dst) return -1;
    if (f->index < 0 || f->index >= RAMFS_MAX_FILES || !g_files[f->index].used) return -1;
    n = &g_files[f->index];
    if (f->pos >= n->size) return 0;
    if (len > n->size - f->pos) len = n->size - f->pos;
    memcpy(dst, n->data + f->pos, (size_t)len);
    f->pos += len;
    return (int64_t)len;
}

int64_t dos_ramfs_write(dos_ramfs_file *f, const void *src, uint64_t len)
{
    ramfs_node *n;
    if (!f || !f->used || !f->writable || !src) return -1;
    if (f->index < 0 || f->index >= RAMFS_MAX_FILES || !g_files[f->index].used) return -1;
    n = &g_files[f->index];
    if (f->append) f->pos = n->size;
    if (ensure_capacity(n, f->pos + len) != 0) return -1;
    if (f->pos > n->size)
        memset(n->data + n->size, 0, (size_t)(f->pos - n->size));
    memcpy(n->data + f->pos, src, (size_t)len);
    f->pos += len;
    if (f->pos > n->size) n->size = f->pos;
    n->mtime = g_epoch++;
    return (int64_t)len;
}

int64_t dos_ramfs_seek(dos_ramfs_file *f, int64_t off, int whence)
{
    ramfs_node *n;
    int64_t base;
    if (!f || !f->used) return -1;
    if (f->index < 0 || f->index >= RAMFS_MAX_FILES || !g_files[f->index].used) return -1;
    n = &g_files[f->index];
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)f->pos; break;
        case 2: base = (int64_t)n->size; break;
        default: return -1;
    }
    base += off;
    if (base < 0) base = 0;
    f->pos = (uint64_t)base;
    return base;
}

int dos_ramfs_close(dos_ramfs_file *f)
{
    if (!f || !f->used) return -1;
    f->used = 0;
    return 0;
}

int dos_ramfs_fstat(const dos_ramfs_file *f, uint64_t *size, uint32_t *mtime)
{
    const ramfs_node *n;
    if (!f || !f->used) return -1;
    if (f->index < 0 || f->index >= RAMFS_MAX_FILES || !g_files[f->index].used) return -1;
    n = &g_files[f->index];
    if (size) *size = n->size;
    if (mtime) *mtime = n->mtime;
    return 0;
}

int dos_ramfs_unlink(const char *path)
{
    char key[RAMFS_PATH_MAX];
    int idx;
    if (norm_path(path, key, sizeof key) != 0) return -1;
    idx = find_file(key);
    if (idx < 0) return -1;
    free(g_files[idx].data);
    memset(&g_files[idx], 0, sizeof g_files[idx]);
    return 0;
}

int dos_ramfs_rename(const char *old_path, const char *new_path)
{
    char old_key[RAMFS_PATH_MAX], new_key[RAMFS_PATH_MAX];
    int old_idx, new_idx;
    if (norm_path(old_path, old_key, sizeof old_key) != 0) return -1;
    if (norm_path(new_path, new_key, sizeof new_key) != 0) return -1;
    old_idx = find_file(old_key);
    if (old_idx < 0) return -1;
    new_idx = find_file(new_key);
    if (new_idx >= 0) {
        free(g_files[new_idx].data);
        memset(&g_files[new_idx], 0, sizeof g_files[new_idx]);
    }
    strncpy(g_files[old_idx].path, new_key, sizeof g_files[old_idx].path);
    g_files[old_idx].path[sizeof(g_files[old_idx].path) - 1] = 0;
    ensure_parent_dirs(new_key);
    g_files[old_idx].mtime = g_epoch++;
    return 0;
}

int dos_ramfs_stat(const char *path, uint64_t *size, int *is_dir, uint32_t *mtime)
{
    char key[RAMFS_PATH_MAX];
    int idx, i;
    if (norm_path(path, key, sizeof key) != 0) return -1;
    idx = find_file(key);
    if (idx >= 0) {
        if (size) *size = g_files[idx].size;
        if (is_dir) *is_dir = 0;
        if (mtime) *mtime = g_files[idx].mtime;
        return 0;
    }
    if (find_dir(key) >= 0) {
        if (size) *size = 0;
        if (is_dir) *is_dir = 1;
        if (mtime) *mtime = g_epoch;
        return 0;
    }
    for (i = 0; i < RAMFS_MAX_FILES; ++i) {
        if (g_files[i].used && has_prefix_dir(g_files[i].path, key)) {
            if (size) *size = 0;
            if (is_dir) *is_dir = 1;
            if (mtime) *mtime = g_epoch;
            return 0;
        }
    }
    return -1;
}

int dos_ramfs_list(const char *dir, const char *ext, dos_ramfs_list_cb cb, void *user)
{
    char base[RAMFS_PATH_MAX];
    char seen[RAMFS_MAX_FILES + RAMFS_MAX_DIRS][RAMFS_PATH_MAX];
    int nseen = 0;
    int matches = 0;
    int exists = 0;
    int dironly = (ext && ext[0] == '/' && ext[1] == 0);
    int i;

    if (norm_path(dir ? dir : "", base, sizeof base) != 0) return -1;
    if (!base[0] || find_dir(base) >= 0) exists = 1;

    for (i = 0; i < RAMFS_MAX_DIRS; ++i) {
        const char *rest, *slash;
        char child[RAMFS_PATH_MAX];
        size_t len;
        if (!g_dir_used[i] || !has_prefix_dir(g_dirs[i], base)) continue;
        exists = 1;
        rest = base[0] ? g_dirs[i] + strlen(base) + 1 : g_dirs[i];
        slash = strchr(rest, '/');
        len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (len == 0 || len >= sizeof child) continue;
        memcpy(child, rest, len);
        child[len] = 0;
        if (dironly || !ext || !ext[0])
            matches += emit_once(seen, &nseen, child, 1, cb, user);
    }

    for (i = 0; i < RAMFS_MAX_FILES; ++i) {
        const char *rest, *slash;
        char child[RAMFS_PATH_MAX];
        size_t len;
        if (!g_files[i].used || !has_prefix_dir(g_files[i].path, base)) continue;
        exists = 1;
        rest = base[0] ? g_files[i].path + strlen(base) + 1 : g_files[i].path;
        slash = strchr(rest, '/');
        len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (len == 0 || len >= sizeof child) continue;
        memcpy(child, rest, len);
        child[len] = 0;
        if (slash) {
            if (dironly || !ext || !ext[0])
                matches += emit_once(seen, &nseen, child, 1, cb, user);
        } else if (!dironly && ext_match(child, ext)) {
            matches += emit_once(seen, &nseen, child, 0, cb, user);
        }
    }

    return exists ? matches : -1;
}

uint32_t dos_ramfs_epoch(void)
{
    return g_epoch;
}

const char *dos_ramfs_path(const dos_ramfs_file *f)
{
    if (!f || !f->used) return 0;
    if (f->index < 0 || f->index >= RAMFS_MAX_FILES || !g_files[f->index].used) return 0;
    return g_files[f->index].path;
}
