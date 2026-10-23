#include "kernel.h"
#include "serial.h"
#include "fbcon.h"
#include "cpu.h"

void console_write(const char *s, size_t n) {
    u64 f = irq_save();
    serial_write(s, n);
    fbcon_write(s, n);
    irq_restore(f);
}

int vprintk(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return n;
    console_write(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}

int printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

/* Each subsystem tag gets a stable colour on screen; the serial log stays plain text. */
static u32 tag_color(const char *tag) {
    static const u32 colors[] = { 0x729FCF, 0x8AE234, 0xFCE94F, 0xAD7FA8, 0x34E2E2, 0xFCAF3E, 0xEF9A9A, 0xB4F9C4 };
    u32 h = 5381;
    for (const char *p = tag; *p; p++) h = h * 33 + (u8)*p;
    return colors[h % ARRAY_SIZE(colors)];
}

void klog(const char *tag, const char *fmt, ...) {
    char buf[1024];
    char head[48];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t len = (size_t)n < sizeof buf - 1 ? (size_t)n : sizeof buf - 2;
    buf[len++] = '\n';
    int hn = snprintf(head, sizeof head, "[%s] ", tag);
    if (hn >= (int)sizeof head) hn = sizeof head - 1;

    u64 f = irq_save();
    serial_write(head, (size_t)hn);
    fbcon_set_fg(tag_color(tag));
    fbcon_write(head, (size_t)hn);
    fbcon_reset_colors();
    serial_write(buf, len);
    fbcon_write(buf, len);
    irq_restore(f);
}
