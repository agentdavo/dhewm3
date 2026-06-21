/*
 * Secondary-hart entry + interrupt-aware trap vector for the dOS scheduler test.
 * =============================================================================
 *
 * This is a scheduler-test-local sibling of the smoke test's secondary.S. It is
 * kept separate so the smoke test stays byte-for-byte unchanged. Differences vs
 * secondary.S:
 *
 *   1. It installs its OWN trap vector (sched_trap_vector) instead of the shared
 *      boot trap_vector. The boot vector treats EVERY trap (including interrupts)
 *      as fatal; the scheduler needs supervisor software interrupts (SSIP) to be
 *      RECOVERABLE so a parked `wfi` worker can be woken by an SBI IPI and simply
 *      fall through to re-check the queue. sched_trap_vector handles the
 *      SSIP/STIP interrupt cases (clear pending, sret) and only falls back to the
 *      fatal C handler for genuine synchronous faults.
 *
 *   2. It ENABLES sie.SSIE (bit 1) so the hart actually takes the IPI that wakes
 *      it from `wfi`. (The submitter likewise enables SSIE itself in C before it
 *      waits, so workers can wake it at the barrier.)
 *
 * Reached only via sbi_hart_start(hartid, sched_secondary_entry, hartid): S-mode,
 * a0 = hartid, a1 = opaque (= hartid), interrupts masked, MMU off. Per-hart sp/tp
 * come from the same hart_stacks[]/hart_scratch[] arrays the smoke test uses (the
 * scheduler test defines them in sched_kmain.c).
 */

.option norvc

/* sie bits */
#define SIE_SSIE  (1 << 1)        /* supervisor software interrupt enable */
#define SIE_STIE  (1 << 5)        /* supervisor timer interrupt enable    */

.section .text.smp,"ax",@progbits
.balign 4
.globl sched_secondary_entry
sched_secondary_entry:
    csrw    sie, zero                 /* mask all S-mode interrupts first   */
    csrw    sip, zero                 /* clear any pending soft interrupts  */

    mv      s0, a0                    /* s0 = hartid (preserved)            */

    /* sp = &hart_stacks[hartid + 1][0]  (top of this hart's stack region). */
    la      t0, hart_stacks
    li      t1, SMP_HART_STACK_SIZE
    addi    t2, s0, 1
    mul     t2, t2, t1
    add     sp, t0, t2
    andi    sp, sp, -16

    /* tp = &hart_scratch[hartid][0]  (per-hart private scratch / TLS base). */
    la      t0, hart_scratch
    li      t1, SMP_HART_SCRATCH_SIZE
    mul     t2, s0, t1
    add     tp, t0, t2

    /* Install the interrupt-aware trap vector (direct mode). */
    la      t0, sched_trap_vector
    csrw    stvec, t0

    /* Enable supervisor software interrupts so the IPI wake is delivered. */
    li      t0, SIE_SSIE
    csrs    sie, t0

    mv      a0, s0                    /* a0 = hartid for the C worker        */
    call    secondary_worker          /* -> hart_worker_main(); returns on   */
                                      /*    sched_shutdown so we can stop.   */

    /* Worker returned (scheduler shut down): stop this hart cleanly. */
    li      a7, 0x48534D              /* HSM EID                             */
    li      a6, 1                     /* fid 1 = sbi_hart_stop               */
    ecall
1:  wfi                              /* defensive: park if stop ever returns */
    j       1b

/*
 * sched_trap_vector — direct-mode S-mode trap entry.
 *
 * Distinguishes interrupts (scause MSB set) from synchronous exceptions:
 *   - Supervisor software interrupt (cause 1) or timer interrupt (cause 5):
 *     clear the pending bit in sip and `sret` — this is the IPI/timer wake that
 *     pulls a hart out of `wfi`. Only a0/t0/t1 are touched and are saved/restored
 *     so the interrupted context is preserved exactly.
 *   - Anything else (a real fault, or an unexpected ecall): hand off to the
 *     shared fatal C trap_handler() (reports cause/epc/tval and powers off).
 *
 * A 64-byte scratch save area per hart lives at the TOP of tp's scratch slot;
 * the worker never uses tp for anything else, so this is safe and lock-free.
 */
    .balign 4
    .globl sched_trap_vector
sched_trap_vector:
    /* Stash a0/t0/t1 in this hart's scratch (tp-relative) so we don't clobber
     * the interrupted register state. SMP_HART_SCRATCH_SIZE is >= 256, plenty. */
    sd      a0, 0(tp)
    sd      t0, 8(tp)
    sd      t1, 16(tp)

    csrr    t0, scause
    bltz    t0, .Lirq                 /* MSB set => interrupt                */

    /* Synchronous exception: restore scratch regs and fall into the fatal
     * handler (which never returns, so it's fine to load the live cause args). */
    ld      a0, 0(tp)
    ld      t0, 8(tp)
    ld      t1, 16(tp)
    csrr    a0, scause
    csrr    a1, sepc
    csrr    a2, stval
    mv      a3, ra
    call    trap_handler              /* shared boot fatal handler (no return) */
1:  wfi
    j       1b

.Lirq:
    /* Interrupt: clear the corresponding pending bit and return. For SSIP we
     * clear sip.SSIP (bit 1); for STIP the timer is one-shot (we re-arm in C),
     * so just clearing the local pending and returning is enough. Clearing both
     * the SSIP and STIP soft bits we own is harmless. */
    li      t1, (1 << 1) | (1 << 5)   /* SSIP | STIP                          */
    csrc    sip, t1

    ld      a0, 0(tp)
    ld      t0, 8(tp)
    ld      t1, 16(tp)
    sret
