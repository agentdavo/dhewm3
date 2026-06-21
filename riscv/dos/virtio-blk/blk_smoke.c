/*
 * blk_smoke.c — standalone bare-metal smoke test for the virtio-blk driver +
 * dosfs reader. Boots on QEMU virt with a dosfs image attached as a
 * virtio-blk-device, reads sector 0, verifies the dosfs magic, then exercises
 * dosfs_open/dosfs_read/dosfs_list, and powers off via SBI.
 *
 * Built+run by riscv/scripts/blk_smoke.sh (not part of the engine link).
 */
#include "boot/runtime.h"
#include "engine/dos_virtio_blk.h"
#include "engine/dosfs.h"
#include "engine/dosfs_format.h"
#include <stdint.h>
#include <string.h>

static void puts_(const char *s) { sbi_console_write(s, (unsigned long)strlen(s)); }

static void put_hex(uint32_t v)
{
    char buf[11]; int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; ++i) {
        int nib = (v >> ((7 - i) * 4)) & 0xf;
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[10] = 0;
    puts_(buf);
}

static void put_u64(uint64_t v)
{
    char buf[21]; int i = 20;
    buf[20] = 0;
    if (v == 0) { puts_("0"); return; }
    while (v && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    puts_(&buf[i]);
}

static void list_cb(const char *base, int is_dir, void *user)
{
    (void)user;
    puts_("    "); puts_(base); puts_(is_dir ? " [dir]\n" : "\n");
}

int kmain(void)
{
    static uint8_t sec0[512] __attribute__((aligned(64)));
    dosfs_file f;
    int rc;

    puts_("\n=== virtio-blk smoke test ===\n");

    /* diagnostic: dump all 8 MMIO slots (magic/version/device-id) */
    {
        int i;
        for (i = 0; i < 8; ++i) {
            uintptr_t b = 0x10001000UL + (uintptr_t)i * 0x1000UL;
            uint32_t magic = *(volatile uint32_t *)(b + 0x000);
            uint32_t ver   = *(volatile uint32_t *)(b + 0x004);
            uint32_t did   = *(volatile uint32_t *)(b + 0x008);
            puts_("  slot "); put_hex((uint32_t)b);
            puts_(" magic="); put_hex(magic);
            puts_(" ver=");   put_hex(ver);
            puts_(" id=");    put_hex(did); puts_("\n");
        }
    }

    rc = vblk_init();
    puts_("vblk_init rc="); put_hex((uint32_t)rc); puts_("\n");
    if (rc != 0) { puts_("FAIL: no virtio-blk device\n"); sbi_shutdown(); }

    puts_("capacity sectors="); put_u64(vblk_capacity_sectors()); puts_("\n");

    rc = vblk_read_blocks(0, 1, sec0);
    puts_("read sector 0 rc="); put_hex((uint32_t)rc); puts_("\n");
    if (rc != 0) { puts_("FAIL: read error\n"); sbi_shutdown(); }

    {
        uint32_t magic = *(uint32_t *)sec0;
        puts_("sector0 magic="); put_hex(magic);
        puts_(magic == DOSFS_MAGIC ? "  (DOFS OK)\n" : "  (MISMATCH)\n");
        if (magic != DOSFS_MAGIC) { puts_("FAIL: bad magic\n"); sbi_shutdown(); }
    }

    rc = dosfs_init(&vblk_read);
    puts_("dosfs_init rc="); put_hex((uint32_t)rc); puts_("\n");
    if (rc != 0) { puts_("FAIL: dosfs_init\n"); sbi_shutdown(); }
    puts_("dosfs entries="); put_u64((uint64_t)dosfs_entry_count()); puts_("\n");

    puts_("list /base:\n");
    rc = dosfs_list("base", "", &list_cb, 0);
    puts_("  count="); put_hex((uint32_t)rc); puts_("\n");

    puts_("list /base *.pk4:\n");
    (void)dosfs_list("base", ".pk4", &list_cb, 0);

    /* Try to open + read the first few bytes of default.cfg (loose or absent). */
    if (dosfs_open("base/default.cfg", &f) == 0) {
        static char head[64];
        int64_t n = dosfs_read(&f, head, sizeof head - 1);
        if (n > 0) { head[n] = 0; puts_("base/default.cfg head: "); puts_(head); puts_("\n"); }
        puts_("default.cfg size="); put_u64(f.size); puts_("\n");
    } else {
        puts_("base/default.cfg not present as a loose file (ok if only in pk4)\n");
    }

    puts_("=== smoke test PASS ===\n");
    sbi_shutdown();
    return 0;
}
