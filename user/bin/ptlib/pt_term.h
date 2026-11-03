/*
 * pt_term.h: terminal plumbing for the full-screen text tools (pnote, patchouli-pager).
 *
 *  - PtBuf        growable byte string
 *  - PtTerm       raw mode, window size (TIOCGWINSZ, LINES/COLUMNS override), key reader, buffered output
 *  - PtScreen     row-wise frame buffer that only repaints rows that changed
 *
 * The console is a plain ANSI terminal: the kernel framebuffer console ignores underline and the alternate screen,
 * so the tools keep to SGR 0/1/7, 16 colours and CSI cursor addressing, and clear the screen themselves on exit.
 */
#ifndef PT_TERM_H
#define PT_TERM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "pt_utf8.h"

/* ------------------------------------------------------------------ growable string */

typedef struct { char *p; size_t len, cap; } PtBuf;

static inline void pt_oom(void);

static inline void pb_reserve(PtBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) pt_oom();
    b->p = np;
    b->cap = nc;
}

static inline void pb_add(PtBuf *b, const char *s, size_t n) {
    pb_reserve(b, n);
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static inline void pb_adds(PtBuf *b, const char *s) { pb_add(b, s, strlen(s)); }

static inline void pb_addc(PtBuf *b, char c) {
    pb_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static inline void pb_addf(PtBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void pb_addf(PtBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { pb_add(b, tmp, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) pt_oom();
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    pb_add(b, big, (size_t)n);
    free(big);
}

static inline void pb_clear(PtBuf *b) { b->len = 0; if (b->p) b->p[0] = 0; }
static inline void pb_free(PtBuf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }
static inline int  pb_equal(const PtBuf *a, const PtBuf *b) { return a->len == b->len && (a->len == 0 || !memcmp(a->p, b->p, a->len)); }

/* ------------------------------------------------------------------ keys */

#define PT_KEY_EOF   (-1)
#define PT_KEY_NONE  (-2)                /* read timed out (or an escape sequence we do not know) */

/* Plain keys are their code point; control keys are 1..26 (Ctrl-A..Ctrl-Z) except these four, which get their usual names. */
#define PT_K_TAB        9
#define PT_K_ENTER      13               /* both CR and LF */
#define PT_K_ESC        27
#define PT_K_BACKSPACE  127              /* DEL and Ctrl-H */
#define PT_CTRL(c)      ((c) & 0x1F)

#define PT_K_BASE   0x200000
enum {
    PT_K_UP = PT_K_BASE, PT_K_DOWN, PT_K_LEFT, PT_K_RIGHT, PT_K_HOME, PT_K_END, PT_K_PGUP, PT_K_PGDN,
    PT_K_INS, PT_K_DEL, PT_K_BTAB,
    PT_K_F1, PT_K_F2, PT_K_F3, PT_K_F4, PT_K_F5, PT_K_F6, PT_K_F7, PT_K_F8, PT_K_F9, PT_K_F10, PT_K_F11, PT_K_F12,
};
#define PT_M_SHIFT  0x1000000
#define PT_M_ALT    0x2000000
#define PT_M_CTRL   0x4000000
#define PT_KEY_BASE(k) ((k) & 0xFFFFFF)

/* ------------------------------------------------------------------ terminal */

typedef struct PtTerm {
    int in_fd, out_fd;
    int own_fd;                          /* /dev/tty was opened by us (and is shared by in_fd/out_fd as needed) */
    int raw, ui;
    struct termios saved;
    int rows, cols;
    unsigned char ibuf[1024];
    int ipos, ilen;
    int unget;                           /* one pushed-back input byte, or -1 */
    PtBuf out;
} PtTerm;

static PtTerm *pt_active;

#define PT_ESC_WAIT_MS 250               /* how long a lone ESC waits for the rest of an escape sequence */

/* Picks the terminal: stdin/stdout when they are terminals, /dev/tty otherwise (so `cmd | pnote -` still works). */
static inline int pt_term_open(PtTerm *t) {
    memset(t, 0, sizeof *t);
    t->in_fd = 0;
    t->out_fd = 1;
    t->unget = -1;
    if (!isatty(0) || !isatty(1)) {
        int tty = open("/dev/tty", O_RDWR);
        if (tty < 0) return -1;
        if (!isatty(tty)) { close(tty); return -1; }
        t->own_fd = tty;
        if (!isatty(0)) t->in_fd = tty;
        if (!isatty(1)) t->out_fd = tty;
    }
    t->rows = 24;
    t->cols = 80;
    return 0;
}

/* Re-reads the window size. Returns 1 if it changed. LINES and COLUMNS in the environment take precedence. */
static inline int pt_term_size(PtTerm *t) {
    struct winsize ws;
    int rows = 0, cols = 0;
    memset(&ws, 0, sizeof ws);
    if (ioctl(t->out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) { rows = ws.ws_row; cols = ws.ws_col; }
    const char *e = getenv("LINES");
    if (e && atoi(e) > 0) rows = atoi(e);
    e = getenv("COLUMNS");
    if (e && atoi(e) > 0) cols = atoi(e);
    if (rows <= 0) rows = 24;
    if (cols <= 0) cols = 80;
    if (rows > 200) rows = 200;
    if (cols > 400) cols = 400;
    int changed = rows != t->rows || cols != t->cols;
    t->rows = rows;
    t->cols = cols;
    return changed;
}

static inline void pt_flush(PtTerm *t) {
    size_t off = 0;
    while (off < t->out.len) {
        ssize_t w = write(t->out_fd, t->out.p + off, t->out.len - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    pb_clear(&t->out);
}

static inline void pt_out(PtTerm *t, const char *s) { pb_adds(&t->out, s); }

/* Puts the terminal back the way we found it. Safe to call from a signal handler. */
static inline void pt_term_restore(PtTerm *t) {
    if (t->ui) {
        static const char leave[] = "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
        ssize_t w = write(t->out_fd, leave, sizeof leave - 1);
        (void)w;
        t->ui = 0;
    }
    if (t->raw) {
        tcsetattr(t->in_fd, TCSANOW, &t->saved);
        t->raw = 0;
    }
}

static inline void pt_on_signal(int sig) {
    if (pt_active) pt_term_restore(pt_active);
    _exit(128 + sig);
}

static inline void pt_atexit(void) { if (pt_active) pt_term_restore(pt_active); }

static inline void pt_oom(void) {
    if (pt_active) pt_term_restore(pt_active);
    static const char msg[] = "out of memory (Yuyuko is hungry)\n";
    ssize_t w = write(2, msg, sizeof msg - 1);
    (void)w;
    _exit(1);
}

/* Switches to raw mode and (optionally) the alternate screen; arranges for the terminal to be restored on exit and on
 * SIGINT/SIGTERM/SIGHUP/SIGQUIT. */
static inline void pt_term_enter(PtTerm *t, int alt_screen) {
    if (!t->raw) {
        tcgetattr(t->in_fd, &t->saved);
        struct termios r = t->saved;
        r.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
        r.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR);
        r.c_cc[VMIN] = 1;
        r.c_cc[VTIME] = 0;
        tcsetattr(t->in_fd, TCSANOW, &r);
        t->raw = 1;
    }
    pt_active = t;
    static int handlers;
    if (!handlers) {
        handlers = 1;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = pt_on_signal;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        atexit(pt_atexit);
    }
    if (alt_screen) {
        t->ui = 1;
        pt_out(t, "\x1b[?1049h\x1b[0m\x1b[2J\x1b[H");
        pt_flush(t);
    }
}

static inline void pt_term_leave(PtTerm *t) { pt_term_restore(t); }

/* Fills the input buffer. Returns 1 if bytes are available, 0 on timeout/interrupt, -1 at end of input.
 * timeout_ms < 0 blocks. */
static inline int pt_fill(PtTerm *t, int timeout_ms) {
    if (t->unget >= 0 || t->ipos < t->ilen) return 1;
    t->ipos = t->ilen = 0;
    if (timeout_ms >= 0) {
        struct pollfd pf;
        pf.fd = t->in_fd;
        pf.events = POLLIN;
        pf.revents = 0;
        int r = poll(&pf, 1, timeout_ms);
        if (r < 0) return errno == EINTR ? 0 : -1;
        if (r == 0) return 0;
    }
    ssize_t n = read(t->in_fd, t->ibuf, sizeof t->ibuf);
    if (n > 0) { t->ilen = (int)n; return 1; }
    if (n < 0 && errno == EINTR) return 0;
    return -1;
}

/* True when typed-ahead input is already buffered (so a caller can skip repainting between keys). */
static inline int pt_pending(PtTerm *t) { return t->unget >= 0 || t->ipos < t->ilen; }

static inline int pt_next_byte(PtTerm *t, int wait_ms) {
    if (t->unget >= 0) { int b = t->unget; t->unget = -1; return b; }
    if (pt_fill(t, wait_ms) <= 0) return -1;
    return t->ibuf[t->ipos++];
}

static inline void pt_unget_byte(PtTerm *t, int b) { t->unget = b; }

static inline int pt_csi_key(int final, int p0, int p1) {
    int k = 0;
    switch (final) {
    case 'A': k = PT_K_UP; break;
    case 'B': k = PT_K_DOWN; break;
    case 'C': k = PT_K_RIGHT; break;
    case 'D': k = PT_K_LEFT; break;
    case 'H': k = PT_K_HOME; break;
    case 'F': k = PT_K_END; break;
    case 'Z': return PT_K_BTAB;
    case 'P': k = PT_K_F1; break;
    case 'Q': k = PT_K_F2; break;
    case 'R': k = PT_K_F3; break;
    case 'S': k = PT_K_F4; break;
    case '~':
        switch (p0) {
        case 1: case 7: k = PT_K_HOME; break;
        case 2: k = PT_K_INS; break;
        case 3: k = PT_K_DEL; break;
        case 4: case 8: k = PT_K_END; break;
        case 5: k = PT_K_PGUP; break;
        case 6: k = PT_K_PGDN; break;
        case 11: case 12: case 13: case 14: k = PT_K_F1 + (p0 - 11); break;
        case 15: k = PT_K_F5; break;
        case 17: case 18: case 19: case 20: case 21: k = PT_K_F6 + (p0 - 17); break;
        case 23: k = PT_K_F11; break;
        case 24: k = PT_K_F12; break;
        default: return PT_KEY_NONE;
        }
        break;
    default: return PT_KEY_NONE;
    }
    if (p1 > 1) {                        /* xterm modifier parameter: 1 + (shift=1, alt=2, ctrl=4) */
        int m = p1 - 1;
        if (m & 1) k |= PT_M_SHIFT;
        if (m & 2) k |= PT_M_ALT;
        if (m & 4) k |= PT_M_CTRL;
    }
    return k;
}

/* After "ESC [" or "ESC O": reads the parameters and final byte. */
static inline int pt_parse_csi(PtTerm *t) {
    int params[4] = { 0, 0, 0, 0 }, np = 0, fin;
    for (;;) {
        fin = pt_next_byte(t, 100);
        if (fin < 0) return PT_K_ESC;
        if (fin >= '0' && fin <= '9') { params[np] = params[np] * 10 + (fin - '0'); continue; }
        if (fin == ';') { if (np < 3) np++; continue; }
        if (fin == '?' || fin == '>' || fin == '=' || fin == ' ') continue;
        break;
    }
    return pt_csi_key(fin, params[0], np >= 1 ? params[1] : 0);
}

/* Reads one key. timeout_ms < 0 waits forever. Returns a code point, a Ctrl code, a PT_K_* value (possibly with
 * PT_M_* modifier bits), PT_KEY_NONE on timeout or PT_KEY_EOF when the input is gone. */
static inline int pt_term_getkey(PtTerm *t, int timeout_ms) {
    int c;
    if (t->unget >= 0) {
        c = t->unget;
        t->unget = -1;
    } else {
        int r = pt_fill(t, timeout_ms);
        if (r < 0) return PT_KEY_EOF;
        if (r == 0) return PT_KEY_NONE;
        c = t->ibuf[t->ipos++];
    }
    if (c == 27) {
        int c1 = pt_next_byte(t, PT_ESC_WAIT_MS);
        if (c1 < 0) return PT_K_ESC;
        if (c1 == '[' || c1 == 'O') return pt_parse_csi(t);
        if (c1 == 27) {                  /* ESC ESC [ A: Alt+arrow from the console keyboard; ESC ESC: a double tap */
            int c2 = pt_next_byte(t, 50);
            if (c2 == '[' || c2 == 'O') {
                int k = pt_parse_csi(t);
                return k >= PT_K_BASE ? (k | PT_M_ALT) : k;
            }
            if (c2 >= 0) pt_unget_byte(t, c2);
            return PT_K_ESC;
        }
        if (c1 == 127 || c1 == 8) return PT_K_BACKSPACE | PT_M_ALT;
        if (c1 == '\r' || c1 == '\n') return PT_K_ENTER | PT_M_ALT;
        if (c1 < 0x80) return c1 | PT_M_ALT;
        pt_unget_byte(t, c1);
        return PT_K_ESC;
    }
    if (c == '\r' || c == '\n') return PT_K_ENTER;
    if (c == 8 || c == 127) return PT_K_BACKSPACE;
    if (c < 0x80) return c;
    /* UTF-8 sequence: gather the continuation bytes */
    unsigned char seq[4];
    int need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0, n = 1;
    seq[0] = (unsigned char)c;
    if (!need) return PT_KEY_NONE;       /* stray continuation byte: ignore */
    while (n <= need) {
        int b = pt_next_byte(t, 100);
        if (b < 0) return PT_KEY_NONE;
        if ((b & 0xC0) != 0x80) { pt_unget_byte(t, b); return PT_KEY_NONE; }
        seq[n++] = (unsigned char)b;
    }
    uint32_t cp;
    if (pt_u8_decode(seq, (size_t)n, &cp) != n) return PT_KEY_NONE;
    return (int)cp;
}

/* ------------------------------------------------------------------ screen */

typedef struct { PtBuf b; int w; } PtRow;

typedef struct {
    int rows, cols;
    PtRow *cur, *prev;
    int force;                           /* repaint every row on the next flush */
} PtScreen;

static inline void pts_free(PtScreen *s) {
    for (int i = 0; i < s->rows; i++) { pb_free(&s->cur[i].b); pb_free(&s->prev[i].b); }
    free(s->cur);
    free(s->prev);
    s->cur = s->prev = NULL;
    s->rows = 0;
}

static inline void pts_resize(PtScreen *s, int rows, int cols) {
    if (rows != s->rows) {
        pts_free(s);
        s->cur = calloc((size_t)rows, sizeof(PtRow));
        s->prev = calloc((size_t)rows, sizeof(PtRow));
        if (!s->cur || !s->prev) pt_oom();
        s->rows = rows;
    }
    s->cols = cols;
    s->force = 1;
}

static inline void pts_invalidate(PtScreen *s) { s->force = 1; }

/* Starts (empties) row r and returns it for building. */
static inline PtRow *pts_row(PtScreen *s, int r) {
    PtRow *row = &s->cur[r];
    pb_clear(&row->b);
    row->w = 0;
    return row;
}

static inline void pr_raw(PtRow *r, const char *esc) { pb_adds(&r->b, esc); }
static inline void pr_sgr(PtRow *r, const char *codes) { pb_adds(&r->b, "\x1b["); pb_adds(&r->b, codes); pb_addc(&r->b, 'm'); }
static inline void pr_reset(PtRow *r) { pb_adds(&r->b, "\x1b[0m"); }
static inline void pr_cp(PtRow *r, uint32_t cp) {
    char u[4];
    if (pt_is_rawbyte(cp)) cp = '?';
    pb_add(&r->b, u, (size_t)pt_u8_encode(cp, u));
    r->w++;
}
static inline void pr_spaces(PtRow *r, int n) { while (n-- > 0) { pb_addc(&r->b, ' '); r->w++; } }

/* Appends up to maxw columns of UTF-8 text (s, n bytes); returns the columns used. Control characters are dropped. */
static inline int pr_text(PtRow *r, const char *s, size_t n, int maxw) {
    int used = 0;
    size_t i = 0;
    while (i < n && used < maxw) {
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)s + i, n - i, &cp);
        i += (size_t)l;
        if (cp < 0x20 || cp == 0x7F) continue;
        pr_cp(r, cp);
        used++;
    }
    return used;
}
static inline int pr_str(PtRow *r, const char *s, int maxw) { return pr_text(r, s, strlen(s), maxw); }

static inline int pts_row_dirty(const PtScreen *s, int r) {
    return s->force || s->cur[r].w != s->prev[r].w || !pb_equal(&s->cur[r].b, &s->prev[r].b);
}

/* Writes changed rows to the terminal, then positions the cursor (0-based row/col) and shows or hides it.
 * Runs of consecutive changed rows are written top to bottom with CR LF between them, so the text stream reads like
 * the screen does. */
static inline void pts_flush(PtScreen *s, PtTerm *t, int crow, int ccol, int show_cursor) {
    PtBuf *o = &t->out;
    pb_adds(o, "\x1b[?25l");
    int r = 0;
    while (r < s->rows) {
        if (!pts_row_dirty(s, r)) { r++; continue; }
        if (r == 0) pb_adds(o, "\x1b[H"); else pb_addf(o, "\x1b[%d;1H", r + 1);
        int first = r;
        while (r < s->rows && pts_row_dirty(s, r)) {
            if (r > first) pb_adds(o, "\r\n");
            pb_add(o, s->cur[r].b.p ? s->cur[r].b.p : "", s->cur[r].b.len);
            pb_adds(o, "\x1b[0m");
            if (s->cur[r].w < s->cols) pb_adds(o, "\x1b[K");
            PtRow tmp = s->prev[r];
            s->prev[r] = s->cur[r];
            s->cur[r] = tmp;
            pb_clear(&s->cur[r].b);
            r++;
        }
    }
    s->force = 0;
    if (crow >= 0) pb_addf(o, "\x1b[%d;%dH", crow + 1, ccol + 1);
    if (show_cursor) pb_adds(o, "\x1b[?25h");
    pt_flush(t);
}

#endif
