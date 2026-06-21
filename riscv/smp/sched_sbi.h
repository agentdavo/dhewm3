/*
 * SBI IPI helper for the dOS scheduler test  (riscv/smp/sched_sbi.h)
 * =================================================================
 *
 * The scheduler parks idle harts in real `wfi` (per the brief: "WFI when idle —
 * NO tight spins"). To make WFI safe under QEMU's round-robin TCG (where a bare
 * `wfi` on the last runnable hart wedges the VM — 01-scalable-threading.md §6.2)
 * every parked hart is woken by an explicit inter-processor interrupt:
 *
 *   - the SUBMITTER sends an IPI to all workers after publishing a job batch;
 *   - each WORKER, on finishing the batch, sends an IPI back to the submitter.
 *
 * This is the SBI IPI extension (EID 0x735049 "sPI", fid 0 = send_ipi), taking a
 * hart mask + base. The matching SSIP trap handler lives in secondary.S
 * (sched_trap_vector): it just clears the pending soft-interrupt and returns, so
 * the `wfi` falls through to re-check the queue/counter.
 *
 * smp_sbi.h already provides smp_sbi_ecall(); we reuse it here.
 */
#ifndef DHEWM3_RV_SMP_SCHED_SBI_H
#define DHEWM3_RV_SMP_SCHED_SBI_H

#include "smp_sbi.h"

/* SBI IPI extension: EID 'sPI' = 0x735049, fid 0 = sbi_send_ipi(mask, base). */
static inline void sbi_send_ipi(unsigned long hart_mask, unsigned long base)
{
    smp_sbi_ecall(0x735049, 0, (long)hart_mask, (long)base, 0, 0, 0, 0);
}

#endif /* DHEWM3_RV_SMP_SCHED_SBI_H */
