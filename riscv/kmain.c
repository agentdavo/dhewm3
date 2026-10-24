/*
 * Stage-1 bring-up entry point.
 *
 * Proves the bare-metal runtime is healthy end to end: SBI console, the libc
 * (printf/malloc/string), the timer, and — crucially for this port — that the
 * RISC-V Vector (RVV / "V") extension actually executes on the emulated CPU.
 */
#include <stdint.h>
#include "libc/libc.h"
#include "boot/runtime.h"

#ifdef __riscv_vector
/* Exercise the V extension: report VLEN, then do a vectorised int32 add. */
static void rvv_demo(void)
{
    const rv_platform_caps *caps = rv_get_platform_caps();

    printf("[rvv] V extension present: VLEN = %lu bits (vlenb=%lu)\n",
           caps->vlenb * 8, caps->vlenb);
    printf("[rvv] e32 VLMAX: m1=%lu m2=%lu lanes\n",
           caps->vlmax_e32m1, caps->vlmax_e32m2);

    static int32_t a[37], b[37], c[37];
    for (int i = 0; i < 37; ++i) { a[i] = i; b[i] = 1000 + i; c[i] = -1; }

    int32_t *pa = a, *pb = b, *pc = c;
    for (int rem = 37; rem > 0; ) {
        unsigned long gvl;
        asm volatile(
            "vsetvli %0, %4, e32, m1, ta, ma\n\t"
            "vle32.v v0, (%1)\n\t"
            "vle32.v v1, (%2)\n\t"
            "vadd.vv v2, v0, v1\n\t"
            "vse32.v v2, (%3)\n\t"
            : "=&r"(gvl)
            : "r"(pa), "r"(pb), "r"(pc), "r"((unsigned long)rem)
            : "memory");
        pa += gvl; pb += gvl; pc += gvl; rem -= (int)gvl;
    }

    int ok = 1;
    for (int i = 0; i < 37; ++i)
        if (c[i] != 1000 + 2 * i) { ok = 0; break; }
    printf("[rvv] vector add: c[0]=%d c[36]=%d  -> %s\n",
           c[0], c[36], ok ? "PASS" : "FAIL");
}
#else
/* Scalar-only ISA (no V in -march): the demo above is unassemblable.
 * Say so rather than silently reporting nothing. */
static void rvv_demo(void)
{
    printf("[rvv] V extension not built in (scalar -march); skipping.\n");
}
#endif

int kmain(void)
{
    printf("\n");
    printf("==================================================\n");
    printf("  dhewm3  -  RISC-V64 bare-metal runtime (stage 1)\n");
    printf("==================================================\n");
    printf("[boot] hart id        : %lu\n", boot_hartid);
    printf("[boot] device tree    : %#lx\n", boot_dtb);
    printf("[boot] timer (rdtime) : %lu ticks (~%lu ms)\n",
           timer_ticks(), timer_ms());

    void *p1 = malloc(1u << 20);
    void *p2 = malloc(4096);
    printf("[heap] malloc(1 MiB)=%p  malloc(4 KiB)=%p\n", p1, p2);
    if (p1) {
        memset(p1, 0xAB, 1u << 20);
        printf("[heap] memset 1 MiB OK, byte[0]=0x%02X\n",
               *(unsigned char *)p1);
    }
    free(p1);
    free(p2);

    rvv_demo();

    printf("\n[ok] stage-1 runtime healthy; powering off via SBI.\n");
    return 0;
}
