/*
 * The record layer (RFC 8446 section 5, RFC 5246 section 6.2, RFC 7905 for ChaCha20-Poly1305 in TLS 1.2), the stream of
 * handshake messages on top of it, alerts, the ChangeCipherSpec rules and the transcript hash.
 *
 * Nothing here waits: input is taken with MSG_DONTWAIT as far as it goes (TLS_WANT_READ when a record is not complete yet), output
 * is queued and written as far as the socket takes it (TLS_WANT_WRITE). A record's plaintext stays where it was decrypted, in the
 * input buffer, until the next record is asked for.
 */
#include "conn.h"

static const suite_info SUITES[] = {
    { 0x1301, "TLS_AES_128_GCM_SHA256",                         0x0304, CIPHER_AES128, TLSC_SHA256, AUTH_ANY },
    { 0x1302, "TLS_AES_256_GCM_SHA384",                         0x0304, CIPHER_AES256, TLSC_SHA384, AUTH_ANY },
    { 0x1303, "TLS_CHACHA20_POLY1305_SHA256",                   0x0304, CIPHER_CHACHA, TLSC_SHA256, AUTH_ANY },
    { 0xc02b, "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",        0x0303, CIPHER_AES128, TLSC_SHA256, AUTH_ECDSA },
    { 0xc02c, "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",        0x0303, CIPHER_AES256, TLSC_SHA384, AUTH_ECDSA },
    { 0xcca9, "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",  0x0303, CIPHER_CHACHA, TLSC_SHA256, AUTH_ECDSA },
    { 0xc02f, "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",          0x0303, CIPHER_AES128, TLSC_SHA256, AUTH_RSA },
    { 0xc030, "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",          0x0303, CIPHER_AES256, TLSC_SHA384, AUTH_RSA },
    { 0xcca8, "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",    0x0303, CIPHER_CHACHA, TLSC_SHA256, AUTH_RSA },
};

const suite_info *suite_find(unsigned id) {
    for (size_t i = 0; i < TLSC_ARRAY_SIZE(SUITES); i++)
        if (SUITES[i].id == id) return &SUITES[i];
    return NULL;
}

int tls_rand(TlsConn *c, void *buf, size_t n) {
    if (c->random) {
        c->random(c->random_ctx, buf, n);
        return 0;
    }
    return tls_os_random(buf, n);
}

int cipher_keylen(int cipher) { return cipher == CIPHER_AES128 ? 16 : 32; }

void rec_set_keys(cipher_state *cs, int cipher, const u8 *key, const u8 *iv, size_t ivlen) {
    tlsc_wipe(cs, sizeof *cs);
    cs->cipher = cipher;
    memcpy(cs->iv, iv, ivlen > 12 ? 12 : ivlen);
    if (cipher == CIPHER_CHACHA) memcpy(cs->key, key, 32);
    else tlsc_gcm_init(&cs->gcm, key, (size_t)cipher_keylen(cipher));
}

static void put_seq(u64 seq, u8 out[8]) {
    for (int i = 0; i < 8; i++) out[i] = (u8)(seq >> (56 - 8 * i));
}

/* TLS 1.3 and TLS 1.2 ChaCha20: the IV with the sequence number xor-ed into its last eight bytes */
static void xor_nonce(const cipher_state *cs, u8 nonce[12]) {
    u8 s[8];
    put_seq(cs->seq, s);
    memcpy(nonce, cs->iv, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] ^= s[i];
}

static void aead_seal(const cipher_state *cs, const u8 nonce[12], const u8 *aad, size_t aadlen, u8 *buf, size_t n, u8 *tag) {
    if (cs->cipher == CIPHER_CHACHA) tlsc_chacha20poly1305_seal(cs->key, nonce, aad, aadlen, buf, n, buf, tag);
    else tlsc_gcm_seal(&cs->gcm, nonce, aad, aadlen, buf, n, buf, tag);
}

static int aead_open(const cipher_state *cs, const u8 nonce[12], const u8 *aad, size_t aadlen, u8 *buf, size_t n, const u8 *tag) {
    if (cs->cipher == CIPHER_CHACHA) return tlsc_chacha20poly1305_open(cs->key, nonce, aad, aadlen, buf, n, buf, tag);
    return tlsc_gcm_open(&cs->gcm, nonce, aad, aadlen, buf, n, buf, tag);
}

/* ---------------------------------------------------------------------------------------------------- output */

static void out_compact(TlsConn *c) {
    if (c->out_start == 0) return;
    memmove(c->out, c->out + c->out_start, c->out_end - c->out_start);
    c->out_end -= c->out_start;
    c->out_start = 0;
}

size_t rec_room(const TlsConn *c) {
    size_t used = c->out_end - c->out_start, overhead = 5 + 8 + 1 + 16;
    if (used + overhead >= TLS_OUTBUF) return 0;
    size_t room = TLS_OUTBUF - used - overhead;
    return room > TLS_MAX_PLAIN ? TLS_MAX_PLAIN : room;
}

int rec_queue_raw(TlsConn *c, const u8 *data, size_t len) {
    out_compact(c);
    if (c->out_end + len > TLS_OUTBUF) return TLS_E_ARG;
    memcpy(c->out + c->out_end, data, len);
    c->out_end += len;
    return 0;
}

int rec_queue(TlsConn *c, int type, const u8 *data, size_t len) {
    if (len > TLS_MAX_PLAIN) return TLS_E_ARG;
    out_compact(c);
    if (c->out_end + 5 + 8 + len + 1 + 16 > TLS_OUTBUF) return TLS_E_ARG;             /* the callers make room first */
    u8 *w = c->out + c->out_end;
    cipher_state *cs = &c->wr;
    size_t total;
    if (cs->cipher == CIPHER_NONE) {
        w[0] = (u8)type;
        w[1] = 3;
        w[2] = c->legacy_record_version ? 1 : 3;
        w[3] = (u8)(len >> 8);
        w[4] = (u8)len;
        memmove(w + 5, data, len);
        total = 5 + len;
    } else if (c->version == 0x0304) {                               /* TLSInnerPlaintext: content, type, no padding */
        size_t inner = len + 1, clen = inner + 16;
        w[0] = CT_APPDATA;
        w[1] = 3;
        w[2] = 3;
        w[3] = (u8)(clen >> 8);
        w[4] = (u8)clen;
        memmove(w + 5, data, len);
        w[5 + len] = (u8)type;
        u8 nonce[12];
        xor_nonce(cs, nonce);
        aead_seal(cs, nonce, w, 5, w + 5, inner, w + 5 + inner);
        total = 5 + clen;
    } else if (cs->cipher == CIPHER_CHACHA) {                        /* TLS 1.2, RFC 7905 */
        u8 nonce[12], aad[13], s[8];
        xor_nonce(cs, nonce);
        put_seq(cs->seq, s);
        memcpy(aad, s, 8);
        aad[8] = (u8)type; aad[9] = 3; aad[10] = 3; aad[11] = (u8)(len >> 8); aad[12] = (u8)len;
        size_t clen = len + 16;
        w[0] = (u8)type; w[1] = 3; w[2] = 3; w[3] = (u8)(clen >> 8); w[4] = (u8)clen;
        memmove(w + 5, data, len);
        aead_seal(cs, nonce, aad, 13, w + 5, len, w + 5 + len);
        total = 5 + clen;
    } else {                                                         /* TLS 1.2 AES-GCM (RFC 5288): an explicit 8-byte nonce first */
        u8 nonce[12], aad[13], s[8];
        put_seq(cs->seq, s);
        memcpy(nonce, cs->iv, 4);
        memcpy(nonce + 4, s, 8);
        memcpy(aad, s, 8);
        aad[8] = (u8)type; aad[9] = 3; aad[10] = 3; aad[11] = (u8)(len >> 8); aad[12] = (u8)len;
        size_t clen = 8 + len + 16;
        w[0] = (u8)type; w[1] = 3; w[2] = 3; w[3] = (u8)(clen >> 8); w[4] = (u8)clen;
        memmove(w + 13, data, len);
        memcpy(w + 5, s, 8);
        aead_seal(cs, nonce, aad, 13, w + 13, len, w + 13 + len);
        total = 5 + clen;
    }
    if (cs->cipher != CIPHER_NONE && ++cs->seq == 0) return TLS_E_PROTOCOL;   /* 2^64 records: never, but never wrap a nonce */
    c->out_end += total;
    return 0;
}

int rec_flush(TlsConn *c) {
    while (c->out_start < c->out_end) {
        long n = tls_os_send(c->fd, c->out + c->out_start, c->out_end - c->out_start);
        if (n == TLS_WANT_WRITE) return TLS_WANT_WRITE;
        if (n <= 0) {
            c->out_start = c->out_end = 0;                           /* nothing more can go out */
            return tls_fail(c, TLS_E_IO, -1, "writing to the socket failed (the server reset the connection?)");
        }
        c->out_start += (size_t)n;
    }
    c->out_start = c->out_end = 0;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- input */

static int decrypt(TlsConn *c, u8 *r, int t, size_t length, int *type, u8 **data, size_t *len) {
    cipher_state *cs = &c->rd;
    if (cs->cipher == CIPHER_NONE) {
        *type = t;
        *data = r + 5;
        *len = length;
        return 0;
    }
    if (c->version == 0x0304) {
        if (t == CT_CCS) {                                           /* never protected: handed up, the caller decides */
            *type = t;
            *data = r + 5;
            *len = length;
            return 0;
        }
        if (t != CT_APPDATA) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a TLS 1.3 record of outer type %d", t);
        if (length < 17) return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC, "a protected record too short to hold a tag");
        u8 nonce[12];
        xor_nonce(cs, nonce);
        size_t n = length - 16;
        if (aead_open(cs, nonce, r, 5, r + 5, n, r + 5 + n) != 0)
            return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC, "a record from the server failed authentication (it was changed on the way, or the keys disagree)");
        cs->seq++;
        while (n > 0 && r[5 + n - 1] == 0) n--;                      /* padding */
        if (n == 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a protected record without a content type");
        int inner = r[5 + n - 1];
        n--;
        if (inner != CT_ALERT && inner != CT_HANDSHAKE && inner != CT_APPDATA)
            return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a protected record of type %d", inner);
        if (n > TLS_MAX_PLAIN) return tls_fail(c, TLS_E_PROTOCOL, AL_RECORD_OVERFLOW, "a record of more than 16 KiB");
        *type = inner;
        *data = r + 5;
        *len = n;
        return 0;
    }
    u8 aad[13], s[8], nonce[12];
    put_seq(cs->seq, s);
    memcpy(aad, s, 8);
    aad[8] = (u8)t; aad[9] = 3; aad[10] = 3;
    size_t n;
    u8 *body;
    if (cs->cipher == CIPHER_CHACHA) {
        if (length < 16) return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC, "a protected record too short to hold a tag");
        n = length - 16;
        body = r + 5;
        xor_nonce(cs, nonce);
    } else {
        if (length < 8 + 16) return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC, "a protected record too short to hold a tag");
        n = length - 8 - 16;
        body = r + 13;
        memcpy(nonce, cs->iv, 4);
        memcpy(nonce + 4, r + 5, 8);
    }
    aad[11] = (u8)(n >> 8); aad[12] = (u8)n;
    if (aead_open(cs, nonce, aad, 13, body, n, body + n) != 0)
        return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC, "a record from the server failed authentication (it was changed on the way, or the keys disagree)");
    cs->seq++;
    if (n > TLS_MAX_PLAIN) return tls_fail(c, TLS_E_PROTOCOL, AL_RECORD_OVERFLOW, "a record of more than 16 KiB");
    *type = t;
    *data = body;
    *len = n;
    return 0;
}

static int looks_like_text(const u8 *p, size_t n) {
    int printable = 0;
    for (size_t i = 0; i < n; i++) printable += (p[i] >= 0x20 && p[i] < 0x7f) || p[i] == '\r' || p[i] == '\n';
    return n >= 4 && (size_t)printable == n;
}

int rec_next(TlsConn *c, int *type, u8 **data, size_t *len) {
    for (;;) {
        size_t avail = c->in_end - c->in_start;
        if (avail >= 5) {
            u8 *r = c->in + c->in_start;
            int t = r[0];
            size_t length = (size_t)r[3] << 8 | r[4];
            if (r[1] != 3 || t < CT_CCS || t > CT_APPDATA) {
                if (c->first_record) {
                    char peek[24];
                    size_t k = avail < sizeof peek - 1 ? avail : sizeof peek - 1;
                    for (size_t i = 0; i < k; i++) peek[i] = r[i] >= 0x20 && r[i] < 0x7f ? (char)r[i] : '.';
                    peek[k] = 0;
                    if (looks_like_text(r, avail < 8 ? avail : 8))
                        return tls_fail(c, TLS_E_PLAINTEXT, -1, "the server did not answer in TLS; it sent \"%s\" (is this a plain HTTP port?)", peek);
                    return tls_fail(c, TLS_E_PLAINTEXT, -1, "the server's answer is not TLS");
                }
                return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a record with type %d, version %d.%d", t, r[1], r[2]);
            }
            size_t max = c->rd.cipher == CIPHER_NONE ? TLS_MAX_PLAIN : (c->version == 0x0304 ? TLS_MAX_PLAIN + 256 : TLS_MAX_PLAIN + 2048);
            if (c->version == 0x0304 && t == CT_CCS) max = TLS_MAX_PLAIN;
            if (length > max) return tls_fail(c, TLS_E_PROTOCOL, AL_RECORD_OVERFLOW, "a record of %zu bytes", length);
            if (avail >= 5 + length) {
                c->in_start += 5 + length;
                c->first_record = 0;
                return decrypt(c, r, t, length, type, data, len);
            }
        }
        if (c->eof) {
            if (avail == 0) return TLS_E_EOF;
            return tls_fail(c, TLS_E_TRUNCATED, -1, "the connection ended in the middle of a record");
        }
        if (c->in_start == c->in_end) c->in_start = c->in_end = 0;
        else if (TLS_INBUF - c->in_end < TLS_REC_IN_MAX) {
            memmove(c->in, c->in + c->in_start, avail);
            c->in_start = 0;
            c->in_end = avail;
        }
        long n = tls_os_recv(c->fd, c->in + c->in_end, TLS_INBUF - c->in_end);
        if (n == TLS_WANT_READ) return TLS_WANT_READ;
        if (n < 0) {
            c->eof = 1;
            return tls_fail(c, TLS_E_IO, -1, "reading from the socket failed (the server reset the connection?)");
        }
        if (n == 0) c->eof = 1;
        else c->in_end += (size_t)n;
    }
}

/* ---------------------------------------------------------------------------------------------------- transcript and messages */

void transcript_add(TlsConn *c, const u8 *msg, size_t len) {
    tlsc_hash_update(&c->th256, msg, len);
    tlsc_hash_update(&c->th384, msg, len);
}

void transcript_hash(TlsConn *c, int alg, u8 *out) {
    tlsc_hash t = alg == TLSC_SHA384 ? c->th384 : c->th256;
    tlsc_hash_final(&t, out);
}

int hs_send(TlsConn *c, const u8 *msg, size_t len) {
    transcript_add(c, msg, len);
    while (len) {
        size_t take = len > TLS_MAX_PLAIN ? TLS_MAX_PLAIN : len;
        if (rec_room(c) < take) {
            int rc = rec_flush(c);                                   /* a long flight (never with our sizes): make room */
            if (rc) return rc == TLS_WANT_WRITE ? tls_fail(c, TLS_E_ARG, -1, "a handshake flight that does not fit") : rc;
        }
        int rc = rec_queue(c, CT_HANDSHAKE, msg, take);
        if (rc) return tls_fail(c, rc, AL_INTERNAL_ERROR, "could not queue a handshake record");
        msg += take;
        len -= take;
    }
    return 0;
}

int hs_append(TlsConn *c, const u8 *d, size_t n) {
    if (c->hs_len + n > c->hs_cap) {
        size_t cap = c->hs_cap ? c->hs_cap : 4096;
        while (cap < c->hs_len + n) cap *= 2;
        if (cap > TLS_MAX_HS + 4 + TLS_MAX_PLAIN) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake data beyond any message size accepted");
        u8 *nb = realloc(c->hs, cap);
        if (!nb) return tls_fail(c, TLS_E_NOMEM, AL_INTERNAL_ERROR, "out of memory for a handshake message");
        c->hs = nb;
        c->hs_cap = cap;
    }
    memcpy(c->hs + c->hs_len, d, n);
    c->hs_len += n;
    return 0;
}

void hs_consume(TlsConn *c, size_t len) {
    if (len >= c->hs_len) {
        c->hs_len = 0;
        return;
    }
    memmove(c->hs, c->hs + len, c->hs_len - len);
    c->hs_len -= len;
}

int handle_alert(TlsConn *c, const u8 *d, size_t n) {
    if (n != 2) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed alert");
    int level = d[0], desc = d[1];
    if (desc == AL_CLOSE_NOTIFY) {
        c->rx_closed = 1;
        return 1;
    }
    if (desc == AL_USER_CANCELED || (c->version != 0x0304 && level == 1)) {   /* harmless; TLS 1.2 warnings (no_renegotiation, ...) */
        if (++c->idle > TLS_MAX_IDLE) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "too many warning alerts");
        return 0;
    }
    c->info.alert_received = desc;
    return tls_fail(c, TLS_E_ALERT, -1, "the server sent a fatal alert: %s (%d)", tls_alert_name(desc), desc);
}

/* The dummy ChangeCipherSpec of TLS 1.3 (dropped between the first ClientHello and the server's Finished, RFC 8446 5) and the
 * real one of TLS 1.2 (it switches the server's side to the new keys). */
static int ccs_record(TlsConn *c, const u8 *d, size_t n) {
    if (n != 1 || d[0] != 1) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a malformed ChangeCipherSpec");
    if (c->version == 0x0304 && c->state != ST_CONNECTED) {
        if (++c->idle > TLS_MAX_IDLE) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "too many ChangeCipherSpec records");
        return 0;
    }
    if (c->version == 0x0303 && c->state == ST12_WAIT_CCS) {
        if (c->hs_len != 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "ChangeCipherSpec in the middle of a handshake message");
        return hs12_ccs(c);
    }
    return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "an unexpected ChangeCipherSpec");
}

int hs_next(TlsConn *c, u8 *type, const u8 **msg, size_t *len) {
    for (;;) {
        if (c->hs_len >= 4) {
            size_t mlen = (size_t)c->hs[1] << 16 | (size_t)c->hs[2] << 8 | c->hs[3];
            if (mlen > TLS_MAX_HS) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a handshake message of %zu bytes", mlen);
            if (c->hs_len >= 4 + mlen) {
                *type = c->hs[0];
                *msg = c->hs;
                *len = 4 + mlen;
                return 0;
            }
        }
        int t;
        u8 *d;
        size_t n;
        int rc = rec_next(c, &t, &d, &n);
        if (rc == TLS_E_EOF) return tls_fail(c, TLS_E_EOF, -1, "the server closed the connection during the handshake");
        if (rc) return rc;
        switch (t) {
        case CT_HANDSHAKE:
            if (n == 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "an empty handshake record");
            if ((rc = hs_append(c, d, n))) return rc;
            c->idle = 0;
            break;
        case CT_CCS:
            if ((rc = ccs_record(c, d, n))) return rc;
            break;
        case CT_ALERT:
            rc = handle_alert(c, d, n);
            if (rc == 1) return tls_fail(c, TLS_E_EOF, -1, "the server closed the connection (close_notify) during the handshake");
            if (rc < 0) return rc;
            break;
        default:
            return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "application data during the handshake");
        }
    }
}
