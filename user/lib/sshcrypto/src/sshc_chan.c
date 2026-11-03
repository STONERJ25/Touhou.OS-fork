/*
 * The connection layer (RFC 4254): channels with windows, requests, global requests.
 *
 * A channel has two windows. The peer's window (remote_win) is how many bytes of data we may still send it: sshc_chan_send() sends no more than that and the
 * program stops reading its input when it is closed, so a slow server slows the producer down instead of filling memory. Our window (local_win) is how many
 * bytes the peer may still send us; it is reopened by sshc_chan_consumed() when the program has really taken the bytes (written them to the terminal, the file,
 * the socket), so a program that is blocked on its output never grants more than it can hold. We grant SSHC_WINDOW (1 MiB) and take packets of up to
 * SSHC_MAXPKT (32 KiB), the sizes OpenSSH's own session channels use.
 *
 * A channel ends when both sides have sent CLOSE: the first to want out sends it, the other answers, and only then is the number free again.
 */
#include <sshc.h>
#include <stdio.h>
#include <string.h>

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static SshcChan *by_id(SshcConn *c, uint32_t id) { return id < SSHC_MAX_CHAN && c->chan[id].used ? &c->chan[id] : NULL; }

static void release(SshcConn *c, SshcChan *ch) {
    if (ch->ops && ch->ops->closed) ch->ops->closed(c, ch);
    memset(ch, 0, sizeof *ch);
}

bool sshc_chans_open(const SshcConn *c) {
    for (int i = 0; i < SSHC_MAX_CHAN; i++) if (c->chan[i].used) return true;
    return false;
}

static int send_simple(SshcConn *c, uint8_t type, uint32_t remote) {
    uint8_t m[5];
    m[0] = type;
    put_be32(m + 1, remote);
    return sshc_send(c, m, sizeof m);
}

SshcChan *sshc_chan_open(SshcConn *c, const char *type, const uint8_t *extra, size_t extra_len, const SshcChanOps *ops, void *user) {
    SshcChan *ch = NULL;
    for (int i = 0; i < SSHC_MAX_CHAN && !ch; i++) if (!c->chan[i].used) ch = &c->chan[i];
    if (!ch) return NULL;
    memset(ch, 0, sizeof *ch);
    ch->used = true;
    ch->id = (uint32_t)(ch - c->chan);
    ch->local_win = SSHC_WINDOW;
    ch->ops = ops;
    ch->user = user;
    uint8_t m[1024];
    SshcWr w;
    sshc_wr_init(&w, m, sizeof m);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_OPEN);
    sshc_wr_cstr(&w, type);
    sshc_wr_u32(&w, ch->id);
    sshc_wr_u32(&w, SSHC_WINDOW);
    sshc_wr_u32(&w, SSHC_MAXPKT);
    if (extra_len) sshc_wr_raw(&w, extra, extra_len);
    if (w.bad || sshc_send(c, m, w.n) < 0) { memset(ch, 0, sizeof *ch); return NULL; }
    return ch;
}

int sshc_chan_request(SshcConn *c, SshcChan *ch, const char *name, bool want_reply, const uint8_t *args, size_t n) {
    if (!ch->used || ch->close_out) return -1;
    uint8_t m[2048];
    SshcWr w;
    sshc_wr_init(&w, m, sizeof m);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_REQUEST);
    sshc_wr_u32(&w, ch->remote_id);
    sshc_wr_cstr(&w, name);
    sshc_wr_bool(&w, want_reply);
    if (n) sshc_wr_raw(&w, args, n);
    if (w.bad) return -1;
    if (want_reply) ch->replies_due++;
    return sshc_send(c, m, w.n);
}

bool sshc_chan_can_send(const SshcConn *c, const SshcChan *ch) {
    return ch->used && ch->open && !ch->eof_out && !ch->close_out && ch->remote_win > 0 && (c->tx_len - c->tx_off) < (256u << 10);
}

size_t sshc_chan_send(SshcConn *c, SshcChan *ch, const void *data, size_t n) {
    if (!ch->used || !ch->open || ch->eof_out || ch->close_out) return 0;
    const uint8_t *p = data;
    size_t sent = 0;
    while (sent < n) {
        size_t room = ch->remote_win;
        if (room > ch->remote_max) room = ch->remote_max;
        if (room > SSHC_MAXPKT - 16) room = SSHC_MAXPKT - 16;
        if (room == 0) break;
        size_t take = n - sent < room ? n - sent : room;
        uint8_t *m = c->chunk;
        m[0] = SSHC_MSG_CHANNEL_DATA;
        put_be32(m + 1, ch->remote_id);
        put_be32(m + 5, (uint32_t)take);
        memcpy(m + 9, p + sent, take);
        if (sshc_send(c, m, 9 + take) < 0) break;
        ch->remote_win -= (uint32_t)take;
        sent += take;
    }
    return sent;
}

int sshc_chan_send_eof(SshcConn *c, SshcChan *ch) {
    if (!ch->used || ch->eof_out || ch->close_out) return 0;
    ch->eof_out = true;
    return send_simple(c, SSHC_MSG_CHANNEL_EOF, ch->remote_id);
}

int sshc_chan_close(SshcConn *c, SshcChan *ch) {
    if (!ch->used || ch->close_out) return 0;
    ch->close_out = true;
    if (!ch->open) return 0;                                   /* not confirmed yet: the peer has no number to close; the CLOSE goes out when the answer comes */
    int rc = send_simple(c, SSHC_MSG_CHANNEL_CLOSE, ch->remote_id);
    if (ch->close_in) release(c, ch);
    return rc;
}

void sshc_chan_consumed(SshcConn *c, SshcChan *ch, size_t n) {
    if (!ch->used) return;
    ch->unacked += (uint32_t)n;
    if (ch->unacked >= SSHC_WINDOW / 4 && !ch->close_in && !ch->close_out) {
        uint8_t m[9];
        m[0] = SSHC_MSG_CHANNEL_WINDOW_ADJUST;
        put_be32(m + 1, ch->remote_id);
        put_be32(m + 5, ch->unacked);
        ch->local_win += ch->unacked;
        ch->unacked = 0;
        sshc_send(c, m, sizeof m);
    }
}

int sshc_global_request(SshcConn *c, const char *name, const uint8_t *args, size_t n, SshcGlobalReplyFn fn, void *ctx) {
    uint8_t m[512];
    SshcWr w;
    sshc_wr_init(&w, m, sizeof m);
    sshc_wr_u8(&w, SSHC_MSG_GLOBAL_REQUEST);
    sshc_wr_cstr(&w, name);
    sshc_wr_bool(&w, fn != NULL);
    if (n) sshc_wr_raw(&w, args, n);
    if (w.bad) return -1;
    if (fn) {
        if (c->n_greply >= (int)(sizeof c->greply / sizeof c->greply[0])) return -1;
        c->greply[c->n_greply].fn = fn;
        c->greply[c->n_greply].ctx = ctx;
        c->n_greply++;
    }
    return sshc_send(c, m, w.n);
}

static int unimplemented(SshcConn *c) {
    uint8_t m[5];
    m[0] = SSHC_MSG_UNIMPLEMENTED;
    put_be32(m + 1, c->last_seq);
    return sshc_send(c, m, sizeof m);
}

static int open_failure(SshcConn *c, uint32_t remote, uint32_t reason, const char *text) {
    uint8_t m[200];
    SshcWr w;
    sshc_wr_init(&w, m, sizeof m);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_OPEN_FAILURE);
    sshc_wr_u32(&w, remote);
    sshc_wr_u32(&w, reason);
    sshc_wr_cstr(&w, text);
    sshc_wr_cstr(&w, "");
    return w.bad ? -1 : sshc_send(c, m, w.n);
}

/* A channel the server opens: accepted only when the program installed a handler for that type. */
static int on_server_open(SshcConn *c, SshcRd *r) {
    size_t tl;
    const uint8_t *tp = sshc_rd_str(r, &tl);
    uint32_t sender = sshc_rd_u32(r), win = sshc_rd_u32(r), max = sshc_rd_u32(r);
    if (r->bad) { snprintf(c->err, sizeof c->err, "Malformed channel open from the server"); return -1; }
    char type[64];
    sshc_printable(type, sizeof type, tp, tl, false);
    if (!c->open_fn) return open_failure(c, sender, SSHC_OPEN_UNKNOWN_CHANNEL_TYPE, "this client does not accept channels from the server");
    SshcChan *ch = NULL;
    for (int i = 0; i < SSHC_MAX_CHAN && !ch; i++) if (!c->chan[i].used) ch = &c->chan[i];
    if (!ch) return open_failure(c, sender, SSHC_OPEN_RESOURCE_SHORTAGE, "too many channels");
    memset(ch, 0, sizeof *ch);
    ch->used = true;
    ch->id = (uint32_t)(ch - c->chan);
    ch->remote_id = sender;
    ch->remote_win = win;
    ch->remote_max = max < 128 ? 128 : max;
    ch->local_win = SSHC_WINDOW;
    int rc = c->open_fn(c->open_ctx, c, type, r, ch);
    if (rc != 0) {
        memset(ch, 0, sizeof *ch);
        return open_failure(c, sender, (uint32_t)rc, "administratively prohibited");
    }
    ch->open = true;
    uint8_t m[24];
    SshcWr w;
    sshc_wr_init(&w, m, sizeof m);
    sshc_wr_u8(&w, SSHC_MSG_CHANNEL_OPEN_CONFIRMATION);
    sshc_wr_u32(&w, sender);
    sshc_wr_u32(&w, ch->id);
    sshc_wr_u32(&w, SSHC_WINDOW);
    sshc_wr_u32(&w, SSHC_MAXPKT);
    if (sshc_send(c, m, w.n) < 0) return -1;
    if (ch->ops && ch->ops->confirmed) ch->ops->confirmed(c, ch);
    return 0;
}

int sshc_dispatch(SshcConn *c, const uint8_t *pl, size_t n) {
    SshcRd r;
    sshc_rd_init(&r, pl, n);
    uint8_t t = sshc_rd_u8(&r);
    switch (t) {
    case SSHC_MSG_CHANNEL_OPEN:
        return on_server_open(c, &r);
    case SSHC_MSG_CHANNEL_OPEN_CONFIRMATION: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        uint32_t remote = sshc_rd_u32(&r), win = sshc_rd_u32(&r), max = sshc_rd_u32(&r);
        if (r.bad || !ch || ch->open) return 0;
        ch->remote_id = remote;
        ch->remote_win = win;
        ch->remote_max = max < 128 ? 128 : max;
        ch->open = true;
        if (ch->close_out) return send_simple(c, SSHC_MSG_CHANNEL_CLOSE, remote);          /* we gave up on it while the answer was on its way */
        if (ch->ops && ch->ops->confirmed) ch->ops->confirmed(c, ch);
        return 0;
    }
    case SSHC_MSG_CHANNEL_OPEN_FAILURE: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        uint32_t reason = sshc_rd_u32(&r);
        size_t tl = 0;
        const uint8_t *text = sshc_rd_str(&r, &tl);
        if (r.bad || !ch || ch->open) return 0;
        char shown[160];
        sshc_printable(shown, sizeof shown, text, tl, false);
        if (ch->ops && ch->ops->open_failed) ch->ops->open_failed(c, ch, reason, shown);
        ch->ops = NULL;
        memset(ch, 0, sizeof *ch);
        return 0;
    }
    case SSHC_MSG_CHANNEL_WINDOW_ADJUST: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        uint32_t add = sshc_rd_u32(&r);
        if (r.bad) return 0;
        if (ch) ch->remote_win = add > 0xFFFFFFFFu - ch->remote_win ? 0xFFFFFFFFu : ch->remote_win + add;
        return 0;
    }
    case SSHC_MSG_CHANNEL_DATA:
    case SSHC_MSG_CHANNEL_EXTENDED_DATA: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        uint32_t ext = 0;
        if (t == SSHC_MSG_CHANNEL_EXTENDED_DATA) ext = sshc_rd_u32(&r);
        size_t len;
        const uint8_t *d = sshc_rd_str(&r, &len);
        if (r.bad) { snprintf(c->err, sizeof c->err, "Malformed channel data from the server"); return -1; }
        if (!ch || !ch->open) return 0;
        if (len > ch->local_win) {
            snprintf(c->err, sizeof c->err, "The server sent %zu bytes into a window of %u: connection dropped", len, (unsigned)ch->local_win);
            sshc_disconnect(c, SSHC_DISC_PROTOCOL_ERROR, "window exceeded");
            return -1;
        }
        ch->local_win -= (uint32_t)len;
        if (ch->ops && ch->ops->data && !ch->close_out) ch->ops->data(c, ch, d, len, ext);
        else sshc_chan_consumed(c, ch, len);
        return 0;
    }
    case SSHC_MSG_CHANNEL_EOF: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        if (!ch) return 0;
        ch->eof_in = true;
        if (ch->ops && ch->ops->eof) ch->ops->eof(c, ch);
        return 0;
    }
    case SSHC_MSG_CHANNEL_CLOSE: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        if (!ch) return 0;
        ch->close_in = true;
        if (!ch->close_out) {                              /* the peer wants out: answer, and the number is free */
            ch->close_out = true;
            int rc = send_simple(c, SSHC_MSG_CHANNEL_CLOSE, ch->remote_id);
            release(c, ch);
            return rc;
        }
        release(c, ch);
        return 0;
    }
    case SSHC_MSG_CHANNEL_REQUEST: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        size_t nl;
        const uint8_t *name = sshc_rd_str(&r, &nl);
        bool want = sshc_rd_bool(&r);
        if (r.bad) { snprintf(c->err, sizeof c->err, "Malformed channel request from the server"); return -1; }
        if (!ch) return 0;
        char nm[64];
        sshc_printable(nm, sizeof nm, name, nl, false);
        bool ok = ch->ops && ch->ops->request ? ch->ops->request(c, ch, nm, &r, want) : false;
        if (want && !ch->close_out) return send_simple(c, ok ? SSHC_MSG_CHANNEL_SUCCESS : SSHC_MSG_CHANNEL_FAILURE, ch->remote_id);
        return 0;
    }
    case SSHC_MSG_CHANNEL_SUCCESS:
    case SSHC_MSG_CHANNEL_FAILURE: {
        SshcChan *ch = by_id(c, sshc_rd_u32(&r));
        if (!ch || ch->replies_due <= 0) return 0;
        ch->replies_due--;
        if (ch->ops && ch->ops->reply) ch->ops->reply(c, ch, t == SSHC_MSG_CHANNEL_SUCCESS);
        return 0;
    }
    case SSHC_MSG_GLOBAL_REQUEST: {
        size_t nl;
        sshc_rd_str(&r, &nl);
        bool want = sshc_rd_bool(&r);
        if (r.bad) return 0;
        if (!want) return 0;
        uint8_t m = SSHC_MSG_REQUEST_FAILURE;               /* keepalive@openssh.com asks to be told "no" just so that something comes back */
        return sshc_send(c, &m, 1);
    }
    case SSHC_MSG_REQUEST_SUCCESS:
    case SSHC_MSG_REQUEST_FAILURE: {
        if (c->n_greply <= 0) return 0;
        SshcGlobalReplyFn fn = c->greply[0].fn;
        void *ctx = c->greply[0].ctx;
        memmove(&c->greply[0], &c->greply[1], (size_t)(c->n_greply - 1) * sizeof c->greply[0]);
        c->n_greply--;
        if (fn) fn(ctx, c, t == SSHC_MSG_REQUEST_SUCCESS, &r);
        return 0;
    }
    case SSHC_MSG_USERAUTH_BANNER:
    case SSHC_MSG_USERAUTH_SUCCESS:                          /* late authentication traffic: nothing to do any more */
        return 0;
    default:
        return unimplemented(c);
    }
}

int sshc_pump(SshcConn *c, int timeout_ms) {
    const uint8_t *p;
    int n = sshc_recv(c, &p, timeout_ms);
    if (n == SSHC_RECV_TIMEOUT) return 0;
    if (n <= 0) return -1;
    if (sshc_dispatch(c, p, (size_t)n) < 0) return -1;
    return 1;
}
