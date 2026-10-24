#include "engine/dos_disk_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void le32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
static void part(uint8_t *s, unsigned i, uint8_t type, uint32_t start, uint32_t size)
{
    uint8_t *p = s + 446 + i * 16;
    p[4] = type; le32(p + 8, start); le32(p + 12, size);
}
int main(void)
{
    uint8_t s[512] = {0}; uint64_t start, size;
    assert(dos_disk_volume(s, 20000, &start, &size) < 0);
    le32(s, 0x53464f44);
    assert(!dos_disk_volume(s, 20000, &start, &size) && start == 0 && size == 20000);
    assert(dos_disk_volume(s, 0, &start, &size) < 0);
    memset(s, 0, sizeof s); s[510] = 0x55; s[511] = 0xaa;
    assert(dos_disk_volume(s, 20000, &start, &size) < 0);
    part(s, 0, 0x0e, 2048, 2048); part(s, 1, 0xda, 4096, 10000);
    assert(!dos_disk_volume(s, 20000, &start, &size) && start == 4096 && size == 10000);
    assert(dos_disk_volume(s, 14095, &start, &size) < 0);
    assert(!dos_disk_volume(s, 14096, &start, &size));
    part(s, 2, 0xda, 15000, 1000);
    assert(dos_disk_volume(s, 20000, &start, &size) < 0);
    part(s, 2, 0x83, 14000, 1000);
    assert(dos_disk_volume(s, 20000, &start, &size) < 0);
    part(s, 2, 0, 0, 0); part(s, 1, 0xda, 0, 10000);
    assert(dos_disk_volume(s, 20000, &start, &size) < 0);
    part(s, 1, 0xda, 4096, 0);
    assert(dos_disk_volume(s, 20000, &start, &size) < 0);
    part(s, 1, 0xda, 0xfffffff0u, 0xfffffff0u);
    assert(dos_disk_volume(s, 0xffffffffu, &start, &size) < 0);
    puts("PASS: 12 DOSFS/MBR layout cases");
}
