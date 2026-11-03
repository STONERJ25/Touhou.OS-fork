/*
 * The connection layer (RFC 4254): session channels that run the resident's shell, or one command, on a pty or on pipes.
 *
 * One loop per connection polls the socket and the program ends of every channel (and the sockets of the forwarded channels, which forward.c
 * keeps: their numbers start at FWD_BASE, and their messages are handed over in dispatch()). Flow control is the protocol's window: the peer may send us
 * WINDOW bytes, which wait in a buffer until the program takes them (only then is the window opened again), and we send no more than the peer's
 * window allows (a channel whose window is closed is not read, so the program is slowed down by the pty or pipe filling up).
 *
 * A channel ends in this order: the program exits and its output is drained, then "exit-status", EOF and CLOSE go out, then the peer's CLOSE
 * frees the slot. If the peer closes first, the program is sent SIGHUP (and SIGKILL if it ignores that).
 */
#include "sshd.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <gensokyo.h>

#define MAX_CHANNELS 4
#define WINDOW       65536              /* what the peer may send before we open the window again; also the size of the buffer toward the program */
#define MAX_PACKET   32768              /* the largest data packet we take */
#define CHUNK        16384              /* the most we send in one data packet */
#define EXIT_GRACE_MS 500               /* how long output is still collected after the program exited (a background job may keep the pty open) */

typedef struct {
    bool used;
    uint32_t peer;                      /* the peer's number for this channel */
    uint32_t peer_win, peer_max;        /* what the peer still lets us send, and its largest packet */
    uint32_t our_win, to_adjust;        /* what the peer may still send; bytes taken by the program but not yet given back */
    bool pty;
    char term[48];
    unsigned cols, rows;
    bool started;
    pid_t pid;
    int in_fd, out_fd, err_fd;          /* toward the program; from its stdout (the pty master, for a pty); from its stderr (-1 on a pty) */
    uint8_t inbuf[WINDOW];
    size_t in_len;
    bool client_eof, out_done, err_done;
    bool exited;
    int status;
    long exited_at;
    bool close_sent, close_got;
    char what[100];                     /* "shell" or the command, for the log */
} Chan;

static Chan g_ch[MAX_CHANNELS];
static uint8_t g_msg[CHUNK + 32];

typedef struct { pid_t pid; long since; } Reap;
static Reap g_reap[16];                 /* programs told to hang up and not yet collected */

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Closes the channel's descriptors. On a pty the input and the output are one descriptor (the master) and must be closed once. */
static void close_chan_fds(Chan *ch) {
    int in = ch->in_fd, out = ch->out_fd, err = ch->err_fd;
    ch->in_fd = ch->out_fd = ch->err_fd = -1;
    if (in >= 0) close(in);
    if (out >= 0 && out != in) close(out);
    if (err >= 0) close(err);
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------------------------------------------------------------------------------------ small replies */

static int reply_simple(Conn *c, uint8_t type, uint32_t peer) {
    uint8_t m[5];
    m[0] = type;
    put_be32(m + 1, peer);
    return send_msg(c, m, sizeof m);
}

static int open_failure(Conn *c, uint32_t peer, uint32_t reason, const char *text) {
    uint8_t buf[160];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_CHANNEL_OPEN_FAILURE);
    wr_u32(&w, peer);
    wr_u32(&w, reason);
    wr_cstr(&w, text);
    wr_cstr(&w, "");
    return w.bad ? -1 : send_msg(c, buf, w.n);
}

static int send_exit_status(Conn *c, Chan *ch) {
    uint8_t buf[48];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_CHANNEL_REQUEST);
    wr_u32(&w, ch->peer);
    wr_cstr(&w, "exit-status");
    wr_bool(&w, false);
    wr_u32(&w, (uint32_t)ch->status);
    return send_msg(c, buf, w.n);
}

/* ------------------------------------------------------------------------------------------------ programs */

static void remember_to_reap(pid_t pid) {
    for (int i = 0; i < 16; i++)
        if (!g_reap[i].pid) { g_reap[i].pid = pid; g_reap[i].since = mono_ms(); return; }
    kill(-pid, SIGKILL);                                      /* no room to wait for it politely */
    waitpid(pid, NULL, 0);
}

static void reap_step(bool final) {
    for (int i = 0; i < 16; i++) {
        if (!g_reap[i].pid) continue;
        if (waitpid(g_reap[i].pid, NULL, WNOHANG) == g_reap[i].pid) { g_reap[i].pid = 0; continue; }
        if (final || mono_ms() - g_reap[i].since > 1500) {
            kill(-g_reap[i].pid, SIGKILL);
            waitpid(g_reap[i].pid, NULL, final ? 0 : WNOHANG);
            g_reap[i].pid = 0;
        }
    }
}

/* Called in the new process: becomes the resident's shell or command. Never returns. */
static void child_run(const Account *a, Chan *ch, const char *slave, int inp[2], int outp[2], int errp[2], const char *cmd, const char *program, const Conn *c) {
    setsid();
    if (ch->pty) {
        int s = open(slave, O_RDWR);                          /* as a session leader this becomes the controlling terminal */
        if (s < 0) _exit(126);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
    } else {
        dup2(inp[0], 0); dup2(outp[1], 1); dup2(errp[1], 2);
    }
    for (int fd = 3; fd < 64; fd++) close(fd);                /* the socket, the other channels, the listening socket: none of the program's business */
    signal(SIGPIPE, SIG_DFL);
    if (a->uid) {
        if (initgroups(a->name, (gid_t)a->gid) != 0) _exit(126);              /* (a resident who keeps the groups of sshd's own, root's, is worse than no login) */
        if (setgid((gid_t)a->gid) != 0 || setuid((uid_t)a->uid) != 0) _exit(126);
    }
    for (int guard = 0; environ && environ[0] && guard < 256; guard++) {          /* nothing of sshd's own environment goes along */
        char name[64];
        size_t n = strcspn(environ[0], "=");
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, environ[0], n);
        name[n] = 0;
        unsetenv(name);
    }
    setenv("HOME", a->home, 1);
    setenv("USER", a->name, 1);
    setenv("LOGNAME", a->name, 1);
    setenv("SHELL", a->shell, 1);
    setenv("PATH", "/bin:/sbin:/usr/bin", 1);
    char host[64];
    setenv("HOSTNAME", gethostname(host, sizeof host) == 0 ? host : "gensokyo", 1);
    setenv("TERM", ch->pty ? ch->term : "dumb", 1);
    char conn[200];
    snprintf(conn, sizeof conn, "%s %u %s %u", c->peer, c->peer_port, c->local, c->local_port);
    setenv("SSH_CONNECTION", conn, 1);
    snprintf(conn, sizeof conn, "%s %u %u", c->peer, c->peer_port, c->local_port);
    setenv("SSH_CLIENT", conn, 1);
    if (ch->pty) setenv("SSH_TTY", slave, 1);
    if (chdir(a->home) != 0) chdir("/");
    if (program) {                                            /* a subsystem: its own program, not the shell */
        execl(program, program, (char *)NULL);
        _exit(127);
    }
    if (cmd) {
        if (ch->pty) tcsetpgrp(0, getpid());                  /* not an interactive shell: Ctrl+C has to reach the command */
        execl(a->shell, a->shell, "-c", cmd, (char *)NULL);
    } else {
        execl(a->shell, a->shell, "-l", (char *)NULL);
    }
    _exit(127);
}

static int start_program(Conn *c, Chan *ch, const Account *a, const char *cmd, const char *program) {
    int inp[2] = { -1, -1 }, outp[2] = { -1, -1 }, errp[2] = { -1, -1 }, master = -1;
    char slave[40] = "";
    if (ch->pty) {
        master = ptyopen(slave, sizeof slave);
        if (master < 0) { slog("%s: no pty: %s", a->name, strerror(errno)); return -1; }
        struct winsize ws = { (unsigned short)ch->rows, (unsigned short)ch->cols, 0, 0 };
        ioctl(master, TIOCSWINSZ, &ws);
        chown(slave, (uid_t)a->uid, (gid_t)a->gid);          /* the resident's terminal, so that mesg, chmod and the like work */
    } else if (pipe(inp) < 0 || pipe(outp) < 0 || pipe(errp) < 0) {
        slog("%s: no pipes: %s", a->name, strerror(errno));
        for (int i = 0; i < 2; i++) { if (inp[i] >= 0) close(inp[i]); if (outp[i] >= 0) close(outp[i]); if (errp[i] >= 0) close(errp[i]); }
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        slog("%s: fork failed: %s", a->name, strerror(errno));
        if (master >= 0) close(master);
        for (int i = 0; i < 2; i++) { if (inp[i] >= 0) close(inp[i]); if (outp[i] >= 0) close(outp[i]); if (errp[i] >= 0) close(errp[i]); }
        return -1;
    }
    if (pid == 0) child_run(a, ch, slave, inp, outp, errp, cmd, program, c);
    ch->pid = pid;
    if (ch->pty) {
        ch->in_fd = ch->out_fd = master;
        ch->err_fd = -1;
        ch->err_done = true;
    } else {
        close(inp[0]); close(outp[1]); close(errp[1]);
        ch->in_fd = inp[1];
        ch->out_fd = outp[0];
        ch->err_fd = errp[0];
    }
    set_nonblock(ch->in_fd);
    set_nonblock(ch->out_fd);
    if (ch->err_fd >= 0) set_nonblock(ch->err_fd);
    ch->started = true;
    return 0;
}

static void signal_program(Chan *ch, int sig) {
    if (ch->started && !ch->exited && ch->pid > 0) kill(-ch->pid, sig);
}

/* ------------------------------------------------------------------------------------------------ channel life */

static void chan_free(Chan *ch) {
    close_chan_fds(ch);
    memset(ch, 0, sizeof *ch);
    ch->in_fd = ch->out_fd = ch->err_fd = -1;
}

/* Closes our side: the program gets its hangup, the peer our CLOSE. The slot is free once the peer's CLOSE has come as well. */
static int chan_close(Conn *c, Chan *ch) {
    if (ch->close_sent) return 0;
    close_chan_fds(ch);                                       /* a pty: the master, and closing it hangs up the foreground job */
    if (ch->started && !ch->exited) {
        signal_program(ch, SIGHUP);
        remember_to_reap(ch->pid);
        ch->exited = true;
    }
    ch->close_sent = true;
    int rc = reply_simple(c, MSG_CHANNEL_CLOSE, ch->peer);
    if (ch->close_got) chan_free(ch);
    return rc;
}

/* The program is done and its output has been sent: tell the peer how it went. */
static int chan_finish(Conn *c, Chan *ch) {
    if (ch->close_sent) return 0;
    if (send_exit_status(c, ch) < 0 || reply_simple(c, MSG_CHANNEL_EOF, ch->peer) < 0) return -1;
    slog("%s: session ended, exit status %d", ch->what, ch->status);
    return chan_close(c, ch);
}

static Chan *chan_of(uint32_t id) { return id < MAX_CHANNELS && g_ch[id].used ? &g_ch[id] : NULL; }

static bool safe_term(const uint8_t *t, size_t n) {
    if (n == 0 || n >= sizeof ((Chan *)0)->term) return false;
    for (size_t i = 0; i < n; i++)
        if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '-' || t[i] == '_' || t[i] == '.')) return false;
    return true;
}

static int on_channel_open(Conn *c, Rd *r) {
    size_t tl;
    const uint8_t *type = rd_str(r, &tl);
    uint32_t sender = rd_u32(r), win = rd_u32(r), max = rd_u32(r);
    if (r->bad) return -1;
    if (str_is(type, tl, "direct-tcpip")) return fwd_open(c, r, sender, win, max);      /* ssh -L, -D, -W: forward.c */
    if (!str_is(type, tl, "session")) return open_failure(c, sender, 3, "only session and direct-tcpip channels are supported");
    int id = -1;
    for (int i = 0; i < MAX_CHANNELS && id < 0; i++)
        if (!g_ch[i].used) id = i;
    if (id < 0) return open_failure(c, sender, 4, "too many channels");
    Chan *ch = &g_ch[id];
    memset(ch, 0, sizeof *ch);
    ch->used = true;
    ch->peer = sender;
    ch->peer_win = win;
    ch->peer_max = max < 128 ? 128 : max;
    ch->our_win = WINDOW;
    ch->in_fd = ch->out_fd = ch->err_fd = -1;
    ch->cols = 80;
    ch->rows = 24;
    snprintf(ch->term, sizeof ch->term, "xterm");
    uint8_t buf[24];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_CHANNEL_OPEN_CONFIRMATION);
    wr_u32(&w, sender);
    wr_u32(&w, (uint32_t)id);
    wr_u32(&w, WINDOW);
    wr_u32(&w, MAX_PACKET);
    return send_msg(c, buf, w.n);
}

static int signal_number(const uint8_t *s, size_t n) {
    static const struct { const char *name; int sig; } map[] = {
        { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "TERM", SIGTERM }, { "KILL", SIGKILL }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (str_is(s, n, map[i].name)) return map[i].sig;
    return 0;
}

static int on_channel_request(Conn *c, Rd *r, const Account *a) {
    uint32_t id = rd_u32(r);
    size_t tl;
    const uint8_t *type = rd_str(r, &tl);
    bool want = rd_bool(r);
    Chan *ch = chan_of(id);
    if (r->bad) return -1;
    if (!ch || ch->close_sent) return 0;
    bool ok = false;
    if (str_is(type, tl, "pty-req")) {
        size_t ml, nl;
        const uint8_t *term = rd_str(r, &nl);
        uint32_t cols = rd_u32(r), rows = rd_u32(r);
        rd_u32(r); rd_u32(r);
        rd_str(r, &ml);                                       /* the terminal modes: the pty's own defaults serve */
        if (!r->bad && !ch->started && !ch->pty) {
            ch->pty = true;
            if (safe_term(term, nl)) { memcpy(ch->term, term, nl); ch->term[nl] = 0; }
            ch->cols = cols && cols < 1000 ? cols : 80;
            ch->rows = rows && rows < 1000 ? rows : 24;
            ok = true;
        }
    } else if (str_is(type, tl, "shell") || str_is(type, tl, "exec")) {
        bool exec = str_is(type, tl, "exec");
        size_t cl = 0;
        const uint8_t *cmd = exec ? rd_str(r, &cl) : (const uint8_t *)"";
        static char cmdz[4096];
        if (!r->bad && !ch->started && (!exec || (cl > 0 && cl < sizeof cmdz))) {
            memcpy(cmdz, cmd, cl);
            cmdz[cl] = 0;
            if (exec) {
                char shown[sizeof ch->what - 8];
                if (g_log_commands) {
                    printable(shown, sizeof shown, cmd, cl);
                    snprintf(ch->what, sizeof ch->what, "%s: exec %s", a->name, shown);
                } else {                                      /* a command line can hold things that do not belong in a log: the first word and the length are enough to follow what happened */
                    size_t fw = 0;
                    while (fw < cl && cmd[fw] != ' ' && cmd[fw] != '\t') fw++;
                    char first[24];
                    printable(first, sizeof first, cmd, fw);
                    snprintf(ch->what, sizeof ch->what, "%s: exec %s (%zu bytes; sshd.logcmds shows them)", a->name, first, cl);
                }
            } else {
                snprintf(ch->what, sizeof ch->what, "%s: shell%s", a->name, ch->pty ? "" : " (no terminal)");
            }
            slog("%s: %s opened%s", c->peer, ch->what, ch->pty ? " on a pty" : "");
            ok = start_program(c, ch, a, exec ? cmdz : NULL, NULL) == 0;
        }
    } else if (str_is(type, tl, "subsystem")) {
        size_t nl;
        const uint8_t *name = rd_str(r, &nl);
        if (!r->bad && str_is(name, nl, "sftp") && !ch->started && !ch->pty && access("/bin/sftp-server", X_OK) == 0) {
            snprintf(ch->what, sizeof ch->what, "%s: sftp", a->name);
            slog("%s: %s opened", c->peer, ch->what);
            ok = start_program(c, ch, a, NULL, "/bin/sftp-server") == 0;
        }
    } else if (str_is(type, tl, "window-change")) {
        uint32_t cols = rd_u32(r), rows = rd_u32(r);
        if (!r->bad && ch->pty && ch->started && ch->in_fd >= 0 && cols && rows && cols < 1000 && rows < 1000) {
            struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
            ioctl(ch->in_fd, TIOCSWINSZ, &ws);
        }
        return 0;
    } else if (str_is(type, tl, "signal")) {
        size_t sl;
        const uint8_t *name = rd_str(r, &sl);
        int sig = r->bad ? 0 : signal_number(name, sl);
        if (sig) signal_program(ch, sig);
        return 0;
    }
    if (!want) return 0;
    return reply_simple(c, ok ? MSG_CHANNEL_SUCCESS : MSG_CHANNEL_FAILURE, ch->peer);
}

static int on_channel_data(Conn *c, Rd *r, bool extended) {
    uint32_t id = rd_u32(r);
    if (extended) rd_u32(r);
    size_t n;
    const uint8_t *data = rd_str(r, &n);
    if (r->bad) return -1;
    Chan *ch = chan_of(id);
    if (!ch) return 0;
    if (n > ch->our_win) {
        slog("%s: the peer sent %zu bytes into a window of %u", c->peer, n, (unsigned)ch->our_win);
        send_disconnect(c, DISC_PROTOCOL_ERROR, "window exceeded");
        return -1;
    }
    ch->our_win -= (uint32_t)n;
    if (extended || !ch->started || ch->in_fd < 0 || ch->close_sent) {       /* nowhere to put it: taken and dropped */
        ch->to_adjust += (uint32_t)n;
        return 0;
    }
    memcpy(ch->inbuf + ch->in_len, data, n);                  /* fits: in_len + our_win + to_adjust never exceeds WINDOW */
    ch->in_len += n;
    return 0;
}

/* Gives bytes back to the peer once the program has taken them. */
static int give_window_back(Conn *c, Chan *ch) {
    if (ch->close_sent || ch->to_adjust < WINDOW / 4) return 0;      /* one packet per quarter window, not one per keystroke; the window never closes
                                                                          for good because our_win + in_len + to_adjust is always WINDOW */
    uint8_t m[12];
    m[0] = MSG_CHANNEL_WINDOW_ADJUST;
    put_be32(m + 1, ch->peer);
    put_be32(m + 5, ch->to_adjust);
    ch->our_win += ch->to_adjust;
    ch->to_adjust = 0;
    return send_msg(c, m, 9);
}

static int on_global_request(Conn *c, Rd *r) {
    size_t nl;
    const uint8_t *name = rd_str(r, &nl);
    bool want = rd_bool(r);
    if (r->bad) return -1;
    bool cancel = str_is(name, nl, "cancel-tcpip-forward");
    if (cancel || str_is(name, nl, "tcpip-forward")) return fwd_global(c, cancel, r, want);      /* ssh -R: forward.c */
    if (!want) return 0;
    uint8_t m = MSG_REQUEST_FAILURE;
    return send_msg(c, &m, 1);
}

static int dispatch(Conn *c, const uint8_t *pl, size_t n, const Account *a) {
    Rd r;
    rd_init(&r, pl, n);
    uint8_t t = rd_u8(&r);
    if (t >= MSG_CHANNEL_OPEN_CONFIRMATION && t <= MSG_CHANNEL_FAILURE && n >= 5) {        /* a message about one of our channels: a forward's? */
        uint32_t id = (uint32_t)pl[1] << 24 | (uint32_t)pl[2] << 16 | (uint32_t)pl[3] << 8 | pl[4];
        if (fwd_owns(id)) return fwd_message(c, t, &r);
    }
    switch (t) {
    case MSG_CHANNEL_OPEN:
        return on_channel_open(c, &r);
    case MSG_CHANNEL_REQUEST:
        return on_channel_request(c, &r, a);
    case MSG_CHANNEL_DATA:
        return on_channel_data(c, &r, false);
    case MSG_CHANNEL_EXTENDED_DATA:
        return on_channel_data(c, &r, true);
    case MSG_CHANNEL_WINDOW_ADJUST: {
        Chan *ch = chan_of(rd_u32(&r));
        uint32_t add = rd_u32(&r);
        if (r.bad) return -1;
        if (ch) ch->peer_win = add > 0xFFFFFFFFu - ch->peer_win ? 0xFFFFFFFFu : ch->peer_win + add;
        return 0;
    }
    case MSG_CHANNEL_EOF: {
        Chan *ch = chan_of(rd_u32(&r));
        if (ch) ch->client_eof = true;
        return 0;
    }
    case MSG_CHANNEL_CLOSE: {
        Chan *ch = chan_of(rd_u32(&r));
        if (!ch) return 0;
        ch->close_got = true;
        if (ch->close_sent) chan_free(ch);
        else return chan_close(c, ch);
        return 0;
    }
    case MSG_CHANNEL_SUCCESS: case MSG_CHANNEL_FAILURE: case MSG_REQUEST_SUCCESS: case MSG_REQUEST_FAILURE:
        return 0;
    case MSG_GLOBAL_REQUEST:
        return on_global_request(c, &r);
    case MSG_USERAUTH_REQUEST: {                              /* a late or repeated attempt: already in, so it is simply a success */
        uint8_t ok = MSG_USERAUTH_SUCCESS;
        return send_msg(c, &ok, 1);
    }
    default:
        send_unimplemented(c);
        return 0;
    }
}

/* ------------------------------------------------------------------------------------------------ moving bytes */

/* Reads what the program wrote and sends it, as far as the peer's window and packet size allow. */
static int pump_output(Conn *c, Chan *ch, bool is_err) {
    int fd = is_err ? ch->err_fd : ch->out_fd;
    bool *done = is_err ? &ch->err_done : &ch->out_done;
    uint32_t room = ch->peer_win < ch->peer_max ? ch->peer_win : ch->peer_max;
    if (room > CHUNK) room = CHUNK;
    if (fd < 0 || *done || room == 0) return 0;
    size_t head = is_err ? 13 : 9;
    ssize_t got = read(fd, g_msg + head, room);
    if (got > 0) {
        g_msg[0] = is_err ? MSG_CHANNEL_EXTENDED_DATA : MSG_CHANNEL_DATA;
        put_be32(g_msg + 1, ch->peer);
        if (is_err) put_be32(g_msg + 5, 1);
        put_be32(g_msg + head - 4, (uint32_t)got);
        ch->peer_win -= (uint32_t)got;
        return send_msg(c, g_msg, head + (size_t)got);
    }
    if (got == 0 || (errno != EAGAIN && errno != EINTR)) *done = true;       /* end of file, or the pty is gone (EIO) */
    return 0;
}

/* Hands the peer's bytes to the program. */
static void pump_input(Chan *ch) {
    if (ch->in_len == 0 || ch->in_fd < 0) return;
    size_t chunk = ch->pty && ch->in_len > 1024 ? 1024 : ch->in_len;      /* a write to a pty master is not interrupted: keep the loop responsive */
    ssize_t put = write(ch->in_fd, ch->inbuf, chunk);
    if (put > 0) {
        memmove(ch->inbuf, ch->inbuf + put, ch->in_len - (size_t)put);
        ch->in_len -= (size_t)put;
        ch->to_adjust += (uint32_t)put;
    } else if (put < 0 && errno != EAGAIN && errno != EINTR) {         /* the program closed its stdin (or the pty went away): the bytes have no taker */
        ch->to_adjust += (uint32_t)ch->in_len;
        ch->in_len = 0;
        if (!ch->pty) { close(ch->in_fd); ch->in_fd = -1; }
    }
}

/* Called once per round for every channel: moves bytes, collects the exit status, ends the channel when it is time. */
static int service(Conn *c, Chan *ch, const struct pollfd *pin, const struct pollfd *pout, const struct pollfd *perr) {
    if (!ch->used || ch->close_sent) return 0;
    if (pin && (pin->revents & (POLLOUT | POLLERR | POLLHUP))) pump_input(ch);
    if (ch->started) {
        if (pout && (pout->revents & (POLLIN | POLLHUP | POLLERR)) && pump_output(c, ch, false) < 0) return -1;
        if (perr && (perr->revents & (POLLIN | POLLHUP | POLLERR)) && pump_output(c, ch, true) < 0) return -1;
        if (give_window_back(c, ch) < 0) return -1;
        /* the peer is finished sending and everything it sent has been taken: a pipe-fed program sees end of file */
        if (ch->client_eof && ch->in_len == 0 && !ch->pty && ch->in_fd >= 0) { close(ch->in_fd); ch->in_fd = -1; }
        if (!ch->exited) {
            int st = 0;
            if (waitpid(ch->pid, &st, WNOHANG) == ch->pid) {
                ch->exited = true;
                ch->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                ch->exited_at = mono_ms();
            }
        }
        if (ch->exited) {
            bool drained = ch->out_done && ch->err_done;
            if (ch->peer_win == 0) ch->exited_at = mono_ms();                 /* waiting for the peer, not for the program */
            if (drained || mono_ms() - ch->exited_at > EXIT_GRACE_MS) {
                while (!drained && ch->peer_win > 0) {                              /* the last of it */
                    bool was_out = ch->out_done, was_err = ch->err_done;
                    uint32_t before = ch->peer_win;
                    if (pump_output(c, ch, false) < 0 || pump_output(c, ch, true) < 0) return -1;
                    drained = ch->out_done && ch->err_done;
                    if (before == ch->peer_win && was_out == ch->out_done && was_err == ch->err_done) break;
                }
                return chan_finish(c, ch);
            }
        }
    }
    return 0;
}

int session_run(Conn *c, const Account *a) {
    for (int i = 0; i < MAX_CHANNELS; i++) {
        memset(&g_ch[i], 0, sizeof g_ch[i]);
        g_ch[i].in_fd = g_ch[i].out_fd = g_ch[i].err_fd = -1;
    }
    int rc = 0, probes = 0;
    long last_probe = 0;
    c->last_rx = mono_ms();
    fwd_begin(c, a);
    for (;;) {
        struct pollfd pf[1 + 3 * MAX_CHANNELS + FWD_MAX_HARD + FWD_LISTENERS];    /* the socket, the session programs, the forwards (at most 64 in all) */
        struct pollfd *slot[MAX_CHANNELS][3] = { { NULL } };
        int np = 0;
        pf[np++] = (struct pollfd){ .fd = c->fd, .events = POLLIN };
        bool busy = false;
        for (int i = 0; i < MAX_CHANNELS; i++) {
            Chan *ch = &g_ch[i];
            if (!ch->used || ch->close_sent || !ch->started) continue;
            busy = true;
            if (ch->in_len > 0 && ch->in_fd >= 0) { pf[np] = (struct pollfd){ .fd = ch->in_fd, .events = POLLOUT }; slot[i][0] = &pf[np++]; }
            if (ch->out_fd >= 0 && !ch->out_done && ch->peer_win > 0) {
                pf[np] = (struct pollfd){ .fd = ch->out_fd, .events = POLLIN };
                slot[i][1] = &pf[np++];
            }
            if (ch->err_fd >= 0 && !ch->err_done && ch->peer_win > 0) { pf[np] = (struct pollfd){ .fd = ch->err_fd, .events = POLLIN }; slot[i][2] = &pf[np++]; }
        }
        np = fwd_poll_add(pf, np, (int)(sizeof pf / sizeof pf[0]));
        int pr = poll(pf, (nfds_t)np, busy || fwd_busy() ? 100 : 1000);
        if (pr < 0 && errno != EINTR) { rc = -1; break; }
        if (pr > 0 && (pf[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            const uint8_t *pl;
            int n = next_msg_step(c, &pl);
            if (n == 0 || n == -1) { rc = n < 0 ? -1 : 0; break; }
            if (n > 0 && dispatch(c, pl, (size_t)n, a) < 0) { rc = -1; break; }
        }
        bool failed = false;
        for (int i = 0; i < MAX_CHANNELS && !failed; i++) {
            struct pollfd none = { .fd = -1 };
            const struct pollfd *pin = slot[i][0] ? slot[i][0] : &none, *pout = slot[i][1] ? slot[i][1] : &none, *perr = slot[i][2] ? slot[i][2] : &none;
            if (service(c, &g_ch[i], pin, pout, perr) < 0) failed = true;
        }
        if (failed || fwd_service(c, pf) < 0) { rc = -1; break; }
        reap_step(false);
        /* A client that vanished (a laptop that went to sleep, a cable pulled) would keep its programs and its place among the 16 for ever: ask
         * whoever is silent, and drop the connection when three rounds go unanswered. Any packet counts as an answer. */
        if (c->keepalive_ms) {
            long now = mono_ms();
            if (now - c->last_rx < (long)c->keepalive_ms) {
                probes = 0;
            } else if (now - last_probe >= (long)c->keepalive_ms) {
                if (probes >= 3) { slog("%s: the client has not answered for %d probes: connection dropped", c->peer, probes); rc = -1; break; }
                uint8_t m[40];
                Wr w;
                wr_init(&w, m, sizeof m);
                wr_u8(&w, MSG_GLOBAL_REQUEST);
                wr_cstr(&w, "keepalive@openssh.com");
                wr_bool(&w, true);
                if (send_msg(c, m, w.n) < 0) { rc = -1; break; }
                probes++;
                last_probe = now;
            }
        }
        (void)busy;
    }
    fwd_end();                                                /* the forwarded connections end with it */
    /* the connection is over: every program still running hears a hangup, and is shot if it ignores it */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        Chan *ch = &g_ch[i];
        if (!ch->used) continue;
        close_chan_fds(ch);
        if (ch->started && !ch->exited) { signal_program(ch, SIGHUP); remember_to_reap(ch->pid); }
    }
    long t0 = mono_ms();
    for (;;) {
        bool any = false;
        for (int i = 0; i < 16; i++) any |= g_reap[i].pid != 0;
        if (!any) break;
        reap_step(mono_ms() - t0 > 1000);
        usleep(50000);
    }
    return rc;
}
