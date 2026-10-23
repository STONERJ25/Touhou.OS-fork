/* Machine exit paths: QEMU debug-exit, ACPI-style poweroff, reset. */
#include "arch.h"
#include "cpu.h"
#include "kernel.h"
#include "acpi.h"
#include "fbcon.h"
#include "irq.h"

NORETURN void qemu_exit(int code) {
    /* The test harness reads the serial port over a TCP connection and QEMU leaves through exit(): on a loaded Windows host the connection
     * can be reset before the harness has read the last lines, the verdict ("UTEST PASS") among them, and a passing run is reported as
     * "QEMU exited (code 1)" (about 1 run in 60 for the utest suite while the host was busy, on main as well). Give the reader a moment. */
    udelay(300000);
    outb(0xF4, (u8)code);               /* isa-debug-exit: QEMU exits with (code << 1) | 1 */
    for (;;) hlt();
}

NORETURN void machine_poweroff(void) {
    cli();
    acpi_poweroff();                    /* real hardware: the FADT's PM1 control block and the _S5 sleep type */
    outw(0x604, 0x2000);                /* QEMU q35 / ACPI PM1a_CNT shortcut */
    outw(0xB004, 0x2000);               /* older QEMU / Bochs */
    outw(0x4004, 0x3400);               /* VirtualBox */
    /* Still running: none of the ways to switch off worked. Whoever is looking at a frozen desktop deserves to be told. */
    fbcon_force_on();
    printk("\n\x1b[1;33m[Gensokyo] the machine did not switch itself off (ACPI S5 failed). It is safe to hold the power button now.\x1b[0m\n");
    for (;;) hlt();
}

NORETURN void machine_reboot(void) {
    cli();
    acpi_reset();                       /* the FADT reset register, when the firmware describes one */
    outb(0xCF9, 0x02);                  /* PCI reset control: request, then full reset */
    outb(0xCF9, 0x06);
    for (int i = 0; i < 100000; i++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0xFE);                   /* pulse the CPU reset line through the keyboard controller */
    struct { u16 limit; u64 base; } PACKED empty = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(empty));      /* last resort: triple fault */
    for (;;) hlt();
}
