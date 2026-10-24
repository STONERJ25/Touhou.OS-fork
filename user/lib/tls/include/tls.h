/*
 * libtls: a TLS 1.3 / 1.2 client for TouhouOS.
 *
 *   tls_ctx *ctx = tls_ctx_new();
 *   tls_ctx_load_default_ca(ctx);                       // /etc/ssl/certs/ca-bundle.pem
 *   tls_config cfg = { .server_name = "example.org", .alpn = "http/1.1", .ctx = ctx };
 *   tls_conn *c = tls_connect_fd(sockfd, &cfg, &err);   // runs the handshake on a connected TCP socket
 *   tls_write(c, "GET / HTTP/1.1\r\n...", n);
 *   n = tls_read(c, buf, sizeof buf);
 *   tls_close(c);                                       // sends close_notify, frees everything (does not close the socket)
 *
 * What it does and does not do is written down in docs/TLS.md. In short: client only, no client certificates, no session
 * resumption, no renegotiation, no revocation checking (CRL/OCSP), no name constraints, no Ed25519 or P-521.
 */
#ifndef LIBTLS_H
#define LIBTLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes (negative); tls_strerror() turns them into text ---- */
enum {
    TLS_OK = 0,
    TLS_E_ARG = -1,                 /* bad argument */
    TLS_E_NOMEM = -2,
    TLS_E_IO = -3,                  /* the transport failed or the peer closed the connection mid-handshake */
    TLS_E_TRUNCATED = -4,           /* the connection ended without close_notify: the data may have been cut off */
    TLS_E_PROTOCOL = -5,            /* malformed, oversized or unexpected message */
    TLS_E_ALERT = -6,               /* the peer sent a fatal alert (tls_alert_received()) */
    TLS_E_VERSION = -7,             /* no common protocol version, or a downgrade attempt */
    TLS_E_SUITE = -8,               /* the server chose something that was not offered */
    TLS_E_DECRYPT = -9,             /* a record failed authentication */
    TLS_E_HANDSHAKE_SIG = -10,      /* the signature in CertificateVerify / ServerKeyExchange is wrong */
    TLS_E_FINISHED = -11,           /* the Finished message does not match */
    TLS_E_ENTROPY = -12,            /* the system has not collected enough randomness yet */
    TLS_E_CERT_PARSE = -20,         /* a certificate could not be parsed */
    TLS_E_CERT_UNTRUSTED = -21,     /* the chain does not end in a trusted root (unknown CA) */
    TLS_E_CERT_SELF_SIGNED = -22,   /* a self-signed certificate that is not trusted */
    TLS_E_CERT_EXPIRED = -23,
    TLS_E_CERT_NOT_YET_VALID = -24,
    TLS_E_CERT_NAME = -25,          /* the certificate is not valid for the host name */
    TLS_E_CERT_SIGNATURE = -26,     /* a signature in the chain does not verify */
    TLS_E_CERT_USAGE = -27,         /* not a CA / path too long / key usage or extended key usage forbids it */
    TLS_E_CERT_UNSUPPORTED = -28,   /* weak or unsupported algorithm, or an unknown critical extension */
    TLS_E_CERT_EMPTY = -29,         /* the server sent no certificate */
};

/* ---- flags for tls_config.flags ---- */
#define TLS_F_NO_VERIFY    0x0001   /* carry on when the certificate is not acceptable (the reason is kept in tls_info.verify_error) */
#define TLS_F_TLS12_ONLY   0x0002   /* offer TLS 1.2 only (tests) */
#define TLS_F_TLS13_ONLY   0x0004   /* offer TLS 1.3 only */
#define TLS_F_WEAK_RANDOM  0x0008   /* connect even though the kernel says its random number generator is not seeded yet */
#define TLS_F_NO_AESNI     0x0010   /* tests: use the portable AES even when the CPU has AES-NI */

/* cipher suite masks for tls_config.suites (0 = all) */
#define TLS_S_AES128_GCM   0x01
#define TLS_S_AES256_GCM   0x02
#define TLS_S_CHACHA20     0x04

typedef struct tls_ctx tls_ctx;     /* a set of trusted root certificates */
typedef struct tls_conn tls_conn;

/* The transport: send returns the number of bytes written (> 0) or < 0; recv returns bytes read (> 0), 0 at end of stream, or < 0. */
typedef struct {
    long (*send)(void *user, const void *buf, size_t len);
    long (*recv)(void *user, void *buf, size_t len);
    void *user;
} tls_io;

typedef struct {
    const char *server_name;        /* for SNI and for checking the certificate; a dotted IPv4 literal is matched against IP SANs */
    const char *alpn;               /* e.g. "http/1.1", or NULL */
    const tls_ctx *ctx;             /* trusted roots; NULL trusts nothing */
    unsigned flags;                 /* TLS_F_* */
    unsigned suites;                /* TLS_S_* mask of allowed bulk ciphers, 0 for all */
    int64_t now;                    /* the time for validity checks (Unix seconds); 0 asks the OS */
    void (*random)(void *buf, size_t n);   /* tests: replaces the operating system's random numbers */
} tls_config;

typedef struct {
    int version;                    /* 0x0303 (TLS 1.2) or 0x0304 (TLS 1.3) */
    const char *version_name;
    const char *suite_name;
    const char *group_name;         /* key exchange group */
    char alpn[16];                  /* negotiated protocol, "" if none */
    int verified;                   /* 1: the chain led to a trusted root and the name matched */
    int verify_error;               /* with TLS_F_NO_VERIFY: the TLS_E_CERT_* error that was ignored, else 0 */
    char subject[200];              /* "C=US, O=Example, CN=example.org" */
    char issuer[200];
    int64_t not_before, not_after;
    uint8_t fingerprint[32];        /* SHA-256 of the server's certificate */
    unsigned chain_length;
    int hello_retry;                /* the server asked for a different key exchange group */
} tls_info;

/* ---- trust anchors ---- */
tls_ctx *tls_ctx_new(void);
void tls_ctx_free(tls_ctx *ctx);
int  tls_ctx_load_pem(tls_ctx *ctx, const void *data, size_t len);          /* adds every CERTIFICATE block; returns how many roots were added or < 0 */
int  tls_ctx_load_der(tls_ctx *ctx, const void *data, size_t len);          /* adds one certificate */
int  tls_ctx_load_file(tls_ctx *ctx, const char *path);                     /* PEM bundle or a single DER certificate */
int  tls_ctx_load_default_ca(tls_ctx *ctx);                                 /* /etc/ssl/certs/ca-bundle.pem */
size_t tls_ctx_count(const tls_ctx *ctx);
size_t tls_ctx_skipped(const tls_ctx *ctx);                                 /* roots that were read but use an unsupported algorithm */

/* ---- connections ---- */
tls_conn *tls_new(const tls_io *io, const tls_config *cfg);
int  tls_handshake(tls_conn *c);                                            /* 0 or a negative TLS_E_* */
long tls_read(tls_conn *c, void *buf, size_t n);                            /* > 0 bytes, 0 after close_notify, < 0 error */
long tls_write(tls_conn *c, const void *buf, size_t n);                     /* n on success, < 0 error */
int  tls_shutdown(tls_conn *c);                                             /* sends close_notify */
void tls_free(tls_conn *c);
const tls_info *tls_get_info(const tls_conn *c);
int  tls_alert_received(const tls_conn *c);                                 /* the alert description of TLS_E_ALERT, else -1 */
int  tls_last_error(const tls_conn *c);

/* a tls_io that sends and receives on a connected socket */
void tls_io_fd(tls_io *io, int fd);
/* convenience for a connected socket: new + handshake; on failure returns NULL and stores the error in *err */
tls_conn *tls_connect_fd(int fd, const tls_config *cfg, int *err);
void tls_close(tls_conn *c);                                                /* shutdown + free (the descriptor stays open) */

const char *tls_strerror(int err);
const char *tls_alert_name(int alert);

/* Reports how well seeded the operating system's random number generator is: 1 ready, 0 not yet, -1 unknown. */
int  tls_random_ready(void);

/* Certificate helpers, used by the tests and by fetch -v */
int  tls_x509_match_name(const uint8_t *der, size_t len, const char *host);  /* 0 if the certificate is valid for host */

#ifdef __cplusplus
}
#endif
#endif
