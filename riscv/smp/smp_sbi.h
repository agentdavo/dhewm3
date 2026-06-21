/*
 * SBI Hart State Management (HSM) + console shim for the dOS SMP smoke test.
 *
 * riscv/boot/sbi.c keeps its sbi_ecall() static, so the smoke test provides its
 * own minimal ecall vehicle here rather than touching the read-only boot files.
 * Only the calls the smoke test needs are wrapped: HSM (start/stop/get_status),
 * legacy console putchar, and the DBCN bulk console write.
 *
 * SBI spec references:
 *   HSM extension EID = 0x48534D ("HSM")
 *     fid 0: sbi_hart_start(hartid, start_addr, opaque)
 *     fid 1: sbi_hart_stop(void)
 *     fid 2: sbi_hart_get_status(hartid)
 *   DBCN extension EID = 0x4442434E ("DBCN"), fid 0: console_write(len, addr_lo, addr_hi)
 *   Legacy console putchar EID = 0x01
 */
#ifndef DHEWM3_RV_SMP_SBI_H
#define DHEWM3_RV_SMP_SBI_H

#include <stdint.h>

/* HSM hart-state codes (sbi_hart_get_status .value). */
#define SBI_HSM_STATE_STARTED        0
#define SBI_HSM_STATE_STOPPED        1
#define SBI_HSM_STATE_START_PENDING  2
#define SBI_HSM_STATE_STOP_PENDING   3
#define SBI_HSM_STATE_SUSPENDED      4
#define SBI_HSM_STATE_SUSPEND_PENDING 5
#define SBI_HSM_STATE_RESUME_PENDING  6

/* SBI standard error codes (subset). */
#define SBI_SUCCESS              0
#define SBI_ERR_FAILED         (-1)
#define SBI_ERR_NOT_SUPPORTED  (-2)
#define SBI_ERR_INVALID_PARAM  (-3)
#define SBI_ERR_ALREADY_AVAILABLE (-6) /* hart already started */

struct smp_sbiret { long error; long value; };

static inline struct smp_sbiret
smp_sbi_ecall(long eid, long fid,
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

    struct smp_sbiret r = { a0, a1 };
    return r;
}

/* HSM: wake hartid, jumping it to start_addr in S-mode with a1=opaque. */
static inline struct smp_sbiret
sbi_hart_start(long hartid, long start_addr, long opaque)
{
    return smp_sbi_ecall(0x48534D, 0, hartid, start_addr, opaque, 0, 0, 0);
}

/* HSM: the calling hart stops itself (does not return on success). */
static inline struct smp_sbiret
sbi_hart_stop(void)
{
    return smp_sbi_ecall(0x48534D, 1, 0, 0, 0, 0, 0, 0);
}

/* HSM: query the run-state of hartid (.value holds SBI_HSM_STATE_*). */
static inline struct smp_sbiret
sbi_hart_get_status(long hartid)
{
    return smp_sbi_ecall(0x48534D, 2, hartid, 0, 0, 0, 0, 0);
}

/* DBCN bulk console write; falls back to legacy putchar per byte. */
static inline long
smp_sbi_console_write(const char *buf, unsigned long len)
{
    struct smp_sbiret r = smp_sbi_ecall(0x4442434E, 0, (long)len,
                                        (long)(uintptr_t)buf, 0, 0, 0, 0);
    if (r.error == SBI_SUCCESS)
        return r.value;
    for (unsigned long i = 0; i < len; ++i)
        smp_sbi_ecall(0x01, 0, (unsigned char)buf[i], 0, 0, 0, 0, 0);
    return (long)len;
}

#endif /* DHEWM3_RV_SMP_SBI_H */
