/*
 * The session: one channel carrying a shell or a command between the server and the user's terminal (or files, or pipes), and the loop that keeps it going.
 *
 * With a terminal (-t) the local terminal is put in raw mode for as long as the session lasts: every key goes to the server as it is typed (the server's pty does
 * the echo and the line editing), Ctrl+C is a character like any other and reaches the program at the far end, and output is written without any translation.
 * The terminal is put back on every way out: normal end, `~.`, a lost connection, a protocol error, SIGTERM or SIGHUP, and exit(). The window size goes
 * with the pty request and again as a window-change whenever it changes (SIGWINCH, and a look once a second as well).
 *
 * The escape character (`~` by default) is recognised at the start of a line only: ~. ends the session at once, ~? lists the escapes, ~~ sends one `~`, ~R asks for a
 * new key exchange, ~V / ~v change how much is said about the connection, ~# lists the forwarded connections.
 *
 * Flow control: input is read from the terminal only while the server's window for the channel is open and little is queued for the socket, and what the server
 * sends is acknowledged (the window reopened) only after it has been written to the terminal or pipe, so a slow reader slows the server down.
 */
#include "ssh.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

static volatile int g_winch, g_term;                      /* (sig_atomic_t, which this libc does not name, is an int) */
static struct termios g_saved;
static bool g_raw;

static void on_winch(int sig) { (void)sig; g_winch = 1; }
static void on_term(int sig) { (void)sig; g_term = 1; }

/* ------------------------------------------------------------------------------------------------ the terminal */

static void tty_restore(void) {
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &g_saved);
        g_raw = false;
    }
}

static int tty_raw(void) {
    struct termios t;
    if (g_raw) return 0;
    if (tcgetattr(STDIN_FILENO, &g_saved) != 0) return -1;
    t = g_saved;
    t.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR);                     /* Enter is a CR, as the server's terminal expects */
#ifdef IXON
    t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | ISTRIP | BRKINT | PARMRK | INPCK);
#endif
    t.c_oflag &= ~(tcflag_t)OPOST;                                        /* the server's pty has already turned LF into CR LF */
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
#ifdef IEXTEN
    t.c_lflag &= ~(tcflag_t)(IEXTEN | ECHONL);
#endif
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &t) != 0) return -1;
    g_raw = true;
    atexit(tty_restore);
    return 0;
}

static void get_winsize(unsigned *cols, unsigned *rows) {
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        const char *c = getenv("COLUMNS"), *l = getenv("LINES");
        if (c && atoi(c) > 0) *cols = (unsigned)atoi(c);
        if (l && atoi(l) > 0) *rows = (unsigned)atoi(l);
    }
}

/* The terminal modes of RFC 4254, section 8: the control characters the local terminal uses, so that backspace and the like feel the same at the far end. */
static size_t terminal_modes(uint8_t *out, size_t cap) {
    SshcWr w;
    sshc_wr_init(&w, out, cap);
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        struct { uint8_t op; int idx; } map[] = { { 1, VINTR }, { 2, VQUIT }, { 3, VERASE }, { 4, VKILL }, { 5, VEOF }, { 10, VSUSP } };
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
            sshc_wr_u8(&w, map[i].op);
            sshc_wr_u32(&w, t.c_cc[map[i].idx]);
        }
    }
    sshc_wr_u8(&w, 128); sshc_wr_u32(&w, 38400);                        /* TTY_OP_ISPEED, TTY_OP_OSPEED */
    sshc_wr_u8(&w, 129); sshc_wr_u32(&w, 38400);
    sshc_wr_u8(&w, 0);                                                    /* TTY_OP_END */
    return w.n;
}

/* ------------------------------------------------------------------------------------------------ output */

/* Writes everything, waiting when the descriptor is full (it may be non-blocking). false when the reader is gone. */
static bool write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            poll(&pf, 1, 1000);
            continue;
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ the session state and the channel's handlers */

typedef struct {
    SshcChan *ch;
    const SessionOpts *o;
    Forwards *fw;
    bool confirmed, open_failed, closed;
    char fail_text[160];
    uint32_t fail_reason;
    int replies_wanted, reply_ok, replies_seen;          /* for the requests made at the start, whose answers are waited for */
    bool have_status, have_signal;
    uint32_t status;
    char signal_name[32];
    bool out_dead, err_dead;                             /* the reader of standard output / error has gone: the bytes are dropped */
    bool pty;
    bool in_eof_sent;
    bool quit;                                           /* ~. */
    bool at_newline;
    bool tilde;                                          /* the escape character has been typed and the next one decides */
    uint8_t sendbuf[8192];
    size_t send_len, send_off;
} Session;

static void on_confirmed(SshcConn *c, SshcChan *ch) { (void)c; ((Session *)ch->user)->confirmed = true; }

static void on_open_failed(SshcConn *c, SshcChan *ch, uint32_t reason, const char *text) {
    (void)c;
    Session *S = ch->user;
    S->open_failed = true;
    S->fail_reason = reason;
    snprintf(S->fail_text, sizeof S->fail_text, "%s", text);
}

static void on_data(SshcConn *c, SshcChan *ch, const uint8_t *d, size_t n, uint32_t ext) {
    Session *S = ch->user;
    int fd = ext == 1 ? STDERR_FILENO : STDOUT_FILENO;
    bool *dead = ext == 1 ? &S->err_dead : &S->out_dead;
    if (!*dead && !write_all(fd, d, n)) *dead = true;
    sshc_chan_consumed(c, ch, n);                                         /* only now may the server send more */
}

static void on_eof(SshcConn *c, SshcChan *ch) { (void)c; (void)ch; }

static void on_closed(SshcConn *c, SshcChan *ch) {
    (void)c;
    ((Session *)ch->user)->closed = true;
}

static bool on_request(SshcConn *c, SshcChan *ch, const char *name, SshcRd *args, bool want_reply) {
    (void)c; (void)want_reply;
    Session *S = ch->user;
    if (!strcmp(name, "exit-status")) {
        uint32_t st = sshc_rd_u32(args);
        if (!args->bad) { S->have_status = true; S->status = st; }
        return true;
    }
    if (!strcmp(name, "exit-signal")) {
        size_t nl;
        const uint8_t *sig = sshc_rd_str(args, &nl);
        if (!args->bad) {
            S->have_signal = true;
            sshc_printable(S->signal_name, sizeof S->signal_name, sig, nl, false);
        }
        return true;
    }
    return false;                                                         /* eow@openssh.com and the rest: nothing to do */
}

static void on_reply(SshcConn *c, SshcChan *ch, bool ok) {
    (void)c;
    Session *S = ch->user;
    S->replies_seen++;
    if (ok) S->reply_ok |= 1 << (S->replies_seen - 1);
}

static const SshcChanOps SESSION_OPS = { on_confirmed, on_open_failed, on_data, on_eof, on_closed, on_request, on_reply };

/* ------------------------------------------------------------------------------------------------ the escape character */

static const char ESCAPE_HELP[] =
    "\r\nSupported escape sequences:\r\n"
    "  ~.   - terminate connection\r\n"
    "  ~R   - request rekey\r\n"
    "  ~V/v - decrease/increase verbosity (LogLevel)\r\n"
    "  ~#   - list forwarded connections\r\n"
    "  ~?   - this message\r\n"
    "  ~~   - send the escape character by typing it twice\r\n"
    "(Note that escapes are only recognized immediately after newline.)\r\n";

static void say(const char *s) { write_all(STDERR_FILENO, (const uint8_t *)s, strlen(s)); }

/* Turns what was typed into what is sent: the escape character at the start of a line is taken out and acted upon. */
static size_t filter_input(Session *S, SshcConn *c, const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0;
    int esc = S->o->escape;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = in[i];
        if (S->tilde) {
            S->tilde = false;
            switch (ch) {
            case '.':
                say("\r\n");
                S->quit = true;
                return o;
            case '?':
                say(ESCAPE_HELP);
                S->at_newline = true;
                continue;
            case 'R':
                say("\r\nRekeying...\r\n");
                sshc_rekey_if_due(c, true);
                continue;
            case 'V':
            case 'v': {
                if (ch == 'V' && sshc_verbose > -1) sshc_verbose--;
                if (ch == 'v' && sshc_verbose < 3) sshc_verbose++;
                char m[80];
                snprintf(m, sizeof m, "%c: verbosity level %d\r\n", ch, sshc_verbose);
                say(m);
                continue;
            }
            case '#': {
                char list[1024] = "";
                if (S->fw) forwards_list(S->fw, list, sizeof list);
                say("\r\nThe following connections are open:\r\n");
                say(list[0] ? list : "  (none)\r\n");
                S->at_newline = true;
                continue;
            }
            default:
                if (ch == esc) { out[o++] = ch; S->at_newline = false; continue; }        /* ~~ : a single ~ */
                out[o++] = (uint8_t)esc;                                                   /* not an escape after all: both characters go on */
                out[o++] = ch;
                S->at_newline = ch == '\r' || ch == '\n';
                continue;
            }
        }
        if (esc >= 0 && ch == esc && S->at_newline) { S->tilde = true; continue; }
        out[o++] = ch;
        S->at_newline = ch == '\r' || ch == '\n';
    }
    return o;
}

/* ------------------------------------------------------------------------------------------------ the loop */

static void print_exit_note(const SessionOpts *o) {
    char m[200];
    snprintf(m, sizeof m, "Connection to %s closed.\r\n", o->host);
    say(m);
}

static void ignore_reply(void *ctx, SshcConn *c, bool ok, SshcRd *data) { (void)ctx; (void)c; (void)ok; (void)data; }

int session_run(SshcConn *c, const SessionOpts *o) {
    Session S;
    memset(&S, 0, sizeof S);
    S.o = o;
    S.at_newline = true;
    int status = 255;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    bool tty_in = isatty(STDIN_FILENO);
    S.pty = o->pty && !o->no_session;

    if (o->nlocal + o->nremote + o->ndynamic > 0) {
        S.fw = forwards_new(c, o);
        if (S.fw && forwards_start(S.fw, c, o) < 0) {
            fprintf(stderr, "ssh: a requested forwarding failed and ExitOnForwardFailure is set\n");
            forwards_free(S.fw);
            return 255;
        }
    }

    if (!o->no_session) {
        S.ch = sshc_chan_open(c, "session", NULL, 0, &SESSION_OPS, &S);
        if (!S.ch) { fprintf(stderr, "ssh: cannot open a session channel\n"); return 255; }
        while (!S.confirmed && !S.open_failed) {
            if (sshc_pump(c, -1) < 0) { fprintf(stderr, "%s\n", c->err); forwards_free(S.fw); return 255; }
        }
        if (S.open_failed) {
            fprintf(stderr, "ssh: channel open failed: %s (reason %u)\n", S.fail_text, (unsigned)S.fail_reason);
            forwards_free(S.fw);
            return 255;
        }

        /* the pty, if wanted, and then the shell or command; each is answered, and the answers come in the order of the requests */
        if (S.pty) {
            unsigned cols, rows;
            get_winsize(&cols, &rows);
            uint8_t req[600], modes[64];
            size_t ml = terminal_modes(modes, sizeof modes);
            const char *term = getenv("TERM");
            SshcWr w;
            sshc_wr_init(&w, req, sizeof req);
            sshc_wr_cstr(&w, term && term[0] ? term : "xterm");
            sshc_wr_u32(&w, cols);
            sshc_wr_u32(&w, rows);
            sshc_wr_u32(&w, 0);
            sshc_wr_u32(&w, 0);
            sshc_wr_str(&w, modes, ml);
            sshc_chan_request(c, S.ch, "pty-req", true, req, w.n);
            while (S.replies_seen < 1) {
                if (sshc_pump(c, -1) < 0) { fprintf(stderr, "%s\n", c->err); forwards_free(S.fw); return 255; }
            }
            if (!(S.reply_ok & 1)) {
                fprintf(stderr, "PTY allocation request failed on channel 0\n");
                S.pty = false;
            }
        }
        uint8_t req[4200];
        SshcWr w;
        sshc_wr_init(&w, req, sizeof req);
        const char *kind = o->subsystem ? "subsystem" : o->command ? "exec" : "shell";
        if (o->command) sshc_wr_cstr(&w, o->command);
        int expect = S.replies_seen + 1;
        if (S.pty && tty_in) tty_raw();
        sshc_chan_request(c, S.ch, kind, true, req, w.n);
        while (S.replies_seen < expect) {
            if (sshc_pump(c, -1) < 0) { tty_restore(); fprintf(stderr, "%s\n", c->err); forwards_free(S.fw); return 255; }
        }
        if (!(S.reply_ok & (1 << (expect - 1)))) {
            tty_restore();
            fprintf(stderr, "ssh: the server refused the %s request\n", kind);
            sshc_chan_close(c, S.ch);
            forwards_free(S.fw);
            return 255;
        }
        if (o->no_stdin) { sshc_chan_send_eof(c, S.ch); S.in_eof_sent = true; }
    }

    unsigned last_cols = 0, last_rows = 0;
    get_winsize(&last_cols, &last_rows);
    long last_size_check = sshc_mono_ms(), last_probe = sshc_mono_ms();
    int probes = 0;
    bool stdin_eof = o->no_session || o->no_stdin;
    bool fatal = false;
    uint8_t rbuf[4096];

    for (;;) {
        if (g_term) { tty_restore(); fprintf(stderr, "Terminated\n"); status = 255; break; }
        if (S.quit) { status = 255; break; }
        if (!o->no_session && S.closed) break;
        if (S.pty && g_winch) {
            g_winch = 0;
            unsigned cols, rows;
            get_winsize(&cols, &rows);
            if (cols != last_cols || rows != last_rows) {
                uint8_t m[16];
                SshcWr w;
                sshc_wr_init(&w, m, sizeof m);
                sshc_wr_u32(&w, cols);
                sshc_wr_u32(&w, rows);
                sshc_wr_u32(&w, 0);
                sshc_wr_u32(&w, 0);
                if (S.ch) sshc_chan_request(c, S.ch, "window-change", false, m, w.n);
                last_cols = cols; last_rows = rows;
            }
        }
        long now = sshc_mono_ms();
        if (S.pty && now - last_size_check >= 1000) {                      /* a size change nobody signalled */
            last_size_check = now;
            g_winch = 1;
        }
        if (sshc_rekey_if_due(c, false) < 0) { fatal = true; break; }
        if (o->alive_interval_s > 0) {                                     /* ServerAliveInterval: a silent server is asked, and given up on after CountMax tries */
            long quiet_for = now - c->last_rx_ms, ms = (long)o->alive_interval_s * 1000;
            if (quiet_for < ms) probes = 0;
            else if (now - last_probe >= ms) {
                if (probes >= (o->alive_count_max > 0 ? o->alive_count_max : 3)) {
                    tty_restore();
                    fprintf(stderr, "Timeout, server %s not responding.\n", o->host);
                    status = 255;
                    goto out;
                }
                sshc_global_request(c, "keepalive@openssh.com", NULL, 0, ignore_reply, NULL);          /* it must ask for a reply, or nothing need come back */
                probes++;
                last_probe = now;
            }
        }

        struct pollfd pf[2 + 64];
        int np = 0;
        pf[np++] = (struct pollfd){ .fd = c->fd, .events = (short)(POLLIN | (sshc_tx_pending(c) ? POLLOUT : 0)) };
        int stdin_slot = -1;
        if (S.ch && !stdin_eof && S.send_len == S.send_off && sshc_chan_can_send(c, S.ch)) {
            stdin_slot = np;
            pf[np++] = (struct pollfd){ .fd = STDIN_FILENO, .events = POLLIN };
        }
        int fw_first = np;
        if (S.fw) np += forwards_fds(S.fw, pf + np, (int)(sizeof pf / sizeof pf[0]) - np);
        int timeout = sshc_rx_pending(c) || (S.send_len > S.send_off && S.ch && sshc_chan_can_send(c, S.ch)) ? 0 : 1000;
        int pr = poll(pf, (nfds_t)np, timeout);
        if (pr < 0 && errno != EINTR) { tty_restore(); fprintf(stderr, "ssh: poll: %s\n", strerror(errno)); break; }

        if (pr > 0 && (pf[0].revents & POLLOUT)) sshc_flush(c, 0);
        if ((pr > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) || sshc_rx_pending(c)) {
            for (int k = 0; k < 64; k++) {                                   /* what has arrived, but not so much that the keyboard starves */
                int r = sshc_pump(c, 0);
                if (r < 0) { fatal = true; break; }
                if (r == 0 && !sshc_rx_pending(c)) break;
                if (S.closed && !o->no_session) break;
            }
            if (fatal) break;
        }
        if (S.fw) forwards_service(S.fw, c, pf + fw_first, np - fw_first);                  /* (also with nothing to look at: it frees the connections that ended) */

        if (S.send_len > S.send_off && S.ch) {                              /* input still waiting for the server's window */
            size_t sent = sshc_chan_send(c, S.ch, S.sendbuf + S.send_off, S.send_len - S.send_off);
            S.send_off += sent;
            if (S.send_off == S.send_len) S.send_off = S.send_len = 0;
        }
        if (stdin_slot >= 0 && (pf[stdin_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            size_t room = c->tx_len - c->tx_off < (128u << 10) ? sizeof rbuf : 0;
            ssize_t got = room ? read(STDIN_FILENO, rbuf, sizeof rbuf / 2) : -1;
            if (got == 0) {
                stdin_eof = true;
                if (S.ch && !S.in_eof_sent && S.send_len == S.send_off) { sshc_chan_send_eof(c, S.ch); S.in_eof_sent = true; }
            } else if (got > 0) {
                size_t n;
                if (S.pty && S.o->escape >= 0) {
                    n = filter_input(&S, c, rbuf, (size_t)got, S.sendbuf);
                } else {
                    memcpy(S.sendbuf, rbuf, (size_t)got);
                    n = (size_t)got;
                }
                S.send_len = n;
                S.send_off = sshc_chan_send(c, S.ch, S.sendbuf, n);
                if (S.send_off == S.send_len) S.send_off = S.send_len = 0;
            }
        }
        if (stdin_eof && S.ch && !S.in_eof_sent && S.send_len == S.send_off) { sshc_chan_send_eof(c, S.ch); S.in_eof_sent = true; }
    }

    if (fatal) {
        tty_restore();
        if (!(S.closed && S.have_status)) fprintf(stderr, "%s\n", c->err);
        status = 255;
    } else if (S.ch && S.have_status && !S.quit) {
        status = (int)(S.status & 0xff);
    } else if (S.have_signal && !S.quit) {
        tty_restore();
        if (sshc_verbose >= 0) fprintf(stderr, "Killed by signal %s.\n", S.signal_name);
        status = 255;
    }
out:
    tty_restore();
    if (S.ch && !fatal) {
        if (sshc_verbose >= 0 && (S.pty || S.quit) && !g_term) print_exit_note(o);
        sshc_chan_close(c, S.ch);
    }
    if (!fatal && !g_term) {
        sshc_disconnect(c, SSHC_DISC_BY_APPLICATION, "disconnected by user");
    }
    forwards_free(S.fw);
    return status;
}
