#include "kernel.h"
#include "cpu.h"

NORETURN void panic(const char *fmt, ...) {
    cli();
    va_list ap;
    va_start(ap, fmt);
    printk("\n\x1b[1;31m*** INCIDENT: KERNEL PANIC ***\x1b[0m\n");
    vprintk(fmt, ap);
    printk("\n");
    va_end(ap);
    for (;;) hlt();
}
