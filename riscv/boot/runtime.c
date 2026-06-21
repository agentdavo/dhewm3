/*
 * dhewm3 RISC-V64 bare-metal C runtime.
 *
 * Zeroes .bss, runs .init_array (C++ static constructors), then calls kmain().
 * The C library lives in riscv/libc; the heap arena it uses is the linker
 * window [__heap_start,__heap_end). Timing comes from the rdtime CSR.
 */
#include <stdint.h>
#include "runtime.h"

extern char __bss_start[], __bss_end[];
extern char __tbss_start[], __tbss_end[];
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

unsigned long boot_hartid = 0;
unsigned long boot_dtb    = 0;

/* Called from start.S with a0 = hartid, a1 = dtb. */
void _cstart(unsigned long hartid, unsigned long dtb)
{
    for (char *p = __bss_start; p < __bss_end; ++p)
        *p = 0;

    /* zero the thread-local .tbss (main thread uses the TLS image in place) */
    for (char *p = __tbss_start; p < __tbss_end; ++p)
        *p = 0;

    boot_hartid = hartid;
    boot_dtb    = dtb;

    for (void (**fn)(void) = __init_array_start; fn < __init_array_end; ++fn)
        (*fn)();

    kmain();

    sbi_shutdown();
}

void panic(const char *msg)
{
    static const char pfx[] = "\n*** PANIC: ";
    for (const char *p = pfx; *p; ++p) sbi_console_putchar(*p);
    while (msg && *msg) sbi_console_putchar(*msg++);
    sbi_console_putchar('\n');
    sbi_shutdown();
}

uint64_t timer_ticks(void)
{
    uint64_t t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

uint64_t timer_ms(void)
{
    return timer_ticks() / (TIMEBASE_HZ / 1000UL);
}
