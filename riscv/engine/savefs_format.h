/*
 * savefs_format.h — on-disk layout for the single-image, append-only WHOLE-FILE
 * journal that persists dhewm3 config/saves in a write-region appended after the
 * read-only dosfs pack on the SAME virtio-blk image.
 *
 * Shared verbatim by the dOS target (riscv/engine/savefs.c) and the host tools
 * (riscv/image/mkdosfs.py stamps savefs_region_desc + writes savefs_jhdr;
 * riscv/image/dumpsavefs.py mirrors these struct layouts in its struct.pack
 * format strings). All integers little-endian (RISC-V LE; virtio modern mandates
 * LE — no byte-swap anywhere). Sector == 512 bytes; every record and payload
 * starts on a 512-byte edge so the driver only ever issues whole-sector writes.
 *
 * The region descriptor lives in the dosfs superblock's reserved[] tail (which
 * the read-only reader, dosfs.c, never inspects) so the existing image format and
 * DOSFS_VERSION are UNCHANGED. region_magic==0 in reserved[] => legacy image, no
 * journal, today's volatile-ramfs behaviour (never corruption).
 *
 * See riscv/docs/fs/13-savefs.md.
 */
#ifndef SAVEFS_FORMAT_H
#define SAVEFS_FORMAT_H

#include <stdint.h>

#define SAVEFS_SECTOR        512u

/* 'S','V','F','S' little-endian — region descriptor marker in dosfs reserved[]. */
#define SAVEFS_REGION_MAGIC  0x53465653u
/* 'S','V','J','1' — write-region journal-header marker (region sector 0).        */
#define SAVEFS_JHDR_MAGIC    0x314A5653u
/* 'S','V','R','1' — per-record header marker.                                    */
#define SAVEFS_REC_MAGIC     0x31525653u

#define SAVEFS_VERSION       1u
#define SAVEFS_PATH_MAX      160u   /* == RAMFS_PATH_MAX; the normalized ramfs key */

/* record op codes */
#define SAVEFS_OP_PUT        1u     /* write whole file (payload = file bytes)     */
#define SAVEFS_OP_DELETE     2u     /* tombstone the path (no payload)             */

/* The descriptor is placed at byte offset 48 of the dosfs superblock — the first
 * byte after dosfs_super's 48-byte fixed head, i.e. reserved[0..23], which
 * dosfs.c never reads. 24 bytes. The old reader sees region_magic==0 => none. */
typedef struct savefs_region_desc {
    uint32_t region_magic;    /* SAVEFS_REGION_MAGIC, or 0 for a legacy image      */
    uint32_t version;         /* SAVEFS_VERSION                                    */
    uint64_t region_lba;      /* start LBA of the write-region (== image_bytes/512)*/
    uint32_t region_sectors;  /* write-region length in 512-byte sectors           */
    uint32_t flags;           /* reserved, 0                                       */
} savefs_region_desc;

/* Journal header — fills region sector 0 (written once at image build). */
typedef struct savefs_jhdr {
    uint32_t magic;           /* SAVEFS_JHDR_MAGIC                                  */
    uint32_t version;         /* SAVEFS_VERSION                                    */
    uint32_t sector_size;     /* SAVEFS_SECTOR (512); sanity only                  */
    uint32_t hdr_crc;         /* CRC32(IEEE) of this 512B sector, hdr_crc field = 0*/
    uint64_t region_sectors;  /* self-describing extent (mirrors region_desc)      */
    uint64_t epoch_base;      /* seed for dos_ramfs mtime epoch across boots       */
    uint8_t  pad[SAVEFS_SECTOR - 32];
} savefs_jhdr;

/* Record header — fills one 512B sector; the payload (PUT only) follows in the
 * next sector(s), zero-padded to a 512-byte edge. Fixed fields = 32 bytes; with
 * path[160] the used prefix is 192 bytes, padded to 512. A non-SAVEFS_REC_MAGIC
 * sector marks end-of-log. */
typedef struct savefs_rec {
    uint32_t magic;           /* SAVEFS_REC_MAGIC (else: end-of-log sentinel)      */
    uint32_t hdr_crc;         /* CRC32 of this 512B header, hdr_crc field = 0       */
    uint64_t seq;             /* monotonic, +1 per committed record (first == 1)   */
    uint32_t op;              /* SAVEFS_OP_PUT / SAVEFS_OP_DELETE                  */
    uint32_t data_len;        /* payload bytes (file size) for PUT; 0 for DELETE   */
    uint32_t data_crc;        /* CRC32 of the data_len payload bytes (0 for DELETE)*/
    uint32_t path_len;        /* strlen(path), <= SAVEFS_PATH_MAX-1                */
    char     path[SAVEFS_PATH_MAX];  /* normalized ramfs key, NUL-terminated      */
    uint8_t  pad[SAVEFS_SECTOR - 32 - SAVEFS_PATH_MAX];
} savefs_rec;

/* Build-time layout guards (mirrors the dosfs_format.h style). */
typedef char savefs_assert_desc[sizeof(savefs_region_desc) == 24  ? 1 : -1];
typedef char savefs_assert_jhdr[sizeof(savefs_jhdr)        == 512 ? 1 : -1];
typedef char savefs_assert_rec [sizeof(savefs_rec)         == 512 ? 1 : -1];

#endif /* SAVEFS_FORMAT_H */
