/*
 * dosfs_format.h — on-disk layout for the flat read-only "dosfs" image.
 *
 * Shared verbatim by the dOS target reader (riscv/engine/dosfs.c) and the host
 * authoring tool (riscv/image/mkdosfs.py mirrors these struct layouts in its
 * struct.pack format strings). All integers are little-endian (RISC-V LE; virtio
 * modern mandates LE — no byte-swap anywhere). Sector == 512 bytes (the
 * virtio-blk unit); every region and every file blob starts on a 512-byte edge
 * so the driver only ever issues whole-sector reads.
 *
 * Layout:
 *   sector 0          : dosfs_super (exactly 512 bytes)
 *   sector 1 ...      : dosfs_dirent[entry_count], padded to a sector edge
 *   data_off ...      : concatenated file blobs, each sector-aligned
 *
 * See riscv/docs/fs/12-dosfs.md.
 */
#ifndef DOSFS_FORMAT_H
#define DOSFS_FORMAT_H

#include <stdint.h>

#define DOSFS_MAGIC       0x53464F44u   /* 'D','O','F','S' little-endian      */
#define DOSFS_VERSION     1u
#define DOSFS_SECTOR      512u          /* virtio-blk sector; all alignment   */
#define DOSFS_PATH_MAX    96u           /* incl. NUL; e.g. "base/demo00.pk4"  */

/* Superblock — fills sector 0 (the trailing reserved[] pads to 512). */
typedef struct dosfs_super {
    uint32_t magic;          /* DOSFS_MAGIC                                   */
    uint32_t version;        /* DOSFS_VERSION                                 */
    uint32_t sector_size;    /* DOSFS_SECTOR (512); sanity only               */
    uint32_t entry_count;    /* number of dosfs_dirent records that follow    */
    uint64_t dir_off;        /* byte offset of directory == 512               */
    uint64_t data_off;       /* byte offset where the first file blob starts  */
    uint64_t image_bytes;    /* total image size (== final padded length)     */
    uint32_t build_epoch;    /* one Unix time stamp for every file            */
    uint32_t flags;          /* bit0 = names are pre-lowercased (always 1)    */
    uint8_t  reserved[512 - 48];
} dosfs_super;

/* Directory entry — fixed 128 bytes, no string pool, no hashing.
 * The directory is the only index: a flat array, one record per file. Lookup is
 * a linear scan (entry_count is tens). Sorted ascending by `path`. */
typedef struct dosfs_dirent {
    char     path[DOSFS_PATH_MAX]; /* full image-relative, lowercased,        */
                                   /* fwd-slash, NUL-terminated, e.g.         */
                                   /* "base/demo00.pk4"                       */
    uint64_t offset;               /* absolute byte offset of blob (mult. 512)*/
    uint64_t size;                 /* exact file size in bytes                 */
    uint8_t  pad[128 - DOSFS_PATH_MAX - 16];
} dosfs_dirent;

/* Build-time layout guards (target + a manual host check). */
typedef char dosfs_assert_super [sizeof(dosfs_super)  == 512 ? 1 : -1];
typedef char dosfs_assert_dirent[sizeof(dosfs_dirent) == 128 ? 1 : -1];

#endif /* DOSFS_FORMAT_H */
