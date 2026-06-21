/*
 * Fatal-fault trap handler (S-mode). For bring-up any synchronous trap is a
 * bug; we report it and power off. Recoverable traps come in a later phase.
 */
#include "libc/libc.h"
#include "runtime.h"

__attribute__((weak)) int os_handle_irq(unsigned long scause)
{
    (void)scause;
    return 0;
}

static const char *exc_name(unsigned long scause)
{
    if ((long)scause < 0)
        return "interrupt";

    switch (scause) {
    case 0:  return "instruction address misaligned";
    case 1:  return "instruction access fault";
    case 2:  return "illegal instruction";
    case 3:  return "breakpoint";
    case 4:  return "load address misaligned";
    case 5:  return "load access fault";
    case 6:  return "store/AMO address misaligned";
    case 7:  return "store/AMO access fault";
    case 8:  return "ecall from U-mode";
    case 9:  return "ecall from S-mode";
    case 12: return "instruction page fault";
    case 13: return "load page fault";
    case 15: return "store/AMO page fault";
    default: return "unknown";
    }
}

void trap_handler(unsigned long scause, unsigned long sepc, unsigned long stval, unsigned long ra)
{
    if ((long)scause < 0 && os_handle_irq(scause))
        return;

    printf("\n!!! FATAL TRAP !!!\n");
    printf("    scause = %#lx (%s)\n", scause, exc_name(scause));
    printf("    sepc   = %#lx\n", sepc);
    printf("    stval  = %#lx\n", stval);
    printf("    ra     = %#lx\n", ra);
    sbi_shutdown();
}
