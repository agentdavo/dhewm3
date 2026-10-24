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
     * Enable the FP and vector units (audit 7.1 B3). Never done before -- this
     * worked only because OpenSBI happens to leave sstatus.FS/VS on. A hart may
     * legally enter S-mode with VS=Off, which turns the very first `vsetvli` in
     * rv_detect_platform_caps() into an illegal-instruction panic.
     */
#ifdef __riscv_vector
    li      t0, (3 << 13) | (3 << 9)   /* sstatus.FS = Dirty, sstatus.VS = Dirty */
#else
    li      t0, (3 << 13)              /* sstatus.FS = Dirty; no V in -march     */
#endif
    csrs    sstatus, t0

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
 *
 * VECTOR AND FP STATE (audit 7.1 A1). This used to save x1-x31 and nothing else.
 * The boot hart both enables sstatus.SIE (irq.c) and runs softgles2's RVV raster
 * loops, and GCC 16 at -O2 autovectorises freely -- `memset` in the payload is
 * already RVV-compiled -- so any C reached from here could clobber vtype/vl or
 * v0-v31 underneath an interrupted vector loop. The failure mode is silently
 * wrong pixels or a `vill` fault, not a clean crash.
 *
 * Saved lazily off sstatus.VS/FS: state is only spilled when the interrupted
 * context actually had any. The vector area is sized from `vlenb` at run time
 * (32*vlenb = 512 B at VLEN=128, 4 KiB at VLEN=1024) and lives on the trap
 * stack, so this is naturally per-hart and re-entrant with no scratch array.
 *
 * Frame below s0 (the integer-frame base), fixed 320 B:
 *   s0-320 .. s0-65  f0-f31        s0-64  fcsr    s0-56  vstart
 *   s0-48  vtype     s0-40  vl     s0-32  vcsr    s0-24  (pad)
 * then, only when VS != Off, 32*vlenb bytes of v0-v31 at sp.
 */
#define SSTATUS_FS_MASK (3 << 13)
#ifdef __riscv_vector
#define SSTATUS_VS_MASK (3 << 9)
#define SSTATUS_FSVS_ON ((1 << 13) | (1 << 9))   /* both Initial */
#else
/* No V in -march: sstatus.VS is read-only zero, there is no vector state to
 * spill, and every vector opcode below is unassemblable. */
#define SSTATUS_VS_MASK 0
#define SSTATUS_FSVS_ON (1 << 13)
#endif
#define SSTATUS_FSVS    (SSTATUS_FS_MASK | SSTATUS_VS_MASK)

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

    /* s0/s1/s2 are already spilled above, and trap_handler must preserve them
     * per the ABI, so they carry the extended-state bookkeeping across the call.
     *   s0 = integer-frame base   s1 = what we spilled   s2 = entry sstatus */
    mv      s0, sp
    addi    sp, sp, -320
    li      s1, 0
    csrr    s2, sstatus

    li      t0, SSTATUS_FS_MASK
    and     t1, s2, t0
    beqz    t1, 1f                  /* FS == Off: no FP state to preserve */
    fsd     f0,   -320(s0)
    fsd     f1,   -312(s0)
    fsd     f2,   -304(s0)
    fsd     f3,   -296(s0)
    fsd     f4,   -288(s0)
    fsd     f5,   -280(s0)
    fsd     f6,   -272(s0)
    fsd     f7,   -264(s0)
    fsd     f8,   -256(s0)
    fsd     f9,   -248(s0)
    fsd     f10,  -240(s0)
    fsd     f11,  -232(s0)
    fsd     f12,  -224(s0)
    fsd     f13,  -216(s0)
    fsd     f14,  -208(s0)
    fsd     f15,  -200(s0)
    fsd     f16,  -192(s0)
    fsd     f17,  -184(s0)
    fsd     f18,  -176(s0)
    fsd     f19,  -168(s0)
    fsd     f20,  -160(s0)
    fsd     f21,  -152(s0)
    fsd     f22,  -144(s0)
    fsd     f23,  -136(s0)
    fsd     f24,  -128(s0)
    fsd     f25,  -120(s0)
    fsd     f26,  -112(s0)
    fsd     f27,  -104(s0)
    fsd     f28,   -96(s0)
    fsd     f29,   -88(s0)
    fsd     f30,   -80(s0)
    fsd     f31,   -72(s0)
    csrr    t2, fcsr
    sd      t2, -64(s0)
    ori     s1, s1, 2
1:
#ifdef __riscv_vector
    li      t0, SSTATUS_VS_MASK
    and     t1, s2, t0
    beqz    t1, 2f                  /* VS == Off: no vector state to preserve */
    csrr    t2, vstart
    sd      t2, -56(s0)
    csrr    t2, vtype
    sd      t2, -48(s0)
    csrr    t2, vl
    sd      t2, -40(s0)
    csrr    t2, vcsr
    sd      t2, -32(s0)
    /* vse8.v starts at vstart; it must be 0 or the spill loses the head. */
    csrw    vstart, zero
    csrr    t3, vlenb
    slli    t4, t3, 5               /* 32 * vlenb, always a multiple of 16     */
    sub     sp, sp, t4
    mv      t5, sp
    vsetvli t6, zero, e8, m8, ta, ma
    slli    t3, t3, 3               /* 8 * vlenb = one LMUL=8 group            */
    vse8.v  v0, (t5)
    add     t5, t5, t3
    vse8.v  v8, (t5)
    add     t5, t5, t3
    vse8.v  v16, (t5)
    add     t5, t5, t3
    vse8.v  v24, (t5)
    ori     s1, s1, 1
#endif
2:
    /* Make sure the handler itself can use FP/vector even if the interrupted
     * context had them Off -- otherwise autovectorised C here takes a nested
     * illegal-instruction trap. csrs only raises the field, so Dirty stays Dirty. */
    li      t0, SSTATUS_FSVS_ON
    csrs    sstatus, t0

    csrr    a0, scause
    csrr    a1, sepc
    csrr    a2, stval
    ld      a3, 0(s0)        /* return addr of the faulting call (NULL-call debug) */
    call    trap_handler

    /* --- restore extended state (sp is unchanged across the call) --- */
#ifdef __riscv_vector
    andi    t0, s1, 1
    beqz    t0, 3f
    csrw    vstart, zero
    csrr    t3, vlenb
    mv      t5, sp
    vsetvli t6, zero, e8, m8, ta, ma
    slli    t3, t3, 3
    vle8.v  v0, (t5)
    add     t5, t5, t3
    vle8.v  v8, (t5)
    add     t5, t5, t3
    vle8.v  v16, (t5)
    add     t5, t5, t3
    vle8.v  v24, (t5)
    ld      t1, -40(s0)             /* vl    */
    ld      t2, -48(s0)             /* vtype */
    vsetvl  x0, t1, t2              /* saved vl <= VLMAX, so this is exact */
    ld      t2, -32(s0)
    csrw    vcsr, t2
    ld      t2, -56(s0)
    csrw    vstart, t2              /* vstart LAST: any vector op above zeroes it */
#endif
3:
    andi    t0, s1, 2
    beqz    t0, 4f
    ld      t2, -64(s0)
    csrw    fcsr, t2
    fld     f0,   -320(s0)
    fld     f1,   -312(s0)
    fld     f2,   -304(s0)
    fld     f3,   -296(s0)
    fld     f4,   -288(s0)
    fld     f5,   -280(s0)
    fld     f6,   -272(s0)
    fld     f7,   -264(s0)
    fld     f8,   -256(s0)
    fld     f9,   -248(s0)
    fld     f10,  -240(s0)
    fld     f11,  -232(s0)
    fld     f12,  -224(s0)
    fld     f13,  -216(s0)
    fld     f14,  -208(s0)
    fld     f15,  -200(s0)
    fld     f16,  -192(s0)
    fld     f17,  -184(s0)
    fld     f18,  -176(s0)
    fld     f19,  -168(s0)
    fld     f20,  -160(s0)
    fld     f21,  -152(s0)
    fld     f22,  -144(s0)
    fld     f23,  -136(s0)
    fld     f24,  -128(s0)
    fld     f25,  -120(s0)
    fld     f26,  -112(s0)
    fld     f27,  -104(s0)
    fld     f28,   -96(s0)
    fld     f29,   -88(s0)
    fld     f30,   -80(s0)
    fld     f31,   -72(s0)
4:
    /* Put sstatus.FS/VS back exactly as the interrupted context left them. Must
     * be the last thing that touches them: dropping VS to Off makes every vector
     * CSR access above illegal. sret does not restore these fields. */
    li      t0, SSTATUS_FSVS
    csrc    sstatus, t0
    and     t1, s2, t0
    csrs    sstatus, t1

    mv      sp, s0
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
