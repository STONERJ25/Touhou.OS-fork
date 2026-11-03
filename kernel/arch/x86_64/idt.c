#include "arch.h"
#include "kernel.h"

typedef struct PACKED {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} IdtEntry;

typedef struct PACKED {
    u16 limit;
    u64 base;
} DescPtr;

extern const u64 isr_stub_table[256];

static IdtEntry g_idt[256] ALIGNED(16);

static void set_gate(int v, u64 handler, u8 ist, u8 type) {
    g_idt[v].off_lo  = handler & 0xFFFF;
    g_idt[v].sel     = KERNEL_CS;
    g_idt[v].ist     = ist;
    g_idt[v].type    = type;
    g_idt[v].off_mid = (handler >> 16) & 0xFFFF;
    g_idt[v].off_hi  = (u32)(handler >> 32);
    g_idt[v].zero    = 0;
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) set_gate(v, isr_stub_table[v], 0, 0x8E);  /* present, DPL0, interrupt gate */
    set_gate(2,  isr_stub_table[2],  2, 0x8E);      /* NMI on its own stack */
    set_gate(8,  isr_stub_table[8],  1, 0x8E);      /* double fault on its own stack */
    set_gate(18, isr_stub_table[18], 3, 0x8E);      /* machine check */
    set_gate(3,  isr_stub_table[3],  0, 0xEE);      /* int3 callable from ring 3 */

    DescPtr p = { sizeof g_idt - 1, (u64)g_idt };
    __asm__ volatile ("lidt %0" : : "m"(p) : "memory");
}
