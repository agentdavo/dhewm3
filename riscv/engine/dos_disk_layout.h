/* On-disk MBR selection, independent of transport for host validation. */
#ifndef DOS_DISK_LAYOUT_H
#define DOS_DISK_LAYOUT_H
#include <stdint.h>

static inline uint32_t disk_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Returns a bounded DOSFS volume; legacy whole-disk images remain supported. */
static inline int dos_disk_volume(const uint8_t sector[512], uint64_t capacity,
                                  uint64_t *start, uint64_t *size)
{
    *start = *size = 0;
    if (!capacity) return -1;
    if (disk_le32(sector) == 0x53464f44u) {
        *size = capacity;
        return 0;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa) return -2;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *p = sector + 446 + i * 16;
        if (p[4] != 0xda) continue;
        if (*size) return -3; /* Ambiguous: never guess a writable volume. */
        *start = disk_le32(p + 8);
        *size = disk_le32(p + 12);
        if (!*start || !*size || *start >= capacity || *size > capacity - *start)
            return -4;
    }
    if (!*size) return -5;
    /* Reject overlapping primary partitions before enabling savefs writes. */
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *p = sector + 446 + i * 16;
        uint64_t other = disk_le32(p + 8), count = disk_le32(p + 12);
        if (p[4] && p[4] != 0xda && count && other < *start + *size && *start < other + count)
            return -6;
    }
    return 0;
}
#endif
