#ifndef K_IRQ_H
#define K_IRQ_H

#include "types.h"
#include "arch.h"

#define IRQ_VECTOR_BASE 32

typedef void (*irq_handler_t)(int irq, void *ctx);

void pic_init(void);
void lapic_virtual_wire(void);       /* make sure the local APIC passes the PIC's interrupts to the boot processor */
void irq_register(int irq, irq_handler_t handler, void *ctx);
void irq_mask(int irq);
void irq_unmask(int irq);
void irq_dispatch(int irq, TrapFrame *tf);
u64  irq_count(int irq);

/* PIT-driven system tick */
#define TIMER_HZ 1000
void     timer_init(void);
u64      timer_ticks(void);
u64      uptime_ms(void);
void     udelay(u32 us);                 /* busy wait, calibrated against the TSC after timer_init */

#endif
