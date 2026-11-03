/*
 * The connection object and the pieces shared by the record layer, the two handshakes and the API (internal to libtls).
 *
 * The engine never waits. Records are read from the socket with MSG_DONTWAIT into `in`, decrypted in place and handed up one at
 * a time; records to send are encrypted into the queue `out` and written as far as the socket takes them. A handshake is a state
 * machine driven one complete message at a time (hs13.c, hs12.c), so it can stop wherever input runs out and continue later.
 * Blocking calls are the same calls with a poll() between the attempts (api.c).
 */
#ifndef LIBTLS_CONN_H
#define LIBTLS_CONN_H

#include <stdarg.h>
#include <stdlib.h>
#include "x509.h"

#define TLS_MAX_PLAIN   16384
#define TLS_REC_IN_MAX  (5 + TLS_MAX_PLAIN + 2048)          /* the largest record accepted (TLS 1.2 allows 2048 bytes of expansion) */
#define TLS_INBUF       (2 * TLS_REC_IN_MAX)                /* a record and the start of the next */
#define TLS_REC_OUT_MAX (5 + 8 + TLS_MAX_PLAIN + 1 + 16)    /* the largest record sent */
#define TLS_OUTBUF      (2 * TLS_REC_OUT_MAX + 4096)        /* one application record, plus alerts or a KeyUpdate queued in front */
#define TLS_MAX_HS      (256 * 1024)                        /* the biggest handshake message accepted (a certificate chain) */
#define TLS_MAX_CERTS   10                                  /* certificates of the server's list that are considered */
#define TLS_MAX_IDLE    32                                  /* records in a row that bring nothing new (empty, warnings, CCS) */

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum { HS_HELLO_REQUEST = 0, HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
       HS_CERTIFICATE = 11, HS_SERVER_KEY_EXCHANGE = 12, HS_CERTIFICATE_REQUEST = 13, HS_SERVER_HELLO_DONE = 14,
       HS_CERTIFICATE_VERIFY = 15, HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20, HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254 };
enum { AL_CLOSE_NOTIFY = 0, AL_UNEXPECTED_MESSAGE = 10, AL_BAD_RECORD_MAC = 20, AL_RECORD_OVERFLOW = 22, AL_HANDSHAKE_FAILURE = 40,
       AL_BAD_CERTIFICATE = 42, AL_UNSUPPORTED_CERTIFICATE = 43, AL_CERTIFICATE_EXPIRED = 45, AL_CERTIFICATE_UNKNOWN = 46,
       AL_ILLEGAL_PARAMETER = 47, AL_UNKNOWN_CA = 48, AL_DECODE_ERROR = 50, AL_DECRYPT_ERROR = 51, AL_PROTOCOL_VERSION = 70,
       AL_INTERNAL_ERROR = 80, AL_USER_CANCELED = 90, AL_NO_RENEGOTIATION = 100, AL_MISSING_EXTENSION = 109,
       AL_UNSUPPORTED_EXTENSION = 110, AL_UNRECOGNIZED_NAME = 112, AL_NO_APPLICATION_PROTOCOL = 120 };

/* extension types */
enum { EXT_SERVER_NAME = 0x0000, EXT_SUPPORTED_GROUPS = 0x000a, EXT_EC_POINT_FORMATS = 0x000b, EXT_SIGNATURE_ALGORITHMS = 0x000d,
       EXT_ALPN = 0x0010, EXT_EXTENDED_MASTER_SECRET = 0x0017, EXT_SESSION_TICKET = 0x0023, EXT_PRE_SHARED_KEY = 0x0029,
       EXT_SUPPORTED_VERSIONS = 0x002b, EXT_COOKIE = 0x002c, EXT_SIGNATURE_ALGORITHMS_CERT = 0x0032, EXT_KEY_SHARE = 0x0033,
       EXT_RENEGOTIATION_INFO = 0xff01 };

enum { CIPHER_NONE = 0, CIPHER_AES128, CIPHER_AES256, CIPHER_CHACHA };
enum { AUTH_ANY = 0, AUTH_RSA, AUTH_ECDSA };

#define GROUP_SECP256R1 0x0017
#define GROUP_SECP384R1 0x0018
#define GROUP_X25519    0x001d

enum {
    ST_NEW = 0, ST_WAIT_SH,
    ST13_WAIT_EE, ST13_WAIT_CERT_CR, ST13_WAIT_CERT, ST13_WAIT_CV, ST13_WAIT_FIN,
    ST12_WAIT_CERT, ST12_WAIT_SKE, ST12_WAIT_SHD, ST12_WAIT_CCS, ST12_WAIT_FIN,
    ST_CONNECTED, ST_FAILED
};

typedef struct {
    unsigned id;
    const char *name;
    int version;                                   /* 0x0304 or 0x0303 */
    int cipher;
    int hash;                                      /* the transcript / PRF hash */
    int auth;                                      /* TLS 1.2 only */
} suite_info;

typedef struct {
    int cipher;                                    /* CIPHER_NONE: records go out in the clear */
    tlsc_gcm_ctx gcm;
    u8 key[32];
    u8 iv[12];                                     /* TLS 1.3: the whole IV; TLS 1.2 GCM: the 4-byte implicit part; TLS 1.2 ChaCha: the whole IV */
    u64 seq;
} cipher_state;

/* What our ClientHello offered: read back from the bytes that were sent (hello.c), so the checks of the server's answer are
 * against exactly what went on the wire. */
#define OFFER_MAX 32
typedef struct {
    u8 random[32];
    u8 sid[32];
    size_t sid_len;
    unsigned suites[OFFER_MAX];   int nsuites;
    unsigned groups[OFFER_MAX];   int ngroups;     /* supported_groups */
    unsigned shares[4];           int nshares;     /* groups with a key share */
    unsigned sigalgs[OFFER_MAX];  int nsigalgs;    /* signature_algorithms */
    unsigned exts[OFFER_MAX];     int nexts;       /* extension types, in order */
    int v13, v12;                                  /* versions */
    char alpn[4][32];             int nalpn;
} offer_t;

typedef struct {
    int n;                                         /* scripted ClientHellos (0: build them) */
    const u8 *ch[2];
    size_t ch_len[2];
    u8 x25519[32];
    u8 p256[32];
    int have_x25519, have_p256;
} script_t;

typedef struct {
    const u8 *p;
    size_t n;
    int err;
} rd_t;

static inline u8 rd8(rd_t *r) { if (r->n < 1) { r->err = 1; return 0; } r->n--; return *r->p++; }
static inline unsigned rd16(rd_t *r) { unsigned a = rd8(r); unsigned b = rd8(r); return a << 8 | b; }
static inline unsigned rd24(rd_t *r) { unsigned a = rd8(r); unsigned b = rd8(r); unsigned c = rd8(r); return a << 16 | b << 8 | c; }
static inline const u8 *rdn(rd_t *r, size_t n) {
    if (r->n < n) { r->err = 1; return NULL; }
    const u8 *p = r->p;
    r->p += n;
    r->n -= n;
    return p;
}
static inline rd_t rd_sub(rd_t *r, size_t n) {              /* a reader over the next n bytes */
    rd_t s = { NULL, 0, 0 };
    const u8 *p = rdn(r, n);
    if (p) { s.p = p; s.n = n; } else s.err = 1;
    return s;
}

struct TlsConn {
    int fd;
    unsigned flags;
    int timeout_ms;
    unsigned suites_mask;
    void (*random)(void *ctx, void *buf, size_t n);
    void *random_ctx;
    const TlsTrust *trust;                          /* NULL with TLS_INSECURE when no store could be loaded */
    TlsTrust *own_trust;                            /* loaded from cfg->ca_file: freed with the connection */
    char host[256];                                 /* lower case, without a trailing dot */
    char alpn[32];
    int host_is_ip;

    int state;                                      /* ST_* */
    int err;                                        /* the first fatal error (sticky), 0 */
    int want;                                       /* the last TLS_WANT_* returned */
    char errmsg[400];
    char where[48];                                 /* "hs13.c:231": where the first error was raised (bug reports) */
    int rx_closed, tx_closed;                       /* close_notify received / sent */
    int eof;                                        /* the transport reported end of stream */
    int version;                                    /* negotiated: 0x0304 / 0x0303, 0 until the ServerHello */
    const suite_info *suite;

    cipher_state rd, wr;
    u8 *in;                                         /* TLS_INBUF bytes: received, [in_start, in_end) not yet used */
    size_t in_start, in_end;
    u8 *out;                                        /* TLS_OUTBUF bytes: queued, [out_start, out_end) not yet written */
    size_t out_start, out_end;
    const u8 *app_p;                                /* decrypted application data waiting (points into in) */
    size_t app_len;
    u8 *hs;                                         /* handshake bytes received but not yet consumed */
    size_t hs_len, hs_cap;
    int idle;                                       /* records in a row without progress */
    int first_record;                               /* nothing has come from the server yet */

    tlsc_hash th256, th384;                         /* running transcript hashes: the suite is not known until the ServerHello */
    offer_t offer;
    script_t script;
    int hello_retry;
    unsigned retry_group;
    u8 cookie[512];
    size_t cookie_len;
    int ccs_sent;                                   /* the TLS 1.3 compatibility ChangeCipherSpec has gone out */

    u8 x25519_priv[32], x25519_pub[32];
    int have_x25519;
    u8 ec_priv[2][48];                              /* P-256, P-384 private keys, when made */
    u8 ec_pub[2][97];
    int ec_have[2];
    unsigned sh_group;                              /* the server's group */
    u8 sh_share[97];
    size_t sh_share_len;

    /* TLS 1.3 */
    u8 secret_hs[48], secret_master[48];
    u8 secret_c_hs[48], secret_s_hs[48];
    u8 secret_c_ap[48], secret_s_ap[48];
    int cert_requested;

    /* TLS 1.2 */
    u8 server_random[32];
    u8 master[48];
    u8 pending_key[32], pending_iv[12];             /* the server's keys, armed by its ChangeCipherSpec */
    int ems;
    int alpn_from_sh;
    int legacy_record_version;                      /* the record being queued is the first ClientHello (version 0x0301) */

    x509_cert chain[TLS_MAX_CERTS];
    int chain_n;
    u8 *cert_blob;                                  /* the certificate list, kept while chain[] points into it */
    unsigned sigalg;                                /* the scheme of the server's handshake signature */

    TlsInfo info;
};

/* ---- errors (api.c) ---- */
int  tls_fail_at(TlsConn *c, int err, int alert, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
#define tls_fail(c, err, alert, ...) tls_fail_at((c), (err), (alert), __FILE__, __LINE__, __VA_ARGS__)
const char *tls_cert_error_hint(TlsConn *c, int err, char *buf, size_t cap);

/* ---- records and handshake messages (record.c) ---- */
int    rec_flush(TlsConn *c);                                         /* 0 all written, TLS_WANT_WRITE, < 0 */
int    rec_queue(TlsConn *c, int type, const u8 *data, size_t len);   /* encrypted with the current keys, appended */
int    rec_queue_raw(TlsConn *c, const u8 *data, size_t len);         /* bytes as they are (the dummy ChangeCipherSpec) */
size_t rec_room(const TlsConn *c);                                    /* plaintext that still fits as one more record */
int    rec_next(TlsConn *c, int *type, u8 **data, size_t *len);       /* 0 a record, TLS_WANT_READ, TLS_E_EOF (clean, at a record boundary), < 0 */
void   rec_set_keys(cipher_state *cs, int cipher, const u8 *key, const u8 *iv, size_t ivlen);
int    cipher_keylen(int cipher);
int    hs_send(TlsConn *c, const u8 *msg, size_t len);                /* transcript, then queued as handshake records */
int    hs_next(TlsConn *c, u8 *type, const u8 **msg, size_t *len);    /* the next whole message (4-byte header included) */
void   hs_consume(TlsConn *c, size_t len);
int    hs_append(TlsConn *c, const u8 *d, size_t n);
int    handle_alert(TlsConn *c, const u8 *d, size_t n);               /* 0 to carry on, 1 close_notify, < 0 fatal */
void   transcript_add(TlsConn *c, const u8 *msg, size_t len);
void   transcript_hash(TlsConn *c, int alg, u8 *out);
const suite_info *suite_find(unsigned id);
int    tls_rand(TlsConn *c, void *buf, size_t n);

/* ---- hellos (hello.c) ---- */
int  hello_send(TlsConn *c);                                         /* the first or (after a HelloRetryRequest) second ClientHello */
int  hello_parse_offer(const u8 *msg, size_t len, offer_t *o);         /* reads a ClientHello back; 0 or -1 */
int  hello_process_server(TlsConn *c, const u8 *msg, size_t len);     /* 0 ServerHello, 1 HelloRetryRequest, < 0 */
int  ec_keygen(TlsConn *c, unsigned group);
int  offered(const unsigned *list, int n, unsigned v);

/* ---- the handshakes (hs13.c, hs12.c) ---- */
int  hs13_message(TlsConn *c, unsigned type, const u8 *msg, size_t len);
int  hs13_after_server_hello(TlsConn *c);
int  hs13_post_handshake(TlsConn *c, const u8 *msg, size_t len);
int  hs13_key_update(TlsConn *c, int request_peer);
int  hs12_message(TlsConn *c, unsigned type, const u8 *msg, size_t len);
int  hs12_ccs(TlsConn *c);
int  verify_peer(TlsConn *c, const u8 *list, size_t len, int tls13);   /* the Certificate message's list */
int  check_handshake_signature(TlsConn *c, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen);

/* ---- the clock (clock.c) ---- */
int  clock_now(int64_t *now);                                         /* TLS_CLOCK_* */

/* ---- the operating system (os_io.c; the host build uses the same file) ---- */
int     tls_os_random(void *buf, size_t n);                           /* 0 or -1 */
int64_t tls_os_time(void);
long    tls_os_recv(int fd, void *buf, size_t n);                     /* > 0, 0 end of stream, TLS_WANT_READ, TLS_E_IO */
long    tls_os_send(int fd, const void *buf, size_t n);               /* > 0, TLS_WANT_WRITE, TLS_E_IO */
int     tls_os_wait(int fd, int want, int timeout_ms);                /* 0 ready, TLS_E_TIMEOUT, TLS_E_IO */
int     tls_os_read_file(const char *path, size_t max, u8 **buf, size_t *len);   /* 0, TLS_E_IO, TLS_E_NO_TRUST (missing), TLS_E_NOMEM */
const char *tls_os_getenv(const char *name);
int64_t tls_os_monotonic_ms(void);

/* ---- tests (api.c) ---- */
/* The ClientHellos to send instead of building them, with the private keys of their shares (RFC 8448 replays). */
int tls_test_script(TlsConn *c, const u8 *ch1, size_t ch1_len, const u8 *ch2, size_t ch2_len, const u8 *x25519_priv, const u8 *p256_priv);

#endif
