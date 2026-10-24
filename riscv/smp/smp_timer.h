/*
 * smp_timer.h — timer helpers for the riscv/smp test harness.
 *
 * These are implemented in riscv/boot/runtime.c and normally declared by
 * boot/runtime.h. The harness cannot include that header: runtime.h also
 * prototypes the SBI calls as `long sbi_*(...)`, while smp_sbi.h / sched_sbi.h
 * define static inline wrappers of the SAME names with different return types.
 * Including both is a hard "conflicting types" error, and it broke the entire
 * riscv/smp build -- `make sched` failed on four such errors.
 *
 * Declaring just the timer entry points keeps the harness's own SBI wrappers
 * authoritative (they are self-contained ecalls, which is what a standalone test
 * image wants) without dragging in the conflicting prototypes.
 *
 * The duplication itself is the underlying wart: sbi_send_ipi and friends have
 * two independent implementations. Unifying them means picking one owner for the
 * SBI layer, which is a larger change than unbreaking the build.
 */
#ifndef SMP_TIMER_H
#define SMP_TIMER_H

#include <stdint.h>

uint64_t timer_ticks(void);   /* raw rdtime counter          */
uint64_t timer_hz(void);      /* discovered timebase, Hz     */
uint64_t timer_ms(void);      /* milliseconds since power-on */

/* Published by start.S / _cstart, same as boot/runtime.h declares them. */
extern unsigned long boot_hartid;
extern unsigned long boot_dtb;

#endif /* SMP_TIMER_H */
