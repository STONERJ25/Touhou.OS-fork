/*
 * libtls: the record layer, handshake plumbing and the public API. The handshakes themselves are in hello.c, hs13.c and hs12.c.
 */
#include "tlsconn.h"

/* ---------------------------------------------------------------------------------------------------- suites and names */

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

const suite_info *tls_suite_find(unsigned id) {
    for (size_t i = 0; i < TLSC_ARRAY_SIZE(SUITES); i++)
        if (SUITES[i].id == id) return &SUITES[i];
    return NULL;
}

const char *tls_strerror(int err) {
    switch (err) {
    case TLS_OK: return "no error";
    case TLS_E_ARG: return "bad argument";
    case TLS_E_NOMEM: return "out of memory";
    case TLS_E_IO: return "the connection failed or was closed during the handshake";
    case TLS_E_TRUNCATED: return "the connection was closed without close_notify (the data may be incomplete)";
    case TLS_E_PROTOCOL: return "protocol error (malformed or unexpected message)";
    case TLS_E_ALERT: return "the server sent a fatal alert";
    case TLS_E_VERSION: return "no common TLS version (or a downgrade was detected)";
    case TLS_E_SUITE: return "the server selected something we did not offer";
    case TLS_E_DECRYPT: return "a record failed authentication (bad MAC)";
    case TLS_E_HANDSHAKE_SIG: return "the server's handshake signature is wrong";
    case TLS_E_FINISHED: return "the server's Finished message is wrong";
    case TLS_E_ENTROPY: return "the random number generator is not seeded yet (see /proc/random)";
    case TLS_E_CERT_PARSE: return "the server certificate could not be parsed";
    case TLS_E_CERT_UNTRUSTED: return "the certificate was not issued by a trusted authority (unknown CA)";
    case TLS_E_CERT_SELF_SIGNED: return "the certificate is self-signed and not trusted";
    case TLS_E_CERT_EXPIRED: return "the certificate has expired";
    case TLS_E_CERT_NOT_YET_VALID: return "the certificate is not valid yet (check the system clock)";
    case TLS_E_CERT_NAME: return "the certificate is not valid for this host name";
    case TLS_E_CERT_SIGNATURE: return "a signature in the certificate chain is invalid";
    case TLS_E_CERT_USAGE: return "the certificate chain is not allowed to be used this way (CA flag, path length or key usage)";
    case TLS_E_CERT_UNSUPPORTED: return "the certificate chain uses a weak or unsupported algorithm or extension";
    case TLS_E_CERT_EMPTY: return "the server sent no certificate";
    default: return "unknown TLS error";
    }
}

const char *tls_alert_name(int a) {
    switch (a) {
    case 0: return "close_notify";
    case 10: return "unexpected_message";
    case 20: return "bad_record_mac";
    case 22: return "record_overflow";
    case 40: return "handshake_failure";
    case 42: return "bad_certificate";
    case 43: return "unsupported_certificate";
    case 44: return "certificate_revoked";
    case 45: return "certificate_expired";
    case 46: return "certificate_unknown";
    case 47: return "illegal_parameter";
    case 48: return "unknown_ca";
    case 49: return "access_denied";
    case 50: return "decode_error";
    case 51: return "decrypt_error";
    case 70: return "protocol_version";
    case 71: return "insufficient_security";
    case 80: return "internal_error";
    case 86: return "inappropriate_fallback";
    case 90: return "user_canceled";
    case 100: return "no_renegotiation";
    case 109: return "missing_extension";
    case 110: return "unsupported_extension";
    case 112: return "unrecognized_name";
    case 115: return "unknown_psk_identity";
    case 116: return "certificate_required";
    case 120: return "no_application_protocol";
    default: return "unknown_alert";
    }
}

/* ---------------------------------------------------------------------------------------------------- plumbing */

int tls_rand(struct tls_conn *c, void *buf, size_t n) {
    if (c->cfg.random) {
        c->cfg.random(buf, n);
        return 0;
    }
    return tls_os_random(buf, n) == 0 ? 0 : TLS_E_ENTROPY;
}

int64_t tls_now(struct tls_conn *c) { return c->cfg.now ? c->cfg.now : tls_os_time(); }

int tls_random_ready(void) { return tls_os_random_ready(); }

int tls_fail(struct tls_conn *c, int err, int alert) {
    if (c->state == 1 || c->state == 2) {
        if (alert >= 0 && !c->close_notify_sent) tls_send_alert(c, 2, alert);
    }
    c->last_error = err;
    c->state = 4;
    return err;
}

int tls_cipher_keylen(int cipher) { return cipher == CIPHER_AES128 ? 16 : 32; }

void tls_cipher_set(cipher_state *cs, int cipher, const u8 *key, const u8 *iv, size_t ivlen) {
    memset(cs, 0, sizeof *cs);
    cs->cipher = cipher;
    memcpy(cs->iv, iv, ivlen > 12 ? 12 : ivlen);
    if (cipher == CIPHER_CHACHA) memcpy(cs->key, key, 32);
    else tlsc_gcm_init(&cs->gcm, key, (size_t)tls_cipher_keylen(cipher));
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

static int write_all(struct tls_conn *c, const u8 *p, size_t n) {
    while (n) {
        long r = c->io.send(c->io.user, p, n);
        if (r <= 0) return TLS_E_IO;
        if ((size_t)r > n) r = (long)n;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int tls_send_raw(struct tls_conn *c, const u8 *data, size_t len) { return write_all(c, data, len); }

/* Reads exactly n bytes. Returns 0, 1 for end of stream before the first byte, TLS_E_IO otherwise. */
static int read_exact(struct tls_conn *c, u8 *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        long r = c->io.recv(c->io.user, buf + got, n - got);
        if (r < 0) return TLS_E_IO;
        if (r == 0) return got == 0 ? 1 : TLS_E_IO;
        if ((size_t)r > n - got) r = (long)(n - got);
        got += (size_t)r;
    }
    return 0;
}

int tls_send_record(struct tls_conn *c, int type, const u8 *data, size_t len) {
    if (len > TLS_MAX_PLAIN) return TLS_E_ARG;
    u8 *w = c->wbuf;
    cipher_state *cs = &c->wr;
    size_t total;
    if (cs->cipher == CIPHER_NONE) {
        w[0] = (u8)type;
        w[1] = 3;
        w[2] = (type == CT_HANDSHAKE && !c->first_hello_sent && c->version == 0) ? 1 : 3;     /* the very first ClientHello says 0x0301, as everybody does */
        w[3] = (u8)(len >> 8);
        w[4] = (u8)len;
        memcpy(w + 5, data, len);
        total = 5 + len;
    } else if (c->version == 0x0304) {
        size_t inner = len + 1, clen = inner + 16;
        w[0] = CT_APPDATA;
        w[1] = 3;
        w[2] = 3;
        w[3] = (u8)(clen >> 8);
        w[4] = (u8)clen;
        memcpy(w + 5, data, len);
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
        memcpy(w + 5, data, len);
        aead_seal(cs, nonce, aad, 13, w + 5, len, w + 5 + len);
        total = 5 + clen;
    } else {                                                         /* TLS 1.2 AES-GCM: an explicit 8-byte nonce precedes the ciphertext */
        u8 nonce[12], aad[13], s[8];
        put_seq(cs->seq, s);
        memcpy(nonce, cs->iv, 4);
        memcpy(nonce + 4, s, 8);
        memcpy(aad, s, 8);
        aad[8] = (u8)type; aad[9] = 3; aad[10] = 3; aad[11] = (u8)(len >> 8); aad[12] = (u8)len;
        size_t clen = 8 + len + 16;
        w[0] = (u8)type; w[1] = 3; w[2] = 3; w[3] = (u8)(clen >> 8); w[4] = (u8)clen;
        memcpy(w + 5, s, 8);
        memcpy(w + 13, data, len);
        aead_seal(cs, nonce, aad, 13, w + 13, len, w + 13 + len);
        total = 5 + clen;
    }
    if (cs->cipher != CIPHER_NONE) cs->seq++;
    return write_all(c, w, total);
}

int tls_send_alert(struct tls_conn *c, int level, int desc) {
    u8 a[2] = { (u8)level, (u8)desc };
    return tls_send_record(c, CT_ALERT, a, 2);
}

int tls_recv_record(struct tls_conn *c, int *type, u8 **data, size_t *len) {
    u8 *r = c->rbuf;
    int rc = read_exact(c, r, 5);
    if (rc != 0) return rc;
    int t = r[0];
    size_t length = (size_t)r[3] << 8 | r[4];
    if (r[1] != 3 || (t != CT_CCS && t != CT_ALERT && t != CT_HANDSHAKE && t != CT_APPDATA)) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
    size_t max = c->rd.cipher == CIPHER_NONE ? TLS_MAX_PLAIN : (c->version == 0x0304 ? TLS_MAX_PLAIN + 256 : TLS_MAX_PLAIN + 2048);
    if (length > max) return tls_fail(c, TLS_E_PROTOCOL, AL_RECORD_OVERFLOW);
    rc = read_exact(c, r + 5, length);
    if (rc != 0) return rc > 0 ? TLS_E_IO : rc;
    cipher_state *cs = &c->rd;
    if (cs->cipher == CIPHER_NONE) {
        *type = t;
        *data = r + 5;
        *len = length;
        return 0;
    }
    if (c->version == 0x0304) {
        if (t == CT_CCS) {                                           /* the compatibility-mode dummy: handed up to be dropped */
            *type = t;
            *data = r + 5;
            *len = length;
            return 0;
        }
        if (t != CT_APPDATA || length < 17) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        u8 nonce[12];
        xor_nonce(cs, nonce);
        size_t n = length - 16;
        if (aead_open(cs, nonce, r, 5, r + 5, n, r + 5 + n) != 0) return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC);
        cs->seq++;
        while (n > 0 && r[5 + n - 1] == 0) n--;                      /* padding */
        if (n == 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        *type = r[5 + n - 1];
        n--;
        if (n > TLS_MAX_PLAIN) return tls_fail(c, TLS_E_PROTOCOL, AL_RECORD_OVERFLOW);
        *data = r + 5;
        *len = n;
        return 0;
    }
    u8 aad[13], s[8], nonce[12];
    put_seq(cs->seq, s);
    memcpy(aad, s, 8);
    aad[8] = (u8)t; aad[9] = 3; aad[10] = 3;
    if (cs->cipher == CIPHER_CHACHA) {
        if (length < 16) return tls_fail(c, TLS_E_PROTOCOL, AL_BAD_RECORD_MAC);
        size_t n = length - 16;
        aad[11] = (u8)(n >> 8); aad[12] = (u8)n;
        xor_nonce(cs, nonce);
        if (aead_open(cs, nonce, aad, 13, r + 5, n, r + 5 + n) != 0) return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC);
        cs->seq++;
        *type = t;
        *data = r + 5;
        *len = n;
        return 0;
    }
    if (length < 8 + 16) return tls_fail(c, TLS_E_PROTOCOL, AL_BAD_RECORD_MAC);
    size_t n = length - 8 - 16;
    aad[11] = (u8)(n >> 8); aad[12] = (u8)n;
    memcpy(nonce, cs->iv, 4);
    memcpy(nonce + 4, r + 5, 8);
    if (aead_open(cs, nonce, aad, 13, r + 13, n, r + 13 + n) != 0) return tls_fail(c, TLS_E_DECRYPT, AL_BAD_RECORD_MAC);
    cs->seq++;
    *type = t;
    *data = r + 13;
    *len = n;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- transcript and handshake messages */

void tls_transcript_add(struct tls_conn *c, const u8 *msg, size_t len) {
    tlsc_hash_update(&c->th256, msg, len);
    tlsc_hash_update(&c->th384, msg, len);
}

void tls_transcript_hash(struct tls_conn *c, int alg, u8 *out) {
    tlsc_hash t = alg == TLSC_SHA384 ? c->th384 : c->th256;
    tlsc_hash_final(&t, out);
}

int tls_send_handshake(struct tls_conn *c, const u8 *msg, size_t len) {
    tls_transcript_add(c, msg, len);
    while (len) {
        size_t take = len > TLS_MAX_PLAIN ? TLS_MAX_PLAIN : len;
        int rc = tls_send_record(c, CT_HANDSHAKE, msg, take);
        if (rc) return rc;
        msg += take;
        len -= take;
    }
    c->first_hello_sent = 1;
    return 0;
}

static int handle_alert(struct tls_conn *c, const u8 *d, size_t n) {
    if (n != 2) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
    int level = d[0], desc = d[1];
    if (desc == AL_CLOSE_NOTIFY) {
        c->close_notify_rx = 1;
        return 0;
    }
    if (desc == 90) return 0;                                        /* user_canceled: harmless */
    if (level == 1 && c->version != 0x0304) return 0;                /* TLS 1.2 warnings (unrecognized_name, no_renegotiation) */
    c->alert_rx = desc;
    c->last_error = TLS_E_ALERT;
    c->state = 4;
    return TLS_E_ALERT;
}

/* Appends handshake bytes; returns the next complete message (header included) or reads more records until there is one. */
int tls_get_handshake(struct tls_conn *c, u8 *type, const u8 **msg, size_t *len) {
    for (;;) {
        if (c->hs_len >= 4) {
            size_t mlen = (size_t)c->hs[1] << 16 | (size_t)c->hs[2] << 8 | c->hs[3];
            if (mlen > TLS_MAX_HS) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
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
        int rc = tls_recv_record(c, &t, &d, &n);
        if (rc == 1) return tls_fail(c, c->state == 1 ? TLS_E_IO : TLS_E_TRUNCATED, -1);
        if (rc < 0) return tls_fail(c, rc, -1);
        switch (t) {
        case CT_HANDSHAKE:
            if (c->hs_len + n > c->hs_cap) {
                size_t cap = c->hs_cap ? c->hs_cap : 4096;
                while (cap < c->hs_len + n) cap *= 2;
                u8 *nb = realloc(c->hs, cap);
                if (!nb) return tls_fail(c, TLS_E_NOMEM, AL_INTERNAL_ERROR);
                c->hs = nb;
                c->hs_cap = cap;
            }
            memcpy(c->hs + c->hs_len, d, n);
            c->hs_len += n;
            break;
        case CT_CCS:                                                 /* TLS 1.3 compatibility mode sends a dummy; TLS 1.2 reads its CCS itself */
            if (c->version != 0x0304 || n != 1 || d[0] != 1) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
            break;
        case CT_ALERT:
            rc = handle_alert(c, d, n);
            if (rc == 0) return tls_fail(c, c->state == 1 ? TLS_E_IO : TLS_E_TRUNCATED, -1);       /* close_notify in the middle of a handshake message */
            return rc;
        default:
            return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        }
    }
}

void tls_hs_consume(struct tls_conn *c, size_t len) {
    if (len >= c->hs_len) {
        c->hs_len = 0;
        return;
    }
    memmove(c->hs, c->hs + len, c->hs_len - len);
    c->hs_len -= len;
}

/* ---------------------------------------------------------------------------------------------------- the certificate chain */

static int alert_for_cert_error(int err) {
    switch (err) {
    case TLS_E_CERT_UNTRUSTED: case TLS_E_CERT_SELF_SIGNED: return AL_UNKNOWN_CA;
    case TLS_E_CERT_EXPIRED: case TLS_E_CERT_NOT_YET_VALID: return AL_CERTIFICATE_EXPIRED;
    case TLS_E_CERT_UNSUPPORTED: return AL_UNSUPPORTED_CERTIFICATE;
    case TLS_E_CERT_PARSE: return AL_DECODE_ERROR;
    default: return AL_BAD_CERTIFICATE;
    }
}

/* cert_list: the entries of the Certificate message, each "cert_data<3>" followed, in TLS 1.3 only, by "extensions<2>". */
int tls_verify_peer(struct tls_conn *c, const u8 *cert_list, size_t cert_list_len, int tls13) {
    /* the parsed certificates point into this copy, which outlives the handshake message it came from */
    free(c->cert_blob);
    c->cert_blob = malloc(cert_list_len ? cert_list_len : 1);
    if (!c->cert_blob) return tls_fail(c, TLS_E_NOMEM, AL_INTERNAL_ERROR);
    memcpy(c->cert_blob, cert_list, cert_list_len);
    c->cert_blob_len = cert_list_len;
    rd_t r = { c->cert_blob, cert_list_len, 0 };
    c->chain_n = 0;
    while (r.n > 0) {
        unsigned clen = rd24(&r);
        const u8 *der = rdn(&r, clen);
        if (r.err || clen == 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        if (tls13) {
            unsigned elen = rd16(&r);
            rdn(&r, elen);
            if (r.err) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR);
        }
        if (c->chain_n >= TLS_MAX_CERTS) continue;                   /* the rest cannot be part of a path we are willing to build */
        int rc = x509_parse(&c->chain[c->chain_n], der, clen);
        if (rc != 0) {
            if (c->chain_n == 0) return tls_fail(c, TLS_E_CERT_PARSE, AL_BAD_CERTIFICATE);      /* without the leaf there is nothing to go on */
            continue;                                                /* junk after the leaf: ignore it, the path may not need it */
        }
        c->chain_n++;
    }
    if (c->chain_n == 0) return tls_fail(c, TLS_E_CERT_EMPTY, AL_DECODE_ERROR);
    const x509_cert *leaf = &c->chain[0];
    x509_name_string(leaf->subject, c->info.subject, sizeof c->info.subject);
    x509_name_string(leaf->issuer, c->info.issuer, sizeof c->info.issuer);
    c->info.not_before = leaf->not_before;
    c->info.not_after = leaf->not_after;
    c->info.chain_length = (unsigned)c->chain_n;
    tlsc_hash_once(TLSC_SHA256, leaf->der, leaf->der_len, c->info.fingerprint);
    int rc = x509_verify_chain(c->cfg.ctx, c->chain, c->chain_n, c->sni[0] ? c->sni : NULL, tls_now(c));
    if (rc == 0) {
        c->info.verified = 1;
        return 0;
    }
    c->info.verify_error = rc;
    if (c->cfg.flags & TLS_F_NO_VERIFY) return 0;
    return tls_fail(c, rc, alert_for_cert_error(rc));
}

int tls_check_signature(struct tls_conn *c, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen) {
    if (x509_verify_handshake_signature(&c->chain[0], scheme, tls13, msg, msglen, sig, siglen) != 0) return tls_fail(c, TLS_E_HANDSHAKE_SIG, AL_DECRYPT_ERROR);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- public API */

tls_conn *tls_new(const tls_io *io, const tls_config *cfg) {
    if (!io || !io->send || !io->recv || !cfg) return NULL;
    struct tls_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->io = *io;
    c->cfg = *cfg;
    c->rbuf = malloc(TLS_REC_MAX);
    c->wbuf = malloc(TLS_REC_MAX);
    if (!c->rbuf || !c->wbuf) {
        free(c->rbuf);
        free(c->wbuf);
        free(c);
        return NULL;
    }
    if (cfg->server_name) {
        size_t n = strlen(cfg->server_name);
        if (n >= sizeof c->sni) { free(c->rbuf); free(c->wbuf); free(c); return NULL; }
        memcpy(c->sni, cfg->server_name, n + 1);
        if (n && c->sni[n - 1] == '.') c->sni[n - 1] = 0;
    }
    if (cfg->alpn) {
        size_t n = strlen(cfg->alpn);
        if (n >= sizeof c->alpn_offer) { free(c->rbuf); free(c->wbuf); free(c); return NULL; }
        memcpy(c->alpn_offer, cfg->alpn, n + 1);
    }
    c->cfg.server_name = c->sni[0] ? c->sni : NULL;
    c->cfg.alpn = c->alpn_offer[0] ? c->alpn_offer : NULL;
    tlsc_hash_init(&c->th256, TLSC_SHA256);
    tlsc_hash_init(&c->th384, TLSC_SHA384);
    c->alert_rx = -1;
    return c;
}

static void wipe_handshake_secrets(struct tls_conn *c) {
    tlsc_wipe(c->secret_hs, sizeof c->secret_hs);
    tlsc_wipe(c->secret_master, sizeof c->secret_master);
    tlsc_wipe(c->secret_c_hs, sizeof c->secret_c_hs);
    tlsc_wipe(c->secret_s_hs, sizeof c->secret_s_hs);
    tlsc_wipe(c->master, sizeof c->master);
    tlsc_wipe(c->x25519_priv, sizeof c->x25519_priv);
    tlsc_wipe(c->ec_priv, sizeof c->ec_priv);
    free(c->cert_blob);
    c->cert_blob = NULL;
    c->chain_n = 0;
}

static int handshake_inner(struct tls_conn *c) {
    int rc = tls_send_client_hello(c, 0);
    if (rc) return rc;
    unsigned retry = 0;
    for (;;) {
        u8 type;
        const u8 *msg;
        size_t len;
        rc = tls_get_handshake(c, &type, &msg, &len);
        if (rc) return rc;
        if (type != HS_SERVER_HELLO) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        rc = tls_process_server_hello(c, msg, len, &retry);         /* consumes the message */
        if (rc < 0) return rc;
        if (rc == 0) break;
        rc = tls_send_client_hello(c, retry);                        /* a HelloRetryRequest: the second ClientHello names the group the server wants */
        if (rc) return rc;
    }
    return c->version == 0x0304 ? tls13_client(c) : tls12_client(c);
}

int tls_handshake(tls_conn *c) {
    if (!c) return TLS_E_ARG;
    if (c->state != 0) return c->state == 2 ? 0 : (c->last_error ? c->last_error : TLS_E_ARG);
    if (!(c->cfg.flags & TLS_F_NO_VERIFY) && !c->sni[0]) return TLS_E_ARG;          /* nothing to check the certificate against */
    if (!c->cfg.random && !(c->cfg.flags & TLS_F_WEAK_RANDOM) && tls_os_random_ready() == 0) {
        c->last_error = TLS_E_ENTROPY;
        c->state = 4;
        return TLS_E_ENTROPY;
    }
    if (c->cfg.flags & TLS_F_NO_AESNI) tlsc_aes_force_software(1);
    c->state = 1;
    int rc = handshake_inner(c);
    if (c->cfg.flags & TLS_F_NO_AESNI) tlsc_aes_force_software(0);
    if (rc) {
        if (c->state != 4) {
            c->state = 4;
            c->last_error = rc;
        }
        tlsc_wipe(c->secret_hs, sizeof c->secret_hs);
        tlsc_wipe(c->master, sizeof c->master);
        return rc;
    }
    wipe_handshake_secrets(c);
    free(c->hs);
    c->hs = NULL;
    c->hs_len = c->hs_cap = 0;
    c->state = 2;
    return 0;
}

const tls_info *tls_get_info(const tls_conn *c) { return c ? &c->info : NULL; }
int tls_alert_received(const tls_conn *c) { return c ? c->alert_rx : -1; }
int tls_last_error(const tls_conn *c) { return c ? c->last_error : TLS_E_ARG; }

/* Messages that may follow the handshake. */
static int post_handshake(struct tls_conn *c) {
    for (;;) {
        if (c->hs_len < 4) return 0;
        size_t mlen = (size_t)c->hs[1] << 16 | (size_t)c->hs[2] << 8 | c->hs[3];
        if (mlen > TLS_MAX_HS) return tls_fail(c, TLS_E_PROTOCOL, AL_ILLEGAL_PARAMETER);
        if (c->hs_len < 4 + mlen) return 0;
        int rc = 0;
        if (c->version == 0x0304) {
            rc = tls13_handle_post_handshake(c, c->hs, 4 + mlen);
        } else if (c->hs[0] == 0) {                                  /* HelloRequest: we never renegotiate */
            tls_send_alert(c, 1, 100);
        } else {
            rc = tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        }
        if (rc) return rc;
        tls_hs_consume(c, 4 + mlen);
    }
}

long tls_read(tls_conn *c, void *buf, size_t n) {
    if (!c || (c->state != 2 && c->state != 3 && c->state != 4)) return TLS_E_ARG;
    if (n == 0) return 0;
    for (;;) {
        if (c->app_len) {
            size_t k = c->app_len < n ? c->app_len : n;
            memcpy(buf, c->app_p, k);
            c->app_p += k;
            c->app_len -= k;
            return (long)k;
        }
        if (c->state == 4) return c->last_error;
        if (c->close_notify_rx) return 0;
        if (c->state == 3) return c->last_error ? c->last_error : 0;
        int t;
        u8 *d;
        size_t len;
        int rc = tls_recv_record(c, &t, &d, &len);
        if (rc == 1) {
            c->state = 3;
            c->last_error = TLS_E_TRUNCATED;
            return TLS_E_TRUNCATED;
        }
        if (rc < 0) return rc;
        switch (t) {
        case CT_APPDATA:
            c->app_p = d;
            c->app_len = len;
            break;
        case CT_HANDSHAKE:
            if (c->hs_len + len > c->hs_cap) {
                size_t cap = c->hs_cap ? c->hs_cap : 1024;
                while (cap < c->hs_len + len) cap *= 2;
                u8 *nb = realloc(c->hs, cap);
                if (!nb) return tls_fail(c, TLS_E_NOMEM, AL_INTERNAL_ERROR);
                c->hs = nb;
                c->hs_cap = cap;
            }
            memcpy(c->hs + c->hs_len, d, len);
            c->hs_len += len;
            rc = post_handshake(c);
            if (rc) return rc;
            break;
        case CT_ALERT:
            rc = handle_alert(c, d, len);
            if (rc < 0) return rc;
            break;
        case CT_CCS:
            return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        default:
            return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE);
        }
    }
}

long tls_write(tls_conn *c, const void *buf, size_t n) {
    if (!c || c->state != 2 || c->close_notify_sent) return c && c->last_error ? c->last_error : TLS_E_ARG;
    const u8 *p = buf;
    size_t left = n;
    while (left) {
        size_t take = left > TLS_MAX_PLAIN ? TLS_MAX_PLAIN : left;
        int rc = tls_send_record(c, CT_APPDATA, p, take);
        if (rc) {
            c->last_error = rc;
            c->state = 4;
            return rc;
        }
        p += take;
        left -= take;
    }
    return (long)n;
}

int tls_shutdown(tls_conn *c) {
    if (!c || c->close_notify_sent) return 0;
    if (c->state != 2) return 0;
    c->close_notify_sent = 1;
    return tls_send_alert(c, 1, AL_CLOSE_NOTIFY);
}

void tls_free(tls_conn *c) {
    if (!c) return;
    free(c->rbuf);
    free(c->wbuf);
    free(c->hs);
    free(c->cert_blob);
    tlsc_wipe(c, sizeof *c);
    free(c);
}

void tls_close(tls_conn *c) {
    tls_shutdown(c);
    tls_free(c);
}
