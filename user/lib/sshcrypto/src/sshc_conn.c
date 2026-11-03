/*
 * The client's transport layer (RFC 4253): the TCP connection, the identification strings, binary packets and the key exchange.
 *
 * One algorithm set (see sshc.h): curve25519-sha256 key exchange, ssh-ed25519 host key, chacha20-poly1305@openssh.com packets, no compression. The strict key
 * exchange extension of OpenSSH (the answer to the Terrapin attack on this very cipher) is announced and, when the server announces it too, enforced: the
 * server's KEXINIT must be the first packet, nothing but key-exchange messages may arrive until the new keys are in, and both sequence numbers start from
 * zero again after every NEWKEYS.
 *
 * Packets with chacha20-poly1305@openssh.com (OpenSSH's PROTOCOL.chacha20poly1305): the 4-byte length is encrypted by one ChaCha20 instance (the header key,
 * nonce = the packet's sequence number, block 0); the rest by a second instance (the main key, same nonce, from block 1) whose block 0 gives the Poly1305 key; the
 * 16-byte tag covers the encrypted length and the encrypted rest. The 64 bytes of key material per direction are the main key followed by the header key.
 *
 * Output is queued: sshc_send() encrypts a packet into a buffer and pushes what the socket takes; whatever waits is pushed out again whenever the program waits
 * for input, so a program that is busy reading never makes the server wait for a write that does not come.
 */
#include <sshc.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sshcrypto.h>

#define CLIENT_ID "SSH-2.0-TouhouOS_ssh_1"
#define SEQ_LIMIT 0xFFFFFF00u               /* the sequence number is the cipher's nonce: it must never come round (a re-key resets it long before) */
#define STALL_MS 30000                      /* a packet may take this long from its first byte to its last */
#define TX_MAX (8u << 20)                   /* a queue this long means the program ignored the peer's windows */
#define DEFAULT_REKEY_BYTES (1ull << 30)

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

static void fail(SshcConn *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(SshcConn *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------------------------------ connecting */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int sshc_tcp_connect(const char *host, int port, int timeout_ms, char *why, size_t wc, char *ip, size_t ic) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;                              /* the system's network stack is IPv4 only */
    hints.ai_socktype = SOCK_STREAM;
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(why, wc, "Could not resolve hostname %s: %s", host, rc ? gai_strerror(rc) : "no address");
        return -1;
    }
    long until = timeout_ms > 0 ? sshc_mono_ms() + timeout_ms : 0;
    int last = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, 0);
        if (fd < 0) { last = errno; continue; }
        set_nonblock(fd);
        int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            for (;;) {
                int ms = -1;
                if (until) {
                    long left = until - sshc_mono_ms();
                    if (left <= 0) { errno = ETIMEDOUT; r = -1; break; }
                    ms = left > 1000 ? 1000 : (int)left;
                } else {
                    ms = 1000;
                }
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                int pr = poll(&p, 1, ms);
                if (pr < 0 && errno != EINTR) { r = -1; break; }
                if (pr > 0) {
                    int err = 0;
                    socklen_t el = sizeof err;
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                    if (err) { errno = err; r = -1; } else { r = 0; }
                    break;
                }
            }
        }
        if (r == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);          /* keystrokes should not wait for company */
            if (ip && ic) {
                if (!inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, (socklen_t)ic)) sshc_cpy(ip, ic, host);
            }
            freeaddrinfo(res);
            return fd;
        }
        last = errno;
        close(fd);
    }
    freeaddrinfo(res);
    snprintf(why, wc, "connect to host %s port %d: %s", host, port, strerror(last));
    return -1;
}

SshcConn *sshc_conn_new(int fd) {
    SshcConn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    c->in = malloc(SSHC_PKT_MAX + 64);
    c->out = malloc(SSHC_PKT_MAX + 64);
    c->is = malloc(SSHC_PKT_MAX);
    if (!c->in || !c->out || !c->is) { free(c->in); free(c->out); free(c->is); free(c); return NULL; }
    set_nonblock(fd);
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin_family == AF_INET) {
        inet_ntop(AF_INET, &sa.sin_addr, c->peer, sizeof c->peer);
        c->peer_port = ntohs(sa.sin_port);
    } else {
        sshc_cpy(c->peer, sizeof c->peer, "unknown");
    }
    c->last_rx_ms = sshc_mono_ms();
    return c;
}

void sshc_conn_free(SshcConn *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    memset(c->main_in, 0, sizeof c->main_in);
    memset(c->hdr_in, 0, sizeof c->hdr_in);
    memset(c->main_out, 0, sizeof c->main_out);
    memset(c->hdr_out, 0, sizeof c->hdr_out);
    free(c->in); free(c->out); free(c->is); free(c->txq); free(c->pend); free(c->cur);
    free(c);
}

/* ------------------------------------------------------------------------------------------------ socket IO */

/* Pushes the send queue into the socket as far as it goes. 0 (something may be left: look at tx_len), -1 on a broken connection. */
static int flush_nb(SshcConn *c) {
    while (c->tx_off < c->tx_len) {
        ssize_t r = send(c->fd, c->txq + c->tx_off, c->tx_len - c->tx_off, MSG_NOSIGNAL);
        if (r > 0) { c->tx_off += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        fail(c, "Write failed: %s", r < 0 ? strerror(errno) : "the connection is closed");
        return -1;
    }
    c->tx_off = c->tx_len = 0;
    return 0;
}

bool sshc_tx_pending(const SshcConn *c) { return c->tx_off < c->tx_len; }

/* Waits until the socket is readable (want_read) or the send queue has drained (!want_read); the queue is pushed out meanwhile.
 * 1 ready, 0 the deadline `until` (absolute, 0 = none) passed, -1 error. */
static int wait_io(SshcConn *c, bool want_read, long until) {
    for (;;) {
        if (!want_read && !sshc_tx_pending(c)) return 1;
        int ms = 1000;
        if (until) {
            long left = until - sshc_mono_ms();
            if (left <= 0) return 0;
            if (left < ms) ms = (int)left;
        }
        struct pollfd p = { .fd = c->fd, .events = (short)((want_read ? POLLIN : 0) | (sshc_tx_pending(c) ? POLLOUT : 0)) };
        int r = poll(&p, 1, ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            fail(c, "poll failed: %s", strerror(errno));
            return -1;
        }
        if (r > 0) {
            if ((p.revents & (POLLOUT | POLLERR | POLLHUP)) && sshc_tx_pending(c) && flush_nb(c) < 0) return -1;
            if (want_read && (p.revents & (POLLIN | POLLHUP | POLLERR))) return 1;
        }
    }
}

int sshc_flush(SshcConn *c, int timeout_ms) {
    if (flush_nb(c) < 0) return -1;
    if (!sshc_tx_pending(c) || timeout_ms == 0) return 0;
    return wait_io(c, false, timeout_ms < 0 ? 0 : sshc_mono_ms() + timeout_ms) < 0 ? -1 : 0;
}

/* Reads exactly n bytes. With `first`, `until` is how long to wait for the first byte (absolute, 0 = for ever): 1 when none arrived by then. Once a byte is
 * there, or when `first` is false (then `until` is the packet's stall limit), a timeout is an error. 0 done; -1 error; -2 the peer closed the connection. */
static int rx_exact(SshcConn *c, uint8_t *buf, size_t n, long until, bool first) {
    size_t got = 0;
    long stall = first ? 0 : until;
    while (got < n) {
        ssize_t r = recv(c->fd, buf + got, n - got, 0);
        if (r > 0) {
            got += (size_t)r;
            if (first && !stall) stall = sshc_mono_ms() + STALL_MS;
            continue;
        }
        if (r == 0) {
            fail(c, "Connection closed by %s port %u", c->peer, c->peer_port);
            return -2;
        }
        if (errno == EINTR) continue;
        if (errno == ECONNRESET) { fail(c, "Connection reset by %s port %u", c->peer, c->peer_port); return -2; }
        if (errno != EAGAIN && errno != EWOULDBLOCK) { fail(c, "Read from %s port %u failed: %s", c->peer, c->peer_port, strerror(errno)); return -1; }
        long limit = (first && !got) ? until : stall;
        if (c->deadline_ms && (!limit || c->deadline_ms < limit)) limit = c->deadline_ms;
        int w = wait_io(c, true, limit);
        if (w < 0) return -1;
        if (w == 0) {
            bool handshake_over = c->deadline_ms && sshc_mono_ms() >= c->deadline_ms;
            if (first && !got && !handshake_over) return 1;
            fail(c, "Connection to %s port %u timed out", c->peer, c->peer_port);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ packets */

static void poly_key(uint8_t out[32], const uint8_t key[32], uint32_t seq) {
    uint8_t nonce[8], zero[32] = { 0 };
    put_be64(nonce, seq);
    chacha20_xor(out, zero, 32, key, nonce, 0);
}

/* Reads one packet. 0 done (payload and length set), 1 nothing arrived in time, -1 error, -2 the peer closed the connection. */
static int rx_packet(SshcConn *c, long until, const uint8_t **payload, size_t *plen) {
    uint8_t *g = c->in;
    uint32_t len;
    int r;
    if (c->seq_in >= SEQ_LIMIT) { fail(c, "The packet counter has run out without a new key exchange"); return -1; }
    if (c->enc_in) {
        uint8_t nonce[8], lenb[4];
        put_be64(nonce, c->seq_in);
        if ((r = rx_exact(c, g, 4, until, true)) != 0) return r;
        chacha20_xor(lenb, g, 4, c->hdr_in, nonce, 0);
        len = get_be32(lenb);
        if (len < 8 || len > SSHC_PKT_MAX || (len & 7)) { fail(c, "Bad packet length %u from %s port %u", len, c->peer, c->peer_port); return -1; }
        if ((r = rx_exact(c, g + 4, (size_t)len + 16, sshc_mono_ms() + STALL_MS, false)) != 0) return r;
        uint8_t pk[32], tag[16];
        poly_key(pk, c->main_in, c->seq_in);
        poly1305_mac(tag, g, 4 + (size_t)len, pk);
        int bad = !sshc_ct_equal(tag, g + 4 + len, 16);
        memset(pk, 0, sizeof pk);
        if (bad) { fail(c, "Corrupted MAC on input (message authentication code incorrect) from %s port %u", c->peer, c->peer_port); return -1; }
        chacha20_xor(g + 4, g + 4, len, c->main_in, nonce, 1);
        c->bytes_kex += 4 + (uint64_t)len + 16;
    } else {
        if ((r = rx_exact(c, g, 4, until, true)) != 0) return r;
        len = get_be32(g);
        if (len < 12 || len > SSHC_PKT_MAX || ((len + 4) & 7)) { fail(c, "Bad packet length %u from %s port %u", len, c->peer, c->peer_port); return -1; }
        if ((r = rx_exact(c, g + 4, len, sshc_mono_ms() + STALL_MS, false)) != 0) return r;
    }
    uint8_t padlen = g[4];
    if (padlen < 4 || (size_t)padlen + 1 >= len) { fail(c, "Bad padding length %u in a packet of %u bytes", padlen, len); return -1; }
    *payload = g + 5;
    *plen = len - padlen - 1u;
    c->last_seq = c->seq_in++;
    c->last_rx_ms = sshc_mono_ms();
    return 0;
}

int sshc_send(SshcConn *c, const uint8_t *payload, size_t n) {
    if (n == 0 || n > SSHC_PKT_MAX - 64) { fail(c, "Internal error: a packet of %zu bytes", n); return -1; }
    if (c->seq_out >= SEQ_LIMIT) { fail(c, "The packet counter has run out without a new key exchange"); return -1; }
    uint8_t *p = c->out;
    size_t pad = 8 - ((c->enc_out ? 1 + n : 5 + n) % 8);       /* an AEAD cipher leaves the length field out of the alignment */
    if (pad < 4) pad += 8;
    uint32_t len = (uint32_t)(1 + n + pad);
    put_be32(p, len);
    p[4] = (uint8_t)pad;
    memcpy(p + 5, payload, n);
    sshc_random(p + 5 + n, pad);
    size_t total = 4 + (size_t)len;
    if (c->enc_out) {
        uint8_t nonce[8], pk[32];
        put_be64(nonce, c->seq_out);
        chacha20_xor(p + 4, p + 4, len, c->main_out, nonce, 1);
        chacha20_xor(p, p, 4, c->hdr_out, nonce, 0);
        poly_key(pk, c->main_out, c->seq_out);
        poly1305_mac(p + total, p, total, pk);
        memset(pk, 0, sizeof pk);
        total += 16;
    }
    c->seq_out++;
    c->bytes_kex += total;
    if (c->tx_off == c->tx_len) c->tx_off = c->tx_len = 0;
    if (c->tx_off && c->tx_len + total > c->tx_cap) {          /* make room at the end by moving what is left to the front */
        memmove(c->txq, c->txq + c->tx_off, c->tx_len - c->tx_off);
        c->tx_len -= c->tx_off;
        c->tx_off = 0;
    }
    if (c->tx_len + total > c->tx_cap) {
        if (c->tx_len + total > TX_MAX) { fail(c, "Internal error: %zu bytes are waiting to be sent", c->tx_len); return -1; }
        size_t nc = c->tx_cap ? c->tx_cap : 65536;
        while (nc < c->tx_len + total) nc *= 2;
        uint8_t *nb = realloc(c->txq, nc);
        if (!nb) { fail(c, "Out of memory"); return -1; }
        c->txq = nb;
        c->tx_cap = nc;
    }
    memcpy(c->txq + c->tx_len, p, total);
    c->tx_len += total;
    return flush_nb(c);
}

void sshc_disconnect(SshcConn *c, uint32_t reason, const char *text) {
    uint8_t buf[300];
    SshcWr w;
    sshc_wr_init(&w, buf, sizeof buf);
    sshc_wr_u8(&w, SSHC_MSG_DISCONNECT);
    sshc_wr_u32(&w, reason);
    sshc_wr_cstr(&w, text);
    sshc_wr_cstr(&w, "");
    if (!w.bad) {
        char keep[sizeof c->err];
        memcpy(keep, c->err, sizeof keep);                  /* sending must not replace the reason we are leaving for */
        if (sshc_send(c, buf, w.n) == 0) sshc_flush(c, 1000);
        memcpy(c->err, keep, sizeof keep);
    }
}

/* ------------------------------------------------------------------------------------------------ key exchange */

static int send_kexinit(SshcConn *c) {
    uint8_t buf[1024];
    SshcWr w;
    sshc_wr_init(&w, buf, sizeof buf);
    sshc_wr_u8(&w, SSHC_MSG_KEXINIT);
    uint8_t cookie[16];
    sshc_random(cookie, sizeof cookie);
    sshc_wr_raw(&w, cookie, sizeof cookie);
    sshc_wr_cstr(&w, c->kex_done ? "curve25519-sha256,curve25519-sha256@libssh.org"
                                 : "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com");
    sshc_wr_cstr(&w, "ssh-ed25519");
    sshc_wr_cstr(&w, "chacha20-poly1305@openssh.com");
    sshc_wr_cstr(&w, "chacha20-poly1305@openssh.com");
    sshc_wr_cstr(&w, "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512");    /* named but unused: an AEAD brings its own integrity */
    sshc_wr_cstr(&w, "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512");
    sshc_wr_cstr(&w, "none");
    sshc_wr_cstr(&w, "none");
    sshc_wr_cstr(&w, "");
    sshc_wr_cstr(&w, "");
    sshc_wr_bool(&w, false);
    sshc_wr_u32(&w, 0);
    if (w.bad) { fail(c, "Internal error: KEXINIT too long"); return -1; }
    memcpy(c->ic, buf, w.n);
    c->ic_len = w.n;
    return sshc_send(c, buf, w.n);
}

/* The next packet during a key exchange: its length (>= 1), 0 when the peer is gone (c->err says why), -1 on an error. Ignore and debug messages are skipped,
 * except in strict mode, where they do not belong between the KEXINITs and NEWKEYS and the caller refuses what it did not expect. */
static int kex_recv(SshcConn *c, const uint8_t **pl) {
    for (;;) {
        size_t n;
        int r = rx_packet(c, c->deadline_ms, pl, &n);
        if (r == 1) { fail(c, "Timeout during key exchange with %s port %u", c->peer, c->peer_port); return -1; }
        if (r == -2) return 0;
        if (r < 0) return -1;
        uint8_t t = (*pl)[0];
        if (t == SSHC_MSG_DISCONNECT) {
            SshcRd rd;
            sshc_rd_init(&rd, *pl + 1, n - 1);
            uint32_t reason = sshc_rd_u32(&rd);
            size_t tl = 0;
            const uint8_t *text = sshc_rd_str(&rd, &tl);
            char shown[200];
            sshc_printable(shown, sizeof shown, rd.bad ? (const uint8_t *)"" : text, rd.bad ? 0 : tl, false);
            fail(c, "Received disconnect from %s port %u:%u: %s", c->peer, c->peer_port, reason, shown);
            return 0;
        }
        if ((t == SSHC_MSG_IGNORE || t == SSHC_MSG_DEBUG) && !c->strict) continue;
        return (int)n;
    }
}

/* What the two KEXINITs agree on. Returns 0, or -1 with c->err (after telling the server why). *discard: a guessed packet from the server follows and is wrong. */
static int negotiate(SshcConn *c, bool *discard) {
    SshcRd r;
    sshc_rd_init(&r, c->is + 1, c->is_len - 1);
    sshc_rd_skip(&r, 16);
    size_t kl, hl, ecl, esl, ml, cl, csl, ll;
    const uint8_t *kex = sshc_rd_str(&r, &kl), *hk = sshc_rd_str(&r, &hl), *ec = sshc_rd_str(&r, &ecl), *es = sshc_rd_str(&r, &esl);
    sshc_rd_str(&r, &ml); sshc_rd_str(&r, &ml);
    const uint8_t *cc = sshc_rd_str(&r, &cl), *cs = sshc_rd_str(&r, &csl);
    sshc_rd_str(&r, &ll); sshc_rd_str(&r, &ll);
    bool follows = sshc_rd_bool(&r);
    sshc_rd_u32(&r);
    if (r.bad) { sshc_disconnect(c, SSHC_DISC_PROTOCOL_ERROR, "malformed KEXINIT"); fail(c, "The server's key exchange message is damaged"); return -1; }
    const char *what = NULL;
    const uint8_t *offer = NULL;
    size_t offer_len = 0;
    if (!sshc_namelist_has(kex, kl, "curve25519-sha256") && !sshc_namelist_has(kex, kl, "curve25519-sha256@libssh.org")) { what = "key exchange method"; offer = kex; offer_len = kl; }
    else if (!sshc_namelist_has(hk, hl, "ssh-ed25519")) { what = "host key type"; offer = hk; offer_len = hl; }
    else if (!sshc_namelist_has(ec, ecl, "chacha20-poly1305@openssh.com")) { what = "cipher"; offer = ec; offer_len = ecl; }
    else if (!sshc_namelist_has(es, esl, "chacha20-poly1305@openssh.com")) { what = "cipher"; offer = es; offer_len = esl; }
    else if (!sshc_namelist_has(cc, cl, "none") || !sshc_namelist_has(cs, csl, "none")) { what = "compression algorithm"; offer = cc; offer_len = cl; }
    if (what) {
        char got[200];
        sshc_printable(got, sizeof got, offer, offer_len, false);
        fail(c, "Unable to negotiate with %s port %u: no matching %s found. Their offer: %s (this client speaks curve25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com)",
             c->peer, c->peer_port, what, got);
        sshc_disconnect(c, SSHC_DISC_KEY_EXCHANGE_FAILED, "no matching algorithm");
        return -1;
    }
    if (!c->kex_done) {
        c->strict = sshc_namelist_has(kex, kl, "kex-strict-s-v00@openssh.com");
        if (c->strict && c->last_seq != 0) {                  /* anything before the KEXINIT is what the Terrapin attack inserts */
            fail(c, "Strict key exchange, but the server's KEXINIT was not its first packet");
            sshc_disconnect(c, SSHC_DISC_PROTOCOL_ERROR, "strict key exchange: the KEXINIT must be the first packet");
            return -1;
        }
        sshc_debug(1, "kex: %s", c->strict ? "strict key exchange is on (kex-strict-s-v00@openssh.com)" : "the server does not announce strict key exchange");
    }
    bool first_kex_ok = kl >= 17 && !memcmp(kex, "curve25519-sha256", 17) && (kl == 17 || kex[17] == ',');
    bool first_hk_ok = hl >= 11 && !memcmp(hk, "ssh-ed25519", 11) && (hl == 11 || hk[11] == ',');
    *discard = follows && !(first_kex_ok && first_hk_ok);
    return 0;
}

static void hash_str(uint8_t *hbuf, size_t *pos, const void *p, size_t n) {
    put_be32(hbuf + *pos, (uint32_t)n);
    memcpy(hbuf + *pos + 4, p, n);
    *pos += 4 + n;
}

/* RFC 4253, section 7.2: the key is HASH(K || H || letter || session_id), extended by HASH(K || H || key so far); two blocks make the 64 bytes this cipher
 * takes: the main key first, then the one for the packet lengths. */
static void derive_key(uint8_t out[64], const uint8_t *kmp, size_t kmp_len, const uint8_t H[32], char letter, const uint8_t sid[32]) {
    uint8_t buf[64 + 5 + 32 + 1 + 32 + 32];
    size_t n = 0;
    memcpy(buf, kmp, kmp_len); n += kmp_len;
    memcpy(buf + n, H, 32); n += 32;
    buf[n++] = (uint8_t)letter;
    memcpy(buf + n, sid, 32); n += 32;
    sshc_sha256(out, buf, n);
    n = 0;
    memcpy(buf, kmp, kmp_len); n += kmp_len;
    memcpy(buf + n, H, 32); n += 32;
    memcpy(buf + n, out, 32); n += 32;
    sshc_sha256(out + 32, buf, n);
}

/* The key exchange proper: both KEXINITs are in c->ic and c->is (the client's and the server's payloads). */
static int kex_exchange(SshcConn *c) {
    uint8_t e[32], qc[32];
    sshc_random(e, sizeof e);
    x25519_base(qc, e);
    uint8_t init[1 + 4 + 32];
    SshcWr w;
    sshc_wr_init(&w, init, sizeof init);
    sshc_wr_u8(&w, SSHC_MSG_KEX_ECDH_INIT);
    sshc_wr_str(&w, qc, 32);
    if (sshc_send(c, init, w.n) < 0) { memset(e, 0, sizeof e); return -1; }

    const uint8_t *pl;
    int n = kex_recv(c, &pl);
    if (n <= 0) { memset(e, 0, sizeof e); return -1; }
    SshcRd r;
    sshc_rd_init(&r, pl, (size_t)n);
    uint8_t t = sshc_rd_u8(&r);
    size_t ksl, qsl, sgl;
    const uint8_t *ks = sshc_rd_str(&r, &ksl), *qs = sshc_rd_str(&r, &qsl), *sig = sshc_rd_str(&r, &sgl);
    if (t != SSHC_MSG_KEX_ECDH_REPLY || r.bad || r.n || qsl != 32 || ksl > SSHC_BLOB_MAX) {
        memset(e, 0, sizeof e);
        fail(c, "Expected the key exchange reply, got message %u", t);
        sshc_disconnect(c, SSHC_DISC_PROTOCOL_ERROR, "expected SSH_MSG_KEX_ECDH_REPLY");
        return -1;
    }
    uint8_t qsc[32], hostblob[SSHC_BLOB_MAX], sigcopy[200], k[32];
    memcpy(qsc, qs, 32);
    memcpy(hostblob, ks, ksl);
    if (sgl > sizeof sigcopy) { memset(e, 0, sizeof e); fail(c, "The server's signature is too long"); return -1; }
    memcpy(sigcopy, sig, sgl);
    if (x25519(k, e, qsc) != 0) {
        memset(e, 0, sizeof e);
        fail(c, "The server sent an invalid key exchange value");
        sshc_disconnect(c, SSHC_DISC_KEY_EXCHANGE_FAILED, "invalid public value");
        return -1;
    }
    memset(e, 0, sizeof e);

    uint8_t kmp[40];
    size_t kmp_len = sshc_mpint(kmp, k, 32);
    size_t cap = 4 * 8 + strlen(c->client_id) + strlen(c->server_id) + c->ic_len + c->is_len + ksl + 32 + 32 + kmp_len;
    uint8_t *hbuf = malloc(cap);
    if (!hbuf) { fail(c, "Out of memory"); return -1; }
    size_t hp = 0;
    hash_str(hbuf, &hp, c->client_id, strlen(c->client_id));
    hash_str(hbuf, &hp, c->server_id, strlen(c->server_id));
    hash_str(hbuf, &hp, c->ic, c->ic_len);
    hash_str(hbuf, &hp, c->is, c->is_len);
    hash_str(hbuf, &hp, hostblob, ksl);
    hash_str(hbuf, &hp, qc, 32);
    hash_str(hbuf, &hp, qsc, 32);
    memcpy(hbuf + hp, kmp, kmp_len);
    hp += kmp_len;
    uint8_t H[32];
    sshc_sha256(H, hbuf, hp);
    free(hbuf);

    /* the host key: an ssh-ed25519 blob, and its signature over H */
    SshcRd hr, sr;
    sshc_rd_init(&hr, hostblob, ksl);
    size_t htl, hkl;
    const uint8_t *htype = sshc_rd_str(&hr, &htl), *hk = sshc_rd_str(&hr, &hkl);
    sshc_rd_init(&sr, sigcopy, sgl);
    size_t stl, sl;
    const uint8_t *stype = sshc_rd_str(&sr, &stl), *sg = sshc_rd_str(&sr, &sl);
    if (hr.bad || hr.n || sr.bad || sr.n || !sshc_str_is(htype, htl, "ssh-ed25519") || hkl != 32 || !sshc_str_is(stype, stl, "ssh-ed25519") || sl != 64 ||
        ed25519_verify(sg, H, 32, hk) != 0) {
        memset(k, 0, sizeof k);
        fail(c, "The server's host key signature is wrong: the key exchange cannot be trusted");
        sshc_disconnect(c, SSHC_DISC_KEY_EXCHANGE_FAILED, "bad host key signature");
        return -1;
    }
    if (!c->kex_done) {
        long asked = sshc_mono_ms();
        int accepted = c->hostkey_fn ? c->hostkey_fn(c->hostkey_ctx, c, hostblob, ksl) : 0;
        if (c->deadline_ms) c->deadline_ms += sshc_mono_ms() - asked;          /* the time a person took to answer is not the server's */
        if (accepted != 0) {
            memset(k, 0, sizeof k);
            if (!c->err[0]) fail(c, "Host key verification failed.");
            sshc_disconnect(c, SSHC_DISC_HOST_KEY_NOT_VERIFIABLE, "host key not verifiable");
            return -1;
        }
        memcpy(c->hostkey, hostblob, ksl);
        c->hostkey_len = ksl;
        memcpy(c->session_id, H, 32);                        /* the first exchange names the session for good */
    } else if (ksl != c->hostkey_len || memcmp(hostblob, c->hostkey, ksl) != 0) {
        memset(k, 0, sizeof k);
        fail(c, "The server changed its host key during a key exchange: connection dropped");
        sshc_disconnect(c, SSHC_DISC_HOST_KEY_NOT_VERIFIABLE, "host key changed");
        return -1;
    }

    uint8_t kc[64], ksv[64];
    derive_key(kc, kmp, kmp_len, H, 'C', c->session_id);     /* client to server: what we send with */
    derive_key(ksv, kmp, kmp_len, H, 'D', c->session_id);    /* server to client: what we read with */
    memset(k, 0, sizeof k);
    memset(kmp, 0, sizeof kmp);

    uint8_t nk = SSHC_MSG_NEWKEYS;
    if (sshc_send(c, &nk, 1) < 0) return -1;
    memcpy(c->main_out, kc, 32);
    memcpy(c->hdr_out, kc + 32, 32);
    c->enc_out = true;
    if (c->strict) c->seq_out = 0;
    memset(kc, 0, sizeof kc);

    n = kex_recv(c, &pl);
    if (n <= 0) return -1;
    if (n != 1 || pl[0] != SSHC_MSG_NEWKEYS) {
        fail(c, "Expected NEWKEYS, got message %u", pl[0]);
        sshc_disconnect(c, SSHC_DISC_PROTOCOL_ERROR, "expected SSH_MSG_NEWKEYS");
        return -1;
    }
    memcpy(c->main_in, ksv, 32);
    memcpy(c->hdr_in, ksv + 32, 32);
    c->enc_in = true;
    if (c->strict) c->seq_in = 0;
    memset(ksv, 0, sizeof ksv);
    c->bytes_kex = 0;
    c->kex_at_ms = sshc_mono_ms();
    return 0;
}

/* Both KEXINITs are exchanged (ours sent, the server's in c->is): negotiate, discard a wrong guess, run the exchange. */
static int kex_after_kexinits(SshcConn *c) {
    bool discard = false;
    if (negotiate(c, &discard) < 0) return -1;
    if (discard) {
        const uint8_t *pl;
        if (kex_recv(c, &pl) <= 0) return -1;
    }
    if (kex_exchange(c) < 0) return -1;
    if (!c->kex_done) sshc_debug(1, "kex: algorithm: curve25519-sha256; host key: ssh-ed25519; cipher: chacha20-poly1305@openssh.com");
    c->kex_done = true;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the start */

/* One line ending in LF, up to cap bytes, read byte by byte so nothing of the packet that follows is taken. 0, or -1 / -2 as rx_exact. */
static int read_id_line(SshcConn *c, char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
        uint8_t b;
        int r = rx_exact(c, &b, 1, c->deadline_ms, true);
        if (r == 1) { fail(c, "Timeout before the server's banner (%s port %u)", c->peer, c->peer_port); return -1; }
        if (r < 0) return r;
        if (b == '\n') break;
        if (n + 1 >= cap) { fail(c, "kex_exchange_identification: banner line too long"); return -1; }
        line[n++] = (char)b;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    return 0;
}

int sshc_handshake(SshcConn *c) {
    sshc_cpy(c->client_id, sizeof c->client_id, CLIENT_ID);
    char hello[80];
    int hl = snprintf(hello, sizeof hello, "%s\r\n", CLIENT_ID);
    if (c->tx_cap == 0) {
        c->txq = malloc(65536);
        if (!c->txq) { fail(c, "Out of memory"); return -1; }
        c->tx_cap = 65536;
    }
    memcpy(c->txq + c->tx_len, hello, (size_t)hl);
    c->tx_len += (size_t)hl;
    if (flush_nb(c) < 0) return -1;

    int tries = 0;
    for (;;) {                                                /* a server may send other lines before its identification (RFC 4253, section 4.2) */
        char line[256];
        int r = read_id_line(c, line, sizeof line);
        if (r == -2) {
            if (!strstr(c->err, "closed")) return -1;
            fail(c, "kex_exchange_identification: Connection closed by remote host (%s port %u)", c->peer, c->peer_port);
            return -1;
        }
        if (r < 0) return -1;
        if (!strncmp(line, "SSH-", 4)) {
            if (strncmp(line, "SSH-2.0-", 8) && strncmp(line, "SSH-1.99-", 9)) {
                char shown[100];
                sshc_printable(shown, sizeof shown, (const uint8_t *)line, strlen(line), false);
                fail(c, "Protocol major versions differ: the server speaks %s, this client SSH 2.0 only", shown);
                return -1;
            }
            sshc_cpy(c->server_id, sizeof c->server_id, line);
            break;
        }
        if (++tries > 64) { fail(c, "kex_exchange_identification: no SSH banner from %s port %u (is that an SSH server?)", c->peer, c->peer_port); return -1; }
    }
    sshc_debug(1, "Remote protocol version %s", c->server_id);

    if (send_kexinit(c) < 0) return -1;
    const uint8_t *pl;
    int n = kex_recv(c, &pl);
    if (n <= 0) return -1;
    if (pl[0] != SSHC_MSG_KEXINIT) {
        fail(c, "Expected the server's KEXINIT, got message %u", pl[0]);
        sshc_disconnect(c, SSHC_DISC_PROTOCOL_ERROR, "expected SSH_MSG_KEXINIT");
        return -1;
    }
    if ((size_t)n > SSHC_PKT_MAX) return -1;
    memcpy(c->is, pl, (size_t)n);
    c->is_len = (size_t)n;
    return kex_after_kexinits(c);
}

/* ------------------------------------------------------------------------------------------------ the message loop and re-keying */

static void pend_push(SshcConn *c, const uint8_t *p, size_t n) {
    if (c->pend_off == c->pend_len) c->pend_off = c->pend_len = 0;
    if (c->pend_len + 4 + n > c->pend_cap) {
        size_t nc = c->pend_cap ? c->pend_cap : 65536;
        while (nc < c->pend_len + 4 + n) nc *= 2;
        uint8_t *nb = realloc(c->pend, nc);
        if (!nb) return;                                      /* out of memory: the message is lost, and the connection will notice */
        c->pend = nb;
        c->pend_cap = nc;
    }
    put_be32(c->pend + c->pend_len, (uint32_t)n);
    memcpy(c->pend + c->pend_len + 4, p, n);
    c->pend_len += 4 + n;
}

bool sshc_rx_pending(const SshcConn *c) { return c->pend_off < c->pend_len; }

/* A re-key the peer started (its KEXINIT is in `kexinit`), or one we start: both end in the same exchange. */
static int rekey_with(SshcConn *c, const uint8_t *kexinit, size_t n) {
    if (n > SSHC_PKT_MAX) return -1;
    memcpy(c->is, kexinit, n);
    c->is_len = n;
    if (send_kexinit(c) < 0) return -1;
    sshc_debug(2, "rekey: key exchange with %s port %u", c->peer, c->peer_port);
    return kex_after_kexinits(c);
}

int sshc_rekey_if_due(SshcConn *c, bool force) {
    uint64_t limit = c->rekey_bytes ? c->rekey_bytes : DEFAULT_REKEY_BYTES;
    long now = sshc_mono_ms();
    bool due = force || c->bytes_kex >= limit || c->seq_out > 0x7FFFFFFFu || c->seq_in > 0x7FFFFFFFu || (c->rekey_ms && now - c->kex_at_ms >= c->rekey_ms);
    if (!due || !c->kex_done) return 0;
    c->deadline_ms = 0;
    if (send_kexinit(c) < 0) return -1;
    sshc_debug(2, "rekey: starting a key exchange (%llu bytes since the last)", (unsigned long long)c->bytes_kex);
    for (;;) {                                                /* until the server's KEXINIT: whatever else it had already sent is kept for the program */
        const uint8_t *pl;
        size_t pn;
        int r = rx_packet(c, sshc_mono_ms() + STALL_MS, &pl, &pn);
        if (r == 1) { fail(c, "Timeout waiting for the server's key exchange message"); return -1; }
        if (r == -2) return 0;
        if (r < 0) return -1;
        if (pl[0] == SSHC_MSG_KEXINIT) {
            memcpy(c->is, pl, pn);
            c->is_len = pn;
            break;
        }
        if (pl[0] == SSHC_MSG_DISCONNECT) { pend_push(c, pl, pn); return 0; }
        if (pl[0] == SSHC_MSG_IGNORE || pl[0] == SSHC_MSG_DEBUG) continue;
        pend_push(c, pl, pn);
    }
    return kex_after_kexinits(c);
}

int sshc_recv(SshcConn *c, const uint8_t **payload, int timeout_ms) {
    long start = sshc_mono_ms();
    if (c->pend_off == c->pend_len && c->pend_len) c->pend_off = c->pend_len = 0;
    for (;;) {
        if (c->pend_off < c->pend_len) {                      /* messages that came in during a re-key we started */
            uint32_t n = get_be32(c->pend + c->pend_off);
            *payload = c->pend + c->pend_off + 4;
            c->pend_off += 4 + n;
            if (**payload == SSHC_MSG_DISCONNECT) {
                SshcRd rd;
                sshc_rd_init(&rd, *payload + 1, n - 1);
                sshc_rd_u32(&rd);
                size_t tl = 0;
                const uint8_t *text = sshc_rd_str(&rd, &tl);
                char shown[200];
                sshc_printable(shown, sizeof shown, rd.bad ? (const uint8_t *)"" : text, rd.bad ? 0 : tl, false);
                fail(c, "Received disconnect from %s port %u: %s", c->peer, c->peer_port, shown);
                return 0;
            }
            return (int)n;
        }
        long until = 0;
        if (timeout_ms >= 0) {
            until = start + timeout_ms;
            if (until <= 0) until = 1;
        }
        const uint8_t *pl;
        size_t n;
        int r = rx_packet(c, timeout_ms == 0 ? sshc_mono_ms() : until, &pl, &n);
        if (r == 1) return SSHC_RECV_TIMEOUT;
        if (r == -2) return 0;
        if (r < 0) return -1;
        switch (pl[0]) {
        case SSHC_MSG_DISCONNECT: {
            SshcRd rd;
            sshc_rd_init(&rd, pl + 1, n - 1);
            uint32_t reason = sshc_rd_u32(&rd);
            size_t tl = 0;
            const uint8_t *text = sshc_rd_str(&rd, &tl);
            char shown[200];
            sshc_printable(shown, sizeof shown, rd.bad ? (const uint8_t *)"" : text, rd.bad ? 0 : tl, false);
            fail(c, "Received disconnect from %s port %u:%u: %s", c->peer, c->peer_port, reason, shown);
            return 0;
        }
        case SSHC_MSG_IGNORE:
        case SSHC_MSG_EXT_INFO:
            continue;
        case SSHC_MSG_DEBUG: {
            SshcRd rd;
            sshc_rd_init(&rd, pl + 1, n - 1);
            bool always = sshc_rd_bool(&rd);
            size_t tl = 0;
            const uint8_t *text = sshc_rd_str(&rd, &tl);
            if (!rd.bad && (always || sshc_verbose >= 1) && sshc_verbose >= 0) {
                char shown[200];
                sshc_printable(shown, sizeof shown, text, tl, false);
                fprintf(stderr, "%s\r\n", shown);
            }
            continue;
        }
        case SSHC_MSG_UNIMPLEMENTED:
            sshc_debug(2, "the server did not understand one of our messages");
            continue;
        case SSHC_MSG_KEXINIT:
            if (rekey_with(c, pl, n) < 0) return -1;
            continue;
        default:
            *payload = pl;
            return (int)n;
        }
    }
}
