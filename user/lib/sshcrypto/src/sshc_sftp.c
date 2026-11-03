/*
 * An SFTP client, protocol version 3 (draft-ietf-secsh-filexfer-02), over a session channel with the subsystem "sftp". scp and sftp are built on it.
 *
 * The protocol is request and answer, every request carrying a number that its answer repeats. Most calls here send one request and wait for its answer. File
 * transfers keep several requests in flight (16 of 32 KiB): a read of the next piece is asked for before the previous one has arrived, which keeps a link with
 * latency full. For get, the answers may come in any order as far as the protocol is concerned, so each one is matched to the offset it was asked for and written
 * there; a short answer is followed up with a request for the rest; the end of the file is the answer "EOF" to a read at or past it.
 *
 * The server's stream is cut into packets from a byte buffer fed by the channel (the packets and the channel's data packets do not line up).
 */
#include <sshc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum {
    FXP_INIT = 1, FXP_VERSION = 2, FXP_OPEN = 3, FXP_CLOSE = 4, FXP_READ = 5, FXP_WRITE = 6, FXP_LSTAT = 7, FXP_FSTAT = 8, FXP_SETSTAT = 9, FXP_FSETSTAT = 10,
    FXP_OPENDIR = 11, FXP_READDIR = 12, FXP_REMOVE = 13, FXP_MKDIR = 14, FXP_RMDIR = 15, FXP_REALPATH = 16, FXP_STAT = 17, FXP_RENAME = 18, FXP_READLINK = 19,
    FXP_SYMLINK = 20, FXP_STATUS = 101, FXP_HANDLE = 102, FXP_DATA = 103, FXP_NAME = 104, FXP_ATTRS = 105,
};
enum { FX_OK = 0, FX_EOF = 1, FX_NO_SUCH_FILE = 2, FX_PERMISSION_DENIED = 3, FX_FAILURE = 4, FX_BAD_MESSAGE = 5, FX_NO_CONNECTION = 6, FX_CONNECTION_LOST = 7, FX_OP_UNSUPPORTED = 8 };
enum { PF_READ = 1, PF_WRITE = 2, PF_CREAT = 8, PF_TRUNC = 0x10 };
#define CHUNK 32768
#define WINDOW 16
#define MAX_PKT (4u << 20)
#define SILENCE_MS 120000                           /* a server that says nothing for this long is given up on */

struct SshcSftp {
    SshcConn *c;
    SshcChan *ch;
    bool confirmed, open_failed, replied, reply_ok, dead;
    uint8_t *rx;
    size_t rx_len, rx_cap;
    uint8_t *pkt;                                   /* the packet being looked at */
    size_t pkt_cap;
    uint8_t *out;                                   /* the packet being built */
    uint32_t next_id;
    int last_code;                                  /* the code of the last STATUS that was not OK */
    char err[300];
};

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void fail(SshcSftp *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(SshcSftp *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof s->err, fmt, ap);
    va_end(ap);
}

const char *sshc_sftp_err(const SshcSftp *s) { return s->err; }
bool sshc_sftp_dead(const SshcSftp *s) { return s->dead; }

/* ------------------------------------------------------------------------------------------------ the channel */

static void on_confirmed(SshcConn *c, SshcChan *ch) { (void)c; ((SshcSftp *)ch->user)->confirmed = true; }
static void on_open_failed(SshcConn *c, SshcChan *ch, uint32_t reason, const char *text) {
    (void)c; (void)reason;
    SshcSftp *s = ch->user;
    s->open_failed = true;
    fail(s, "the server would not open a channel: %s", text);
}

static void on_data(SshcConn *c, SshcChan *ch, const uint8_t *d, size_t n, uint32_t ext) {
    SshcSftp *s = ch->user;
    if (ext == 0) {
        if (s->rx_len + n > s->rx_cap) {
            size_t nc = s->rx_cap ? s->rx_cap : 65536;
            while (nc < s->rx_len + n) nc *= 2;
            uint8_t *nb = realloc(s->rx, nc);
            if (!nb) { s->dead = true; fail(s, "out of memory"); sshc_chan_consumed(c, ch, n); return; }
            s->rx = nb;
            s->rx_cap = nc;
        }
        memcpy(s->rx + s->rx_len, d, n);
        s->rx_len += n;
    }
    sshc_chan_consumed(c, ch, n);                    /* it is in our buffer now */
}

static void on_eof(SshcConn *c, SshcChan *ch) { (void)c; (void)ch; }
static void on_closed(SshcConn *c, SshcChan *ch) { (void)c; ((SshcSftp *)ch->user)->dead = true; }
static bool on_request(SshcConn *c, SshcChan *ch, const char *name, SshcRd *args, bool want) { (void)c; (void)ch; (void)name; (void)args; (void)want; return false; }
static void on_reply(SshcConn *c, SshcChan *ch, bool ok) {
    (void)c;
    SshcSftp *s = ch->user;
    s->replied = true;
    s->reply_ok = ok;
}

static const SshcChanOps OPS = { on_confirmed, on_open_failed, on_data, on_eof, on_closed, on_request, on_reply };

/* One round of waiting for the server. 0, or -1 when the connection is gone. */
static int pump(SshcSftp *s, long *last_progress) {
    int r = sshc_pump(s->c, 1000);
    if (r < 0) {
        s->dead = true;
        fail(s, "%s", s->c->err[0] ? s->c->err : "connection lost");
        return -1;
    }
    if (r > 0) *last_progress = sshc_mono_ms();
    if (sshc_mono_ms() - *last_progress > SILENCE_MS) {
        s->dead = true;
        fail(s, "the server stopped answering");
        return -1;
    }
    if (s->dead) { if (!s->err[0]) fail(s, "the SFTP session was closed by the server"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ packets */

static int send_pkt(SshcSftp *s, const uint8_t *pkt, size_t n) {         /* pkt includes the 4-byte length */
    size_t off = 0;
    long last = sshc_mono_ms();
    while (off < n) {
        size_t sent = sshc_chan_send(s->c, s->ch, pkt + off, n - off);
        off += sent;
        if (off < n && pump(s, &last) < 0) return -1;
        if (sent) last = sshc_mono_ms();
    }
    return 0;
}

/* Reads the next packet from the server: its type, and a reader over the rest. Valid until the next call. */
static int get_pkt(SshcSftp *s, uint8_t *type, SshcRd *r) {
    long last = sshc_mono_ms();
    for (;;) {
        if (s->rx_len >= 4) {
            uint32_t len = get_be32(s->rx);
            if (len < 1 || len > MAX_PKT) { s->dead = true; fail(s, "the server sent a damaged SFTP packet (length %u)", len); return -1; }
            if (s->rx_len >= 4 + (size_t)len) {
                if (len > s->pkt_cap) {
                    uint8_t *nb = realloc(s->pkt, len + 16);
                    if (!nb) { fail(s, "out of memory"); return -1; }
                    s->pkt = nb;
                    s->pkt_cap = len;
                }
                memcpy(s->pkt, s->rx + 4, len);
                memmove(s->rx, s->rx + 4 + len, s->rx_len - 4 - len);
                s->rx_len -= 4 + len;
                *type = s->pkt[0];
                sshc_rd_init(r, s->pkt + 1, len - 1);
                return 0;
            }
        }
        if (pump(s, &last) < 0) return -1;
    }
}

/* Builds a request in s->out: length, type, id, then whatever the caller appends through the writer. Returns the id. */
static uint32_t begin(SshcSftp *s, SshcWr *w, uint8_t type) {
    sshc_wr_init(w, s->out, CHUNK + 1024);
    sshc_wr_u32(w, 0);
    sshc_wr_u8(w, type);
    uint32_t id = s->next_id++;
    sshc_wr_u32(w, id);
    return id;
}

static int finish_send(SshcSftp *s, SshcWr *w) {
    if (w->bad) { fail(s, "request too long"); return -1; }
    put_be32(s->out, (uint32_t)(w->n - 4));
    return send_pkt(s, s->out, w->n);
}

static const char *status_text(int code) {
    switch (code) {
    case FX_EOF: return "end of file";
    case FX_NO_SUCH_FILE: return "No such file or directory";
    case FX_PERMISSION_DENIED: return "Permission denied";
    case FX_FAILURE: return "Failure";
    case FX_BAD_MESSAGE: return "Bad message";
    case FX_NO_CONNECTION: return "No connection";
    case FX_CONNECTION_LOST: return "Connection lost";
    case FX_OP_UNSUPPORTED: return "Operation unsupported";
    default: return "Unknown error";
    }
}

/* Reads a STATUS answer (after the id): 0 for OK, -1 with the reason otherwise. */
static int status_of(SshcSftp *s, SshcRd *r) {
    uint32_t code = sshc_rd_u32(r);
    size_t ml = 0;
    const uint8_t *msg = sshc_rd_str(r, &ml);
    if (r->bad) { fail(s, "damaged status from the server"); return -1; }
    if (code == FX_OK) return 0;
    s->last_code = (int)code;
    char shown[200];
    sshc_printable(shown, sizeof shown, msg, ml, false);
    if (shown[0] && strcmp(shown, status_text((int)code)) != 0 && strcasecmp(shown, "failure") != 0) fail(s, "%s: %s", status_text((int)code), shown);
    else fail(s, "%s", status_text((int)code));
    return -1;
}

/* Waits for the answer to request `id`; any packet with another id is a protocol error here (one request at a time). */
static int wait_answer(SshcSftp *s, uint32_t id, uint8_t *type, SshcRd *r) {
    if (get_pkt(s, type, r) < 0) return -1;
    uint32_t got = sshc_rd_u32(r);
    if (r->bad || got != id) { s->dead = true; fail(s, "the server answered request %u with an answer to %u", id, got); return -1; }
    return 0;
}

/* A request that is answered with a STATUS. */
static int status_request(SshcSftp *s, uint8_t type, const char *a, const char *b, const SshcAttrs *attrs, uint32_t mode, bool with_mode) {
    SshcWr w;
    uint32_t id = begin(s, &w, type);
    sshc_wr_cstr(&w, a);
    if (b) sshc_wr_cstr(&w, b);
    if (with_mode) { sshc_wr_u32(&w, SSHC_ATTR_PERM); sshc_wr_u32(&w, mode); }
    if (attrs) {
        sshc_wr_u32(&w, attrs->flags);
        if (attrs->flags & SSHC_ATTR_SIZE) sshc_wr_u64(&w, attrs->size);
        if (attrs->flags & SSHC_ATTR_UIDGID) { sshc_wr_u32(&w, attrs->uid); sshc_wr_u32(&w, attrs->gid); }
        if (attrs->flags & SSHC_ATTR_PERM) sshc_wr_u32(&w, attrs->perm);
        if (attrs->flags & SSHC_ATTR_TIME) { sshc_wr_u32(&w, attrs->atime); sshc_wr_u32(&w, attrs->mtime); }
    }
    if (finish_send(s, &w) < 0) return -1;
    uint8_t t;
    SshcRd r;
    if (wait_answer(s, id, &t, &r) < 0) return -1;
    if (t != FXP_STATUS) { fail(s, "unexpected answer %u", t); return -1; }
    return status_of(s, &r);
}

static void parse_attrs(SshcRd *r, SshcAttrs *a) {
    memset(a, 0, sizeof *a);
    a->flags = sshc_rd_u32(r);
    if (a->flags & SSHC_ATTR_SIZE) a->size = sshc_rd_u64(r);
    if (a->flags & SSHC_ATTR_UIDGID) { a->uid = sshc_rd_u32(r); a->gid = sshc_rd_u32(r); }
    if (a->flags & SSHC_ATTR_PERM) a->perm = sshc_rd_u32(r);
    if (a->flags & SSHC_ATTR_TIME) { a->atime = sshc_rd_u32(r); a->mtime = sshc_rd_u32(r); }
    if (a->flags & 0x80000000u) {                                       /* extended attributes: counted pairs of strings, skipped */
        uint32_t n = sshc_rd_u32(r);
        for (uint32_t i = 0; i < n && !r->bad; i++) { size_t l; sshc_rd_str(r, &l); sshc_rd_str(r, &l); }
    }
}

/* ------------------------------------------------------------------------------------------------ start and end */

SshcSftp *sshc_sftp_start(SshcConn *c) {
    SshcSftp *s = calloc(1, sizeof *s);
    if (!s) { snprintf(c->err, sizeof c->err, "out of memory"); return NULL; }
    s->c = c;
    s->next_id = 1;
    s->out = malloc(CHUNK + 1024);
    if (!s->out) { free(s); return NULL; }
    s->ch = sshc_chan_open(c, "session", NULL, 0, &OPS, s);
    if (!s->ch) { snprintf(c->err, sizeof c->err, "cannot open a channel"); goto bad; }
    while (!s->confirmed && !s->open_failed) {
        if (sshc_pump(c, -1) < 0) goto bad;
    }
    if (s->open_failed) { snprintf(c->err, sizeof c->err, "%s", s->err); goto bad; }
    uint8_t req[64];
    SshcWr w;
    sshc_wr_init(&w, req, sizeof req);
    sshc_wr_cstr(&w, "sftp");
    sshc_chan_request(c, s->ch, "subsystem", true, req, w.n);
    while (!s->replied) {
        if (sshc_pump(c, -1) < 0) goto bad;
    }
    if (!s->reply_ok) { snprintf(c->err, sizeof c->err, "the server has no sftp subsystem"); goto bad; }
    uint8_t init[9];
    put_be32(init, 5);
    init[4] = FXP_INIT;
    put_be32(init + 5, 3);
    if (send_pkt(s, init, sizeof init) < 0) { snprintf(c->err, sizeof c->err, "%s", s->err); goto bad; }
    uint8_t t;
    SshcRd r;
    if (get_pkt(s, &t, &r) < 0) { snprintf(c->err, sizeof c->err, "%s", s->err); goto bad; }
    uint32_t version = sshc_rd_u32(&r);
    if (t != FXP_VERSION || r.bad || version < 3) {
        snprintf(c->err, sizeof c->err, "the server does not speak SFTP version 3 (%s)", t == FXP_VERSION ? "older version" : "no version answer");
        goto bad;
    }
    return s;
bad:
    free(s->rx); free(s->pkt); free(s->out); free(s);
    return NULL;
}

void sshc_sftp_end(SshcSftp *s) {
    if (!s) return;
    if (s->ch && s->ch->used) {
        sshc_chan_send_eof(s->c, s->ch);
        sshc_chan_close(s->c, s->ch);
    }
    free(s->rx); free(s->pkt); free(s->out);
    free(s);
}

/* ------------------------------------------------------------------------------------------------ simple operations */

int sshc_sftp_stat(SshcSftp *s, const char *path, SshcAttrs *a, bool follow) {
    SshcWr w;
    uint32_t id = begin(s, &w, follow ? FXP_STAT : FXP_LSTAT);
    sshc_wr_cstr(&w, path);
    if (finish_send(s, &w) < 0) return -1;
    uint8_t t;
    SshcRd r;
    if (wait_answer(s, id, &t, &r) < 0) return -1;
    if (t == FXP_STATUS) { if (status_of(s, &r) == 0) fail(s, "no attributes"); return -1; }
    if (t != FXP_ATTRS) { fail(s, "unexpected answer %u", t); return -1; }
    parse_attrs(&r, a);
    if (r.bad) { fail(s, "damaged attributes from the server"); return -1; }
    return 0;
}

int sshc_sftp_setstat(SshcSftp *s, const char *path, const SshcAttrs *a) { return status_request(s, FXP_SETSTAT, path, NULL, a, 0, false); }
int sshc_sftp_mkdir(SshcSftp *s, const char *path, uint32_t mode) { return status_request(s, FXP_MKDIR, path, NULL, NULL, mode, true); }
int sshc_sftp_rmdir(SshcSftp *s, const char *path) { return status_request(s, FXP_RMDIR, path, NULL, NULL, 0, false); }
int sshc_sftp_remove(SshcSftp *s, const char *path) { return status_request(s, FXP_REMOVE, path, NULL, NULL, 0, false); }
int sshc_sftp_rename(SshcSftp *s, const char *from, const char *to) { return status_request(s, FXP_RENAME, from, to, NULL, 0, false); }
int sshc_sftp_symlink(SshcSftp *s, const char *target, const char *path) { return status_request(s, FXP_SYMLINK, target, path, NULL, 0, false); }

/* READLINK and REALPATH answer with a NAME holding one entry. */
static int name_request(SshcSftp *s, uint8_t type, const char *path, char *out, size_t cap) {
    SshcWr w;
    uint32_t id = begin(s, &w, type);
    sshc_wr_cstr(&w, path);
    if (finish_send(s, &w) < 0) return -1;
    uint8_t t;
    SshcRd r;
    if (wait_answer(s, id, &t, &r) < 0) return -1;
    if (t == FXP_STATUS) { status_of(s, &r); return -1; }
    if (t != FXP_NAME) { fail(s, "unexpected answer %u", t); return -1; }
    uint32_t count = sshc_rd_u32(&r);
    size_t nl;
    const uint8_t *name = sshc_rd_str(&r, &nl);
    if (r.bad || count < 1 || nl >= cap) { fail(s, "damaged answer"); return -1; }
    memcpy(out, name, nl);
    out[nl] = 0;
    return 0;
}

int sshc_sftp_readlink(SshcSftp *s, const char *path, char *out, size_t cap) { return name_request(s, FXP_READLINK, path, out, cap); }
int sshc_sftp_realpath(SshcSftp *s, const char *path, char *out, size_t cap) { return name_request(s, FXP_REALPATH, path, out, cap); }

static int close_handle(SshcSftp *s, const uint8_t *h, size_t hl) {
    SshcWr w;
    uint32_t id = begin(s, &w, FXP_CLOSE);
    sshc_wr_str(&w, h, hl);
    if (finish_send(s, &w) < 0) return -1;
    uint8_t t;
    SshcRd r;
    if (wait_answer(s, id, &t, &r) < 0) return -1;
    return t == FXP_STATUS ? status_of(s, &r) : -1;
}

/* OPEN or OPENDIR: the handle (at most 256 bytes). */
static int open_handle(SshcSftp *s, uint8_t type, const char *path, uint32_t pflags, uint32_t mode, uint8_t *h, size_t *hl) {
    SshcWr w;
    uint32_t id = begin(s, &w, type);
    sshc_wr_cstr(&w, path);
    if (type == FXP_OPEN) {
        sshc_wr_u32(&w, pflags);
        if (pflags & PF_CREAT) { sshc_wr_u32(&w, SSHC_ATTR_PERM); sshc_wr_u32(&w, mode); }
        else sshc_wr_u32(&w, 0);
    }
    if (finish_send(s, &w) < 0) return -1;
    uint8_t t;
    SshcRd r;
    if (wait_answer(s, id, &t, &r) < 0) return -1;
    if (t == FXP_STATUS) { status_of(s, &r); return -1; }
    if (t != FXP_HANDLE) { fail(s, "unexpected answer %u", t); return -1; }
    size_t n;
    const uint8_t *hp = sshc_rd_str(&r, &n);
    if (r.bad || n > 256) { fail(s, "damaged handle"); return -1; }
    memcpy(h, hp, n);
    *hl = n;
    return 0;
}

int sshc_sftp_readdir(SshcSftp *s, const char *path, int (*fn)(const SshcDirEnt *, void *), void *ctx) {
    uint8_t h[256];
    size_t hl;
    if (open_handle(s, FXP_OPENDIR, path, 0, 0, h, &hl) < 0) return -1;
    int rc = 0;
    for (;;) {
        SshcWr w;
        uint32_t id = begin(s, &w, FXP_READDIR);
        sshc_wr_str(&w, h, hl);
        if (finish_send(s, &w) < 0) { rc = -1; break; }
        uint8_t t;
        SshcRd r;
        if (wait_answer(s, id, &t, &r) < 0) { rc = -1; break; }
        if (t == FXP_STATUS) {
            uint32_t code = sshc_rd_u32(&r);
            if (code != FX_EOF) { fail(s, "%s", status_text((int)code)); rc = -1; }
            break;
        }
        if (t != FXP_NAME) { fail(s, "unexpected answer %u", t); rc = -1; break; }
        uint32_t count = sshc_rd_u32(&r);
        bool stop = false;
        for (uint32_t i = 0; i < count && !r.bad && !stop; i++) {
            SshcDirEnt e;
            size_t nl, ll;
            const uint8_t *name = sshc_rd_str(&r, &nl), *lng = sshc_rd_str(&r, &ll);
            parse_attrs(&r, &e.a);
            if (r.bad) break;
            sshc_printable(e.name, sizeof e.name, name, nl, false);
            sshc_printable(e.longname, sizeof e.longname, lng, ll, false);
            if (!strcmp(e.name, ".") || !strcmp(e.name, "..") || !e.name[0]) continue;
            int cb = fn(&e, ctx);
            if (cb != 0) { rc = cb; stop = true; }
        }
        if (r.bad) { fail(s, "damaged directory listing"); rc = -1; break; }
        if (stop) break;
    }
    close_handle(s, h, hl);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ transfers */

typedef struct { uint32_t id; uint64_t off; uint32_t len; } Pending;

int sshc_sftp_get(SshcSftp *s, const char *remote, int local_fd, uint64_t total, SshcProgress pr, void *ctx) {
    uint8_t h[256];
    size_t hl;
    if (open_handle(s, FXP_OPEN, remote, PF_READ, 0, h, &hl) < 0) return -1;
    Pending pend[WINDOW];
    int np = 0;
    uint64_t next = 0, done = 0;
    bool eof = false;
    int rc = 0;
    while (rc == 0 && (np > 0 || !eof)) {
        while (!eof && np < WINDOW && rc == 0) {                        /* keep the pipe full */
            SshcWr w;
            uint32_t id = begin(s, &w, FXP_READ);
            sshc_wr_str(&w, h, hl);
            sshc_wr_u64(&w, next);
            sshc_wr_u32(&w, CHUNK);
            if (finish_send(s, &w) < 0) { rc = -1; break; }
            pend[np++] = (Pending){ id, next, CHUNK };
            next += CHUNK;
        }
        if (rc != 0 || np == 0) break;
        uint8_t t;
        SshcRd r;
        if (get_pkt(s, &t, &r) < 0) { rc = -1; break; }
        uint32_t id = sshc_rd_u32(&r);
        int k = -1;
        for (int i = 0; i < np; i++) if (pend[i].id == id) { k = i; break; }
        if (k < 0 || r.bad) { s->dead = true; fail(s, "the server answered a request that was never made"); rc = -1; break; }
        Pending p = pend[k];
        memmove(&pend[k], &pend[k + 1], (size_t)(np - k - 1) * sizeof pend[0]);
        np--;
        if (t == FXP_STATUS) {
            uint32_t code = sshc_rd_u32(&r);
            if (code == FX_EOF) { eof = true; continue; }
            fail(s, "%s", status_text((int)code));
            rc = -1;
            break;
        }
        if (t != FXP_DATA) { fail(s, "unexpected answer %u", t); rc = -1; break; }
        size_t dl;
        const uint8_t *d = sshc_rd_str(&r, &dl);
        if (r.bad || dl > p.len) { fail(s, "damaged data from the server"); rc = -1; break; }
        if (lseek(local_fd, (off_t)p.off, SEEK_SET) < 0) { fail(s, "cannot seek in the local file: %s", strerror(errno)); rc = -1; break; }
        size_t off = 0;
        while (off < dl) {
            ssize_t wr = write(local_fd, d + off, dl - off);
            if (wr < 0 && errno == EINTR) continue;
            if (wr <= 0) { fail(s, "cannot write the local file: %s", strerror(errno)); rc = -1; break; }
            off += (size_t)wr;
        }
        if (rc) break;
        done += dl;
        if (pr) pr(ctx, done, total);
        if (dl < p.len && !eof) {                                       /* a short answer that was not the end: ask for the rest of that piece */
            SshcWr w;
            uint32_t nid = begin(s, &w, FXP_READ);
            sshc_wr_str(&w, h, hl);
            sshc_wr_u64(&w, p.off + dl);
            sshc_wr_u32(&w, p.len - (uint32_t)dl);
            if (finish_send(s, &w) < 0) { rc = -1; break; }
            if (np < WINDOW) pend[np++] = (Pending){ nid, p.off + dl, p.len - (uint32_t)dl };
        }
    }
    while (np > 0 && !s->dead) {                                        /* answers still on their way: take them so the next request starts clean */
        uint8_t t;
        SshcRd r;
        if (get_pkt(s, &t, &r) < 0) break;
        np--;
    }
    char keep[sizeof s->err];
    memcpy(keep, s->err, sizeof keep);
    close_handle(s, h, hl);
    if (rc) memcpy(s->err, keep, sizeof keep);
    return rc;
}

int sshc_sftp_put(SshcSftp *s, int local_fd, const char *remote, uint32_t mode, uint64_t total, SshcProgress pr, void *ctx) {
    uint8_t h[256];
    size_t hl;
    if (open_handle(s, FXP_OPEN, remote, PF_WRITE | PF_CREAT | PF_TRUNC, mode, h, &hl) < 0) return -1;
    uint32_t ids[WINDOW];
    uint32_t lens[WINDOW];
    int np = 0;
    uint64_t off = 0, done = 0;
    bool eof = false;
    int rc = 0;
    static uint8_t buf[CHUNK];
    while (rc == 0 && (np > 0 || !eof)) {
        while (!eof && np < WINDOW && rc == 0) {
            ssize_t got = read(local_fd, buf, sizeof buf);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) { fail(s, "cannot read the local file: %s", strerror(errno)); rc = -1; break; }
            if (got == 0) { eof = true; break; }
            SshcWr w;
            uint32_t id = begin(s, &w, FXP_WRITE);
            sshc_wr_str(&w, h, hl);
            sshc_wr_u64(&w, off);
            sshc_wr_str(&w, buf, (size_t)got);
            if (finish_send(s, &w) < 0) { rc = -1; break; }
            ids[np] = id;
            lens[np++] = (uint32_t)got;
            off += (uint64_t)got;
        }
        if (rc != 0 || np == 0) break;
        uint8_t t;
        SshcRd r;
        if (get_pkt(s, &t, &r) < 0) { rc = -1; break; }
        uint32_t id = sshc_rd_u32(&r);
        int k = -1;
        for (int i = 0; i < np; i++) if (ids[i] == id) { k = i; break; }
        if (k < 0 || r.bad || t != FXP_STATUS) { s->dead = true; fail(s, "unexpected answer %u from the server", t); rc = -1; break; }
        if (status_of(s, &r) < 0) { rc = -1; break; }
        done += lens[k];
        memmove(&ids[k], &ids[k + 1], (size_t)(np - k - 1) * sizeof ids[0]);
        memmove(&lens[k], &lens[k + 1], (size_t)(np - k - 1) * sizeof lens[0]);
        np--;
        if (pr) pr(ctx, done, total);
    }
    while (np > 0 && !s->dead) {
        uint8_t t;
        SshcRd r;
        if (get_pkt(s, &t, &r) < 0) break;
        np--;
    }
    char keep[sizeof s->err];
    memcpy(keep, s->err, sizeof keep);
    int crc = close_handle(s, h, hl);
    if (rc) memcpy(s->err, keep, sizeof keep);
    return rc ? rc : crc;
}
