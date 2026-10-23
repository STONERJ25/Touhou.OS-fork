/* Legacy 8259 PIC: remapped to vectors 32..47, plus the IRQ handler table. */
#include "irq.h"
#include "kernel.h"
#include "cpu.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static struct {
    irq_handler_t fn;
    void *ctx;
    u64 count;
} g_irq[16];

static u16 g_mask = 0xFFFF;

static void pic_write_mask(void) {
    outb(PIC1_DATA, g_mask & 0xFF);
    outb(PIC2_DATA, g_mask >> 8);
}

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();            /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_VECTOR_BASE); io_wait();        /* ICW2: vector offsets */
    outb(PIC2_DATA, IRQ_VECTOR_BASE + 8); io_wait();
    outb(PIC1_DATA, 4); io_wait();              /* ICW3: slave on IRQ2 */
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 1); io_wait();              /* ICW4: 8086 mode */
    outb(PIC2_DATA, 1); io_wait();
    g_mask = 0xFFFF & ~(1 << 2);                /* everything masked except the cascade */
    pic_write_mask();
}

void irq_register(int irq, irq_handler_t handler, void *ctx) {
    g_irq[irq].fn = handler;
    g_irq[irq].ctx = ctx;
}

void irq_mask(int irq) {
    u64 f = irq_save();
    g_mask |= (u16)(1 << irq);
    pic_write_mask();
    irq_restore(f);
}

void irq_unmask(int irq) {
    u64 f = irq_save();
    g_mask &= (u16)~(1 << irq);
    pic_write_mask();
    irq_restore(f);
}

static u16 pic_read_isr(void) {
    outb(PIC1_CMD, 0x0B);
    outb(PIC2_CMD, 0x0B);
    return (u16)(inb(PIC2_CMD) << 8 | inb(PIC1_CMD));
}

void irq_dispatch(int irq, TrapFrame *tf) {
    (void)tf;
    if (irq == 7 || irq == 15) {                /* possibly spurious: the ISR bit is clear if so */
        if (!(pic_read_isr() & (1 << irq))) {
            if (irq == 15) outb(PIC1_CMD, 0x20);
            return;
        }
    }
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);                       /* EOI first: the handler may end up switching tasks */
    g_irq[irq].count++;
    if (g_irq[irq].fn) g_irq[irq].fn(irq, g_irq[irq].ctx);
}

u64 irq_count(int irq) { return g_irq[irq].count; }
