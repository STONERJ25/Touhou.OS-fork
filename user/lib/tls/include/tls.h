/*
 * libtls: a TLS 1.3 / TLS 1.2 client for TouhouOS, with X.509 certificate verification against a root store read from a PEM file.
 * Specifications: RFC 8446 (TLS 1.3), RFC 5246 with RFC 5289, 7627, 7905 and 8422 (TLS 1.2), RFC 5280 and 6125 (certificates and
 * names), RFC 8017 (RSA), FIPS 186-4 (ECDSA), RFC 8032 (Ed25519). docs/TLS.md says what is and is not done; it is a careful hobby
 * implementation, not a hardened production stack.
 *
 * Blocking use (the socket is an ordinary connected TCP socket):
 *
 *     TlsConfig cfg = { 0 };                                // verify against /etc/ssl/cacert.pem, 30 s timeout, no ALPN
 *     TlsConn *c = tls_connect(fd, "example.org", &cfg);    // runs the handshake
 *     if (tls_status(c) < 0) { fprintf(stderr, "%s\n", tls_error_string(c)); tls_close(c); ... }
 *     tls_write(c, request, len);
 *     while ((n = tls_read(c, buf, sizeof buf)) > 0) ...;   // 0: the server closed cleanly (close_notify)
 *     tls_close(c);                                         // sends close_notify and frees; the socket stays open
 *
 * Non-blocking use (a poll loop): put TLS_NONBLOCK in cfg.flags. tls_connect then only starts the handshake, and every call may
 * return TLS_WANT_READ or TLS_WANT_WRITE instead of waiting: poll the socket for tls_poll_events(c) and call the same function
 * again. Before polling for input, read what tls_pending(c) says is already decrypted. tls_write may accept data that is not on
 * the wire yet; tls_flush(c) returns 0 once everything queued has been handed to the socket.
 *
 *     while ((rc = tls_handshake(c)) == TLS_WANT_READ || rc == TLS_WANT_WRITE) poll_for(fd, tls_poll_events(c));
 *
 * Every connection gets a TlsConn even when it fails (NULL only when memory is short), so that tls_error_string() can say exactly
 * what went wrong; tls_close() it either way. The library never closes the socket.
 */
#ifndef LIBTLS_H
#define LIBTLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- results: >= 0 success, TLS_WANT_* (non-blocking: try again), else a TLS_E_* error ---- */
#define TLS_WANT_READ   (-1)            /* call again when the socket is readable */
#define TLS_WANT_WRITE  (-2)            /* call again when the socket is writable */

enum {
    TLS_E_ARG = -10,                    /* bad argument, or the call does not fit the state of the connection */
    TLS_E_NOMEM = -11,
    TLS_E_IO = -12,                     /* the socket failed (connection reset, ...) */
    TLS_E_TIMEOUT = -13,                /* the server stayed silent longer than TlsConfig.timeout_ms */
    TLS_E_EOF = -14,                    /* the server closed the connection during the handshake */
    TLS_E_TRUNCATED = -15,              /* the connection ended without close_notify: the data may have been cut off */
    TLS_E_PROTOCOL = -16,               /* a malformed, oversized or unexpected message */
    TLS_E_ALERT = -17,                  /* the server sent a fatal alert (tls_info()->alert_received says which) */
    TLS_E_VERSION = -18,                /* no common protocol version, or a downgrade was detected */
    TLS_E_SUITE = -19,                  /* the server chose a cipher suite, group or extension that was not offered */
    TLS_E_DECRYPT = -20,                /* a record failed authentication */
    TLS_E_HANDSHAKE_SIG = -21,          /* the server's handshake signature does not verify with its certificate */
    TLS_E_FINISHED = -22,               /* the server's Finished message is wrong */
    TLS_E_RANDOM = -23,                 /* no random numbers from the kernel */
    TLS_E_NO_TRUST = -24,               /* no trusted root certificates could be loaded (and TLS_INSECURE was not given) */
    TLS_E_CLOCK = -25,                  /* TLS_REQUIRE_CLOCK was given and the clock is not trusted */
    TLS_E_CLOSED = -26,                 /* the connection was shut down (writing after tls_shutdown, reading after an error) */
    TLS_E_PLAINTEXT = -27,              /* the server answered with something that is not TLS (plain HTTP?) */
    TLS_E_CERT_PARSE = -30,             /* a certificate could not be parsed */
    TLS_E_CERT_UNTRUSTED = -31,         /* the chain does not lead to a trusted root */
    TLS_E_CERT_SELF_SIGNED = -32,       /* a self-signed certificate that is not in the trust store */
    TLS_E_CERT_EXPIRED = -33,
    TLS_E_CERT_NOT_YET_VALID = -34,
    TLS_E_CERT_NAME = -35,              /* the certificate is not valid for the host name */
    TLS_E_CERT_SIGNATURE = -36,         /* a signature in the chain does not verify */
    TLS_E_CERT_USAGE = -37,             /* not a CA, path too long, key usage or extended key usage forbids it */
    TLS_E_CERT_UNSUPPORTED = -38,       /* an algorithm or critical extension this library does not know */
    TLS_E_CERT_WEAK = -39,              /* SHA-1 or MD5 signature, RSA key below 2048 bits */
    TLS_E_CERT_EMPTY = -40,             /* the server sent no certificate */
    TLS_E_CERT_CONSTRAINTS = -41,       /* a CA's name constraints forbid the server's name */
};

/* ---- TlsConfig.flags ---- */
#define TLS_NONBLOCK        0x0001      /* calls return TLS_WANT_* instead of waiting (see above) */
#define TLS_INSECURE        0x0002      /* DO NOT verify the certificate chain, dates or name; tls_info()->insecure says so.
                                           The server must still prove that it holds the key of whatever certificate it sent,
                                           which protects against a passive eavesdropper only. For experiments. */
#define TLS_ONLY_TLS12      0x0004      /* offer TLS 1.2 only */
#define TLS_ONLY_TLS13      0x0008      /* offer TLS 1.3 only */
#define TLS_REQUIRE_CLOCK   0x0010      /* fail (TLS_E_CLOCK) unless the clock is trusted (TLS_CLOCK_SET) */
#define TLS_NO_AESNI        0x0020      /* tests: use the portable AES even when the CPU has AES-NI */

/* ---- TlsConfig.suites: the bulk ciphers allowed (0 = all) ---- */
#define TLS_SUITE_AES128_GCM  0x01
#define TLS_SUITE_AES256_GCM  0x02
#define TLS_SUITE_CHACHA20    0x04

#define TLS_DEFAULT_CA_FILE "/etc/ssl/cacert.pem"   /* $SSL_CERT_FILE, when set, is tried instead */
#define TLS_DEFAULT_TIMEOUT_MS 30000

typedef struct TlsConn TlsConn;
typedef struct TlsTrust TlsTrust;       /* a set of trusted root certificates */

typedef struct TlsConfig {
    unsigned flags;                     /* TLS_* above */
    const char *ca_file;                /* PEM bundle (or one DER certificate) of trusted roots; NULL: the default */
    const TlsTrust *trust;              /* or roots the caller loaded (it keeps them; several connections may share them) */
    const char *alpn;                   /* one protocol to offer, e.g. "http/1.1"; NULL offers none */
    unsigned suites;                    /* TLS_SUITE_* mask, 0 = all */
    int timeout_ms;                     /* blocking calls: the longest the server may stay silent; 0 = 30 s, < 0 = forever */
    void (*random)(void *ctx, void *buf, size_t n);   /* tests only: replaces the kernel's random numbers */
    void *random_ctx;
} TlsConfig;

/* ---- what is known about a connection ---- */
#define TLS_MAX_PATH 10                 /* certificates in a verified path, leaf to root */

typedef struct TlsCertInfo {
    char subject[200];                  /* "CN=example.org, O=Example" (printable ASCII only) */
    char issuer[200];
    char names[200];                    /* subjectAltName DNS names and addresses, comma separated */
    int64_t not_before, not_after;      /* Unix seconds */
    char key[24];                       /* "EC P-256", "RSA 2048", "Ed25519" */
    char sig[32];                       /* the algorithm that signed it: "ECDSA SHA-256", "RSA-PSS SHA-384", ... */
    uint8_t sha256[32];                 /* fingerprint of the DER */
    int from_store;                     /* 1: this certificate is the trust anchor from the root store */
} TlsCertInfo;

/* The clock used for certificate dates (tls_clock(), TlsInfo.clock) */
#define TLS_CLOCK_SYSTEM  0             /* the system clock: plausible (not earlier than this library's build) but unconfirmed */
#define TLS_CLOCK_SET     1             /* given by tls_set_time() or a trusted time source */
#define TLS_CLOCK_BEHIND  2             /* the system clock is earlier than this library's build, so it is wrong: dates are
                                           checked against the build time instead, which cannot catch a recent expiry */

typedef struct TlsInfo {
    int version;                        /* 0x0304 TLS 1.3, 0x0303 TLS 1.2, 0 before the ServerHello */
    const char *version_name;           /* "TLS 1.3" */
    const char *suite_name;             /* "TLS_AES_128_GCM_SHA256" */
    const char *group_name;             /* key exchange: "x25519", "secp256r1", "secp384r1" */
    const char *sigalg_name;            /* the server's handshake signature: "ecdsa_secp256r1_sha256", ... */
    char alpn[32];                      /* the protocol the server picked, "" for none */
    int hello_retry;                    /* the server asked for another key share first */
    int verified;                       /* the chain leads to a trusted root, the dates and the name are right */
    int insecure;                       /* TLS_INSECURE: nothing about the server was verified */
    int verify_error;                   /* with TLS_INSECURE: the TLS_E_CERT_* error verification found (0: none) */
    int clock;                          /* TLS_CLOCK_* the dates were checked with */
    int64_t clock_now;                  /* the time they were checked against */
    int sent_certs;                     /* certificates in the server's Certificate message */
    int path_len;                       /* entries in path[]: the verified path, or the certificates sent when unverified */
    TlsCertInfo path[TLS_MAX_PATH];
    int alert_received;                 /* the alert the server sent (TLS_E_ALERT), else -1 */
    int alert_sent;                     /* the alert this side sent before giving up, else -1 */
    unsigned long long bytes_in, bytes_out;   /* application data */
} TlsInfo;

/* ---- connections ---- */
TlsConn *tls_connect(int fd, const char *host, const TlsConfig *cfg);  /* new + handshake (only started with TLS_NONBLOCK) */
TlsConn *tls_new(int fd, const char *host, const TlsConfig *cfg);      /* no I/O yet; host: a DNS name or a dotted IPv4 address */
int   tls_handshake(TlsConn *c);                /* 0 done, TLS_WANT_*, or < 0 */
int   tls_status(const TlsConn *c);             /* 0 connected (or closed cleanly), TLS_WANT_* while the handshake runs, else the error */
long  tls_read(TlsConn *c, void *buf, size_t n);         /* > 0 bytes, 0 after close_notify, TLS_WANT_*, < 0 error */
long  tls_write(TlsConn *c, const void *buf, size_t n);  /* bytes accepted (all of them when blocking), TLS_WANT_*, < 0 */
int   tls_flush(TlsConn *c);                    /* 0 when nothing is queued any more, TLS_WANT_WRITE, < 0 */
size_t tls_pending(const TlsConn *c);           /* decrypted bytes ready for tls_read */
short tls_poll_events(const TlsConn *c);        /* POLLIN / POLLOUT the last TLS_WANT_* asked for */
int   tls_key_update(TlsConn *c, int request_peer);   /* TLS 1.3: new sending keys (and ask the server to renew its own) */
int   tls_shutdown(TlsConn *c);                 /* sends close_notify; 0, TLS_WANT_WRITE, < 0 */
void  tls_close(TlsConn *c);                    /* shutdown (best effort, without waiting long) and free; NULL is fine */
int   tls_fd(const TlsConn *c);

const TlsInfo *tls_info(const TlsConn *c);
const char *tls_error_string(const TlsConn *c); /* a sentence about the connection's error ("" when there is none) */
const char *tls_strerror(int err);              /* the generic text of a TLS_E_* code */
const char *tls_alert_name(int alert);

/* ---- trusted roots ---- */
TlsTrust *tls_trust_new(void);
void   tls_trust_free(TlsTrust *t);
int    tls_trust_load_file(TlsTrust *t, const char *path);          /* a PEM bundle or one DER certificate: roots added, or < 0 */
int    tls_trust_load_pem(TlsTrust *t, const void *pem, size_t len);
int    tls_trust_add_der(TlsTrust *t, const void *der, size_t len); /* 1 added, 0 skipped (unsupported), < 0 error */
size_t tls_trust_count(const TlsTrust *t);
size_t tls_trust_skipped(const TlsTrust *t);    /* certificates that were read but could not serve as roots */
/* The default store ($SSL_CERT_FILE, else TLS_DEFAULT_CA_FILE), loaded once per process; NULL (and *err, *path) when missing. */
const TlsTrust *tls_trust_default(int *err, const char **path);

/* ---- the clock for certificate dates (process-wide) ---- */
void tls_set_time(int64_t unix_seconds);        /* pin "now" (trusted) for every later check; 0 goes back to the system clock */
void tls_set_time_source(int64_t (*fn)(void *ctx, int *trusted), void *ctx);   /* or ask fn; NULL removes it */
int  tls_clock(int64_t *now);                   /* the TLS_CLOCK_* state and the time certificate dates are checked against */
const char *tls_clock_name(int state);          /* "set by the program", "system clock", "system clock is behind" */
int64_t tls_build_time(void);                   /* when this library was compiled (the floor for a clock that is behind) */

/* ---- certificate helpers (tests, https-get -v) ---- */
int tls_cert_matches_host(const void *der, size_t len, const char *host);    /* 0 or TLS_E_CERT_NAME / TLS_E_CERT_PARSE */
int tls_cert_describe(const void *der, size_t len, TlsCertInfo *out);        /* 0 or TLS_E_CERT_PARSE */
/* Verifies chain[0] (leaf) with the other certificates as candidates for intermediates, as the handshake would; 0 or TLS_E_CERT_*. */
int tls_verify_chain(const TlsTrust *t, const void *const *der, const size_t *len, int n, const char *host, int64_t now);

#ifdef __cplusplus
}
#endif
#endif
