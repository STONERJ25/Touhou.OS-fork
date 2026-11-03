/* Terminals: line discipline shared by the console, serial and (later) pseudo-terminals. */
#ifndef K_TTY_H
#define K_TTY_H

#include "types.h"
#include "task.h"
#include "abi.h"

#define TTY_INBUF 4096          /* power of two */
#define TTY_LINE  1024

typedef struct Tty Tty;

typedef struct TtyOps {
    void (*write)(Tty *t, const u8 *buf, size_t n);     /* output: screen, UART, pty master queue */
} TtyOps;

struct Tty {
    const TtyOps *ops;
    void *priv;
    char name[16];
    struct gtermios termios;
    struct gwinsize winsize;
    u8  in[TTY_INBUF];
    u32 in_head, in_tail;
    u8  line[TTY_LINE];
    u32 line_len;
    bool eof;
    WaitQueue rd_wq;
    int fg_pgid;
    int session;
};

void    tty_init(Tty *t, const TtyOps *ops, void *priv, const char *name);
void    tty_receive(Tty *t, const u8 *buf, size_t n);                   /* keyboard/serial/pty-master input; IRQ safe */
ssize_t tty_read(Tty *t, void *buf, size_t n, bool nonblock);
ssize_t tty_write(Tty *t, const void *buf, size_t n);
int     tty_ioctl(Tty *t, unsigned long req, void *arg);                /* arg: kernel pointer */
size_t  tty_pending(Tty *t);
void    tty_flush_input(Tty *t);

Tty    *tty_console(void);
void    tty_console_init(void);

/* Set by the process layer so ^C and friends reach the foreground process group. */
extern void (*g_tty_signal_hook)(int pgid, int sig);

#endif
