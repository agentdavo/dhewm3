#ifndef DHEWM3_RV_IRQ_H
#define DHEWM3_RV_IRQ_H

#include <stdint.h>

typedef int (*os_irq_handler_fn)(uint32_t source, void *opaque);

void os_irq_init_from_dtb(void);
int  os_irq_route(uint32_t source, os_irq_handler_fn handler, void *opaque);
void os_irq_enable(uint32_t source);
void os_irq_disable(uint32_t source);
int  os_irq_available(void);
int  os_handle_irq(unsigned long scause);

#endif /* DHEWM3_RV_IRQ_H */
