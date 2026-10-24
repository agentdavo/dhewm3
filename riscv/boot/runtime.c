/*
 * dhewm3 RISC-V64 bare-metal C runtime.
 *
 * Zeroes .bss, runs .init_array (C++ static constructors), then calls kmain().
 * The C library lives in riscv/libc; the heap arena it uses is the linker
 * window [__heap_start,__heap_end). Timing comes from the rdtime CSR.
 */
#include <stdint.h>
#include <string.h>
#include "runtime.h"
#include "fdt.h"

extern char __bss_start[], __bss_end[];
extern char __tbss_start[], __tbss_end[];
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

unsigned long boot_hartid = 0;
unsigned long boot_dtb    = 0;
rv_platform_caps boot_rv_caps;

static void rv_detect_platform_caps(void)
{
#ifdef __riscv_vector
    asm volatile("csrr %0, vlenb" : "=r"(boot_rv_caps.vlenb));
    asm volatile("vsetvli %0, zero, e32, m1, ta, ma"
                 : "=r"(boot_rv_caps.vlmax_e32m1));
    asm volatile("vsetvli %0, zero, e32, m2, ta, ma"
                 : "=r"(boot_rv_caps.vlmax_e32m2));
#else
    /* No V in -march. vlenb and vsetvli are unassemblable and would trap
     * anyway; leave the geometry zeroed so consumers see "no vector unit". */
    boot_rv_caps.vlenb = 0;
    boot_rv_caps.vlmax_e32m1 = 0;
    boot_rv_caps.vlmax_e32m2 = 0;
#endif
}

/* The image is built with -mrvv-vector-bits=zvl, so GCC folds VLEN into
 * compile-time constants everywhere and fixed-length vector types assume
 * EXACTLY __riscv_v_min_vlen bits. A part with a different VLEN -- larger
 * included -- therefore computes wrong answers with no trap: `RVV_VLEN=256
 * ./run.sh` against a zvl1024b build renders silently wrong pixels, because
 * rv_detect_platform_caps() read vlenb and never compared it to anything.
 *
 * Zvl1024b is also optional; RVA23 mandates only VLEN>=128, and shipping
 * RVA23-class parts are VLEN=256 (SpacemiT K1/M1) or 512 (SiFive X280/XM), on
 * which a zvl1024b binary is architecturally illegal. Fail loudly instead. */
/* The compiler emits Zicboz/Zfa/Zvbb instructions unconditionally because they
 * are in -march, but two things have to be true at run time for that to be
 * legal, and neither was ever checked:
 *
 *  1. the hardware implements them, and
 *  2. for cbo.zero specifically, M-mode firmware has set menvcfg.CBZE -- the
 *     instruction traps as ILLEGAL in S-mode otherwise.
 *
 * (2) is the dangerous one: it works today only because the bundled OpenSBI
 * happens to enable it. Different firmware turns a rasteriser inner loop into a
 * fatal trap whose only evidence is scause=2 at an address in memset.
 *
 * Probing by execution is the only option from S-mode (menvcfg is not readable
 * here), so this runs each instruction once behind a trap handler that steps
 * over a fault instead of panicking. */
static volatile int g_probe_active;
static volatile int g_probe_trapped;

int rv_probe_trap_hook(unsigned long scause, unsigned long *sepc)
{
    if (!g_probe_active || (long)scause < 0 || scause != 2UL)
        return 0;                       /* not ours; let the fatal path have it */
    g_probe_trapped = 1;
    /* +4 is safe because none of the probed instructions (cbo.zero, fli.s,
     * vwsll.vi) has a compressed encoding, so each is exactly 4 bytes even
     * though this translation unit is built with C enabled. */
    *sepc += 4;
    return 1;
}

static int rv_probe(void (*insn)(void))
{
    g_probe_trapped = 0;
    g_probe_active = 1;
    insn();
    g_probe_active = 0;
    return !g_probe_trapped;
}

#ifdef __riscv_zicboz
static void probe_cbo_zero(void)
{
    static char buf[128] __attribute__((aligned(64)));
    asm volatile("cbo.zero (%0)" :: "r"(buf) : "memory");
}
#endif

#ifdef __riscv_zfa
static void probe_fli_s(void)  { asm volatile("fli.s fa0, 1" ::: "fa0"); }
#endif
/* rdcycle/rdinstret are the unprivileged shadows of mcycle/minstret and trap in
 * S-mode unless M-mode firmware set the corresponding mcounteren bits. There is
 * no way to ask from here, so probe by executing. */
static void probe_rdcycle(void)  { unsigned long v; asm volatile("rdcycle %0"  : "=r"(v)); (void)v; }
static void probe_rdinstret(void){ unsigned long v; asm volatile("rdinstret %0": "=r"(v)); (void)v; }
#if defined(__riscv_vector) && defined(__riscv_zvbb)
static void probe_vwsll(void)
{
    asm volatile("vsetivli zero, 1, e16, m1, ta, ma; vwsll.vi v8, v4, 1" ::: "v8");
}
#endif

rv_isa_probe boot_rv_isa;

static void rv_probe_isa(void)
{
#ifdef __riscv_zicboz
    boot_rv_isa.zicboz = rv_probe(probe_cbo_zero);
#endif
#ifdef __riscv_zfa
    boot_rv_isa.zfa    = rv_probe(probe_fli_s);
#endif
#if defined(__riscv_vector) && defined(__riscv_zvbb)
    boot_rv_isa.zvbb   = rv_probe(probe_vwsll);
#endif
    boot_rv_isa.cycle  = rv_probe(probe_rdcycle);
    boot_rv_isa.instret= rv_probe(probe_rdinstret);
}

/* Retired-instruction and cycle counters.
 *
 * The profiler has been rdtime-only, and rdtime under TCG measures HOST WALL
 * TIME -- it cannot distinguish "the guest did less work" from "the host was
 * less busy". That blind spot is not academic: it is what let the [rprof] total
 * bug hide a 4-5x undercount, and why two runs of an identical binary reported
 * 2.10x and 0.92x minutes apart. instret counts GUEST instructions retired and
 * is immune to host contention, so it is the only honest way to compare two
 * builds on a shared machine.
 *
 * Return 0 when unavailable rather than trapping, so callers can log
 * unconditionally. */
uint64_t bm_rdcycle(void)
{
    uint64_t v = 0;
    if (boot_rv_isa.cycle) asm volatile("rdcycle %0" : "=r"(v));
    return v;
}

uint64_t bm_rdinstret(void)
{
    uint64_t v = 0;
    if (boot_rv_isa.instret) asm volatile("rdinstret %0" : "=r"(v));
    return v;
}

static void rv_check_vlen(void)
{
#ifdef __riscv_v_min_vlen
    unsigned long want = (unsigned long)__riscv_v_min_vlen / 8UL;
    if (boot_rv_caps.vlenb != want) {
        static const char msg[] =
            "\n*** PANIC: VLEN mismatch: this image is compiled for a fixed "
            "vector width and the hardware disagrees.\n"
            "    Rebuild with a matching zvlNb, or run QEMU with the matching "
            "vlen= (see riscv/run.sh RVV_VLEN).\n";
        for (const char *p = msg; *p; ++p) sbi_console_putchar(*p);
        sbi_shutdown();
    }
#endif
}

const rv_platform_caps *rv_get_platform_caps(void)
{
    return &boot_rv_caps;
}

/* Widened zero-fill for the startup clear. Not marked static-inline on purpose:
 * keeping it a real function stops GCC from "helpfully" rewriting the body into
 * a memset() call, which would recurse into libc before .bss is live. */
__attribute__((noinline))
static void bm_zero_range(char *p, char *end)
{
    while (p < end && ((uintptr_t)p & 7u)) *p++ = 0;
    uint64_t *w = (uint64_t *)p;
    uint64_t *wend = (uint64_t *)((uintptr_t)end & ~(uintptr_t)7);
    while (w < wend) *w++ = 0;
    p = (char *)w;
    while (p < end) *p++ = 0;
}

/* Called from start.S with a0 = hartid, a1 = dtb. */
extern void BM_BootBegin(uint64_t ticks) __attribute__((weak));
extern void BM_BootMark(const char *stage) __attribute__((weak));

void _cstart(unsigned long hartid, unsigned long dtb)
{
    uint64_t entry_ticks = timer_ticks();
    /* Zero .bss/.tbss 8 bytes at a time. These were byte-at-a-time loops, and
     * .bss is not small -- it carries 2 MB of worker-hart stacks plus 128 KB of
     * TLS blocks (dos_thread.c) on top of everything else. Under TCG that is
     * millions of single-byte stores executed before the first console output
     * exists, i.e. a long stretch of silence right after the OpenSBI banner that
     * is easily mistaken for the intermittent boot hang.
     *
     * Written as an explicit widened loop rather than memset(): this is a
     * freestanding build zeroing the very region memset's own state would live
     * in, and calling into libc here is asking for a chicken-and-egg problem.
     * Head/tail are handled bytewise so it stays correct for any alignment the
     * linker script happens to produce. */
    bm_zero_range(__bss_start, __bss_end);
    /* thread-local .tbss (main thread uses the TLS image in place) */
    bm_zero_range(__tbss_start, __tbss_end);

    boot_hartid = hartid;
    boot_dtb    = dtb;
    rv_detect_platform_caps();
    rv_check_vlen();
    rv_probe_isa();
    /* Before constructors: anything that timestamps during init must already be
     * dividing by the real timebase, not the QEMU-virt compile-time guess. */
    timer_init_from_dtb();
    ram_init_from_dtb();
    if (BM_BootBegin) BM_BootBegin(entry_ticks);
    if (BM_BootMark) BM_BootMark("runtime-ready");

    for (void (**fn)(void) = __init_array_start; fn < __init_array_end; ++fn)
        (*fn)();

    if (BM_BootMark) BM_BootMark("constructors-ready");
    kmain();

    sbi_shutdown();
}

void panic(const char *msg)
{
    static const char pfx[] = "\n*** PANIC: ";
    for (const char *p = pfx; *p; ++p) sbi_console_putchar(*p);
    while (msg && *msg) sbi_console_putchar(*msg++);
    sbi_console_putchar('\n');
    sbi_shutdown();
}

uint64_t timer_ticks(void)
{
    uint64_t t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

/* ---- timebase ---------------------------------------------------------- */

static unsigned long g_timer_hz = TIMEBASE_HZ;
static int g_timer_hz_from_dtb = 0;

unsigned long timer_hz(void) { return g_timer_hz; }

/* Whether g_timer_hz came from the device tree or is still the compile-time
 * fallback. Worth surfacing: QEMU virt's timebase and TIMEBASE_HZ are both
 * 10 MHz, so a silently broken FDT walk is indistinguishable from a working one
 * by looking at the reported frequency alone. */
int timer_hz_is_from_dtb(void) { return g_timer_hz_from_dtb; }

uint64_t timer_ns_from_ticks(uint64_t ticks)
{
    unsigned long hz = g_timer_hz;
    if (!hz) return 0;
    /* Split the quotient so the intermediate cannot overflow: ticks*1e9 wraps
     * uint64 past ~1.8e10 ticks (~30 min at 10 MHz), which a whole-run
     * accumulator reaches. (ticks%hz) < hz, so (ticks%hz)*1e9 stays small. */
    return (ticks / hz) * 1000000000ull + ((ticks % hz) * 1000000000ull) / hz;
}

/* ---- S-mode timer (Sstc when available, SBI TIME otherwise) ------------- */

#define CSR_STIMECMP    0x14d
#define SIE_STIE        (1UL << 5)
#define SSTATUS_SIE     (1UL << 1)

/* -1 until probed: 1 = Sstc present (csrw stimecmp), 0 = go through SBI.
 * Sstc is mandatory in RVA23S64 and QEMU's rva23s64 profile sets ext_sstc, so
 * this is the normal path -- it replaces the whole S->M->MMIO->S round trip an
 * `sbi_set_timer` ecall costs with one CSR write. */
static int g_have_sstc = -1;

static int timer_probe_sstc(void)
{
    int cpu;

    if (g_have_sstc >= 0)
        return g_have_sstc;

    g_have_sstc = 0;
    for (cpu = fdt_find_compatible(FDT_INVALID, "riscv");
         cpu != FDT_INVALID;
         cpu = fdt_find_compatible(cpu, "riscv")) {
        uint64_t reg = 0;
        if (fdt_reg(cpu, 0, &reg, NULL) != 0 || (unsigned long)reg != boot_hartid)
            continue;
        g_have_sstc = fdt_cpu_has_extension(cpu, "sstc");
        break;
    }
    return g_have_sstc;
}

void timer_arm(uint64_t deadline_ticks)
{
    if (timer_probe_sstc())
        asm volatile("csrw %0, %1" :: "i"(CSR_STIMECMP), "r"(deadline_ticks));
    else
        sbi_set_timer(deadline_ticks);
    asm volatile("csrs sie, %0" :: "r"(SIE_STIE) : "memory");
}

void timer_disarm(void)
{
    /* Mask STIE rather than only pushing the comparator out: with Sstc the
     * comparator is architecturally always live, so an unmasked far-future
     * deadline still eventually fires. */
    asm volatile("csrc sie, %0" :: "r"(SIE_STIE) : "memory");
    if (timer_probe_sstc())
        asm volatile("csrw %0, %1" :: "i"(CSR_STIMECMP), "r"(~0ull));
    else
        sbi_set_timer(~0ull);
}

void timer_arm_us(uint64_t us)
{
    timer_arm(timer_ticks() + timer_ticks_from_us(us));
}

void bm_wait_until(uint64_t deadline_ticks)
{
    /* `wfi` resumes on any interrupt ENABLED in sie, whether or not sstatus.SIE
     * lets the trap actually be taken -- so arming the timer first is what makes
     * this safe. A bare `wfi` with STIE clear and nothing armed halts the hart
     * until an interrupt that will never arrive; that is exactly the latent hang
     * in the old select()/usleep busy-wait loops.
     *
     * Under QEMU MTTCG this is the only instruction that yields the host core:
     * helper_wfi() sets EXCP_HLT and cpu_loop_exit()s, genuinely parking the vCPU
     * thread. Zawrs' wrs.nto is a pure permission check with no halt, and
     * Zihintpause decodes to a nop -- neither gives the host CPU back.
     *
     * SINGLE-SHOT, deliberately. This used to loop `while (now < deadline)`,
     * which re-armed and re-parked after every early wakeup and so ALWAYS slept
     * the full slice -- making the deadline the expected exit rather than the
     * lost-wakeup net it is documented to be, and adding the whole slice of
     * latency to every completion. Every caller already re-tests its own
     * predicate in an outer loop (the used ring, the generation counter), so
     * returning early on any enabled interrupt is both correct and the point. */
    unsigned long prev_sie;

    if (timer_ticks() >= deadline_ticks)
        return;

    /* Mask sstatus.SIE across the park. This is load-bearing, and it fixes the
     * long-standing "flaky early boot hang" that was previously written off as
     * something to re-run.
     *
     * With SIE set, the timer we just armed is delivered as a TRAP, and the trap
     * is taken with sepc pointing AT this `wfi` -- so `sret` re-executes it. But
     * os_handle_irq() disarms the timer on delivery (STIE cleared, stimecmp
     * pushed to ~0), so the re-executed `wfi` has nothing left that can wake it
     * and the hart halts forever. Control never returns here to re-arm, because
     * the re-execution happens at the instruction level; looping around the
     * `wfi` (as this function used to) does not help for the same reason.
     * Caught in the act on a hung guest: pc=wfi+4, sepc=wfi,
     * scause=5 (S-timer), stimecmp=~0, sie=SSIE|SEIE with STIE clear.
     *
     * `wfi` resumes on anything enabled in `sie` regardless of sstatus.SIE, so
     * masking costs nothing: the wake still happens, it just falls through
     * instead of trapping, and we disarm below. Any interrupt that came due is
     * still pending and is taken as soon as SIE is restored. */
    asm volatile("csrrc %0, sstatus, %1"
                 : "=r"(prev_sie) : "r"(SSTATUS_SIE) : "memory");

    timer_arm(deadline_ticks);
    asm volatile("wfi" ::: "memory");
    timer_disarm();

    /* Restore only what was there. Read-and-clear rather than assuming SIE was
     * set: a caller that had deliberately masked interrupts must not have them
     * silently re-enabled underneath it. */
    if (prev_sie & SSTATUS_SIE)
        asm volatile("csrs sstatus, %0" :: "r"(SSTATUS_SIE) : "memory");
}

static uint32_t rt_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Walk the FDT structure block for the first `timebase-frequency` property.
 * The existing DTB helper in irq.c only does a substring search, which cannot
 * retrieve a value. Without this the profiler divides by a hardcoded 10 MHz and
 * every reported millisecond is wrong by the ratio of the real timebase. */
void timer_init_from_dtb(void)
{
    const uint8_t *fdt = (const uint8_t *)(uintptr_t)boot_dtb;
    if (!fdt || rt_be32(fdt) != 0xd00dfeedu) return;

    uint32_t total = rt_be32(fdt + 4);
    uint32_t off_struct = rt_be32(fdt + 8);
    uint32_t off_strings = rt_be32(fdt + 12);
    if (total < 40 || total > (1024u * 1024u)) return;
    if (off_struct >= total || off_strings >= total) return;

    const uint8_t *p = fdt + off_struct;
    const uint8_t *end = fdt + total;

    while (p + 4 <= end) {
        uint32_t tok = rt_be32(p);
        p += 4;
        if (tok == 0x1u) {                       /* FDT_BEGIN_NODE: name, padded */
            while (p < end && *p) ++p;
            p = (const uint8_t *)(((uintptr_t)p + 4) & ~(uintptr_t)3);
        } else if (tok == 0x3u) {                /* FDT_PROP */
            if (p + 8 > end) return;
            uint32_t len = rt_be32(p);
            uint32_t nameoff = rt_be32(p + 4);
            p += 8;
            if (p + len > end) return;
            const char *nm = (const char *)(fdt + off_strings + nameoff);
            if (off_strings + nameoff < total && len == 4 &&
                nm[0] == 't' && !strcmp(nm, "timebase-frequency")) {
                uint32_t hz = rt_be32(p);
                if (hz >= 100000u && hz <= 1000000000u) {
                    g_timer_hz = hz;
                    g_timer_hz_from_dtb = 1;
                }
                return;
            }
            p += (len + 3u) & ~3u;
        } else if (tok == 0x9u) {                /* FDT_END */
            return;
        } else if (tok != 0x2u && tok != 0x4u) { /* not END_NODE / NOP: malformed */
            return;
        }
    }
}

/* ---- physical RAM window ------------------------------------------------ */

static uint64_t g_ram_base, g_ram_end;

/* Walk /memory nodes for the range containing the loaded image.
 *
 * RAM base and size were link-time constants (engine.ld: ORIGIN 0x80200000,
 * LENGTH 1920M) and nothing read /memory. Two consequences, in opposite
 * directions: on a platform with less RAM or a different DRAM base, _sbrk hands
 * out addresses that do not exist and the engine dies inside a map load with no
 * diagnostic; and at MEM=4G, RAM runs to 0x180000000 while the heap stops at
 * 0xF8200000, leaving 2176 MiB permanently unusable.
 *
 * The linker window's +/-2 GiB justification is about medany SYMBOL addressing
 * (__stack_top, __heap_end and friends resolve through AUIPC). It does not
 * constrain an arena discovered at run time and reached through an ordinary
 * pointer, which is what the heap is -- so the extra RAM is safe to hand out
 * even though no symbol may live there.
 *
 * QEMU places the DTB near the top of DRAM, so the usable end is clamped below
 * it; overwriting the blob would take the timebase and hart list with it. */
void ram_init_from_dtb(void)
{
    int node;

    for (node = fdt_root(); node != FDT_INVALID; node = fdt_next_node(node)) {
        uint32_t len;
        const char *dt = (const char *)fdt_getprop(node, "device_type", &len);
        unsigned i;

        if (!dt || strcmp(dt, "memory") != 0)
            continue;

        for (i = 0; i < 8; ++i) {
            uint64_t base = 0, size = 0;
            if (fdt_reg(node, i, &base, &size) != 0 || size == 0)
                break;
            /* the bank the image was loaded into */
            if ((uint64_t)(uintptr_t)&g_ram_base >= base &&
                (uint64_t)(uintptr_t)&g_ram_base <  base + size) {
                g_ram_base = base;
                g_ram_end  = base + size;
                if (boot_dtb >= base && boot_dtb < base + size)
                    g_ram_end = boot_dtb & ~(uint64_t)0xFFFFu;   /* stay below the blob */
                return;
            }
        }
    }
}

uint64_t ram_base(void) { return g_ram_base; }
uint64_t ram_end(void)  { return g_ram_end; }

uint64_t timer_ms(void)
{
    /* Must divide by the DISCOVERED timebase, not the compile-time fallback:
     * timer_ns_from_ticks() above already uses g_timer_hz, and two clocks that
     * disagree is worse than one clock that is wrong. */
    unsigned long khz = g_timer_hz / 1000UL;
    if (!khz) return 0;
    return timer_ticks() / khz;
}

uint64_t timer_us(void)
{
    unsigned long hz = g_timer_hz;
    uint64_t t = timer_ticks();
    if (!hz) return 0;
    return (t / hz) * 1000000ull + ((t % hz) * 1000000ull) / hz;
}

uint64_t timer_ticks_from_us(uint64_t us)
{
    unsigned long hz = g_timer_hz;
    /* (us * hz) / 1e6 without overflowing: hz <= 1e9 so us must stay < ~1.8e10. */
    return (us / 1000000ull) * hz + ((us % 1000000ull) * hz) / 1000000ull;
}
