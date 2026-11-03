/*
 * The transport layer (RFC 4253): identification strings, binary packets, key exchange.
 *
 * Offered: kex curve25519-sha256 (and its older name @libssh.org), host key ssh-ed25519, cipher chacha20-poly1305@openssh.com in both directions
 * (an AEAD cipher, so no separate MAC), no compression. That is the first choice of every current OpenSSH client; anything else is refused at the
 * KEXINIT with "no matching algorithm".
 *
 * Strict key exchange (OpenSSH's kex-strict-*-v00@openssh.com, the answer to the Terrapin attack on this very cipher) is announced and honoured:
 * the KEXINIT must be the first packet, nothing but key-exchange messages may arrive until the keys change, and the sequence numbers start from zero
 * again after every NEWKEYS.
 *
 * Packets with chacha20-poly1305@openssh.com (OpenSSH's PROTOCOL.chacha20poly1305): the 4-byte length is encrypted by a second ChaCha20 instance
 * (key K_header, nonce = the packet's sequence number, block 0); the rest is encrypted by the main instance (K_main, same nonce, from block 1); the
 * Poly1305 key is block 0 of the main instance, and the tag covers the encrypted length and the encrypted rest.
 */
#include "sshd.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <crypto.h>
#include <gensokyo.h>
#include <sshcrypto.h>

const char SERVER_ID[] = "SSH-2.0-TouhouOS_1";

/* The packet sequence number is also the nonce of the cipher: it must never come round. A client that rekeys (every OpenSSH does, long before) starts it from zero
 * again; one that does not is cut off a little before the counter ends. */
#define SEQ_LIMIT 0xFFFFFF00u

static uint8_t g_in[PKT_MAX + 64];        /* the packet being read (decrypted in place) */
static uint8_t g_out[PKT_MAX + 64];       /* the packet being sent */

static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }

void random_bytes(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        int got = gap_getrandom(b, n > 4096 ? 4096 : n);
        if (got <= 0) { slog("the kernel gave no random bytes (%d): stopping rather than going on without them", got); _exit(70); }
        b += got;
        n -= (size_t)got;
    }
}

/* ------------------------------------------------------------------------------------------------ socket IO */

/* Waits until the socket can do `events`: 1 ready, 0 the deadline passed, -1 error. */
static int wait_fd(Conn *c, short events, long deadline) {
    for (;;) {
        int ms = 1000;
        if (deadline) {
            long left = deadline - mono_ms();
            if (left <= 0) return 0;
            if (left < ms) ms = (int)left;
        }
        struct pollfd p = { .fd = c->fd, .events = events };
        int r = poll(&p, 1, ms);
        if (r > 0) return 1;
        if (r < 0 && errno != EINTR) return -1;
    }
}

/* 0 done, -1 error or timeout, -2 the peer closed the connection. Before the login is done the whole phase has one deadline; later a packet may
 * take at most 30 seconds from its first byte to its last. */
static int read_full(Conn *c, uint8_t *buf, size_t n) {
    long deadline = c->deadline ? c->deadline : mono_ms() + 30000;
    size_t got = 0;
    while (got < n) {
        int w = wait_fd(c, POLLIN, deadline);
        if (w <= 0) {
            slog("%s: %s", c->peer, w == 0 ? "no data from the peer in time" : "poll failed");
            return -1;
        }
        ssize_t r = recv(c->fd, buf + got, n - got, 0);
        if (r > 0) got += (size_t)r;
        else if (r == 0) return -2;
        else if (errno != EINTR && errno != EAGAIN) {
            if (errno == ECONNRESET) return -2;
            slog("%s: recv failed: %s", c->peer, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int write_full(Conn *c, const uint8_t *buf, size_t n) {
    long deadline = mono_ms() + 60000;
    if (c->deadline && c->deadline < deadline) deadline = c->deadline;          /* before the login the whole phase has its one deadline */
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(c->fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r > 0) { sent += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) { if (wait_fd(c, POLLOUT, deadline) <= 0) return -1; continue; }
        slog("%s: send failed: %s", c->peer, r < 0 ? strerror(errno) : "the connection is closed");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ packets */

static void poly_key(uint8_t out[32], const uint8_t key[32], uint32_t seq) {
    uint8_t nonce[8], zero[32] = { 0 };
    put_be64(nonce, seq);
    chacha20_xor(out, zero, 32, key, nonce, 0);
}

/* Reads one packet into g_in. 0 done (the payload is in g_in + 5), -1 error, -2 closed. */
static int recv_packet(Conn *c, const uint8_t **payload, size_t *plen) {
    int r;
    uint32_t len;
    if (c->seq_in >= SEQ_LIMIT) { slog("%s: the packet counter has run out without a new key exchange: connection dropped", c->peer); return -1; }
    if (c->enc_in) {
        uint8_t nonce[8], lenb[4];
        put_be64(nonce, c->seq_in);
        if ((r = read_full(c, g_in, 4)) < 0) return r;
        chacha20_xor(lenb, g_in, 4, c->hdr_in, nonce, 0);
        len = get_be32(lenb);
        if (len < 8 || len > PKT_MAX || (len & 7)) { slog("%s: packet length %u is not allowed", c->peer, len); return -1; }
        if ((r = read_full(c, g_in + 4, (size_t)len + 16)) < 0) return r;
        uint8_t pk[32], tag[16];
        poly_key(pk, c->main_in, c->seq_in);
        poly1305_mac(tag, g_in, 4 + (size_t)len, pk);
        r = ct_memcmp(tag, g_in + 4 + len, 16);
        memset(pk, 0, sizeof pk);
        if (r) { slog("%s: a packet failed its authentication tag", c->peer); return -1; }
        chacha20_xor(g_in + 4, g_in + 4, len, c->main_in, nonce, 1);
    } else {
        if ((r = read_full(c, g_in, 4)) < 0) return r;
        len = get_be32(g_in);
        if (len < 12 || len > PKT_MAX || ((len + 4) & 7)) { slog("%s: packet length %u is not allowed", c->peer, len); return -1; }
        if ((r = read_full(c, g_in + 4, len)) < 0) return r;
    }
    uint8_t padlen = g_in[4];
    if (padlen < 4 || (size_t)padlen + 1 >= len) { slog("%s: bad padding length %u in a packet of %u", c->peer, padlen, len); return -1; }
    *payload = g_in + 5;
    *plen = len - padlen - 1u;
    c->last_seq = c->seq_in++;
    c->last_rx = mono_ms();
    return 0;
}

int send_msg(Conn *c, const uint8_t *payload, size_t n) {
    if (n == 0 || n > PKT_MAX - 64) return -1;
    if (c->seq_out >= SEQ_LIMIT) return -1;
    uint8_t *p = g_out;
    size_t pad = 8 - ((c->enc_out ? 1 + n : 5 + n) % 8);      /* an AEAD cipher leaves the length out of the alignment */
    if (pad < 4) pad += 8;
    uint32_t len = (uint32_t)(1 + n + pad);
    put_be32(p, len);
    p[4] = (uint8_t)pad;
    memcpy(p + 5, payload, n);
    random_bytes(p + 5 + n, pad);
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
    return write_full(c, p, total);
}

void send_disconnect(Conn *c, uint32_t reason, const char *text) {
    uint8_t buf[256];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_DISCONNECT);
    wr_u32(&w, reason);
    wr_cstr(&w, text);
    wr_cstr(&w, "");
    if (!w.bad) send_msg(c, buf, w.n);
}

void send_unimplemented(Conn *c) {
    uint8_t buf[8];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_UNIMPLEMENTED);
    wr_u32(&w, c->last_seq);
    send_msg(c, buf, w.n);
}

/* ------------------------------------------------------------------------------------------------ key exchange */

static int send_kexinit(Conn *c) {
    static uint8_t buf[1024];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_KEXINIT);
    uint8_t cookie[16];
    random_bytes(cookie, sizeof cookie);
    wr_raw(&w, cookie, sizeof cookie);
    wr_cstr(&w, c->kex_done ? "curve25519-sha256,curve25519-sha256@libssh.org"
                            : "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-s-v00@openssh.com");
    wr_cstr(&w, "ssh-ed25519");
    wr_cstr(&w, "chacha20-poly1305@openssh.com");
    wr_cstr(&w, "chacha20-poly1305@openssh.com");
    wr_cstr(&w, "hmac-sha2-256");                     /* named but unused: an AEAD cipher brings its own integrity */
    wr_cstr(&w, "hmac-sha2-256");
    wr_cstr(&w, "none");
    wr_cstr(&w, "none");
    wr_cstr(&w, "");
    wr_cstr(&w, "");
    wr_bool(&w, false);
    wr_u32(&w, 0);
    if (w.bad) return -1;
    memcpy(c->is, buf, w.n);
    c->is_len = w.n;
    return send_msg(c, buf, w.n);
}

/* The next packet during a key exchange: its length, 0 when the peer is gone, -1 on an error. Outside strict mode ignore and debug messages
 * are skipped as everywhere; in strict mode they do not belong here, and the caller refuses what it did not expect. */
static int kex_recv(Conn *c, const uint8_t **pl) {
    for (;;) {
        size_t n;
        int r = recv_packet(c, pl, &n);
        if (r == -2) return 0;
        if (r < 0) return -1;
        uint8_t t = (*pl)[0];
        if (t == MSG_DISCONNECT) return 0;
        if ((t == MSG_IGNORE || t == MSG_DEBUG) && !c->strict) continue;
        return (int)n;
    }
}

static bool first_is(const uint8_t *list, size_t n, const char *name) {
    size_t len = strlen(name);
    return n >= len && !memcmp(list, name, len) && (n == len || list[len] == ',');
}

/* Reads the peer's KEXINIT (c->ic) and picks the algorithms. *discard is set when a guessed first packet follows that has to be ignored.
 * 0, or -1 after telling the peer why. */
static int negotiate(Conn *c, bool *discard) {
    Rd r;
    rd_init(&r, c->ic + 1, c->ic_len - 1);
    rd_skip(&r, 16);
    size_t kl, hl, ecl, esl, ml, cl, csl, ll;
    const uint8_t *kex = rd_str(&r, &kl), *hk = rd_str(&r, &hl), *ec = rd_str(&r, &ecl), *es = rd_str(&r, &esl);
    rd_str(&r, &ml); rd_str(&r, &ml);
    const uint8_t *cc = rd_str(&r, &cl), *cs = rd_str(&r, &csl);
    rd_str(&r, &ll); rd_str(&r, &ll);
    bool follows = rd_bool(&r);
    rd_u32(&r);
    if (r.bad) { send_disconnect(c, DISC_PROTOCOL_ERROR, "malformed KEXINIT"); return -1; }
    static const char *const kexes[] = { "curve25519-sha256", "curve25519-sha256@libssh.org", NULL };
    if (!namelist_pick(kex, kl, kexes) || !namelist_has(hk, hl, "ssh-ed25519") || !namelist_has(ec, ecl, "chacha20-poly1305@openssh.com") ||
        !namelist_has(es, esl, "chacha20-poly1305@openssh.com") || !namelist_has(cc, cl, "none") || !namelist_has(cs, csl, "none")) {
        char got[160];
        printable(got, sizeof got, kex, kl);
        slog("%s: no matching algorithm (its key exchanges: %s)", c->peer, got);
        send_disconnect(c, DISC_KEY_EXCHANGE_FAILED, "no matching algorithm: need curve25519-sha256, ssh-ed25519, chacha20-poly1305@openssh.com, none");
        return -1;
    }
    if (!c->kex_done) {
        c->strict = namelist_has(kex, kl, "kex-strict-c-v00@openssh.com");
        if (c->strict && c->last_seq != 0) {                  /* anything before the KEXINIT is what the Terrapin attack inserts */
            slog("%s: strict key exchange, but the KEXINIT was not the first packet", c->peer);
            send_disconnect(c, DISC_PROTOCOL_ERROR, "strict key exchange: the KEXINIT must be the first packet");
            return -1;
        }
    }
    *discard = follows && !(first_is(kex, kl, "curve25519-sha256") && first_is(hk, hl, "ssh-ed25519"));
    return 0;
}

static void hash_str(sha256_ctx *h, const void *p, size_t n) {
    uint8_t len[4];
    put_be32(len, (uint32_t)n);
    sha256_update(h, len, 4);
    sha256_update(h, p, n);
}

/* RFC 4253, section 7.2: the key is HASH(K || H || letter || session_id), extended by HASH(K || H || key so far). Two blocks make the 64 bytes
 * this cipher takes: the main key first, then the one for the packet lengths. */
static void derive_key(uint8_t out[64], const uint8_t *kmp, size_t kmp_len, const uint8_t H[32], char letter, const uint8_t sid[32]) {
    sha256_ctx s;
    sha256_init(&s);
    sha256_update(&s, kmp, kmp_len);
    sha256_update(&s, H, 32);
    sha256_update(&s, &letter, 1);
    sha256_update(&s, sid, 32);
    sha256_final(&s, out);
    sha256_init(&s);
    sha256_update(&s, kmp, kmp_len);
    sha256_update(&s, H, 32);
    sha256_update(&s, out, 32);
    sha256_final(&s, out + 32);
}

/* Both KEXINITs are in c->ic and c->is. Runs the Diffie-Hellman exchange and the change of keys. */
static int kex_exchange(Conn *c) {
    const uint8_t *pl;
    int n = kex_recv(c, &pl);
    if (n <= 0) return -1;
    Rd r;
    rd_init(&r, pl, (size_t)n);
    uint8_t t = rd_u8(&r);
    size_t ql;
    const uint8_t *qc = rd_str(&r, &ql);
    if (t != MSG_KEX_ECDH_INIT || r.bad || ql != 32 || r.n) {
        slog("%s: expected the key exchange message, got %u", c->peer, t);
        send_disconnect(c, DISC_PROTOCOL_ERROR, "expected SSH_MSG_KEX_ECDH_INIT");
        return -1;
    }
    uint8_t e[32], qs[32], k[32], qcopy[32];
    memcpy(qcopy, qc, 32);
    random_bytes(e, sizeof e);
    x25519_base(qs, e);
    if (x25519(k, e, qcopy) != 0) {
        memset(e, 0, sizeof e);
        send_disconnect(c, DISC_KEY_EXCHANGE_FAILED, "invalid public value");
        return -1;
    }
    memset(e, 0, sizeof e);

    uint8_t kmp[40];
    size_t kmp_len = mpint_encode(kmp, k, 32);
    sha256_ctx h;
    sha256_init(&h);
    hash_str(&h, c->client_id, strlen(c->client_id));
    hash_str(&h, c->server_id, strlen(c->server_id));
    hash_str(&h, c->ic, c->ic_len);
    hash_str(&h, c->is, c->is_len);
    hash_str(&h, g_host_blob, sizeof g_host_blob);
    hash_str(&h, qcopy, 32);
    hash_str(&h, qs, 32);
    sha256_update(&h, kmp, kmp_len);
    uint8_t H[32];
    sha256_final(&h, H);
    if (!c->kex_done) memcpy(c->session_id, H, 32);           /* the first exchange names the session for good */

    uint8_t sig[64];
    host_key_sign(sig, H, 32);
    uint8_t reply[256];
    Wr w;
    wr_init(&w, reply, sizeof reply);
    wr_u8(&w, MSG_KEX_ECDH_REPLY);
    wr_str(&w, g_host_blob, sizeof g_host_blob);
    wr_str(&w, qs, 32);
    uint8_t sigblob[4 + 11 + 4 + 64];
    Wr sw;
    wr_init(&sw, sigblob, sizeof sigblob);
    wr_cstr(&sw, "ssh-ed25519");
    wr_str(&sw, sig, 64);
    wr_str(&w, sigblob, sw.n);
    if (w.bad || sw.bad || send_msg(c, reply, w.n) < 0) { memset(k, 0, sizeof k); return -1; }

    uint8_t kc[64], ks[64];
    derive_key(kc, kmp, kmp_len, H, 'C', c->session_id);      /* client to server */
    derive_key(ks, kmp, kmp_len, H, 'D', c->session_id);      /* server to client */
    memset(k, 0, sizeof k);
    memset(kmp, 0, sizeof kmp);

    uint8_t nk = MSG_NEWKEYS;
    if (send_msg(c, &nk, 1) < 0) return -1;
    memcpy(c->main_out, ks, 32);
    memcpy(c->hdr_out, ks + 32, 32);
    c->enc_out = true;
    if (c->strict) c->seq_out = 0;
    memset(ks, 0, sizeof ks);

    n = kex_recv(c, &pl);
    if (n <= 0) return -1;
    if (n != 1 || pl[0] != MSG_NEWKEYS) {
        slog("%s: expected NEWKEYS, got message %u", c->peer, pl[0]);
        send_disconnect(c, DISC_PROTOCOL_ERROR, "expected SSH_MSG_NEWKEYS");
        return -1;
    }
    memcpy(c->main_in, kc, 32);
    memcpy(c->hdr_in, kc + 32, 32);
    c->enc_in = true;
    if (c->strict) c->seq_in = 0;
    memset(kc, 0, sizeof kc);
    return 0;
}

/* Our KEXINIT may already be sent (the first exchange) or not (the peer started a new one); `discard` as from negotiate(). */
static int kex_after_kexinits(Conn *c) {
    bool discard = false;
    if (negotiate(c, &discard) < 0) return -1;
    if (discard) {
        const uint8_t *pl;
        if (kex_recv(c, &pl) <= 0) return -1;
    }
    if (kex_exchange(c) < 0) return -1;
    c->kex_done = true;
    return 0;
}

static int rekey(Conn *c, const uint8_t *kexinit, size_t n) {
    if (n > sizeof c->ic) return -1;
    memcpy(c->ic, kexinit, n);
    c->ic_len = n;
    if (send_kexinit(c) < 0) return -1;
    return kex_after_kexinits(c);
}

/* ------------------------------------------------------------------------------------------------ start and the message loop */

/* One line of up to 255 bytes ending in LF, read byte by byte so nothing of the packet that follows is taken. */
static int read_id_line(Conn *c, char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
        uint8_t b;
        int r = read_full(c, &b, 1);
        if (r < 0) return r;
        if (b == '\n') break;
        if (n + 1 >= cap) return -1;
        line[n++] = (char)b;
    }
    if (n && line[n - 1] == '\r') n--;
    line[n] = 0;
    return 0;
}

int transport_start(Conn *c) {
    c->deadline = mono_ms() + LOGIN_GRACE_MS;
    snprintf(c->server_id, sizeof c->server_id, "%s", SERVER_ID);
    char hello[80];
    snprintf(hello, sizeof hello, "%s\r\n", SERVER_ID);
    if (write_full(c, (const uint8_t *)hello, strlen(hello)) < 0) return -1;

    int r = read_id_line(c, c->client_id, sizeof c->client_id);       /* the first line is the identification, whatever it says */
    if (r < 0) {
        if (r == -1) slog("%s: no identification string", c->peer);
        return -1;
    }
    if (strncmp(c->client_id, "SSH-2.0-", 8) && strncmp(c->client_id, "SSH-1.99-", 9)) {
        char shown[64];
        printable(shown, sizeof shown, (const uint8_t *)c->client_id, strlen(c->client_id));
        slog("%s: protocol version not supported: %s", c->peer, shown);
        const char *msg = "Protocol mismatch: this server speaks SSH 2.0 only.\r\n";
        write_full(c, (const uint8_t *)msg, strlen(msg));
        return -1;
    }
    char shown[96];
    printable(shown, sizeof shown, (const uint8_t *)c->client_id, strlen(c->client_id));
    slog("%s: connected, client %s", c->peer, shown);

    if (send_kexinit(c) < 0) return -1;
    const uint8_t *pl;
    int n = kex_recv(c, &pl);
    if (n <= 0) return -1;
    if (pl[0] != MSG_KEXINIT) {
        send_disconnect(c, DISC_PROTOCOL_ERROR, "expected SSH_MSG_KEXINIT");
        return -1;
    }
    if ((size_t)n > sizeof c->ic) return -1;
    memcpy(c->ic, pl, (size_t)n);
    c->ic_len = (size_t)n;
    return kex_after_kexinits(c);
}

int next_msg_step(Conn *c, const uint8_t **payload) {
    size_t n;
    int r = recv_packet(c, payload, &n);
    if (r == -2) return 0;
    if (r < 0) return -1;
    switch ((*payload)[0]) {
    case MSG_DISCONNECT: {
        Rd rd;
        rd_init(&rd, *payload + 1, n - 1);
        uint32_t reason = rd_u32(&rd);
        size_t tl = 0;
        const uint8_t *text = rd_str(&rd, &tl);
        char shown[96];
        printable(shown, sizeof shown, rd.bad ? (const uint8_t *)"" : text, rd.bad ? 0 : tl);
        slog("%s: disconnected by the client (%u) %s", c->peer, reason, shown);
        return 0;
    }
    case MSG_IGNORE: case MSG_DEBUG: case MSG_UNIMPLEMENTED:
        return NEXT_SWALLOWED;
    case MSG_KEXINIT:
        return rekey(c, *payload, n) < 0 ? -1 : NEXT_SWALLOWED;
    default:
        return (int)n;
    }
}

int next_msg(Conn *c, const uint8_t **payload) {
    for (;;) {
        int n = next_msg_step(c, payload);
        if (n != NEXT_SWALLOWED) return n;
    }
}
