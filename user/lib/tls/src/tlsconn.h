/* The connection object and the helpers shared by the record layer and the two handshakes (internal to libtls). */
#ifndef LIBTLS_CONN_H
#define LIBTLS_CONN_H

#include <stdlib.h>
#include "x509.h"

#define TLS_MAX_PLAIN   16384
#define TLS_REC_MAX     (5 + TLS_MAX_PLAIN + 2048)
#define TLS_MAX_HS      (160 * 1024)               /* the biggest handshake message we accept (a certificate chain) */
#define TLS_MAX_CERTS   10

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11,
       HS_SERVER_KEY_EXCHANGE = 12, HS_CERTIFICATE_REQUEST = 13, HS_SERVER_HELLO_DONE = 14, HS_CERTIFICATE_VERIFY = 15,
       HS_CLIENT_KEY_EXCHANGE = 16, HS_FINISHED = 20, HS_KEY_UPDATE = 24, HS_MESSAGE_HASH = 254 };
enum { AL_CLOSE_NOTIFY = 0, AL_UNEXPECTED_MESSAGE = 10, AL_BAD_RECORD_MAC = 20, AL_RECORD_OVERFLOW = 22, AL_HANDSHAKE_FAILURE = 40,
       AL_BAD_CERTIFICATE = 42, AL_UNSUPPORTED_CERTIFICATE = 43, AL_CERTIFICATE_REVOKED = 44, AL_CERTIFICATE_EXPIRED = 45,
       AL_CERTIFICATE_UNKNOWN = 46, AL_ILLEGAL_PARAMETER = 47, AL_UNKNOWN_CA = 48, AL_ACCESS_DENIED = 49, AL_DECODE_ERROR = 50,
       AL_DECRYPT_ERROR = 51, AL_PROTOCOL_VERSION = 70, AL_INTERNAL_ERROR = 80, AL_MISSING_EXTENSION = 109, AL_UNSUPPORTED_EXTENSION = 110,
       AL_UNRECOGNIZED_NAME = 112 };

enum { CIPHER_NONE = 0, CIPHER_AES128, CIPHER_AES256, CIPHER_CHACHA };
enum { AUTH_ANY = 0, AUTH_RSA, AUTH_ECDSA };

/* named groups */
#define GROUP_SECP256R1 0x0017
#define GROUP_SECP384R1 0x0018
#define GROUP_X25519    0x001d

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

struct tls_conn {
    tls_io io;
    tls_config cfg;
    char sni[256];
    char alpn_offer[16];
    int sni_is_name;                                /* server_name is a host name (not an IP literal) */

    int version;                                    /* negotiated: 0x0304 / 0x0303 */
    const suite_info *suite;
    int state;                                      /* 0 new, 1 handshaking, 2 established, 3 closed, 4 failed */
    int rx_eof, close_notify_sent, close_notify_rx;
    int last_error, alert_rx;
    int hello_retry;
    int first_hello_sent;

    cipher_state rd, wr;

    u8 *rbuf;                                       /* one record in, TLS_REC_MAX bytes */
    const u8 *app_p;                                /* decrypted application data waiting in rbuf */
    size_t app_len;
    u8 *wbuf;                                       /* one record out */

    u8 *hs;                                         /* handshake bytes received but not yet consumed */
    size_t hs_len, hs_cap;

    tlsc_hash th256, th384;                         /* running transcript hashes: the suite is not known until the ServerHello */
    int hash_alg;                                   /* of the suite, once known */

    u8 client_random[32], server_random[32];
    u8 session_id[32];
    u8 x25519_priv[32], x25519_pub[32];
    u8 ec_priv[2][48];                              /* P-256, P-384 private keys, if generated */
    u8 ec_pub[2][97];
    int ec_have[2];
    u8 cookie[256];
    size_t cookie_len;
    unsigned offered_suites[16];
    int n_offered;
    int offer_13, offer_12;

    u8 ch_copy[1024];                               /* the first ClientHello, kept for the HelloRetryRequest transcript rule */
    size_t ch_len;
    u8 sh_share[97];                                /* the key share of the ServerHello */
    size_t sh_share_len;
    int sh_alpn_seen;

    /* TLS 1.3 */
    u8 secret_hs[48], secret_master[48];
    u8 secret_c_hs[48], secret_s_hs[48];
    u8 secret_c_ap[48], secret_s_ap[48];

    /* TLS 1.2 */
    u8 master[48];
    int ems;
    int sh_group;                                   /* the server's chosen group */

    int cert_requested;
    x509_cert chain[TLS_MAX_CERTS];
    int chain_n;
    u8 *cert_blob;                                  /* the Certificate message, kept while the chain is in use */
    size_t cert_blob_len;

    tls_info info;
};

/* record layer and handshake plumbing (tls.c) */
int  tls_rand(struct tls_conn *c, void *buf, size_t n);
int  tls_fail(struct tls_conn *c, int err, int alert);
int  tls_send_record(struct tls_conn *c, int type, const u8 *data, size_t len);
int  tls_recv_record(struct tls_conn *c, int *type, u8 **data, size_t *len);        /* 0 ok, 1 clean end of stream at a record boundary, < 0 error */
int  tls_send_alert(struct tls_conn *c, int level, int desc);
int  tls_send_raw(struct tls_conn *c, const u8 *data, size_t len);                    /* bytes straight to the transport (the dummy ChangeCipherSpec) */
int  tls_send_handshake(struct tls_conn *c, const u8 *msg, size_t len);           /* hashes and sends */
int  tls_get_handshake(struct tls_conn *c, u8 *type, const u8 **msg, size_t *len);   /* a complete message including its 4-byte header */
void tls_hs_consume(struct tls_conn *c, size_t len);
void tls_transcript_add(struct tls_conn *c, const u8 *msg, size_t len);
void tls_transcript_hash(struct tls_conn *c, int alg, u8 *out);
void tls_cipher_set(cipher_state *cs, int cipher, const u8 *key, const u8 *iv, size_t ivlen);
int  tls_cipher_keylen(int cipher);
const suite_info *tls_suite_find(unsigned id);
int64_t tls_now(struct tls_conn *c);
int  tls_verify_peer(struct tls_conn *c, const u8 *cert_list, size_t cert_list_len, int tls13);   /* parses and validates the chain; sets info */
int  tls_check_signature(struct tls_conn *c, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen);

/* ClientHello / ServerHello (hello.c) */
int  tls_send_client_hello(struct tls_conn *c, unsigned retry_group);
int  tls_process_server_hello(struct tls_conn *c, const u8 *msg, size_t len, unsigned *retry_group);   /* 0 done, 1 HelloRetryRequest, < 0 error */
int  tls_ec_keygen(struct tls_conn *c, unsigned group);                                              /* makes our P-256 / P-384 key if we have none */

/* the two handshakes */
int  tls13_client(struct tls_conn *c);
int  tls12_client(struct tls_conn *c);
int  tls13_handle_post_handshake(struct tls_conn *c, const u8 *msg, size_t len);

/* OS services (os_io.c; the host tests provide their own) */
int  tls_os_random(void *buf, size_t n);
int  tls_os_random_ready(void);
int64_t tls_os_time(void);

#endif
