/*
 * dhewm3 RISC-V64 bare-metal entry point.
 *
 * OpenSBI enters here in S-mode with:
 *   a0 = hart id
 *   a1 = physical address of the flattened device tree (FDT)
 *
 * We park every secondary hart, set up the boot hart's stack and trap vector,
 * then jump into the C runtime (_cstart) which clears .bss, runs constructors
 * and calls kmain().
 */

.option norvc

.section .text.init,"ax",@progbits
.globl _start
_start:
    csrw    sie, zero            /* mask all S-mode interrupts for now      */
    csrw    sip, zero            /* clear any pending soft interrupts       */

    /*
     * Boot whichever hart OpenSBI hands us. OpenSBI's cold-boot lottery picks a
     * NON-DETERMINISTIC boot hart — with -smp N it is often NOT hart 0 (the
     * banner's "Boot HART ID" varies run to run). OpenSBI brings only that boot
     * hart here; secondaries stay parked in OpenSBI until we sbi_hart_start them
     * to their own entry, so they never reach _start. The old `bnez a0,park_hart`
     * assumed the boot hart was hart 0 and parked any other lottery winner —
     * causing a flaky hang that got worse with more harts (1/2 at -smp 2, 3/4 at
     * -smp 4). An atomic claim keeps this correct even if firmware ever brought
     * more than one hart here: the first to claim boots, the rest park.
     */
    la      t0, boot_claimed
    li      t1, 1
    amoswap.w.aq t2, t1, (t0)     /* t2 = old flag; 0 => we won the boot claim */
    bnez    t2, park_hart         /* already claimed by another hart -> park   */

    la      sp, __stack_top      /* boot stack                              */
    la      tp, __tls_base       /* thread pointer -> static TLS block      */
    csrw    sscratch, a0         /* current hart id for dOS spin primitives */

    la      t0, trap_vector      /* direct-mode trap vector                 */
    csrw    stvec, t0

    /* a0 = hartid (the actual boot hart, maybe != 0), a1 = dtb; pass to _cstart */
    call    _cstart

    /* _cstart is not expected to return */
    call    sbi_shutdown
spin:
    wfi
    j       spin

park_hart:
    wfi
    j       park_hart

    /*
     * Boot-claim flag. MUST live in an initialised section (.data), NOT .bss:
     * .bss is not zeroed until _cstart, which runs *after* this claim, so a .bss
     * flag could read garbage. .data is loaded from the image as 0.
     */
.section .data
.balign 4
boot_claimed:
    .word 0
.section .text.init,"ax",@progbits

/*
 * Shared trap vector. Synchronous traps are still fatal, but interrupt handlers
 * may return after acknowledging their source. Save the full interrupted integer
 * register state before calling C; an interrupt is not a function call.
 */
    .balign 4
    .globl  trap_vector
trap_vector:
    addi    sp, sp, -256
    sd      ra,   0(sp)
    sd      gp,   8(sp)
    sd      tp,  16(sp)
    sd      t0,  24(sp)
    sd      t1,  32(sp)
    sd      t2,  40(sp)
    sd      s0,  48(sp)
    sd      s1,  56(sp)
    sd      a0,  64(sp)
    sd      a1,  72(sp)
    sd      a2,  80(sp)
    sd      a3,  88(sp)
    sd      a4,  96(sp)
    sd      a5, 104(sp)
    sd      a6, 112(sp)
    sd      a7, 120(sp)
    sd      s2, 128(sp)
    sd      s3, 136(sp)
    sd      s4, 144(sp)
    sd      s5, 152(sp)
    sd      s6, 160(sp)
    sd      s7, 168(sp)
    sd      s8, 176(sp)
    sd      s9, 184(sp)
    sd      s10,192(sp)
    sd      s11,200(sp)
    sd      t3, 208(sp)
    sd      t4, 216(sp)
    sd      t5, 224(sp)
    sd      t6, 232(sp)

    csrr    a0, scause
    csrr    a1, sepc
    csrr    a2, stval
    ld      a3, 0(sp)        /* return addr of the faulting call (NULL-call debug) */
    call    trap_handler

    ld      ra,   0(sp)
    ld      gp,   8(sp)
    ld      tp,  16(sp)
    ld      t0,  24(sp)
    ld      t1,  32(sp)
    ld      t2,  40(sp)
    ld      s0,  48(sp)
    ld      s1,  56(sp)
    ld      a0,  64(sp)
    ld      a1,  72(sp)
    ld      a2,  80(sp)
    ld      a3,  88(sp)
    ld      a4,  96(sp)
    ld      a5, 104(sp)
    ld      a6, 112(sp)
    ld      a7, 120(sp)
    ld      s2, 128(sp)
    ld      s3, 136(sp)
    ld      s4, 144(sp)
    ld      s5, 152(sp)
    ld      s6, 160(sp)
    ld      s7, 168(sp)
    ld      s8, 176(sp)
    ld      s9, 184(sp)
    ld      s10,192(sp)
    ld      s11,200(sp)
    ld      t3, 208(sp)
    ld      t4, 216(sp)
    ld      t5, 224(sp)
    ld      t6, 232(sp)
    addi    sp, sp, 256
    sret
