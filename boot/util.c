/* Loader utilities: serial/console logging and the few libc functions the compiler may call. */
#include "loader.h"

#define COM1 0x3F8

static inline void outb(UINT16 port, UINT8 v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline UINT8 inb(UINT16 port) { UINT8 v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);   /* FIFO on, cleared, 14-byte trigger */
    outb(COM1 + 4, 0x0B);   /* DTR, RTS, OUT2 */
}

static void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (UINT8)c);
}

void log_str(const char *s) {
    CHAR16 buf[128];
    UINTN n = 0;
    for (; *s; s++) {
        char c = *s;
        /* While boot services are alive the firmware console already mirrors to the serial port (OVMF),
         * so drive only ConOut then; after ExitBootServices fall back to the UART directly. */
        if (!(g_bs && g_st && g_st->ConOut)) serial_putc(c);
        if (g_bs && g_st && g_st->ConOut) {
            if (c == '\n') buf[n++] = '\r';
            buf[n++] = (CHAR16)(UINT8)c;
            if (n >= 124) {
                buf[n] = 0;
                g_st->ConOut->OutputString(g_st->ConOut, buf);
                n = 0;
            }
        }
    }
    if (n) {
        buf[n] = 0;
        g_st->ConOut->OutputString(g_st->ConOut, buf);
    }
}

void log_hex(UINT64 v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = 0;
    log_str(buf);
}

void log_dec(UINT64 v) {
    char buf[21];
    int i = 20;
    buf[i] = 0;
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    log_str(&buf[i]);
}

/* The PC speaker, driven straight from PIT channel 2: about 1 kHz, 0.1 to 0.2 s per beep depending on the CPU (the delay counts TSC
 * cycles, no timer service is needed, so it also works after ExitBootServices). Silent where there is no speaker. */
static inline UINT64 tsc_now(void) { UINT32 lo, hi; __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi)); return ((UINT64)hi << 32) | lo; }

static void tsc_wait(UINT64 cycles) {
    UINT64 t = tsc_now();
    while (tsc_now() - t < cycles) __asm__ volatile ("pause");
}

void pc_beep(int n) {
    for (int i = 0; i < n; i++) {
        outb(0x43, 0xB6);                       /* channel 2, lobyte/hibyte, square wave */
        outb(0x42, 0xA9);
        outb(0x42, 0x04);                       /* 1193 = 1.193182 MHz / 1 kHz */
        outb(0x61, (UINT8)(inb(0x61) | 3));     /* gate on, speaker on */
        tsc_wait(400000000ULL);
        outb(0x61, (UINT8)(inb(0x61) & ~3));
        tsc_wait(300000000ULL);
    }
    tsc_wait(600000000ULL);
}

/* IA32_EFER.NXE must be set before any page table entry uses the NX bit. */
void enable_nxe(void) {
    UINT32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1u << 11);
    __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080));
}

int guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

__attribute__((noreturn)) void loader_halt(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

/* Bodies are inline asm so the compiler cannot turn them back into calls to themselves. */
void *memcpy(void *dst, const void *src, size_t n) {
    void *r = dst;
    __asm__ volatile ("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return r;
}

void *memset(void *dst, int c, size_t n) {
    void *r = dst;
    __asm__ volatile ("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return r;
}

void *memmove(void *dst, const void *src, size_t n) {
    if ((UINT8 *)dst <= (const UINT8 *)src || (UINT8 *)dst >= (const UINT8 *)src + n)
        return memcpy(dst, src, n);
    UINT8 *d = (UINT8 *)dst + n - 1;
    const UINT8 *s = (const UINT8 *)src + n - 1;
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
