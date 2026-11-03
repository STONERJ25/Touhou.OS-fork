/*
 * Port forwarding (RFC 4254, section 7).
 *
 *   -L [bind:]port:host:hostport   listens on a port of this machine; every connection to it is carried to host:hostport as seen from the server ("direct-tcpip")
 *   -D [bind:]port                 the same with the destination chosen by the program that connects: a SOCKS server (versions 4, 4a and 5, CONNECT only)
 *   -R [bind:]port:host:hostport   asks the server to listen on a port ("tcpip-forward"); every connection it gets comes back as a "forwarded-tcpip" channel and is
 *                                  made to host:hostport from here
 *
 * Listeners are bound to the loopback address unless an address is given: forwarding only adds reach if the person asks for it. Each forwarded connection is a
 * channel; bytes are moved with the channel's windows as the only buffers: data from the server is acknowledged (the window reopened) only after it has been
 * written to the socket, and the socket is read only while the server's window is open, so a slow side slows the other down instead of filling memory.
 *
 * A forwarded connection's record outlives its socket: it is freed when the channel is (the channel's "closed" handler), because messages for the channel can still
 * arrive until the server's CLOSE has.
 */
#include "ssh.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#define MAX_LISTENERS 16
#define MAX_REMOTE 16

typedef struct FwdConn {
    int fd;
    SshcChan *ch;
    bool opening, socks, socks5, socks_pending_reply;
    bool eof_in, sock_eof, shut_wr, dead;
    uint8_t hs[300];                                    /* the SOCKS handshake so far */
    size_t hs_len;
    uint8_t *out;                                       /* from the server, not yet taken by the socket (at most the window we granted) */
    size_t out_len, out_off, out_cap;
    struct FwdConn *next;
    char desc[96];
} FwdConn;

typedef struct { int fd; char kind; char addr[64]; int port; char host[256]; int hostport; } Listener;
typedef struct { char addr[64]; int port; char host[256]; int hostport; bool active; } Remote;

struct Forwards {
    Listener lis[MAX_LISTENERS];
    int nlis;
    Remote rem[MAX_REMOTE];
    int nrem;
    FwdConn *conns;
    struct { int kind; void *ptr; } map[MAX_LISTENERS + 128];      /* what each descriptor handed to poll belongs to: 1 a listener, 2 a connection */
    int nmap;
    SshcConn *c;
    int pending_global;                                 /* tcpip-forward answers still to come */
    bool remote_failed;
    int last_remote_port;
};

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    if (sshc_verbose < 0) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs("\r\n", stderr);
}

/* ------------------------------------------------------------------------------------------------ the specifications */

/* "[addr:]port:host:hostport" (kinds L and R) or "[addr:]port" (D). */
static bool parse_spec(const char *spec, char kind, char *addr, int *port, char *host, int *hostport) {
    char buf[400];
    sshc_cpy(buf, sizeof buf, spec);
    char *f[5];
    int n = 0;
    for (char *p = buf; n < 5;) {
        f[n++] = p;
        char *colon = strchr(p, ':');
        if (!colon) break;
        *colon = 0;
        p = colon + 1;
    }
    int want = kind == 'D' ? 1 : 3;
    sshc_cpy(addr, 64, kind == 'R' ? "localhost" : "127.0.0.1");                 /* a remote forward asks the server for "localhost": its own loopback */
    int i = 0;
    if (n == want + 1) {
        if (kind == 'R') sshc_cpy(addr, 64, !strcmp(f[0], "*") ? "" : f[0]);       /* "" : every address, if the server allows it */
        else sshc_cpy(addr, 64, !f[0][0] || !strcmp(f[0], "*") ? "0.0.0.0" : !strcmp(f[0], "localhost") ? "127.0.0.1" : f[0]);
        i = 1;
    } else if (n != want) {
        return false;
    }
    *port = atoi(f[i]);
    if (*port < 0 || *port > 65535 || (*port == 0 && kind != 'R')) return false;
    if (kind != 'D') {
        sshc_cpy(host, 256, f[i + 1]);
        *hostport = atoi(f[i + 2]);
        if (!host[0] || *hostport < 1 || *hostport > 65535) return false;
    }
    return true;
}

static int listen_on(const char *where, int port, char *why, size_t cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(why, cap, "%s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, where, &sa.sin_addr) != 1) { snprintf(why, cap, "bad address '%s'", where); close(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 16) < 0) { snprintf(why, cap, "%s", strerror(errno)); close(fd); return -1; }
    set_nonblock(fd);
    return fd;
}

/* ------------------------------------------------------------------------------------------------ connections */

static FwdConn *conn_new(Forwards *f, int fd, const char *desc) {
    FwdConn *fc = calloc(1, sizeof *fc);
    if (!fc) return NULL;
    fc->fd = fd;
    sshc_cpy(fc->desc, sizeof fc->desc, desc);
    fc->next = f->conns;
    f->conns = fc;
    return fc;
}

static void conn_unlink(Forwards *f, FwdConn *fc) {
    for (FwdConn **pp = &f->conns; *pp; pp = &(*pp)->next)
        if (*pp == fc) { *pp = fc->next; break; }
    if (fc->fd >= 0) close(fc->fd);
    free(fc->out);
    free(fc);
}

static void close_socket(FwdConn *fc) {
    if (fc->fd >= 0) { close(fc->fd); fc->fd = -1; }
}

/* We want out: the socket goes, the channel is closed (the record stays until the server's CLOSE has come). */
static void conn_finish(SshcConn *c, FwdConn *fc) {
    if (fc->dead) return;
    fc->dead = true;
    close_socket(fc);
    if (fc->ch) sshc_chan_close(c, fc->ch);
}

/* Bytes the server sent us are written to the socket as far as it takes them; the rest waits in fc->out. */
static void push_out(SshcConn *c, FwdConn *fc) {
    while (fc->out_off < fc->out_len && fc->fd >= 0) {
        ssize_t w = write(fc->fd, fc->out + fc->out_off, fc->out_len - fc->out_off);
        if (w > 0) {
            fc->out_off += (size_t)w;
            if (fc->ch) sshc_chan_consumed(c, fc->ch, (size_t)w);          /* only now may the server send more */
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        conn_finish(c, fc);                                                  /* the program on the socket is gone */
        return;
    }
    if (fc->out_off == fc->out_len) fc->out_off = fc->out_len = 0;
    if (fc->eof_in && fc->out_len == 0 && !fc->shut_wr && fc->fd >= 0) { shutdown(fc->fd, SHUT_WR); fc->shut_wr = true; }
    if (fc->eof_in && fc->sock_eof) conn_finish(c, fc);
}

static void on_confirmed(SshcConn *c, SshcChan *ch) {
    FwdConn *fc = ch->user;
    fc->opening = false;
    if (fc->socks_pending_reply && fc->fd >= 0) {
        static const uint8_t ok5[10] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 }, ok4[8] = { 0, 0x5a, 0, 0, 0, 0, 0, 0 };
        ssize_t w = write(fc->fd, fc->socks5 ? ok5 : ok4, fc->socks5 ? sizeof ok5 : sizeof ok4);
        (void)w;
        fc->socks_pending_reply = false;
    }
    (void)c;
}

static void on_open_failed(SshcConn *c, SshcChan *ch, uint32_t reason, const char *text) {
    FwdConn *fc = ch->user;
    note("channel %u: open failed: %s%s", (unsigned)ch->id, reason == SSHC_OPEN_ADMIN_PROHIBITED ? "administratively prohibited: " : "connect failed: ", text);
    if (fc->socks_pending_reply && fc->fd >= 0) {
        static const uint8_t no5[10] = { 5, 5, 0, 1, 0, 0, 0, 0, 0, 0 }, no4[8] = { 0, 0x5b, 0, 0, 0, 0, 0, 0 };
        ssize_t w = write(fc->fd, fc->socks5 ? no5 : no4, fc->socks5 ? sizeof no5 : sizeof no4);
        (void)w;
    }
    close_socket(fc);
    fc->dead = true;
    fc->ch = NULL;
    (void)c;
    /* the record is freed with the others at the next service round (it is dead and has no channel) */
}

static void on_data(SshcConn *c, SshcChan *ch, const uint8_t *d, size_t n, uint32_t ext) {
    FwdConn *fc = ch->user;
    if (ext != 0 || fc->dead || fc->fd < 0) { sshc_chan_consumed(c, ch, n); return; }
    size_t off = 0;
    if (fc->out_len == fc->out_off) {                                         /* nothing waiting: try the socket straight away */
        while (off < n) {
            ssize_t w = write(fc->fd, d + off, n - off);
            if (w > 0) { off += (size_t)w; sshc_chan_consumed(c, ch, (size_t)w); continue; }
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            conn_finish(c, fc);
            sshc_chan_consumed(c, ch, n - off);
            return;
        }
    }
    if (off < n) {
        size_t rest = n - off;
        if (fc->out_len + rest > fc->out_cap) {
            size_t nc = fc->out_cap ? fc->out_cap : 65536;
            while (nc < fc->out_len + rest) nc *= 2;
            uint8_t *nb = realloc(fc->out, nc);
            if (!nb) { conn_finish(c, fc); sshc_chan_consumed(c, ch, rest); return; }
            fc->out = nb;
            fc->out_cap = nc;
        }
        memcpy(fc->out + fc->out_len, d + off, rest);
        fc->out_len += rest;
    }
}

static void on_eof(SshcConn *c, SshcChan *ch) {
    FwdConn *fc = ch->user;
    fc->eof_in = true;
    push_out(c, fc);
}

static void on_closed(SshcConn *c, SshcChan *ch) {
    (void)c;
    FwdConn *fc = ch->user;
    fc->ch = NULL;
    fc->dead = true;
    close_socket(fc);
}

static bool on_request(SshcConn *c, SshcChan *ch, const char *name, SshcRd *args, bool want) { (void)c; (void)ch; (void)name; (void)args; (void)want; return false; }
static void on_reply(SshcConn *c, SshcChan *ch, bool ok) { (void)c; (void)ch; (void)ok; }

static const SshcChanOps FWD_OPS = { on_confirmed, on_open_failed, on_data, on_eof, on_closed, on_request, on_reply };

/* Opens a direct-tcpip channel for a connection that has arrived at a listener. */
static void open_direct(Forwards *f, SshcConn *c, FwdConn *fc, const char *host, int port, const char *orig, int orig_port) {
    uint8_t extra[600];
    SshcWr w;
    sshc_wr_init(&w, extra, sizeof extra);
    sshc_wr_cstr(&w, host);
    sshc_wr_u32(&w, (uint32_t)port);
    sshc_wr_cstr(&w, orig);
    sshc_wr_u32(&w, (uint32_t)orig_port);
    fc->ch = sshc_chan_open(c, "direct-tcpip", extra, w.n, &FWD_OPS, fc);
    if (!fc->ch) {
        note("channel open failed: too many forwarded connections");
        close_socket(fc);
        fc->dead = true;
        return;
    }
    fc->opening = true;
    (void)f;
}

/* ------------------------------------------------------------------------------------------------ SOCKS */

/* Looks at the handshake bytes so far. Returns 1 when a CONNECT request is complete (host and port set), 0 when more is needed, -1 for a refusal (an answer has
 * been written to the client). */
static int socks_step(FwdConn *fc, char *host, size_t hcap, int *port) {
    uint8_t *h = fc->hs;
    size_t n = fc->hs_len;
    if (n < 1) return 0;
    if (!fc->socks5 && !fc->socks_pending_reply && h[0] == 5 && !fc->socks) {      /* first bytes of a SOCKS5 client: the method list */
        if (n < 2) return 0;
        size_t need = 2 + h[1];
        if (n < need) return 0;
        bool no_auth = false;
        for (size_t i = 0; i < h[1]; i++) if (h[2 + i] == 0) no_auth = true;
        uint8_t reply[2] = { 5, no_auth ? 0 : 0xff };
        ssize_t w = write(fc->fd, reply, 2);
        (void)w;
        if (!no_auth) return -1;
        fc->socks = fc->socks5 = true;
        fc->hs_len = 0;
        return 0;
    }
    if (fc->socks5) {
        if (n < 4) return 0;
        if (h[0] != 5 || h[1] != 1) { uint8_t r[10] = { 5, h[1] == 1 ? 1 : 7, 0, 1, 0, 0, 0, 0, 0, 0 }; ssize_t w = write(fc->fd, r, 10); (void)w; return -1; }
        size_t need;
        if (h[3] == 1) need = 4 + 4 + 2;
        else if (h[3] == 3) { if (n < 5) return 0; need = 4 + 1 + h[4] + 2; }
        else { uint8_t r[10] = { 5, 8, 0, 1, 0, 0, 0, 0, 0, 0 }; ssize_t w = write(fc->fd, r, 10); (void)w; return -1; }   /* IPv6: not available here */
        if (n < need) return 0;
        if (h[3] == 1) snprintf(host, hcap, "%u.%u.%u.%u", h[4], h[5], h[6], h[7]);
        else { size_t l = h[4] < hcap - 1 ? h[4] : hcap - 1; memcpy(host, h + 5, l); host[l] = 0; }
        *port = h[need - 2] << 8 | h[need - 1];
        return 1;
    }
    if (h[0] == 4) {                                                           /* SOCKS4 and 4a: version, command, port, address, user id NUL [host NUL] */
        if (n < 9) return 0;
        size_t i = 8;
        while (i < n && h[i] != 0) i++;
        if (i >= n) return n < sizeof fc->hs ? 0 : -1;
        bool a4 = h[4] == 0 && h[5] == 0 && h[6] == 0 && h[7] != 0;
        size_t end = i + 1;
        if (a4) {
            size_t j = end;
            while (j < n && h[j] != 0) j++;
            if (j >= n) return n < sizeof fc->hs ? 0 : -1;
            size_t l = j - end < hcap - 1 ? j - end : hcap - 1;
            memcpy(host, h + end, l);
            host[l] = 0;
        } else {
            snprintf(host, hcap, "%u.%u.%u.%u", h[4], h[5], h[6], h[7]);
        }
        *port = h[2] << 8 | h[3];
        if (h[1] != 1) { uint8_t r[8] = { 0, 0x5b, 0, 0, 0, 0, 0, 0 }; ssize_t w = write(fc->fd, r, 8); (void)w; return -1; }
        fc->socks = true;
        fc->socks5 = false;
        return 1;
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------------ setting up */

Forwards *forwards_new(SshcConn *c, const SessionOpts *o) {
    (void)o;
    Forwards *f = calloc(1, sizeof *f);
    if (f) f->c = c;
    return f;
}

static void remote_reply(void *ctx, SshcConn *c, bool ok, SshcRd *data) {
    Forwards *f = ctx;
    (void)c;
    f->pending_global--;
    if (!ok) f->remote_failed = true;
    else if (data && data->n >= 4) f->last_remote_port = (int)sshc_rd_u32(data);
}

static int on_server_open(void *ctx, SshcConn *c, const char *type, SshcRd *args, SshcChan *ch) {
    Forwards *f = ctx;
    if (strcmp(type, "forwarded-tcpip") != 0) return SSHC_OPEN_UNKNOWN_CHANNEL_TYPE;
    size_t al, ol;
    const uint8_t *addr = sshc_rd_str(args, &al);
    uint32_t port = sshc_rd_u32(args);
    sshc_rd_str(args, &ol);
    sshc_rd_u32(args);
    if (args->bad) return SSHC_OPEN_ADMIN_PROHIBITED;
    Remote *r = NULL;
    for (int i = 0; i < f->nrem && !r; i++)
        if (f->rem[i].active && ((f->rem[i].port == (int)port) || (f->rem[i].port == 0 && f->last_remote_port == (int)port))) r = &f->rem[i];
    (void)addr; (void)al;
    if (!r) return SSHC_OPEN_ADMIN_PROHIBITED;                                 /* a port nobody asked for */
    char why[200], ip[48];
    int fd = sshc_tcp_connect(r->host, r->hostport, 5000, why, sizeof why, ip, sizeof ip);
    if (fd < 0) { note("connect_to %s port %d: failed (%s)", r->host, r->hostport, why); return SSHC_OPEN_CONNECT_FAILED; }
    char desc[96];
    snprintf(desc, sizeof desc, "remote forward to %.60s:%d", r->host, r->hostport);
    FwdConn *fc = conn_new(f, fd, desc);
    if (!fc) { close(fd); return SSHC_OPEN_RESOURCE_SHORTAGE; }
    fc->ch = ch;
    ch->ops = &FWD_OPS;
    ch->user = fc;
    (void)c;
    return 0;
}

int forwards_start(Forwards *f, SshcConn *c, const SessionOpts *o) {
    int rc = 0;
    for (int i = 0; i < o->nlocal + o->ndynamic; i++) {
        bool dyn = i >= o->nlocal;
        const char *spec = dyn ? o->dynamic[i - o->nlocal] : o->local[i];
        Listener *l = &f->lis[f->nlis];
        if (f->nlis >= MAX_LISTENERS) { fprintf(stderr, "ssh: too many forwardings\n"); rc = -1; break; }
        memset(l, 0, sizeof *l);
        l->kind = dyn ? 'D' : 'L';
        if (!parse_spec(spec, l->kind, l->addr, &l->port, l->host, &l->hostport)) {
            fprintf(stderr, "ssh: bad forwarding specification '%s'\n", spec);
            rc = -1;
            continue;
        }
        char why[200];
        l->fd = listen_on(l->addr, l->port, why, sizeof why);
        if (l->fd < 0) {
            fprintf(stderr, "ssh: bind [%s]:%d: %s\nssh: Could not request local forwarding.\n", l->addr, l->port, why);
            rc = -1;
            continue;
        }
        sshc_debug(1, "Local connections to %s:%d forwarded to %s", l->addr, l->port, l->kind == 'D' ? "a SOCKS client's choice" : l->host);
        f->nlis++;
    }
    c->open_fn = on_server_open;
    c->open_ctx = f;
    for (int i = 0; i < o->nremote; i++) {
        if (f->nrem >= MAX_REMOTE) { rc = -1; break; }
        Remote *r = &f->rem[f->nrem];
        memset(r, 0, sizeof *r);
        if (!parse_spec(o->remote[i], 'R', r->addr, &r->port, r->host, &r->hostport)) {
            fprintf(stderr, "ssh: bad forwarding specification '%s'\n", o->remote[i]);
            rc = -1;
            continue;
        }
        uint8_t args[300];
        SshcWr w;
        sshc_wr_init(&w, args, sizeof args);
        sshc_wr_cstr(&w, r->addr);
        sshc_wr_u32(&w, (uint32_t)r->port);
        f->pending_global++;
        f->remote_failed = false;
        if (sshc_global_request(c, "tcpip-forward", args, w.n, remote_reply, f) < 0) { rc = -1; break; }
        while (f->pending_global > 0) {                                        /* the answer decides whether the forwarding exists */
            if (sshc_pump(c, 30000) < 0) { fprintf(stderr, "%s\n", c->err); return -1; }
        }
        if (f->remote_failed) {
            fprintf(stderr, "Warning: remote port forwarding failed for listen port %d\n", r->port);
            rc = -1;
            continue;
        }
        r->active = true;
        f->nrem++;
        sshc_debug(1, "Remote connections from %s:%d forwarded to local address %s:%d", r->addr[0] ? r->addr : "*", r->port, r->host, r->hostport);
    }
    if (rc < 0 && !o->exit_on_forward_failure) rc = 0;                         /* a forwarding that did not come up is said so, and the session goes on */
    return rc;
}

/* ------------------------------------------------------------------------------------------------ the loop's part */

int forwards_fds(Forwards *f, struct pollfd *fds, int max) {
    int n = 0;
    f->nmap = 0;
    for (int i = 0; i < f->nlis && n < max && f->nmap < (int)(sizeof f->map / sizeof f->map[0]); i++) {
        fds[n] = (struct pollfd){ .fd = f->lis[i].fd, .events = POLLIN };
        f->map[f->nmap].kind = 1;
        f->map[f->nmap++].ptr = &f->lis[i];
        n++;
    }
    for (FwdConn *fc = f->conns; fc && n < max && f->nmap < (int)(sizeof f->map / sizeof f->map[0]); fc = fc->next) {
        if (fc->fd < 0 || fc->dead) continue;
        short ev = 0;
        bool handshake = !fc->ch && !fc->opening && !fc->dead;                  /* a SOCKS client still talking to us */
        if (handshake) ev |= POLLIN;
        else if (fc->ch && fc->ch->open && !fc->sock_eof && sshc_chan_can_send(f->c, fc->ch)) ev |= POLLIN;
        if (fc->out_off < fc->out_len) ev |= POLLOUT;
        fds[n] = (struct pollfd){ .fd = fc->fd, .events = ev };
        f->map[f->nmap].kind = 2;
        f->map[f->nmap++].ptr = fc;
        n++;
    }
    return n;
}

static void accept_one(Forwards *f, SshcConn *c, Listener *l) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    int fd = accept(l->fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) return;
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char orig[48] = "127.0.0.1";
    inet_ntop(AF_INET, &sa.sin_addr, orig, sizeof orig);
    char desc[96];
    snprintf(desc, sizeof desc, "%s %.40s:%d", l->kind == 'D' ? "dynamic" : "local", l->addr, l->port);
    FwdConn *fc = conn_new(f, fd, desc);
    if (!fc) { close(fd); return; }
    if (l->kind == 'L') open_direct(f, c, fc, l->host, l->hostport, orig, ntohs(sa.sin_port));
    /* kind D: the SOCKS handshake comes first; the channel is opened when the request is in */
}

static void service_conn(Forwards *f, SshcConn *c, FwdConn *fc, short revents) {
    if (fc->fd < 0 || fc->dead) return;
    if (revents & POLLOUT) push_out(c, fc);
    if (fc->dead || fc->fd < 0) return;
    if (!(revents & (POLLIN | POLLHUP | POLLERR))) return;
    if (!fc->ch && !fc->opening) {                                              /* SOCKS handshake */
        ssize_t r = read(fc->fd, fc->hs + fc->hs_len, sizeof fc->hs - fc->hs_len);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { conn_finish(c, fc); return; }
        if (r < 0) return;
        fc->hs_len += (size_t)r;
        char host[256];
        int port = 0;
        int s = socks_step(fc, host, sizeof host, &port);
        if (s < 0) { conn_finish(c, fc); return; }
        if (s == 1) {
            fc->socks_pending_reply = true;
            open_direct(f, c, fc, host, port, "127.0.0.1", 0);
            sshc_debug(1, "SOCKS request for %s:%d", host, port);
        }
        return;
    }
    if (!fc->ch || !fc->ch->open || fc->sock_eof) return;
    uint8_t buf[SSHC_MAXPKT - 16];
    size_t want = sizeof buf;
    if (want > fc->ch->remote_win) want = fc->ch->remote_win;
    if (want > fc->ch->remote_max) want = fc->ch->remote_max;
    if (want == 0) return;
    ssize_t r = read(fc->fd, buf, want);
    if (r > 0) {
        size_t sent = sshc_chan_send(c, fc->ch, buf, (size_t)r);
        if (sent < (size_t)r) conn_finish(c, fc);                               /* cannot happen: we asked for no more than the window */
    } else if (r == 0) {
        fc->sock_eof = true;
        sshc_chan_send_eof(c, fc->ch);
        if (fc->eof_in && fc->out_len == 0) conn_finish(c, fc);
    } else if (errno != EAGAIN && errno != EINTR) {
        conn_finish(c, fc);
    }
}

void forwards_service(Forwards *f, SshcConn *c, const struct pollfd *fds, int n) {
    int count = n < f->nmap ? n : f->nmap;
    /* the descriptors were mapped when poll was called; connections may have been freed since, so look each one up again before touching it */
    for (int i = 0; i < count; i++) {
        if (!fds[i].revents) continue;
        if (f->map[i].kind == 1) {
            if (fds[i].revents & POLLIN) for (int k = 0; k < 8; k++) accept_one(f, c, f->map[i].ptr);
        } else {
            FwdConn *fc = f->map[i].ptr;
            bool alive = false;
            for (FwdConn *x = f->conns; x; x = x->next) if (x == fc) { alive = true; break; }
            if (alive) service_conn(f, c, fc, fds[i].revents);
        }
    }
    /* dead connections whose channel is gone are freed; a dead one with a channel waits for the server's CLOSE */
    for (FwdConn **pp = &f->conns; *pp;) {
        FwdConn *fc = *pp;
        if (fc->dead && !fc->ch) { *pp = fc->next; if (fc->fd >= 0) close(fc->fd); free(fc->out); free(fc); }
        else pp = &fc->next;
    }
}

void forwards_list(Forwards *f, char *out, size_t cap) {
    size_t o = 0;
    out[0] = 0;
#define ADD(...) do { if (o < cap) o += (size_t)snprintf(out + o, cap - o, __VA_ARGS__); } while (0)
    for (int i = 0; i < f->nlis; i++) {
        if (f->lis[i].kind == 'L') ADD("  local forwarding %s:%d -> %s:%d\r\n", f->lis[i].addr, f->lis[i].port, f->lis[i].host, f->lis[i].hostport);
        else ADD("  dynamic (SOCKS) forwarding %s:%d\r\n", f->lis[i].addr, f->lis[i].port);
    }
    for (int i = 0; i < f->nrem; i++) ADD("  remote forwarding port %d -> %s:%d\r\n", f->rem[i].port, f->rem[i].host, f->rem[i].hostport);
    for (FwdConn *fc = f->conns; fc; fc = fc->next) ADD("  connection (%s)%s\r\n", fc->desc, fc->dead ? " closing" : "");
#undef ADD
}

bool forwards_busy(const Forwards *f) { return f && f->conns != NULL; }

void forwards_free(Forwards *f) {
    if (!f) return;
    while (f->conns) conn_unlink(f, f->conns);
    for (int i = 0; i < f->nlis; i++) if (f->lis[i].fd >= 0) close(f->lis[i].fd);
    free(f);
}
