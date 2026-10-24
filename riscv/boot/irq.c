#include "boot/irq.h"
#include "boot/runtime.h"
#include "boot/fdt.h"
#include "libc/libc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* QEMU virt PLIC layout. virtio-mmio slot N uses source N+1.
 * PLIC_BASE is the fallback only -- the real base comes from the DTB. */
#define PLIC_BASE           0x0c000000UL
#define PLIC_PRIORITY       0x000000UL
#define PLIC_ENABLE         0x002000UL
#define PLIC_ENABLE_STRIDE  0x80UL
#define PLIC_CONTEXT        0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL
#define PLIC_THRESHOLD      0x000UL
#define PLIC_CLAIM          0x004UL

/* S-mode APLIC/IMSIC. Bases are DTB-discovered; these are QEMU-virt fallbacks. */
#define APLIC_BASE          0x0d000000UL
#define IMSIC_S_BASE        0x28000000UL
#define APLIC_DOMAINCFG     0x0000UL
#define APLIC_DOMAINCFG_IE  (1u << 8)
#define APLIC_DOMAINCFG_DM  (1u << 2)   /* 1 = MSI delivery, 0 = direct */
#define APLIC_SOURCECFG_BASE 0x0004UL
#define APLIC_SOURCECFG_LEVEL_HIGH 0x6u
#define APLIC_CLRIPNUM      0x1ddcUL
#define APLIC_SETIENUM      0x1edcUL
#define APLIC_CLRIENUM      0x1fdcUL
#define APLIC_TARGET_BASE   0x3004UL
#define APLIC_TARGET_HART_IDX_SHIFT 18

/* Per-hart Interrupt Delivery Control block, used only in DIRECT mode.
 * In MSI mode the APLIC has no IDC at all -- delivery goes through the hart's
 * IMSIC file instead, which is why the two modes need separate backends rather
 * than a flag. */
#define APLIC_IDC_BASE      0x4000UL
#define APLIC_IDC_STRIDE    32UL
#define APLIC_IDC_IDELIVERY  0x00UL
#define APLIC_IDC_IFORCE     0x04UL
#define APLIC_IDC_ITHRESHOLD 0x08UL
#define APLIC_IDC_TOPI       0x18UL
#define APLIC_IDC_CLAIMI     0x1cUL
#define APLIC_IDC_TOPI_ID_SHIFT 16

/* IMSIC MMIO: one 4 KiB page per hart; a 32-bit store of an EIID to a page
 * makes that interrupt pending on that hart. QEMU's riscv_imsic_write() accepts
 * it from ANY initiator, so an S-mode hart can ring another hart directly --
 * an IPI with zero ecalls, against ~an S->M->S round trip for sbi_send_ipi. */
#define IMSIC_PAGE_SHIFT    12

/* EIID allocation in MSI mode. EIID 0 is reserved by the architecture (a write
 * of 0 is ignored), so 1 is the first usable and is kept for the IPI; devices
 * start at 8 to leave room for further system EIIDs without renumbering. */
#define IRQ_EIID_IPI        1u
#define IRQ_EIID_DEV_BASE   8u

#define CSR_SISELECT        0x150
#define CSR_SIREG           0x151
#define CSR_STOPEI          0x15c
#define IMSIC_EIDELIVERY    0x70
#define IMSIC_EITHRESHOLD   0x72
#define IMSIC_EIP0          0x80
#define IMSIC_EIE0          0xc0
#define IMSIC_TOPEI_ID_SHIFT 16

#define SCAUSE_IRQ_MASK     (1UL << (sizeof(unsigned long) * 8 - 1))
#define IRQ_SUPERVISOR_SOFTWARE 1UL
#define IRQ_SUPERVISOR_TIMER    5UL
#define IRQ_SUPERVISOR_EXTERNAL 9UL
#define SIE_SSIE            (1UL << 1)
#define SIE_SEIE            (1UL << 9)
#define SIP_SSIP            (1UL << 1)
#define SSTATUS_SIE         (1UL << 1)

/* Routing-table capacity. The number of sources the interrupt controller
 * actually implements is discovered from the DTB (`riscv,ndev`) into
 * g_nsources; QEMU virt implements 96 (VIRT_IRQCHIP_NUM_SOURCES), so the old
 * unconditional 128 let os_irq_route() program enable/priority words past the
 * end of the implemented arrays. */
#define OS_IRQ_MAX_SOURCES  128u
#define OS_IRQ_DEFAULT_SOURCES 96u

enum irq_backend {
    IRQ_BACKEND_NONE = 0,
    IRQ_BACKEND_PLIC,
    IRQ_BACKEND_APLIC_DIRECT,   /* APLIC drives the hart's IDC block directly  */
    IRQ_BACKEND_APLIC_MSI       /* APLIC sends MSIs into per-hart IMSIC files  */
};

typedef struct irq_route {
    os_irq_handler_fn handler;
    void *opaque;
} irq_route_t;

static enum irq_backend g_backend;
static uintptr_t g_plic_base;
static uintptr_t g_plic_enable;
static uintptr_t g_plic_context;
static uintptr_t g_aplic_base;
static uintptr_t g_imsic_base;
static uint32_t  g_aplic_hart_idx;   /* index within the APLIC domain, not a hartid */

/* hartid -> IMSIC page / APLIC IDC index. Both are POSITIONS in the
 * controller's own interrupts-extended list, not hartids: a machine whose harts
 * do not start at 0, or that has more than one socket, makes the two differ. */
#define IRQ_MAX_HARTS 64
static uint8_t g_hart_idx_valid[IRQ_MAX_HARTS];
static uint8_t g_hart_idx[IRQ_MAX_HARTS];
static uint32_t  g_nsources = OS_IRQ_DEFAULT_SOURCES;
static irq_route_t g_routes[OS_IRQ_MAX_SOURCES];

/* One place to ask "may I touch this source?", so the implemented-source count
 * cannot be forgotten at one of the five call sites that index by source. */
static inline int src_ok(uint32_t source)
{
    return source != 0u && source < OS_IRQ_MAX_SOURCES && source < g_nsources;
}

/* sscratch carries this hart's id (start.S / dos_hart_entry.S). boot_hartid is
 * the BOOT hart and is wrong on every worker. */
static inline unsigned long current_hartid(void)
{
    unsigned long h;
    asm volatile("csrr %0, sscratch" : "=r"(h));
    return h;
}

static inline uint32_t reg_rd(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void reg_wr(uintptr_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline void imsic_csr_write(unsigned long isel, unsigned long v)
{
    asm volatile("csrw %0, %1; csrw %2, %3"
                 :
                 : "i"(CSR_SISELECT), "r"(isel), "i"(CSR_SIREG), "r"(v)
                 : "memory");
}

static inline void imsic_csr_set(unsigned long isel, unsigned long mask)
{
    asm volatile("csrw %0, %1; csrs %2, %3"
                 :
                 : "i"(CSR_SISELECT), "r"(isel), "i"(CSR_SIREG), "r"(mask)
                 : "memory");
}

static inline void imsic_csr_clear(unsigned long isel, unsigned long mask)
{
    asm volatile("csrw %0, %1; csrc %2, %3"
                 :
                 : "i"(CSR_SISELECT), "r"(isel), "i"(CSR_SIREG), "r"(mask)
                 : "memory");
}

static inline unsigned long imsic_claim(void)
{
    unsigned long v;
    asm volatile("csrrw %0, %1, zero"
                 : "=r"(v)
                 : "i"(CSR_STOPEI)
                 : "memory");
    return v;
}

static unsigned long imsic_eix_select(unsigned long base, uint32_t eiid,
                                      unsigned long *mask)
{
    unsigned long bits = sizeof(unsigned long) * 8UL;
    unsigned long word = eiid / bits;
    *mask = 1UL << (eiid % bits);
    return base + word * (bits / 32UL);
}

/* phandle of the per-hart interrupt controller (`riscv,cpu-intc`) belonging to
 * hartid, or 0. This is the identity a PLIC/APLIC node names in its
 * `interrupts-extended` list, so it is how a hart finds its own context. */
static uint32_t cpu_intc_phandle(unsigned long hartid)
{
    int n = fdt_find_compatible(FDT_INVALID, "riscv,cpu-intc");

    while (n != FDT_INVALID) {
        int cpu = fdt_parent(n);
        uint64_t reg = 0;
        if (cpu != FDT_INVALID && fdt_reg(cpu, 0, &reg, NULL) == 0 &&
            (unsigned long)reg == hartid)
            return fdt_getprop_u32(n, "phandle", 0);
        n = fdt_find_compatible(n, "riscv,cpu-intc");
    }
    return 0;
}

/* Index into a PLIC/APLIC node's `interrupts-extended` of the entry naming
 * (phandle, cause). For a PLIC that index IS the context number: QEMU virt
 * numbers contexts by position within the node, relative to the socket's
 * base_hartid, and gives each socket its own PLIC.
 *
 * The old `boot_hartid * 2 + 1` is only correct for a single socket whose harts
 * start at 0. With `-smp 8,sockets=2`, or on any SoC with a management core at
 * hartid 0 and application harts above it, it programs a DIFFERENT hart's
 * context: the enable bit is set somewhere nobody claims, vblk_wait_used() takes
 * the g_irq_routed path, and the boot hangs mid-asset-load. */
static int intc_context_index(int ctrl, uint32_t phandle, uint32_t cause)
{
    uint32_t len;
    const uint8_t *ie = (const uint8_t *)fdt_getprop(ctrl, "interrupts-extended", &len);
    uint32_t i;

    if (!ie || !phandle) return -1;
    for (i = 0; i + 8u <= len; i += 8u) {
        if (fdt_be32(ie + i) == phandle && fdt_be32(ie + i + 4u) == cause)
            return (int)(i / 8u);
    }
    return -1;
}

static int plic_init_from_dtb(void)
{
    int plic = fdt_find_compatible(FDT_INVALID, "riscv,plic0");
    uint64_t base = 0;
    uint32_t phandle;
    int context;

    if (plic == FDT_INVALID)
        plic = fdt_find_compatible(FDT_INVALID, "sifive,plic-1.0.0");
    if (plic == FDT_INVALID)
        return 0;

    if (fdt_reg(plic, 0, &base, NULL) != 0 || base == 0)
        base = PLIC_BASE;

    phandle = cpu_intc_phandle(boot_hartid);
    context = intc_context_index(plic, phandle, IRQ_SUPERVISOR_EXTERNAL);
    if (context < 0)
        context = (int)(boot_hartid * 2UL + 1UL);   /* QEMU-virt shape, last resort */

    g_nsources = fdt_getprop_u32(plic, "riscv,ndev", OS_IRQ_DEFAULT_SOURCES) + 1u;
    if (g_nsources > OS_IRQ_MAX_SOURCES) g_nsources = OS_IRQ_MAX_SOURCES;

    g_plic_base = (uintptr_t)base;
    g_plic_enable = g_plic_base + PLIC_ENABLE + (uintptr_t)context * PLIC_ENABLE_STRIDE;
    g_plic_context = g_plic_base + PLIC_CONTEXT + (uintptr_t)context * PLIC_CONTEXT_STRIDE;
    reg_wr(g_plic_context + PLIC_THRESHOLD, 0);
    g_backend = IRQ_BACKEND_PLIC;
    return 1;
}

/* Build hartid -> controller-relative index from a controller's own
 * interrupts-extended list. Each entry is (intc_phandle, cause); the position of
 * a hart's S-mode entry is its IDC index (direct) or IMSIC page (MSI). */
static void build_hart_index(int ctrl, unsigned stride)
{
    uint32_t len;
    const uint8_t *ie = (const uint8_t *)fdt_getprop(ctrl, "interrupts-extended", &len);
    uint32_t i;
    unsigned pos = 0;

    if (!ie) return;
    for (i = 0; i + 8u <= len; i += 8u) {
        uint32_t phandle = fdt_be32(ie + i);
        uint32_t cause = fdt_be32(ie + i + 4u);
        int n;
        if (cause != IRQ_SUPERVISOR_EXTERNAL) continue;
        /* map this phandle back to a hartid */
        for (n = fdt_find_compatible(FDT_INVALID, "riscv,cpu-intc");
             n != FDT_INVALID;
             n = fdt_find_compatible(n, "riscv,cpu-intc")) {
            uint64_t reg = 0;
            int cpu;
            if (fdt_getprop_u32(n, "phandle", 0) != phandle) continue;
            cpu = fdt_parent(n);
            if (cpu != FDT_INVALID && fdt_reg(cpu, 0, &reg, NULL) == 0 &&
                reg < IRQ_MAX_HARTS) {
                g_hart_idx[(unsigned long)reg] = (uint8_t)pos;
                g_hart_idx_valid[(unsigned long)reg] = 1;
            }
            break;
        }
        pos += stride;
    }
}

static int hart_index(unsigned long hartid)
{
    if (hartid < IRQ_MAX_HARTS && g_hart_idx_valid[hartid])
        return g_hart_idx[hartid];
    return (int)hartid;          /* dense-from-zero fallback */
}

static int aplic_init_from_dtb(void)
{
    int aplic = fdt_find_compatible(FDT_INVALID, "riscv,aplic");
    int imsic = fdt_find_compatible(FDT_INVALID, "riscv,imsics");
    uint64_t base = 0;

    if (aplic == FDT_INVALID)
        return 0;
    if (fdt_reg(aplic, 0, &base, NULL) != 0 || base == 0)
        base = APLIC_BASE;
    g_aplic_base = (uintptr_t)base;

    g_nsources = fdt_getprop_u32(aplic, "riscv,num-sources", OS_IRQ_DEFAULT_SOURCES) + 1u;
    if (g_nsources > OS_IRQ_MAX_SOURCES) g_nsources = OS_IRQ_MAX_SOURCES;

    build_hart_index(aplic, 1u);
    g_aplic_hart_idx = (uint32_t)hart_index(boot_hartid);

    /* `msi-parent` present => the APLIC forwards to IMSIC files (MSI mode);
     * absent => it drives each hart's IDC block directly. QEMU decides the same
     * way its DOMAINCFG.DM bit does. */
    if (imsic != FDT_INVALID && fdt_getprop(aplic, "msi-parent", NULL)) {
        uint64_t ibase = 0;
        if (fdt_reg(imsic, 0, &ibase, NULL) != 0 || ibase == 0)
            ibase = IMSIC_S_BASE;
        g_imsic_base = (uintptr_t)ibase;
        build_hart_index(imsic, 1u);     /* IMSIC page index, same shape */
        reg_wr(g_aplic_base + APLIC_DOMAINCFG,
               APLIC_DOMAINCFG_IE | APLIC_DOMAINCFG_DM);
        g_backend = IRQ_BACKEND_APLIC_MSI;
    } else {
        reg_wr(g_aplic_base + APLIC_DOMAINCFG, APLIC_DOMAINCFG_IE);
        g_backend = IRQ_BACKEND_APLIC_DIRECT;
    }
    return 1;
}

/* Per-hart delivery enable. Direct mode programs the hart's IDC block; MSI mode
 * programs its IMSIC file through CSRs. Both are per-hart state, so this runs on
 * every hart, not just the boot hart. */
static void aplic_init_local(void)
{
    if (g_backend == IRQ_BACKEND_APLIC_MSI) {
        imsic_csr_write(IMSIC_EITHRESHOLD, 0);   /* 0 = no threshold */
        imsic_csr_write(IMSIC_EIDELIVERY, 1);
        /* The IPI EIID must be enabled in this hart's file or a wake MSI is
         * recorded pending and never delivered. */
        {
            unsigned long mask, isel = imsic_eix_select(IMSIC_EIE0, IRQ_EIID_IPI, &mask);
            imsic_csr_set(isel, mask);
        }
    } else if (g_backend == IRQ_BACKEND_APLIC_DIRECT) {
        uintptr_t idc = g_aplic_base + APLIC_IDC_BASE +
                        (uintptr_t)hart_index(current_hartid()) * APLIC_IDC_STRIDE;
        reg_wr(idc + APLIC_IDC_ITHRESHOLD, 0);   /* 0 = deliver every priority */
        reg_wr(idc + APLIC_IDC_IDELIVERY, 1);
    }
}

/* Weak so boot/irq.c still links into the small test harnesses that have no
 * printf. The engine build overrides nothing -- libc/libc.h provides printf. */
static void os_irq_report(const char *what)
{
    printf("[doomos-irq] backend=%s hart=%lu sources=%u\n",
           what, boot_hartid, g_nsources);
}

void os_irq_init_from_dtb(void)
{
    if (g_backend != IRQ_BACKEND_NONE)
        return;

    if (!aplic_init_from_dtb() && !plic_init_from_dtb()) {
        /* Say so. With `-machine virt,aia=aplic` QEMU emits an APLIC and NO PLIC
         * node, and APLIC direct mode is not implemented here -- so no backend
         * can be driven and every device falls back to spin-polling its
         * virtqueue. That is a large, silent performance cliff: it used to look
         * identical to a working boot, just slower. */
        os_irq_report("none (no usable controller; devices will spin-poll)");
        return;
    }

    os_irq_report(g_backend == IRQ_BACKEND_APLIC_MSI    ? "aplic+imsic (msi)" :
                  g_backend == IRQ_BACKEND_APLIC_DIRECT ? "aplic (direct)" : "plic");
    os_irq_init_local();
}

/* Per-hart half of the setup. os_irq_init_from_dtb() early-returns once the
 * backend is chosen, so without this the IMSIC delivery CSRs and the sie/sstatus
 * writes only ever happened on the boot hart -- every worker hart ran with
 * SEIE clear and could never take an external interrupt. */
void os_irq_init_local(void)
{
    /* Software and timer interrupts do not go through the external interrupt
     * controller at all, so this must run even when no PLIC/APLIC was found --
     * IPI wakeups and the Sstc timer still work on a machine with no usable
     * external controller. */
    aplic_init_local();
    /* SSIE as well as SEIE: `wfi` only resumes on interrupts ENABLED in sie, so
     * without SSIE an IPI raises sip.SSIP and the target stays parked. That is
     * the wake path the raster workers depend on. */
    asm volatile("csrs sie, %0" :: "r"(SIE_SEIE | SIE_SSIE) : "memory");
    asm volatile("csrs sstatus, %0" :: "r"(SSTATUS_SIE) : "memory");
}

/* Wake `hartid`.
 *
 * With APLIC+IMSIC this is a SINGLE 32-BIT STORE and no ecall: QEMU's
 * riscv_imsic_write() accepts an MSI from any initiator, so one S-mode hart can
 * make an EIID pending on another directly. The SBI path costs an ecall plus an
 * S->M->S privilege round trip, and it sits on the raster wake path (once per
 * parked worker per draw dispatch), so the difference is on the hot path rather
 * than at init.
 *
 * Falls back to SBI IPI whenever there is no IMSIC. Returns 0 on success. */
int os_irq_ipi(unsigned long hartid)
{
    if (g_backend == IRQ_BACKEND_APLIC_MSI && g_imsic_base) {
        uintptr_t page = g_imsic_base +
                         ((uintptr_t)hart_index(hartid) << IMSIC_PAGE_SHIFT);
        /* Order our prior memory writes before the doorbell: the woken hart
         * reads shared state immediately, and `fence rw,rw` would not order a
         * memory write before this I/O write. */
        asm volatile("fence w,o" ::: "memory");
        *(volatile uint32_t *)page = IRQ_EIID_IPI;
        return 0;
    }
    return (int)sbi_send_ipi_hart(hartid);
}

int os_irq_available(void)
{
    os_irq_init_from_dtb();
    return g_backend != IRQ_BACKEND_NONE;
}

int os_irq_route(uint32_t source, os_irq_handler_fn handler, void *opaque)
{
    if (!src_ok(source) || !handler)
        return -1;

    os_irq_init_from_dtb();
    if (g_backend == IRQ_BACKEND_NONE)
        return -1;

    os_irq_disable(source);
    g_routes[source].handler = handler;
    g_routes[source].opaque = opaque;

    if (g_backend == IRQ_BACKEND_PLIC) {
        reg_wr(g_plic_base + PLIC_PRIORITY + (uintptr_t)source * 4UL, 1);
    } else {
        uint32_t target;
        reg_wr(g_aplic_base + APLIC_SOURCECFG_BASE + (uintptr_t)(source - 1u) * 4u,
               APLIC_SOURCECFG_LEVEL_HIGH);
        /* The APLIC target field takes a hart INDEX within the interrupt
         * domain, not a raw hartid -- QEMU's riscv_aplic.c resolves it relative
         * to the domain's first hart. They coincide only when hartids are dense
         * from 0 and there is a single socket.
         *
         * The LOW bits mean different things per mode, which is the one place
         * the two APLIC backends genuinely diverge:
         *   MSI    -> EIID, delivered into the target hart's IMSIC file
         *   direct -> IPRIO, the priority the IDC block arbitrates on
         * Writing an EIID into a direct-mode target lands in the priority field
         * and mis-prioritises silently rather than failing, which is why these
         * are separate backends and not a flag. */
        if (g_backend == IRQ_BACKEND_APLIC_MSI)
            target = ((uint32_t)g_aplic_hart_idx << APLIC_TARGET_HART_IDX_SHIFT) |
                     (source + IRQ_EIID_DEV_BASE);
        else
            target = ((uint32_t)g_aplic_hart_idx << APLIC_TARGET_HART_IDX_SHIFT) | 1u;
        reg_wr(g_aplic_base + APLIC_TARGET_BASE + (uintptr_t)(source - 1u) * 4u, target);
    }

    return 0;
}

void os_irq_disable(uint32_t source)
{
    uintptr_t word;
    unsigned long mask, isel;

    if (!src_ok(source))
        return;

    if (g_backend == IRQ_BACKEND_PLIC) {
        if (!g_plic_enable)
            return;
        word = g_plic_enable + (uintptr_t)(source / 32u) * 4u;
        reg_wr(word, reg_rd(word) & ~(1u << (source % 32u)));
    } else if (g_backend != IRQ_BACKEND_NONE) {
        reg_wr(g_aplic_base + APLIC_CLRIENUM, source);
        reg_wr(g_aplic_base + APLIC_CLRIPNUM, source);
        if (g_backend == IRQ_BACKEND_APLIC_MSI) {
            uint32_t eiid = source + IRQ_EIID_DEV_BASE;
            isel = imsic_eix_select(IMSIC_EIE0, eiid, &mask);
            imsic_csr_clear(isel, mask);
            isel = imsic_eix_select(IMSIC_EIP0, eiid, &mask);
            imsic_csr_clear(isel, mask);
        }
    }
    __sync_synchronize();
}

void os_irq_enable(uint32_t source)
{
    uintptr_t word;
    unsigned long mask, isel;

    if (!src_ok(source))
        return;

    __sync_synchronize();
    if (g_backend == IRQ_BACKEND_PLIC) {
        if (!g_plic_enable)
            return;
        word = g_plic_enable + (uintptr_t)(source / 32u) * 4u;
        reg_wr(word, reg_rd(word) | (1u << (source % 32u)));
    } else if (g_backend != IRQ_BACKEND_NONE) {
        if (g_backend == IRQ_BACKEND_APLIC_MSI) {
            isel = imsic_eix_select(IMSIC_EIE0, source + IRQ_EIID_DEV_BASE, &mask);
            imsic_csr_set(isel, mask);
        }
        reg_wr(g_aplic_base + APLIC_SETIENUM, source);
    }
}

int os_handle_irq(unsigned long scause)
{
    unsigned long cause = scause & ~SCAUSE_IRQ_MASK;
    unsigned long topei;
    uint32_t source;
    irq_route_t *route;
    int handled;

    /* Software interrupt (IPI). Acknowledge and resume: the wakeup itself is the
     * whole payload -- the target re-reads shared state after sret. Returning 0
     * here used to drop through to trap.c's FATAL TRAP path and power the
     * machine off on the first IPI a hart ever received. */
    if (cause == IRQ_SUPERVISOR_SOFTWARE) {
        asm volatile("csrc sip, %0" :: "r"(SIP_SSIP) : "memory");
        return 1;
    }

    /* Timer interrupt. Disarm so it does not re-fire immediately; whoever armed
     * it polls its own deadline after waking. */
    if (cause == IRQ_SUPERVISOR_TIMER) {
        timer_disarm();
        return 1;
    }

    if (cause != IRQ_SUPERVISOR_EXTERNAL)
        return 0;

    if (g_backend == IRQ_BACKEND_APLIC_MSI) {
        uint32_t eiid;
        topei = imsic_claim();                 /* stopei: read + clear */
        eiid = (uint32_t)(topei >> IMSIC_TOPEI_ID_SHIFT);
        if (eiid == 0)
            return 0;
        /* An IPI carries no payload -- being woken IS the message, and the
         * target re-reads shared state after sret. */
        if (eiid == IRQ_EIID_IPI)
            return 1;
        if (eiid < IRQ_EIID_DEV_BASE)
            return 0;
        source = eiid - IRQ_EIID_DEV_BASE;
        if (!src_ok(source))
            return 0;
    } else if (g_backend == IRQ_BACKEND_APLIC_DIRECT) {
        /* claimi returns TOPI and atomically clears the pending bit. */
        uintptr_t idc = g_aplic_base + APLIC_IDC_BASE +
                        (uintptr_t)hart_index(current_hartid()) * APLIC_IDC_STRIDE;
        uint32_t topi = reg_rd(idc + APLIC_IDC_CLAIMI);
        source = topi >> APLIC_IDC_TOPI_ID_SHIFT;
        if (!src_ok(source))
            return 0;
    } else if (g_backend == IRQ_BACKEND_PLIC && g_plic_context) {
        source = reg_rd(g_plic_context + PLIC_CLAIM);
        if (!src_ok(source)) {
            if (source)
                reg_wr(g_plic_context + PLIC_CLAIM, source);
            return 0;
        }
    } else {
        return 0;
    }

    route = &g_routes[source];
    handled = route->handler ? route->handler(source, route->opaque) : 0;

    if (g_backend == IRQ_BACKEND_PLIC)
        reg_wr(g_plic_context + PLIC_CLAIM, source);

    return handled;
}
