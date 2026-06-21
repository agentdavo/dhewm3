#include "boot/irq.h"
#include "boot/runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* QEMU virt PLIC layout. virtio-mmio slot N uses source N+1. */
#define PLIC_BASE           0x0c000000UL
#define PLIC_PRIORITY       0x000000UL
#define PLIC_ENABLE         0x002000UL
#define PLIC_ENABLE_STRIDE  0x80UL
#define PLIC_CONTEXT        0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL
#define PLIC_THRESHOLD      0x000UL
#define PLIC_CLAIM          0x004UL

/* QEMU virt S-mode APLIC/IMSIC layout for -machine virt,aia=aplic-imsic. */
#define APLIC_BASE          0x0d000000UL
#define APLIC_DOMAINCFG     0x0000UL
#define APLIC_DOMAINCFG_IE  (1u << 8)
#define APLIC_SOURCECFG_BASE 0x0004UL
#define APLIC_SOURCECFG_LEVEL_HIGH 0x6u
#define APLIC_CLRIPNUM      0x1ddcUL
#define APLIC_SETIENUM      0x1edcUL
#define APLIC_CLRIENUM      0x1fdcUL
#define APLIC_TARGET_BASE   0x3004UL
#define APLIC_TARGET_HART_IDX_SHIFT 18

#define CSR_SISELECT        0x150
#define CSR_SIREG           0x151
#define CSR_STOPEI          0x15c
#define IMSIC_EIDELIVERY    0x70
#define IMSIC_EITHRESHOLD   0x72
#define IMSIC_EIP0          0x80
#define IMSIC_EIE0          0xc0
#define IMSIC_TOPEI_ID_SHIFT 16

#define SCAUSE_IRQ_MASK     (1UL << (sizeof(unsigned long) * 8 - 1))
#define IRQ_SUPERVISOR_EXTERNAL 9UL
#define SIE_SEIE            (1UL << 9)
#define SSTATUS_SIE         (1UL << 1)

#define OS_IRQ_MAX_SOURCES  128u

enum irq_backend {
    IRQ_BACKEND_NONE = 0,
    IRQ_BACKEND_PLIC,
    IRQ_BACKEND_AIA
};

typedef struct irq_route {
    os_irq_handler_fn handler;
    void *opaque;
} irq_route_t;

static enum irq_backend g_backend;
static uintptr_t g_plic_enable;
static uintptr_t g_plic_context;
static uintptr_t g_aplic_base;
static irq_route_t g_routes[OS_IRQ_MAX_SOURCES];

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

static uint32_t be32_load(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int fdt_contains(const char *needle)
{
    const uint8_t *fdt = (const uint8_t *)(uintptr_t)boot_dtb;
    size_t nlen = strlen(needle);
    uint32_t total;

    if (!fdt || be32_load(fdt) != 0xd00dfeedu || nlen == 0)
        return 0;

    total = be32_load(fdt + 4);
    if (total < 40 || total > (1024u * 1024u))
        return 0;

    for (uint32_t i = 0; i + nlen < total; ++i) {
        if (memcmp(fdt + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

void os_irq_init_from_dtb(void)
{
    unsigned long context;

    if (g_backend != IRQ_BACKEND_NONE)
        return;

    if (fdt_contains("riscv,aplic") && fdt_contains("riscv,imsics")) {
        g_backend = IRQ_BACKEND_AIA;
        g_aplic_base = APLIC_BASE;
        reg_wr(g_aplic_base + APLIC_DOMAINCFG, APLIC_DOMAINCFG_IE);
        imsic_csr_write(IMSIC_EITHRESHOLD, 0);
        imsic_csr_write(IMSIC_EIDELIVERY, 1);
    } else if (fdt_contains("riscv,plic0") || fdt_contains("sifive,plic")) {
        g_backend = IRQ_BACKEND_PLIC;
        context = boot_hartid * 2UL + 1UL; /* QEMU virt S-mode context */
        g_plic_enable = PLIC_BASE + PLIC_ENABLE + context * PLIC_ENABLE_STRIDE;
        g_plic_context = PLIC_BASE + PLIC_CONTEXT + context * PLIC_CONTEXT_STRIDE;
        reg_wr(g_plic_context + PLIC_THRESHOLD, 0);
    } else {
        return;
    }

    asm volatile("csrs sie, %0" :: "r"(SIE_SEIE) : "memory");
    asm volatile("csrs sstatus, %0" :: "r"(SSTATUS_SIE) : "memory");
}

int os_irq_available(void)
{
    os_irq_init_from_dtb();
    return g_backend != IRQ_BACKEND_NONE;
}

int os_irq_route(uint32_t source, os_irq_handler_fn handler, void *opaque)
{
    if (source == 0 || source >= OS_IRQ_MAX_SOURCES || !handler)
        return -1;

    os_irq_init_from_dtb();
    if (g_backend == IRQ_BACKEND_NONE)
        return -1;

    os_irq_disable(source);
    g_routes[source].handler = handler;
    g_routes[source].opaque = opaque;

    if (g_backend == IRQ_BACKEND_PLIC) {
        reg_wr(PLIC_BASE + PLIC_PRIORITY + (uintptr_t)source * 4UL, 1);
    } else if (g_backend == IRQ_BACKEND_AIA) {
        reg_wr(g_aplic_base + APLIC_SOURCECFG_BASE + (uintptr_t)(source - 1u) * 4u,
               APLIC_SOURCECFG_LEVEL_HIGH);
        reg_wr(g_aplic_base + APLIC_TARGET_BASE + (uintptr_t)(source - 1u) * 4u,
               ((uint32_t)boot_hartid << APLIC_TARGET_HART_IDX_SHIFT) | source);
        reg_wr(g_aplic_base + APLIC_DOMAINCFG, APLIC_DOMAINCFG_IE);
    }

    return 0;
}

void os_irq_disable(uint32_t source)
{
    uintptr_t word;
    unsigned long mask, isel;

    if (source == 0 || source >= OS_IRQ_MAX_SOURCES)
        return;

    if (g_backend == IRQ_BACKEND_PLIC) {
        if (!g_plic_enable)
            return;
        word = g_plic_enable + (uintptr_t)(source / 32u) * 4u;
        reg_wr(word, reg_rd(word) & ~(1u << (source % 32u)));
    } else if (g_backend == IRQ_BACKEND_AIA) {
        reg_wr(g_aplic_base + APLIC_CLRIENUM, source);
        reg_wr(g_aplic_base + APLIC_CLRIPNUM, source);
        isel = imsic_eix_select(IMSIC_EIE0, source, &mask);
        imsic_csr_clear(isel, mask);
        isel = imsic_eix_select(IMSIC_EIP0, source, &mask);
        imsic_csr_clear(isel, mask);
    }
    __sync_synchronize();
}

void os_irq_enable(uint32_t source)
{
    uintptr_t word;
    unsigned long mask, isel;

    if (source == 0 || source >= OS_IRQ_MAX_SOURCES)
        return;

    __sync_synchronize();
    if (g_backend == IRQ_BACKEND_PLIC) {
        if (!g_plic_enable)
            return;
        word = g_plic_enable + (uintptr_t)(source / 32u) * 4u;
        reg_wr(word, reg_rd(word) | (1u << (source % 32u)));
    } else if (g_backend == IRQ_BACKEND_AIA) {
        isel = imsic_eix_select(IMSIC_EIE0, source, &mask);
        imsic_csr_set(isel, mask);
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

    if (cause != IRQ_SUPERVISOR_EXTERNAL)
        return 0;

    if (g_backend == IRQ_BACKEND_AIA) {
        topei = imsic_claim();
        source = (uint32_t)(topei >> IMSIC_TOPEI_ID_SHIFT);
        if (source == 0 || source >= OS_IRQ_MAX_SOURCES)
            return 0;
    } else if (g_backend == IRQ_BACKEND_PLIC && g_plic_context) {
        source = reg_rd(g_plic_context + PLIC_CLAIM);
        if (source == 0 || source >= OS_IRQ_MAX_SOURCES) {
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
