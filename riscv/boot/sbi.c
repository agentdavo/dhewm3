/*
 * Supervisor Binary Interface (SBI) calls.
 *
 * We use the SBI v0.2+ extensions where available (TIME, SRST) and fall back
 * to the legacy 0.1 calls (console putchar EID 0x01, shutdown EID 0x08) that
 * OpenSBI always provides.
 */
#include "runtime.h"

struct sbiret { long error; long value; };

static struct sbiret sbi_ecall(long eid, long fid,
                               long a0_, long a1_, long a2_,
                               long a3_, long a4_, long a5_)
{
    register long a0 asm("a0") = a0_;
    register long a1 asm("a1") = a1_;
    register long a2 asm("a2") = a2_;
    register long a3 asm("a3") = a3_;
    register long a4 asm("a4") = a4_;
    register long a5 asm("a5") = a5_;
    register long a6 asm("a6") = fid;
    register long a7 asm("a7") = eid;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");

    struct sbiret r = { a0, a1 };
    return r;
}

/* Legacy console putchar (extension id 0x01). */
void sbi_console_putchar(int ch)
{
    sbi_ecall(0x01, 0, ch, 0, 0, 0, 0, 0);
}

/* SBI Debug Console (DBCN, EID 0x4442434E) console_write (FID 0): write the
 * whole buffer in one ecall. Falls back to legacy putchar if unsupported.
 * Returns bytes written. Much faster than per-char ecalls for bulk output. */
long sbi_console_write(const char *buf, unsigned long len)
{
    struct sbiret r = sbi_ecall(0x4442434E, 0, (long)len,
                                (long)(uintptr_t)buf, 0, 0, 0, 0);
    if (r.error == 0)
        return r.value;
    for (unsigned long i = 0; i < len; ++i) sbi_console_putchar(buf[i]);
    return (long)len;
}

/* TIME extension ("TIME" = 0x54494D45), set_timer (fid 0). */
void sbi_set_timer(uint64_t stime_value)
{
    sbi_ecall(0x54494D45, 0, (long)stime_value, 0, 0, 0, 0, 0);
}

/* Hart State Management ("HSM" = 0x48534D), SBI v0.2+. */
long sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque)
{
    struct sbiret r = sbi_ecall(0x48534D, 0, (long)hartid,
                                (long)start_addr, (long)opaque, 0, 0, 0);
    return r.error;
}

long sbi_hart_stop(void)
{
    struct sbiret r = sbi_ecall(0x48534D, 1, 0, 0, 0, 0, 0, 0);
    return r.error;
}

long sbi_hart_get_status(unsigned long hartid, unsigned long *status)
{
    struct sbiret r = sbi_ecall(0x48534D, 2, (long)hartid, 0, 0, 0, 0, 0);
    if (status)
        *status = (unsigned long)r.value;
    return r.error;
}

/* System reset ("SRST" = 0x53525354): type 0 = shutdown, reason 0 = none.
 * Fall back to legacy shutdown (EID 0x08) if SRST is unavailable. */
void sbi_shutdown(void)
{
    sbi_ecall(0x53525354, 0, 0, 0, 0, 0, 0, 0);
    sbi_ecall(0x08, 0, 0, 0, 0, 0, 0, 0);
    for (;;) { asm volatile("wfi"); }
}
