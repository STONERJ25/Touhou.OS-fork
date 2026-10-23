/* Exception handling, fault diagnostics and the common interrupt entry. */
#include "arch.h"
#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "task.h"
#include "fbcon.h"

static const char *const exc_name[32] = {
    "Divide Error (#DE)", "Debug (#DB)", "Non-Maskable Interrupt", "Breakpoint (#BP)",
    "Overflow (#OF)", "Bound Range Exceeded (#BR)", "Invalid Opcode (#UD)", "Device Not Available (#NM)",
    "Double Fault (#DF)", "Coprocessor Segment Overrun", "Invalid TSS (#TS)", "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)", "General Protection Fault (#GP)", "Page Fault (#PF)", "Reserved",
    "x87 Floating-Point (#MF)", "Alignment Check (#AC)", "Machine Check (#MC)", "SIMD Floating-Point (#XM)",
    "Virtualization (#VE)", "Control Protection (#CP)", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor Injection", "VMM Communication", "Security (#SX)", "Reserved",
};

/* Hooks installed by later subsystems (processes, demand paging). Return true if the fault was handled. */
bool (*g_user_fault_hook)(TrapFrame *tf, u64 cr2);
bool (*g_pagefault_hook)(TrapFrame *tf, u64 cr2);

static int g_panic_depth;

void dump_trapframe(const TrapFrame *tf) {
    u64 off = 0;
    const char *sym = ksym_lookup(tf->rip, &off);
    printk("rip=%016lx", tf->rip);
    if (sym) printk(" <%s+%#lx>", sym, off);
    printk("  cs=%04lx ss=%04lx rflags=%016lx\n", tf->cs, tf->ss, tf->rflags);
    printk("rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", tf->rax, tf->rbx, tf->rcx, tf->rdx);
    printk("rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx\n", tf->rsi, tf->rdi, tf->rbp, tf->rsp);
    printk("r8 =%016lx r9 =%016lx r10=%016lx r11=%016lx\n", tf->r8, tf->r9, tf->r10, tf->r11);
    printk("r12=%016lx r13=%016lx r14=%016lx r15=%016lx\n", tf->r12, tf->r13, tf->r14, tf->r15);
    printk("cr2=%016lx cr3=%016lx vec=%lu err=%#lx\n", read_cr2(), read_cr3(), tf->vector, tf->error);
}

NORETURN void panic_frame(const TrapFrame *tf, const char *fmt, ...) {
    cli();
    if (++g_panic_depth > 1) for (;;) hlt();
    fbcon_force_on();                   /* the window server may own the screen: a panic must be readable (there is no serial port on a PC) */
    va_list ap;
    va_start(ap, fmt);
    printk("\n\x1b[1;31m*** INCIDENT: KERNEL PANIC ***\x1b[0m\n");
    vprintk(fmt, ap);
    printk("\n");
    va_end(ap);
    printk("\x1b[90m%s\x1b[0m\n", incident_quip(tf->vector, read_cr2(), tf->error));
    task_print_current();
    dump_trapframe(tf);
    dump_backtrace(tf->rip, tf->rbp);
    for (;;) hlt();
}

static void describe_page_fault(const TrapFrame *tf, u64 cr2, char *buf, size_t n) {
    u64 e = tf->error;
    snprintf(buf, n, "%s %s of %#lx by %s%s%s", (e & 1) ? "protection violation on" : "not-present page on",
             (e & 16) ? "instruction fetch" : (e & 2) ? "write" : "read", cr2, (e & 4) ? "user" : "kernel",
             (e & 8) ? ", reserved bit set" : "", cr2 < 4096 ? " (null pointer?)" : "");
}

static void handle_exception(TrapFrame *tf) {
    u64 v = tf->vector;
    bool from_user = (tf->cs & 3) == 3;

    if (v == 14) {
        u64 cr2 = read_cr2();
        if (g_pagefault_hook && g_pagefault_hook(tf, cr2)) return;
        if (from_user && g_user_fault_hook && g_user_fault_hook(tf, cr2)) return;
        char why[160];
        describe_page_fault(tf, cr2, why, sizeof why);
        panic_frame(tf, "BOUNDARY VIOLATION: %s", why);
    }
    if (from_user && g_user_fault_hook && g_user_fault_hook(tf, 0)) return;
    if (v == 2) {
        klog("Eiki", "NMI received (rip=%#lx)", tf->rip);
        return;
    }
    panic_frame(tf, "EXCEPTION %lu: %s%s", v, exc_name[v], from_user ? " in user mode" : " in kernel mode");
}

void trap_handler(TrapFrame *tf) {
    u64 v = tf->vector;
    if (v < 32) {
        handle_exception(tf);
    } else if (v < 48) {
        irq_dispatch((int)v - 32, tf);
    } else if (v == LAPIC_TIMER_VECTOR) {
        lapic_timer_irq();                              /* the tick, when the PIT could not provide one */
    } else if (v == 0xFF) {
        /* local APIC spurious vector: by definition no end-of-interrupt */
    } else {
        /* Something the firmware left armed (APIC local source, an IOAPIC line). It is in service until acknowledged, and an
         * unacknowledged vector at 48 or above would hold back the PIC's timer and keyboard (vectors 32..47) for good. */
        static u32 stray;
        if (stray++ < 5) klog("Eiki", "unexpected interrupt vector %lu at rip=%#lx", v, tf->rip);      /* a storm must not drown the log */
        lapic_eoi();
    }
    task_on_trap_return(tf);
}
