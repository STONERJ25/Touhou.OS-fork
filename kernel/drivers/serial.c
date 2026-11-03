/* 16550 UART on COM1: the kernel's first and last-resort output; input feeds the console tty. */
#include "serial.h"
#include "cpu.h"
#include "irq.h"
#include "tty.h"

#define COM1 0x3F8

static bool g_ready;

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* interrupts off */
    outb(COM1 + 3, 0x80);   /* DLAB on */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO on, cleared */
    outb(COM1 + 4, 0x0B);   /* DTR | RTS | OUT2 */
    /* Many PCs have no COM1 at all and the bus answers with whatever it likes: without this check every character of the
     * boot log could cost a long wait for a transmitter that does not exist. A real UART keeps what is written to its
     * scratch register and never reports all line-status bits set. */
    outb(COM1 + 7, 0xA5);
    g_ready = inb(COM1 + 7) == 0xA5 && inb(COM1 + 5) != 0xFF;
}

void serial_putc(char c) {
    if (!g_ready) return;
    if (c == '\n') serial_putc('\r');
    int i = 0;
    while (i < 200000 && !(inb(COM1 + 5) & 0x20)) i++;
    if (i == 200000) { g_ready = false; return; }       /* the transmitter never became ready: stop trying */
    outb(COM1, (u8)c);
}

void serial_write(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) serial_putc(s[i]);
}

int serial_getc(void) {
    if (!g_ready || !(inb(COM1 + 5) & 0x01)) return -1;
    return inb(COM1);
}

static void serial_irq(int irq, void *ctx) {
    (void)irq; (void)ctx;
    for (int i = 0; i < 64; i++) {
        int c = serial_getc();
        if (c < 0) break;
        u8 b = (u8)c;
        tty_receive(tty_console(), &b, 1);
    }
}

void serial_rx_init(void) {
    while (serial_getc() >= 0) { }
    irq_register(4, serial_irq, NULL);
    outb(COM1 + 1, 0x01);   /* interrupt on received data */
    irq_unmask(4);
}
