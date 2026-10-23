/* Thin wrappers over privileged x86-64 instructions. */
#ifndef K_CPU_H
#define K_CPU_H

#include "types.h"

static inline void outb(u16 port, u8 v)  { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v;  __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

static inline u64 read_cr0(void) { u64 v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(u64 v) { __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr3(u64 v) { __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void write_cr4(u64 v) { __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory"); }
static inline void invlpg(const void *addr) { __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory"); }

static inline u64 read_rflags(void) { u64 v; __asm__ volatile ("pushfq; popq %0" : "=r"(v)); return v; }
static inline void cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile ("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile ("hlt"); }
static inline void pause(void) { __asm__ volatile ("pause"); }

static inline void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* Interrupt-flag save/restore, used by every lock in the kernel (single CPU for now). */
static inline u64 irq_save(void) {
    u64 f = read_rflags();
    cli();
    return f;
}
static inline void irq_restore(u64 f) {
    if (f & (1UL << 9)) sti();
}

#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_CSTAR       0xC0000083
#define MSR_SFMASK      0xC0000084
#define MSR_FS_BASE     0xC0000100
#define MSR_GS_BASE     0xC0000101
#define MSR_KERNEL_GS   0xC0000102
#define MSR_PAT         0x277
#define MSR_APIC_BASE   0x1B

#endif
