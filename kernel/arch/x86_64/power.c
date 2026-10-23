/* Machine exit paths: QEMU debug-exit, ACPI poweroff, reset. */
#include "arch.h"
#include "cpu.h"

NORETURN void qemu_exit(int code) {
    outb(0xF4, (u8)code);               /* isa-debug-exit: QEMU exits with (code << 1) | 1 */
    for (;;) hlt();
}
