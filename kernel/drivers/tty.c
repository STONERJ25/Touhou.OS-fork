#include "tty.h"
#include "kernel.h"
#include "cpu.h"
#include "fbcon.h"

void (*g_tty_signal_hook)(int pgid, int sig);

static Tty g_console;

static void tty_out(Tty *t, const u8 *buf, size_t n) {
    if (t->ops && t->ops->write) t->ops->write(t, buf, n);
}

void tty_init(Tty *t, const TtyOps *ops, void *priv, const char *name) {
    memset(t, 0, sizeof *t);
    t->ops = ops;
    t->priv = priv;
    strlcpy(t->name, name, sizeof t->name);
    t->termios.c_iflag = ICRNL;
    t->termios.c_oflag = OPOST | ONLCR;
    t->termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    t->termios.c_cc[VINTR] = 0x03;
    t->termios.c_cc[VQUIT] = 0x1C;
    t->termios.c_cc[VERASE] = 0x7F;
    t->termios.c_cc[VKILL] = 0x15;
    t->termios.c_cc[VEOF] = 0x04;
    t->termios.c_cc[VMIN] = 1;
    t->termios.c_cc[VTIME] = 0;
    t->termios.c_cc[VSUSP] = 0x1A;
    t->termios.c_cc[VWERASE] = 0x17;
    t->winsize.ws_row = 25;
    t->winsize.ws_col = 80;
    wq_init(&t->rd_wq);
}

static void echo_byte(Tty *t, u8 c) {
    if (!(t->termios.c_lflag & ECHO)) return;
    u8 buf[2];
    if (c == '\n' || c == '\t') { buf[0] = c; tty_out(t, buf, 1); }
    else if (c < 0x20) { buf[0] = '^'; buf[1] = (u8)(c + 0x40); tty_out(t, buf, 2); }
    else if (c == 0x7F) { buf[0] = '^'; buf[1] = '?'; tty_out(t, buf, 2); }
    else tty_out(t, &c, 1);
}

static void echo_erase(Tty *t, u8 erased) {
    if (!(t->termios.c_lflag & ECHO)) return;
    static const u8 bs[] = "\b \b";
    tty_out(t, bs, 3);
    if (erased < 0x20 && erased != '\t') tty_out(t, bs, 3);     /* it was echoed as ^X: two cells */
}

static void ring_put(Tty *t, u8 c) {
    u32 next = (t->in_head + 1) & (TTY_INBUF - 1);
    if (next == t->in_tail) return;                              /* full: drop */
    t->in[t->in_head] = c;
    t->in_head = next;
}

static void commit_line(Tty *t) {
    for (u32 i = 0; i < t->line_len; i++) ring_put(t, t->line[i]);
    t->line_len = 0;
    wq_wake_all(&t->rd_wq);
}

static void receive_byte(Tty *t, u8 c) {
    u32 lf = t->termios.c_lflag, inf = t->termios.c_iflag;
    if (c == '\r') {
        if (inf & IGNCR) return;
        if (inf & ICRNL) c = '\n';
    } else if (c == '\n' && (inf & INLCR)) {
        c = '\r';
    }

    if (lf & ISIG) {
        int sig = 0;
        if (c == t->termios.c_cc[VINTR]) sig = SIGINT;
        else if (c == t->termios.c_cc[VQUIT]) sig = SIGQUIT;
        else if (c == t->termios.c_cc[VSUSP]) sig = SIGTSTP;
        if (sig) {
            echo_byte(t, c);
            echo_byte(t, '\n');
            t->line_len = 0;
            t->in_head = t->in_tail = 0;
            if (g_tty_signal_hook && t->fg_pgid) g_tty_signal_hook(t->fg_pgid, sig);
            wq_wake_all(&t->rd_wq);
            return;
        }
    }

    if (lf & ICANON) {
        if (c == t->termios.c_cc[VERASE] || c == 0x08) {
            if (t->line_len) echo_erase(t, t->line[--t->line_len]);
            return;
        }
        if (c == t->termios.c_cc[VKILL]) {
            while (t->line_len) echo_erase(t, t->line[--t->line_len]);
            return;
        }
        if (c == t->termios.c_cc[VWERASE]) {
            while (t->line_len && t->line[t->line_len - 1] == ' ') echo_erase(t, t->line[--t->line_len]);
            while (t->line_len && t->line[t->line_len - 1] != ' ') echo_erase(t, t->line[--t->line_len]);
            return;
        }
        if (c == t->termios.c_cc[VEOF]) {
            if (t->line_len == 0) { t->eof = true; wq_wake_all(&t->rd_wq); }
            else commit_line(t);
            return;
        }
        if (t->line_len < TTY_LINE - 1) {
            t->line[t->line_len++] = c;
            echo_byte(t, c);
            if (c == '\n') commit_line(t);
        }
        return;
    }

    ring_put(t, c);
    echo_byte(t, c);
    wq_wake_all(&t->rd_wq);
}

void tty_receive(Tty *t, const u8 *buf, size_t n) {
    u64 f = irq_save();
    for (size_t i = 0; i < n; i++) receive_byte(t, buf[i]);
    irq_restore(f);
}

size_t tty_pending(Tty *t) {
    u64 f = irq_save();
    size_t n = (t->in_head - t->in_tail) & (TTY_INBUF - 1);
    irq_restore(f);
    return n;
}

void tty_flush_input(Tty *t) {
    u64 f = irq_save();
    t->in_head = t->in_tail = 0;
    t->line_len = 0;
    irq_restore(f);
}

ssize_t tty_read(Tty *t, void *buf, size_t n, bool nonblock) {
    if (n == 0) return 0;
    u8 *out = buf;
    u64 f = irq_save();
    bool canon = t->termios.c_lflag & ICANON;
    u8 vmin = t->termios.c_cc[VMIN];

    while (t->in_head == t->in_tail && !t->eof) {
        if (nonblock || (!canon && vmin == 0)) { irq_restore(f); return nonblock ? -EAGAIN : 0; }
        if (signal_pending()) { irq_restore(f); return -EINTR; }
        current->intr = true;
        wq_block(&t->rd_wq);
        current->intr = false;
    }
    if (t->in_head == t->in_tail && t->eof) {       /* ^D on an empty line */
        t->eof = false;
        irq_restore(f);
        return 0;
    }

    size_t got = 0;
    while (got < n && t->in_head != t->in_tail) {
        u8 c = t->in[t->in_tail];
        t->in_tail = (t->in_tail + 1) & (TTY_INBUF - 1);
        out[got++] = c;
        if (canon && c == '\n') break;
    }
    irq_restore(f);
    return (ssize_t)got;
}

ssize_t tty_write(Tty *t, const void *buf, size_t n) {
    tty_out(t, buf, n);
    return (ssize_t)n;
}

int tty_ioctl(Tty *t, unsigned long req, void *arg) {
    switch (req) {
    case TCGETS:
        memcpy(arg, &t->termios, sizeof t->termios);
        return 0;
    case TCSETS: case TCSETSW: case TCSETSF: {
        u64 f = irq_save();
        u32 was_canon = t->termios.c_lflag & ICANON;
        memcpy(&t->termios, arg, sizeof t->termios);
        /* Leaving canonical mode makes the half-typed line readable, as on Linux; otherwise it would be stranded here. */
        if (was_canon && !(t->termios.c_lflag & ICANON)) commit_line(t);
        irq_restore(f);
        if (req == TCSETSF) tty_flush_input(t);
        wq_wake_all(&t->rd_wq);
        return 0;
    }
    case TIOCGWINSZ:
        memcpy(arg, &t->winsize, sizeof t->winsize);
        return 0;
    case TIOCSWINSZ:
        memcpy(&t->winsize, arg, sizeof t->winsize);
        return 0;
    case TIOCGPGRP:
        *(int *)arg = t->fg_pgid;
        return 0;
    case TIOCSPGRP:
        t->fg_pgid = *(int *)arg;
        return 0;
    case FIONREAD:
        *(int *)arg = (int)tty_pending(t);
        return 0;
    default:
        return -ENOTTY;
    }
}

/* ---------------------------------------------------------------- the console */

static void console_tty_write(Tty *t, const u8 *buf, size_t n) {
    (void)t;
    console_write((const char *)buf, n);
}

static const TtyOps g_console_ops = { console_tty_write };

Tty *tty_console(void) { return &g_console; }

void tty_console_init(void) {
    tty_init(&g_console, &g_console_ops, NULL, "console");
    int cols, rows;
    fbcon_size(&cols, &rows);
    g_console.winsize.ws_col = (u16)cols;
    g_console.winsize.ws_row = (u16)rows;
}
