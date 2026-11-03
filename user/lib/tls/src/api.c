/*
 * libtls: the public API (tls.h), the handshake driver, certificate verification of the peer, errors and their explanations.
 *
 * Every call runs the engine until it needs the socket. In non-blocking mode (TLS_NONBLOCK) TLS_WANT_READ / TLS_WANT_WRITE go
 * back to the caller; otherwise the call waits in poll() (at most TlsConfig.timeout_ms each time) and tries again.
 */
#include <poll.h>
#include <stdio.h>
#include "conn.h"

#define WRITE_RESERVE 4096                                       /* room kept in the queue for alerts and KeyUpdates */
#define KEY_UPDATE_AFTER (1ull << 24)                            /* records under one sending key (RFC 8446 5.5 for AES-GCM) */

/* ---------------------------------------------------------------------------------------------------- names */

const char *tls_strerror(int err) {
    switch (err) {
    case 0: return "no error";
    case TLS_WANT_READ: return "waiting for the server (TLS_WANT_READ)";
    case TLS_WANT_WRITE: return "waiting to send (TLS_WANT_WRITE)";
    case TLS_E_ARG: return "bad argument";
    case TLS_E_NOMEM: return "out of memory";
    case TLS_E_IO: return "the connection failed";
    case TLS_E_TIMEOUT: return "the server did not answer in time";
    case TLS_E_EOF: return "the server closed the connection during the handshake";
    case TLS_E_TRUNCATED: return "the connection was closed without close_notify (the data may be incomplete)";
    case TLS_E_PROTOCOL: return "protocol error (malformed or unexpected message)";
    case TLS_E_ALERT: return "the server sent a fatal alert";
    case TLS_E_VERSION: return "no common TLS version (or a downgrade was detected)";
    case TLS_E_SUITE: return "the server chose something that was not offered";
    case TLS_E_DECRYPT: return "a record failed authentication";
    case TLS_E_HANDSHAKE_SIG: return "the server's handshake signature is wrong";
    case TLS_E_FINISHED: return "the server's Finished message is wrong";
    case TLS_E_RANDOM: return "no random numbers";
    case TLS_E_NO_TRUST: return "no trusted root certificates";
    case TLS_E_CLOCK: return "the clock is not trusted";
    case TLS_E_CLOSED: return "the connection is shut down";
    case TLS_E_PLAINTEXT: return "the server did not answer in TLS";
    case TLS_E_CERT_PARSE: return "the server certificate could not be parsed";
    case TLS_E_CERT_UNTRUSTED: return "the certificate was not issued by a trusted authority";
    case TLS_E_CERT_SELF_SIGNED: return "the certificate is self-signed and not trusted";
    case TLS_E_CERT_EXPIRED: return "the certificate has expired";
    case TLS_E_CERT_NOT_YET_VALID: return "the certificate is not valid yet";
    case TLS_E_CERT_NAME: return "the certificate is not valid for this host name";
    case TLS_E_CERT_SIGNATURE: return "a signature in the certificate chain is invalid";
    case TLS_E_CERT_USAGE: return "the certificate chain may not be used this way (CA flag, path length or key usage)";
    case TLS_E_CERT_UNSUPPORTED: return "the certificate chain uses an unsupported algorithm or critical extension";
    case TLS_E_CERT_WEAK: return "the certificate chain uses a weak algorithm (SHA-1, MD5, or RSA below 2048 bits)";
    case TLS_E_CERT_EMPTY: return "the server sent no certificate";
    case TLS_E_CERT_CONSTRAINTS: return "a certificate authority's name constraints forbid this name";
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
    case 113: return "bad_certificate_status_response";
    case 115: return "unknown_psk_identity";
    case 116: return "certificate_required";
    case 120: return "no_application_protocol";
    default: return "unknown_alert";
    }
}

/* "2026-11-02 14:05:09 UTC" */
static void fmt_time(int64_t t, char *out, size_t cap) {
    int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t secs = t - days * 86400;
    days += 719468;                                                  /* civil from days (Howard Hinnant's algorithm) */
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    snprintf(out, cap, "%04lld-%02u-%02u %02u:%02u:%02u UTC", (long long)y, m, d, (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

/* ---------------------------------------------------------------------------------------------------- errors */

int tls_fail_at(TlsConn *c, int err, int alert, const char *file, int line, const char *fmt, ...) {
    if (c->state == ST_FAILED && c->err) return c->err;              /* the first error is the one that counts */
    c->err = err;
    c->state = ST_FAILED;
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(c->where, sizeof c->where, "%s:%d", base, line);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->errmsg, sizeof c->errmsg, fmt, ap);
        va_end(ap);
    } else {
        snprintf(c->errmsg, sizeof c->errmsg, "%s", tls_strerror(err));
    }
    if (alert >= 0 && !c->tx_closed) {                               /* tell the server why, as far as the socket takes it now */
        u8 a[2] = { 2, (u8)alert };
        if (rec_queue(c, CT_ALERT, a, 2) == 0) {
            c->info.alert_sent = alert;
            c->tx_closed = 1;
            rec_flush(c);
        }
    }
    return err;
}

static int alert_for_cert_error(int err) {
    switch (err) {
    case TLS_E_CERT_UNTRUSTED: case TLS_E_CERT_SELF_SIGNED: return AL_UNKNOWN_CA;
    case TLS_E_CERT_EXPIRED: case TLS_E_CERT_NOT_YET_VALID: return AL_CERTIFICATE_EXPIRED;
    case TLS_E_CERT_UNSUPPORTED: case TLS_E_CERT_WEAK: return AL_UNSUPPORTED_CERTIFICATE;
    default: return AL_BAD_CERTIFICATE;
    }
}

static const char *clock_words(int state) {
    return state == TLS_CLOCK_SET ? "set by the program" : state == TLS_CLOCK_BEHIND ? "the system clock is behind this program's build date; set it with sntp -s" : "system clock";
}

/* A sentence for a certificate error: which certificate, which names or dates, which clock. */
static void cert_error_message(TlsConn *c, int err, int clock_state, int64_t now) {
    char a[48], b[48], names[200];
    const x509_cert *leaf = &c->chain[0];
    char subj[200];
    x509_name_string(leaf->subject, subj, sizeof subj);
    switch (err) {
    case TLS_E_CERT_NAME:
        x509_san_string(leaf, names, sizeof names);
        tls_fail(c, err, AL_BAD_CERTIFICATE, "the certificate is not valid for %s: it names %s", c->host, names[0] ? names : "no host at all (no subjectAltName)");
        return;
    case TLS_E_CERT_EXPIRED:
    case TLS_E_CERT_NOT_YET_VALID:
        for (int i = 0; i < c->chain_n; i++) {
            const x509_cert *x = &c->chain[i];
            if (now >= x->not_before && now <= x->not_after) continue;
            x509_name_string(x->subject, subj, sizeof subj);
            fmt_time(x == leaf || now > x->not_after ? x->not_after : x->not_before, a, sizeof a);
            fmt_time(now, b, sizeof b);
            tls_fail(c, err, alert_for_cert_error(err), "the certificate '%s' %s %s; the clock says %s (%s)", subj,
                     now > x->not_after ? "expired on" : "is valid only from", a, b, clock_words(clock_state));
            return;
        }
        fmt_time(now, b, sizeof b);
        tls_fail(c, err, alert_for_cert_error(err), "%s: a certificate of the chain is outside its dates (the clock says %s, %s)", tls_strerror(err), b, clock_words(clock_state));
        return;
    case TLS_E_CERT_UNTRUSTED: {
        const x509_cert *top = leaf;
        for (int i = 1; i < c->chain_n; i++)
            if (c->chain[i].subject.n == top->issuer.n && memcmp(c->chain[i].subject.p, top->issuer.p, top->issuer.n) == 0) top = &c->chain[i];
        char iss[200];
        x509_name_string(top->issuer, iss, sizeof iss);
        tls_fail(c, err, AL_UNKNOWN_CA, "the certificate chain of %s does not lead to a trusted root: it ends at '%s', which is none of the %zu roots in the trust store",
                 c->host, iss, tls_trust_count(c->trust));
        return;
    }
    case TLS_E_CERT_SELF_SIGNED:
        tls_fail(c, err, AL_UNKNOWN_CA, "the server's certificate '%s' is self-signed and not in the trust store (https-get --ca FILE trusts a file)", subj);
        return;
    default:
        tls_fail(c, err, alert_for_cert_error(err), "%s ('%s')", tls_strerror(err), subj);
        return;
    }
}

/* ---------------------------------------------------------------------------------------------------- the peer's certificates */

static int from_store(const TlsConn *c, const x509_cert *x) {
    return c->trust && c->trust->count && x >= c->trust->anchors && x < c->trust->anchors + c->trust->count;
}

/* list: the entries of the Certificate message, each cert_data<3> followed, in TLS 1.3 only, by extensions<2> */
int verify_peer(TlsConn *c, const u8 *list, size_t len, int tls13) {
    free(c->cert_blob);                                              /* the parsed certificates point into this copy */
    c->cert_blob = malloc(len);
    if (!c->cert_blob) return tls_fail(c, TLS_E_NOMEM, AL_INTERNAL_ERROR, "out of memory for the certificates");
    memcpy(c->cert_blob, list, len);
    rd_t r = { c->cert_blob, len, 0 };
    c->chain_n = 0;
    int sent = 0;
    while (r.n > 0) {
        unsigned clen = rd24(&r);
        const u8 *der = rdn(&r, clen);
        if (r.err || clen == 0) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed certificate list");
        if (tls13) {                                                 /* per-certificate extensions (OCSP, SCT): not asked for, read past */
            rdn(&r, rd16(&r));
            if (r.err) return tls_fail(c, TLS_E_PROTOCOL, AL_DECODE_ERROR, "a malformed certificate list");
        }
        sent++;
        if (c->chain_n >= TLS_MAX_CERTS) continue;                   /* the rest cannot be part of a path we would build */
        if (x509_parse(&c->chain[c->chain_n], der, clen) != 0) {
            if (c->chain_n == 0) return tls_fail(c, TLS_E_CERT_PARSE, AL_BAD_CERTIFICATE, "the server's certificate could not be parsed");
            continue;                                                /* junk after the leaf: ignored, the path may not need it */
        }
        c->chain_n++;
    }
    if (c->chain_n == 0) return tls_fail(c, TLS_E_CERT_EMPTY, AL_DECODE_ERROR, "the server sent no certificate");
    c->info.sent_certs = sent;
    c->info.path_len = c->chain_n < TLS_MAX_PATH ? c->chain_n : TLS_MAX_PATH;   /* until verified: what was sent */
    for (int i = 0; i < c->info.path_len; i++) x509_describe(&c->chain[i], &c->info.path[i]);

    int64_t now;
    int cs = clock_now(&now);
    c->info.clock = cs;
    c->info.clock_now = now;
    const x509_cert *path[TLS_MAX_PATH + 1];
    int pn = 0;
    int rc = c->trust ? x509_verify_chain(c->trust, c->chain, c->chain_n, c->host[0] ? c->host : NULL, now, path, &pn) : TLS_E_CERT_UNTRUSTED;
    if (c->flags & TLS_INSECURE) {
        c->info.insecure = 1;
        c->info.verify_error = rc;
        return 0;
    }
    if (cs != TLS_CLOCK_SET && (c->flags & TLS_REQUIRE_CLOCK))
        return tls_fail(c, TLS_E_CLOCK, AL_CERTIFICATE_UNKNOWN, "the certificate dates cannot be checked: the clock is not trusted (%s) and TLS_REQUIRE_CLOCK was given", clock_words(cs));
    if (rc) {
        cert_error_message(c, rc, cs, now);
        return rc;
    }
    c->info.verified = 1;
    c->info.path_len = pn < TLS_MAX_PATH ? pn : TLS_MAX_PATH;
    for (int i = 0; i < c->info.path_len; i++) {
        x509_describe(path[i], &c->info.path[i]);
        c->info.path[i].from_store = from_store(c, path[i]);
    }
    return 0;
}

int check_handshake_signature(TlsConn *c, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen) {
    if (!offered(c->offer.sigalgs, c->offer.nsigalgs, scheme))
        return tls_fail(c, TLS_E_SUITE, AL_ILLEGAL_PARAMETER, "the server signed with scheme 0x%04x, which was not offered", scheme);
    if (x509_verify_handshake_signature(&c->chain[0], scheme, tls13, msg, msglen, sig, siglen) != 0)
        return tls_fail(c, TLS_E_HANDSHAKE_SIG, AL_DECRYPT_ERROR, "the server's %s signature does not verify with the key of its certificate (%s)",
                        tls13 ? "CertificateVerify" : "ServerKeyExchange", x509_scheme_name(scheme));
    c->sigalg = scheme;
    c->info.sigalg_name = x509_scheme_name(scheme);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- the driver */

/* One step of waiting: in non-blocking mode the TLS_WANT_* goes back, otherwise poll() until the socket is ready. */
static int wait_for(TlsConn *c, int want) {
    c->want = want;
    if (c->flags & TLS_NONBLOCK) return want;
    int rc = tls_os_wait(c->fd, want, c->timeout_ms);
    if (rc == TLS_E_TIMEOUT)
        return tls_fail(c, TLS_E_TIMEOUT, -1, "the server %s for %d s", want == TLS_WANT_READ ? "sent nothing" : "took nothing", c->timeout_ms / 1000);
    if (rc) return tls_fail(c, TLS_E_IO, -1, "waiting for the socket failed");
    c->want = 0;
    return 0;
}

static int drive(TlsConn *c) {
    for (;;) {
        if (c->state == ST_FAILED) return c->err;
        int rc = rec_flush(c);
        if (rc) return rc;
        if (c->state == ST_CONNECTED) return 0;
        if (c->state == ST_NEW) {
            if ((rc = hello_send(c))) return rc;
            c->state = ST_WAIT_SH;
            continue;
        }
        u8 type;
        const u8 *msg;
        size_t len;
        if ((rc = hs_next(c, &type, &msg, &len))) return rc;
        if (c->state == ST_WAIT_SH) {
            if (type != HS_SERVER_HELLO) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake message %u instead of a ServerHello", type);
            rc = hello_process_server(c, msg, len);
            if (rc < 0) return rc;
            hs_consume(c, len);
            if (rc == 1) {                                           /* HelloRetryRequest: the second ClientHello */
                if ((rc = hello_send(c))) return rc;
                continue;
            }
            c->info.version = c->version;
            c->info.version_name = c->version == 0x0304 ? "TLS 1.3" : "TLS 1.2";
            c->info.suite_name = c->suite->name;
            if (c->version == 0x0304) {
                if ((rc = hs13_after_server_hello(c))) return rc;
            } else {
                c->state = ST12_WAIT_CERT;
            }
            continue;
        }
        rc = c->version == 0x0304 ? hs13_message(c, type, msg, len) : hs12_message(c, type, msg, len);
        if (rc < 0) return rc;
        if (rc == 0) hs_consume(c, len);
    }
}

static void handshake_done(TlsConn *c) {
    free(c->cert_blob);                                              /* the certificates were described into c->info */
    c->cert_blob = NULL;
    c->chain_n = 0;
    tlsc_wipe(c->x25519_priv, sizeof c->x25519_priv);
    tlsc_wipe(c->ec_priv, sizeof c->ec_priv);
    tlsc_wipe(c->master, sizeof c->master);
    if (c->hs_len == 0) {
        free(c->hs);
        c->hs = NULL;
        c->hs_cap = 0;
    }
}

int tls_handshake(TlsConn *c) {
    if (!c) return TLS_E_ARG;
    for (;;) {
        int was_connected = c->state == ST_CONNECTED;
        int rc = drive(c);
        if (rc == 0) {
            if (!was_connected) handshake_done(c);
            c->want = 0;
            return 0;
        }
        if (rc != TLS_WANT_READ && rc != TLS_WANT_WRITE) return rc;
        if ((rc = wait_for(c, rc))) return rc;
    }
}

int tls_status(const TlsConn *c) {
    if (!c) return TLS_E_ARG;
    if (c->state == ST_FAILED) return c->err;
    if (c->state == ST_CONNECTED) return 0;
    return c->want ? c->want : TLS_WANT_READ;
}

/* Handshake messages after the handshake (KeyUpdate, NewSessionTicket; a TLS 1.2 HelloRequest). */
static int post_handshake(TlsConn *c) {
    while (c->hs_len >= 4) {
        size_t mlen = (size_t)c->hs[1] << 16 | (size_t)c->hs[2] << 8 | c->hs[3];
        if (mlen > TLS_MAX_HS) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a handshake message of %zu bytes", mlen);
        if (c->hs_len < 4 + mlen) return 0;
        int rc = 0;
        if (c->version == 0x0304) {
            rc = hs13_post_handshake(c, c->hs, 4 + mlen);
        } else if (c->hs[0] == HS_HELLO_REQUEST && mlen == 0) {
            u8 a[2] = { 1, AL_NO_RENEGOTIATION };                     /* we never renegotiate (RFC 5746 4.2) */
            if (!c->tx_closed && rec_room(c) >= 2) (void)rec_queue(c, CT_ALERT, a, 2);
        } else {
            rc = tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "handshake message %u after the TLS 1.2 handshake", c->hs[0]);
        }
        if (rc) return rc;
        hs_consume(c, 4 + mlen);
    }
    return 0;
}

long tls_read(TlsConn *c, void *buf, size_t n) {
    if (!c) return TLS_E_ARG;
    if (c->state != ST_CONNECTED) {
        int rc = tls_handshake(c);
        if (rc) return rc;
    }
    if (n == 0) return 0;
    for (;;) {
        if (c->app_len) {
            size_t k = c->app_len < n ? c->app_len : n;
            memcpy(buf, c->app_p, k);
            c->app_p += k;
            c->app_len -= k;
            return (long)k;
        }
        if (c->rx_closed) return 0;
        if (c->state == ST_FAILED) return c->err;
        if (c->out_end > c->out_start) {                             /* a KeyUpdate or an alert waiting to go out */
            int rc = rec_flush(c);
            if (rc < 0 && rc != TLS_WANT_WRITE) return rc;
        }
        int t;
        u8 *d;
        size_t len;
        int rc = rec_next(c, &t, &d, &len);
        if (rc == TLS_E_EOF) return tls_fail(c, TLS_E_TRUNCATED, -1, "the server closed the connection without close_notify (the data may be incomplete)");
        if (rc == TLS_WANT_READ) {
            if ((rc = wait_for(c, rc))) return rc;
            continue;
        }
        if (rc < 0) return rc;
        switch (t) {
        case CT_APPDATA:
            if (len == 0) {
                if (++c->idle > TLS_MAX_IDLE) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "too many empty records");
                continue;
            }
            c->idle = 0;
            c->app_p = d;
            c->app_len = len;
            c->info.bytes_in += len;
            continue;
        case CT_HANDSHAKE:
            if (len == 0) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "an empty handshake record");
            if ((rc = hs_append(c, d, len))) return rc;
            if ((rc = post_handshake(c))) return rc;
            if (++c->idle > TLS_MAX_IDLE * 32) return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "too many handshake messages after the handshake");
            continue;
        case CT_ALERT:
            rc = handle_alert(c, d, len);
            if (rc == 1) return 0;
            if (rc < 0) return rc;
            continue;
        default:
            return tls_fail(c, TLS_E_PROTOCOL, AL_UNEXPECTED_MESSAGE, "a ChangeCipherSpec after the handshake");
        }
    }
}

static int flush_all(TlsConn *c) {
    for (;;) {
        int rc = rec_flush(c);
        if (rc != TLS_WANT_WRITE) return rc;
        if ((rc = wait_for(c, rc))) return rc;
    }
}

long tls_write(TlsConn *c, const void *buf, size_t n) {
    if (!c) return TLS_E_ARG;
    if (c->state != ST_CONNECTED) {
        int rc = tls_handshake(c);
        if (rc) return rc;
    }
    if (c->tx_closed) return TLS_E_CLOSED;
    const u8 *p = buf;
    size_t done = 0;
    while (done < n) {
        size_t room = rec_room(c);
        room = room > WRITE_RESERVE ? room - WRITE_RESERVE : 0;
        if (room == 0) {
            int rc = rec_flush(c);
            if (rc == TLS_WANT_WRITE) {
                if (done && (c->flags & TLS_NONBLOCK)) break;         /* what is queued counts as written */
                if ((rc = wait_for(c, rc))) return done ? (long)done : rc;
                continue;
            }
            if (rc < 0) return rc;
            continue;
        }
        if (c->version == 0x0304 && c->wr.seq >= KEY_UPDATE_AFTER) {
            int rc = hs13_key_update(c, 0);
            if (rc) return rc;
        }
        size_t take = n - done < room ? n - done : room;
        int rc = rec_queue(c, CT_APPDATA, p + done, take);
        if (rc) return tls_fail(c, rc, AL_INTERNAL_ERROR, "could not queue application data");
        done += take;
        c->info.bytes_out += take;
        rc = rec_flush(c);
        if (rc < 0 && rc != TLS_WANT_WRITE) return rc;
    }
    if (!(c->flags & TLS_NONBLOCK)) {
        int rc = flush_all(c);
        if (rc) return rc;
    }
    return (long)done;
}

int tls_flush(TlsConn *c) {
    if (!c) return TLS_E_ARG;
    if (c->state == ST_FAILED) return c->err;
    return flush_all(c);
}

size_t tls_pending(const TlsConn *c) { return c ? c->app_len : 0; }

short tls_poll_events(const TlsConn *c) {
    if (!c) return 0;
    short ev = c->want == TLS_WANT_WRITE ? POLLOUT : POLLIN;
    if (c->out_end > c->out_start) ev |= POLLOUT;
    return ev;
}

int tls_key_update(TlsConn *c, int request_peer) {
    if (!c || c->state != ST_CONNECTED || c->version != 0x0304 || c->tx_closed) return TLS_E_ARG;
    int rc = hs13_key_update(c, request_peer);
    if (rc) return rc;
    return flush_all(c);
}

int tls_shutdown(TlsConn *c) {
    if (!c) return TLS_E_ARG;
    if (c->state == ST_CONNECTED && !c->tx_closed) {
        u8 a[2] = { 1, AL_CLOSE_NOTIFY };
        if (rec_room(c) < 2) {
            int rc = flush_all(c);
            if (rc) return rc;
        }
        int rc = rec_queue(c, CT_ALERT, a, 2);
        if (rc) return tls_fail(c, rc, -1, "could not queue close_notify");
        c->tx_closed = 1;
    }
    if (c->state == ST_FAILED) return 0;
    return flush_all(c);
}

int tls_fd(const TlsConn *c) { return c ? c->fd : -1; }

void tls_close(TlsConn *c) {
    if (!c) return;
    if (c->state == ST_CONNECTED && !c->tx_closed) {                 /* close_notify, without waiting for a slow server */
        u8 a[2] = { 1, AL_CLOSE_NOTIFY };
        if (rec_room(c) >= 2 && rec_queue(c, CT_ALERT, a, 2) == 0) c->tx_closed = 1;
    }
    if (c->out_end > c->out_start && c->state != ST_FAILED) rec_flush(c);
    free(c->in);
    free(c->out);
    free(c->hs);
    free(c->cert_blob);
    tls_trust_free(c->own_trust);
    tlsc_wipe(c, sizeof *c);
    free(c);
}

const TlsInfo *tls_info(const TlsConn *c) { return c ? &c->info : NULL; }

const char *tls_error_string(const TlsConn *c) {
    if (!c) return "out of memory";
    return c->state == ST_FAILED ? c->errmsg : "";
}

/* ---------------------------------------------------------------------------------------------------- new connections */

/* host names go on the wire (SNI) and into name checks: letters, digits, '-', '_' and dots, or an IPv4 literal */
static int host_ok(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = h[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_')) return 0;
    }
    return h[0] != '.' && strstr(h, "..") == NULL;
}

TlsConn *tls_new(int fd, const char *host, const TlsConfig *cfg) {
    static const TlsConfig defaults;
    if (!cfg) cfg = &defaults;
    TlsConn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->in = malloc(TLS_INBUF);
    c->out = malloc(TLS_OUTBUF);
    if (!c->in || !c->out) {
        free(c->in);
        free(c->out);
        free(c);
        return NULL;
    }
    c->fd = fd;
    c->flags = cfg->flags;
    c->timeout_ms = cfg->timeout_ms == 0 ? TLS_DEFAULT_TIMEOUT_MS : cfg->timeout_ms;
    c->suites_mask = cfg->suites;
    c->random = cfg->random;
    c->random_ctx = cfg->random_ctx;
    c->first_record = 1;
    c->info.alert_received = -1;
    c->info.alert_sent = -1;
    tlsc_hash_init(&c->th256, TLSC_SHA256);
    tlsc_hash_init(&c->th384, TLSC_SHA384);
    c->state = ST_NEW;
    if ((cfg->flags & TLS_ONLY_TLS12) && (cfg->flags & TLS_ONLY_TLS13)) {
        tls_fail(c, TLS_E_ARG, -1, "TLS_ONLY_TLS12 and TLS_ONLY_TLS13 exclude each other");
        return c;
    }
    if (host) {
        size_t n = strlen(host);
        if (n >= sizeof c->host) n = 0;
        for (size_t i = 0; i < n; i++) c->host[i] = (char)(host[i] >= 'A' && host[i] <= 'Z' ? host[i] + 32 : host[i]);
        if (n && c->host[n - 1] == '.') c->host[n - 1] = 0;
    }
    u8 ip[4];
    c->host_is_ip = c->host[0] && x509_host_is_ipv4(c->host, ip);
    if (c->host[0] && !host_ok(c->host)) {
        tls_fail(c, TLS_E_ARG, -1, "'%s' is not a host name this library can use (internationalized names must be given in their xn-- form)", host);
        return c;
    }
    if (!c->host[0] && !(cfg->flags & TLS_INSECURE)) {
        tls_fail(c, TLS_E_ARG, -1, "no host name to check the certificate against");
        return c;
    }
    if (cfg->alpn) {
        size_t n = strlen(cfg->alpn);
        if (n == 0 || n >= sizeof c->alpn) {
            tls_fail(c, TLS_E_ARG, -1, "an ALPN protocol name must have 1 to 31 characters");
            return c;
        }
        memcpy(c->alpn, cfg->alpn, n + 1);
    }
    /* the roots: the caller's, a file, or the default store */
    if (cfg->trust) {
        c->trust = cfg->trust;
    } else if (cfg->ca_file) {
        c->own_trust = tls_trust_new();
        int rc = c->own_trust ? tls_trust_load_file(c->own_trust, cfg->ca_file) : TLS_E_NOMEM;
        if (rc > 0) {
            c->trust = c->own_trust;
        } else if (!(cfg->flags & TLS_INSECURE)) {
            tls_fail(c, TLS_E_NO_TRUST, -1, rc == TLS_E_NO_TRUST ? "the trust store %s does not exist" :
                     rc < 0 ? "the trust store %s cannot be read" : "the trust store %s holds no usable root certificate", cfg->ca_file);
            return c;
        }
    } else {
        int err;
        const char *path;
        c->trust = tls_trust_default(&err, &path);
        if (!c->trust && !(cfg->flags & TLS_INSECURE)) {
            tls_fail(c, TLS_E_NO_TRUST, -1, "no trusted root certificates: %s %s. Put a PEM bundle there (Mozilla's, for example), "
                     "name one with --ca FILE / TlsConfig.ca_file, or use --insecure for an experiment", path,
                     err == TLS_E_NO_TRUST ? "does not exist or holds no usable root" : "cannot be read");
            return c;
        }
    }
    return c;
}

TlsConn *tls_connect(int fd, const char *host, const TlsConfig *cfg) {
    TlsConn *c = tls_new(fd, host, cfg);
    if (c && c->state != ST_FAILED) tls_handshake(c);
    return c;
}

int tls_test_script(TlsConn *c, const u8 *ch1, size_t ch1_len, const u8 *ch2, size_t ch2_len, const u8 *x25519_priv, const u8 *p256_priv) {
    if (!c || c->state != ST_NEW || !ch1) return TLS_E_ARG;
    c->script.n = ch2 ? 2 : 1;
    c->script.ch[0] = ch1;
    c->script.ch_len[0] = ch1_len;
    c->script.ch[1] = ch2;
    c->script.ch_len[1] = ch2_len;
    if (x25519_priv) { memcpy(c->script.x25519, x25519_priv, 32); c->script.have_x25519 = 1; }
    if (p256_priv) { memcpy(c->script.p256, p256_priv, 32); c->script.have_p256 = 1; }
    return 0;
}
