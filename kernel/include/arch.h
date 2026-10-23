/* x86-64 architecture glue: segments, interrupts, trap frames. */
#ifndef K_ARCH_H
#define K_ARCH_H

#include "types.h"

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B      /* 0x18 | RPL 3 */
#define USER_CS   0x23      /* 0x20 | RPL 3 */
#define TSS_SEL   0x28

/* Pushed by isr.asm; layout must match exactly. */
typedef struct TrapFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 vector, error;
    u64 rip, cs, rflags, rsp, ss;
} TrapFrame;

#define RFLAGS_IF  (1UL << 9)
#define RFLAGS_TF  (1UL << 8)
#define RFLAGS_DF  (1UL << 10)

void gdt_init(void);
void tss_set_rsp0(u64 rsp0);
void idt_init(void);

/* Entry from isr.asm for every vector. */
void trap_handler(TrapFrame *tf);

/* Fault reporting and diagnostics (trap.c, backtrace.c) */
void dump_trapframe(const TrapFrame *tf);
void dump_backtrace(u64 rip, u64 rbp);
NORETURN void panic_frame(const TrapFrame *tf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Kernel symbol table embedded by the build (ksyms.c) */
const char *ksym_lookup(u64 addr, u64 *offset);

/* Exit QEMU through the isa-debug-exit device (status = (code << 1) | 1); no-op on real hardware. */
NORETURN void qemu_exit(int code);
NORETURN void machine_poweroff(void);
NORETURN void machine_reboot(void);

#endif
