#include "kernel.h"
#include "cpu.h"
#include "arch.h"
#include "task.h"

static int g_depth;

NORETURN void panic(const char *fmt, ...) {
    cli();
    if (++g_depth > 1) for (;;) hlt();
    va_list ap;
    va_start(ap, fmt);
    printk("\n\x1b[1;31m*** INCIDENT: KERNEL PANIC ***\x1b[0m\n");
    vprintk(fmt, ap);
    printk("\n");
    va_end(ap);
    printk("\x1b[90m%s\x1b[0m\n", incident_quip(255, 0, 0));
    task_print_current();
    dump_backtrace((u64)__builtin_return_address(0), (u64)__builtin_frame_address(0));
    for (;;) hlt();
}
