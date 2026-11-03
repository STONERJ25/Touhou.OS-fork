#include "kernel.h"
#include "serial.h"
#include "fbcon.h"
#include "cpu.h"
#include "ktime.h"

/* ---- in-memory kernel log (dmesg, /dev/kmsg) ---- */
#define KLOG_SIZE (128 * 1024)
static char g_klog[KLOG_SIZE];
static u64 g_klog_total;

static void klog_append(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) g_klog[g_klog_total++ % KLOG_SIZE] = s[i];
}

u64 klog_total(void) { return g_klog_total; }

size_t klog_read(u64 off, void *buf, size_t n) {
    u64 f = irq_save();
    u64 start = g_klog_total > KLOG_SIZE ? g_klog_total - KLOG_SIZE : 0;
    if (off < start) off = start;
    size_t got = 0;
    while (got < n && off < g_klog_total) ((char *)buf)[got++] = g_klog[off++ % KLOG_SIZE];
    irq_restore(f);
    return got;
}

/* Like klog_read(), but a reader that was too slow is told: *off is moved up to the oldest byte still in the ring (the caller can see
 * how much was overwritten by comparing it with what it passed in). Returns the bytes copied; *off is left at the start of them. */
size_t klog_read_clamped(u64 *off, void *buf, size_t n) {
    u64 f = irq_save();
    u64 start = g_klog_total > KLOG_SIZE ? g_klog_total - KLOG_SIZE : 0;
    if (*off < start) *off = start;
    u64 pos = *off;
    size_t got = 0;
    while (got < n && pos < g_klog_total) ((char *)buf)[got++] = g_klog[pos++ % KLOG_SIZE];
    irq_restore(f);
    return got;
}

/* ---- raw console: screen + serial, not logged ---- */
void console_write(const char *s, size_t n) {
    u64 f = irq_save();
    serial_write(s, n);
    fbcon_write(s, n);
    irq_restore(f);
}

static void log_and_print(const char *s, size_t n) {
    u64 f = irq_save();
    klog_append(s, n);
    serial_write(s, n);
    fbcon_write(s, n);
    irq_restore(f);
}

int vprintk(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return n;
    log_and_print(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
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
    klog_append(head, (size_t)hn);
    klog_append(buf, len);
    serial_write(head, (size_t)hn);
    fbcon_set_fg(tag_color(tag));
    fbcon_write(head, (size_t)hn);
    fbcon_reset_colors();
    serial_write(buf, len);
    fbcon_write(buf, len);
    irq_restore(f);
}
