/*
 * dhewm3 RISC-V64 bare-metal runtime: public interface.
 */
#ifndef DHEWM3_RV_RUNTIME_H
#define DHEWM3_RV_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* QEMU `virt` machine timebase (rdtime ticks per second). This is the COMPILE-TIME
 * fallback only -- it is correct for QEMU virt but real RVA23 silicon commonly
 * clocks mtime at 1 MHz or 24 MHz, so anything that hardcodes it produces
 * silently wrong timings on hardware. Prefer timer_hz() / timer_ns_from_ticks(). */
#define TIMEBASE_HZ 10000000UL

/* Actual timebase, taken from the device tree's `timebase-frequency` when it can
 * be read, else TIMEBASE_HZ. Set once by timer_init_from_dtb() during _cstart. */
unsigned long timer_hz(void);
int           timer_hz_is_from_dtb(void);
void          timer_init_from_dtb(void);

/* Exact ticks -> nanoseconds, overflow-safe for full-run accumulators.
 * uint64 nanoseconds hold ~584 years, so the result never wraps in practice;
 * the naive `ticks * 1e9` intermediate WOULD wrap past ~1.8e10 ticks, hence the
 * split-quotient form. */
uint64_t timer_ns_from_ticks(uint64_t ticks);

/* Physical RAM bank containing the loaded image, discovered from the DTB's
 * /memory node during _cstart. Both 0 when no usable DTB was supplied, in which
 * case callers must fall back to the linker-script window. ram_end() is already
 * clamped below the DTB blob. */
void     ram_init_from_dtb(void);
uint64_t ram_base(void);
uint64_t ram_end(void);

/* Set by start.S/_cstart from the values OpenSBI passes in a0/a1. */
extern unsigned long boot_hartid;
extern unsigned long boot_dtb;

/* Homogeneous-hart ISA geometry sampled on the boot hart before constructors.
 * DoomOS currently requires V, so vlenb is always readable.  Consumers must
 * still use vsetvl for tails; these values are for dispatch and validation. */
typedef struct rv_platform_caps {
    unsigned long vlenb;
    unsigned long vlmax_e32m1;
    unsigned long vlmax_e32m2;
} rv_platform_caps;

extern rv_platform_caps boot_rv_caps;
const rv_platform_caps *rv_get_platform_caps(void);

/* Extensions the compiler emits unconditionally (they are in -march) but whose
 * availability is a run-time fact. cbo.zero in particular traps as illegal in
 * S-mode unless M-mode firmware set menvcfg.CBZE, so "the CPU has Zicboz" is not
 * sufficient. Probed by execution during _cstart. */
typedef struct rv_isa_probe {
    int zicboz;   /* cbo.zero usable from S-mode (implies firmware enabled CBZE) */
    int zfa;      /* fli.s and friends */
    int zvbb;     /* vwsll */
    int cycle;    /* rdcycle readable   (mcounteren.CY set by firmware) */
    int instret;  /* rdinstret readable (mcounteren.IR set by firmware) */
} rv_isa_probe;

extern rv_isa_probe boot_rv_isa;

/* Guest cycle / retired-instruction counters, 0 when firmware has not made them
 * readable from S-mode. instret is the one that matters for optimisation work:
 * unlike rdtime it counts GUEST work, so it does not move when the host gets
 * busy -- which is the failure mode that has produced two retracted perf claims
 * on this project. */
uint64_t bm_rdcycle(void);
uint64_t bm_rdinstret(void);

/* SBI services (implemented in sbi.c). */
void     sbi_console_putchar(int ch);
long     sbi_console_write(const char *buf, unsigned long len);  /* DBCN bulk write */
void     sbi_shutdown(void) __attribute__((noreturn));
void     sbi_set_timer(uint64_t stime_value);
long     sbi_probe_extension(long eid);   /* 0 = firmware lacks it */
long     sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base);
long     sbi_send_ipi_hart(unsigned long hartid);
long     sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque);
long     sbi_hart_stop(void);
long     sbi_hart_get_status(unsigned long hartid, unsigned long *status);

/* Timing helpers. */
uint64_t timer_ticks(void);   /* raw rdtime counter           */
uint64_t timer_ms(void);      /* milliseconds since power-on  */
uint64_t timer_us(void);      /* microseconds since power-on  */

/* Microseconds -> raw ticks at the discovered timebase. Overflow-safe for the
 * usec ranges usleep()/nanosleep() deal in. */
uint64_t timer_ticks_from_us(uint64_t us);

/* Arm / disarm this hart's S-mode timer interrupt. Uses Sstc's `stimecmp` when
 * the DTB advertises it (mandatory in RVA23S64), else the SBI TIME extension.
 * Arming also sets sie.STIE, so a subsequent `wfi` is guaranteed to wake.
 * os_handle_irq() disarms on delivery. */
void     timer_arm(uint64_t deadline_ticks);
void     timer_arm_us(uint64_t us);
void     timer_disarm(void);

/* Park this hart until an interrupt or the deadline, whichever comes first.
 * The deadline is the net for a lost wakeup, not the expected exit. */
void     bm_wait_until(uint64_t deadline_ticks);

/* Fatal error: prints msg and powers off. */
void     panic(const char *msg) __attribute__((noreturn));

/* Application entry, provided by kmain.c. */
int      kmain(void);

#endif /* DHEWM3_RV_RUNTIME_H */
