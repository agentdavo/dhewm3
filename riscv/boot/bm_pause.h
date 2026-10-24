/*
 * bm_pause() — the spin-loop backoff hint, in one place.
 *
 * `pause` was hand-encoded as `.insn i 0x0F, 0, x0, x0, 0x010` in eight separate
 * files, each with its own comment, and two other spin loops used a bare `nop`
 * instead.
 *
 * The mnemonic needs Zihintpause in -march, which the scalar rv64imafdc build
 * does not have, so the raw encoding comes back for that case only. It is the
 * same instruction either way: PAUSE is
 *
 *     FENCE  pred=W, succ=(none), fm=0, rd=x0, rs1=x0   ==  0x0100000F
 *
 * which a hart without Zihintpause simply retires as that FENCE. Verified
 * byte-identical to `pause` assembled with -march=..._zihintpause.
 *
 * IMPORTANT — what this is NOT. Under QEMU TCG, `pause` decodes to a nop and
 * gives the host CPU back to nobody. It does not yield, and neither does Zawrs'
 * `wrs.nto`, whose QEMU helper is a pure permission check with no halt. `wfi` is
 * the only instruction that actually parks a vCPU thread under MTTCG. So use
 * bm_pause() for SHORT spins where the wait is expected to end almost
 * immediately, and bm_wait_until() (runtime.h) for anything longer -- a spin
 * loop that reaches for bm_pause() as its idle strategy is burning a host core.
 */
#ifndef DHEWM3_RV_BM_PAUSE_H
#define DHEWM3_RV_BM_PAUSE_H

static inline void bm_pause(void)
{
#if defined(__riscv) && defined(__riscv_zihintpause)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__riscv)
    __asm__ __volatile__(".insn 0x0100000F" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

#endif /* DHEWM3_RV_BM_PAUSE_H */
