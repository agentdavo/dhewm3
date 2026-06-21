/*
 * dhewm3 RISC-V64 bare-metal runtime: public interface.
 */
#ifndef DHEWM3_RV_RUNTIME_H
#define DHEWM3_RV_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* QEMU `virt` machine timebase (rdtime ticks per second). */
#define TIMEBASE_HZ 10000000UL

/* Set by start.S/_cstart from the values OpenSBI passes in a0/a1. */
extern unsigned long boot_hartid;
extern unsigned long boot_dtb;

/* SBI services (implemented in sbi.c). */
void     sbi_console_putchar(int ch);
long     sbi_console_write(const char *buf, unsigned long len);  /* DBCN bulk write */
void     sbi_shutdown(void) __attribute__((noreturn));
void     sbi_set_timer(uint64_t stime_value);
long     sbi_hart_start(unsigned long hartid, unsigned long start_addr, unsigned long opaque);
long     sbi_hart_stop(void);
long     sbi_hart_get_status(unsigned long hartid, unsigned long *status);

/* Timing helpers. */
uint64_t timer_ticks(void);   /* raw rdtime counter           */
uint64_t timer_ms(void);      /* milliseconds since power-on  */

/* Fatal error: prints msg and powers off. */
void     panic(const char *msg) __attribute__((noreturn));

/* Application entry, provided by kmain.c. */
int      kmain(void);

#endif /* DHEWM3_RV_RUNTIME_H */
