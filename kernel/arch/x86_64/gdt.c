/* GDT with kernel/user flat segments and the TSS; layout chosen so SYSCALL/SYSRET can use it (see STAR). */
#include "arch.h"
#include "kernel.h"

typedef struct PACKED {
    u16 limit;
    u64 base;
} DescPtr;

typedef struct PACKED {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iopb;
} Tss;

static u64 g_gdt[7] ALIGNED(16);
static Tss g_tss ALIGNED(16);
static u8 g_ist_stacks[3][8192] ALIGNED(16);    /* #DF, NMI, #MC */

void gdt_init(void) {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFULL;           /* kernel code, 64-bit */
    g_gdt[2] = 0x00CF92000000FFFFULL;           /* kernel data */
    g_gdt[3] = 0x00CFF2000000FFFFULL;           /* user data */
    g_gdt[4] = 0x00AFFA000000FFFFULL;           /* user code, 64-bit */

    u64 base = (u64)&g_tss;
    u64 limit = sizeof(Tss) - 1;
    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
               (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g_gdt[6] = base >> 32;

    memset(&g_tss, 0, sizeof g_tss);
    g_tss.iopb = sizeof(Tss);                   /* no I/O bitmap: user mode may not touch ports */
    for (int i = 0; i < 3; i++)
        g_tss.ist[i] = (u64)g_ist_stacks[i] + sizeof g_ist_stacks[i];

    DescPtr p = { sizeof g_gdt - 1, (u64)g_gdt };
    __asm__ volatile ("lgdt %0" : : "m"(p) : "memory");
    __asm__ volatile (
        "pushq $%c[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw $%c[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        : : [cs] "i"(KERNEL_CS), [ds] "i"(KERNEL_DS) : "rax", "memory");
    __asm__ volatile ("ltr %0" : : "r"((u16)TSS_SEL));
}

extern u64 g_syscall_kstack;

void tss_set_rsp0(u64 rsp0) {
    g_tss.rsp[0] = rsp0;
    g_syscall_kstack = rsp0;            /* SYSCALL does not switch stacks by itself */
}
