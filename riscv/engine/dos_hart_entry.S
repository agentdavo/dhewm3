/*
 * Entry used with SBI HSM hart_start().
 *
 * OpenSBI enters in S-mode with:
 *   a0 = hart id
 *   a1 = opaque pointer passed to sbi_hart_start()
 */
.option norvc

.section .text,"ax",@progbits
.globl dos_hart_entry
dos_hart_entry:
    csrw    sie, zero
    csrw    sip, zero

    /* Enable FP + vector on this hart too (audit 7.1 B3). Worker harts run the
     * RVV raster kernels, so entering with VS=Off would fault on the first
     * vector instruction rather than degrade. */
    li      t0, (3 << 13) | (3 << 9)   /* sstatus.FS = Dirty, sstatus.VS = Dirty */
    csrs    sstatus, t0

    csrw    sscratch, a0

    ld      sp, 0(a1)            /* dos_thread_slot_t.stack_top */
    ld      tp, 8(a1)            /* dos_thread_slot_t.tls_base */

    la      t0, trap_vector
    csrw    stvec, t0

    mv      a0, a1
    call    dos_thread_entry_c

    call    sbi_hart_stop
1:  wfi
    j       1b
