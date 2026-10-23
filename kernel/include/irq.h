#ifndef K_IRQ_H
#define K_IRQ_H

#include "types.h"
#include "arch.h"

#define IRQ_VECTOR_BASE 32

typedef void (*irq_handler_t)(int irq, void *ctx);

void pic_init(void);
void lapic_virtual_wire(void);       /* make sure the local APIC passes the PIC's interrupts to the boot processor */
bool lapic_present(void);            /* the local APIC is enabled and mapped */
void lapic_eoi(void);                /* end of interrupt for a vector the local APIC delivered (not for 8259 ExtINT vectors) */
#define LAPIC_TIMER_VECTOR 0x40      /* the tick when the PIT does not deliver one (see timer_verify) */
bool lapic_timer_start(u32 vector, u32 hz);
void lapic_timer_irq(void);          /* trap.c calls this for LAPIC_TIMER_VECTOR */
void irq_register(int irq, irq_handler_t handler, void *ctx);
void irq_mask(int irq);
void irq_unmask(int irq);
void irq_dispatch(int irq, TrapFrame *tf);
u64  irq_count(int irq);

/* PIT-driven system tick */
#define TIMER_HZ 1000
void     timer_init(void);
void     timer_verify(void);             /* with interrupts on: no tick from the PIT within 200 ms -> switch to the local APIC timer */
bool     timer_is_lapic(void);
u64      timer_tsc_mhz(void);            /* TSC ticks per microsecond */
u64      timer_ticks(void);
u64      uptime_ms(void);
void     udelay(u32 us);                 /* busy wait, calibrated against the TSC after timer_init */

#endif
