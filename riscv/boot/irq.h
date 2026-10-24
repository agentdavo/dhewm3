#ifndef DHEWM3_RV_IRQ_H
#define DHEWM3_RV_IRQ_H

#include <stdint.h>

typedef int (*os_irq_handler_fn)(uint32_t source, void *opaque);

void os_irq_init_from_dtb(void);

/* Per-hart interrupt enable. os_irq_init_from_dtb() does the one-time global
 * discovery and then calls this for the calling hart; every other hart must call
 * it itself from its own entry path or it runs with SEIE clear forever. */
void os_irq_init_local(void);
int  os_irq_route(uint32_t source, os_irq_handler_fn handler, void *opaque);
void os_irq_enable(uint32_t source);
void os_irq_disable(uint32_t source);
int  os_irq_available(void);
int  os_handle_irq(unsigned long scause);

/* Wake another hart. Uses an IMSIC MSI store (no ecall) when the AIA MSI
 * backend is active, else an SBI IPI. Returns 0 on success. */
int  os_irq_ipi(unsigned long hartid);

#endif /* DHEWM3_RV_IRQ_H */
