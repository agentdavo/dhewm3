/*
 * Secondary-hart entry trampoline for the dOS SMP smoke test.
 *
 * Reached only via SBI HSM sbi_hart_start(hartid, secondary_entry, opaque).
 * Per the SBI spec, the firmware enters this address in S-mode with:
 *   a0 = hartid of the started hart
 *   a1 = opaque value passed to sbi_hart_start (we pass the hartid again)
 * S-mode interrupts are disabled and the MMU is off (satp == 0) on entry.
 *
 * This trampoline is independent of riscv/boot/start.S (which only ever runs on
 * hart 0 and parks every secondary). We give each secondary its own stack and a
 * per-hart scratch slot for tp, point stvec at the shared boot trap_vector, then
 * call the C worker secondary_worker(hartid).
 *
 * Per-hart stacks live in C (hart_stacks[8][...]); the symbols
 * smp_hart_sp(id) / smp_hart_tp(id) are tiny C helpers, but to keep the
 * trampoline self-contained we instead import the array bases as data symbols
 * and index them here in asm using the well-known element stride.
 */

.option norvc

.section .text.smp,"ax",@progbits
.balign 4
.globl secondary_entry
secondary_entry:
    csrw    sie, zero                 /* mask all S-mode interrupts        */
    csrw    sip, zero                 /* clear any pending soft interrupts */

    mv      s0, a0                    /* s0 = hartid (preserved across call to sp/tp setup) */

    /* sp = &hart_stacks[hartid + 1][0]  (top of this hart's 64 KiB stack,
     * full-descending: top of slot hartid is the base of slot hartid+1). */
    la      t0, hart_stacks           /* base of char[8][SMP_HART_STACK] */
    li      t1, SMP_HART_STACK_SIZE
    addi    t2, s0, 1                 /* hartid + 1 */
    mul     t2, t2, t1                /* (hartid+1) * stacksize */
    add     sp, t0, t2                /* sp -> one past this hart's stack region */
    andi    sp, sp, -16               /* 16-byte align the stack pointer    */

    /* tp = &hart_scratch[hartid][0] : a safe per-hart TLS/scratch base so any
     * accidental tp-relative access stays in this hart's private region. */
    la      t0, hart_scratch
    li      t1, SMP_HART_SCRATCH_SIZE
    mul     t2, s0, t1
    add     tp, t0, t2

    la      t0, trap_vector           /* shared boot trap handler (start.S) */
    csrw    stvec, t0

    mv      a0, s0                    /* a0 = hartid for the C worker        */
    call    secondary_worker          /* never returns (stops or parks)      */

    /* Defensive: if the worker ever returns, park this hart. */
1:  wfi
    j       1b
