/* Machine exit paths: QEMU debug-exit, ACPI-style poweroff, reset. */
#include "arch.h"
#include "cpu.h"
#include "kernel.h"

NORETURN void qemu_exit(int code) {
    outb(0xF4, (u8)code);               /* isa-debug-exit: QEMU exits with (code << 1) | 1 */
    for (;;) hlt();
}

NORETURN void machine_poweroff(void) {
    cli();
    outw(0x604, 0x2000);                /* QEMU q35 / ACPI PM1a_CNT shortcut */
    outw(0xB004, 0x2000);               /* older QEMU / Bochs */
    outw(0x4004, 0x3400);               /* VirtualBox */
    for (;;) hlt();
}

NORETURN void machine_reboot(void) {
    cli();
    outb(0xCF9, 0x02);                  /* PCI reset control: request, then full reset */
    outb(0xCF9, 0x06);
    for (int i = 0; i < 100000; i++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0xFE);                   /* pulse the CPU reset line through the keyboard controller */
    struct { u16 limit; u64 base; } PACKED empty = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(empty));      /* last resort: triple fault */
    for (;;) hlt();
}
