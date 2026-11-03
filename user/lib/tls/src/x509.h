/*
 * ASN.1 DER reading and X.509 certificates (internal to libtls; the public face is tls.h). RFC 5280 for the certificate profile,
 * RFC 4055 for RSASSA-PSS parameters, RFC 8410 for Ed25519 keys, RFC 6125 for host names.
 */
#ifndef LIBTLS_X509_H
#define LIBTLS_X509_H

#include "tlsint.h"
#include "tls.h"

typedef struct { const u8 *p; size_t n; } span;

/* ---- DER ---- */
/* Reads the TLV at *cur. Only definite, minimal lengths and one-byte tags are accepted (this is DER, not BER).
 * content = the value bytes, whole = tag + length + value. Either may be NULL. Returns 0, or -1 if malformed. */
int asn1_read(const u8 **cur, const u8 *end, unsigned *tag, span *content, span *whole);
int asn1_read_tag(const u8 **cur, const u8 *end, unsigned tag, span *content, span *whole);   /* -1 if the tag differs */
int asn1_oid_is(span oid, const u8 *bytes, size_t n);
/* A non-negative INTEGER's magnitude: the leading zero byte that keeps it positive is removed. -1 for a negative, empty or non-minimal one. */
int asn1_uint(span integer, span *magnitude);
/* UTCTime / GeneralizedTime to Unix seconds. */
int asn1_time(unsigned tag, span value, int64_t *out);
int64_t asn1_days_from_civil(int64_t y, unsigned m, unsigned d);

/* ---- certificates ---- */
enum { X_SIG_NONE = 0, X_SIG_RSA_PKCS1, X_SIG_RSA_PSS, X_SIG_ECDSA, X_SIG_ED25519,
       X_SIG_WEAK /* SHA-1, MD5, MD2, SHA-224 */, X_SIG_UNSUPPORTED };
enum { X_KEY_NONE = 0, X_KEY_RSA, X_KEY_RSA_PSS, X_KEY_P256, X_KEY_P384, X_KEY_ED25519, X_KEY_OTHER };

#define X_KU_DIGITAL_SIGNATURE 0x0001      /* keyUsage bit n is 1 << n, with n numbered as in RFC 5280 */
#define X_KU_KEY_CERT_SIGN     0x0020
#define X_EKU_SERVER_AUTH      0x01
#define X_EKU_ANY              0x02
#define X_EKU_OTHER            0x04

typedef struct {
    const u8 *der;
    size_t der_len;
    span tbs;                       /* the signed part, header included */
    int version;                    /* 1, 2 or 3 */
    span issuer, subject;           /* the whole Name TLVs, compared byte for byte */
    int64_t not_before, not_after;
    int sig_alg;                    /* X_SIG_* of the signature on this certificate */
    int sig_hash;                   /* TLSC_SHA* for RSA and ECDSA */
    unsigned sig_salt;              /* RSASSA-PSS salt length */
    int key_type;                   /* X_KEY_* of the subject's key */
    unsigned key_bits;
    span rsa_n, rsa_e;              /* magnitudes */
    int key_pss_hash;               /* an RSASSA-PSS key restricted to one hash (0: any) */
    span ec_point;                  /* 04 || X || Y, or the 32 bytes of an Ed25519 key */
    span signature;                 /* the signatureValue bits */
    int has_bc, is_ca, path_len;    /* basicConstraints; path_len -1 = unlimited */
    int has_ku;
    unsigned ku;
    int has_eku;
    unsigned eku;
    int has_san;
    span san;                       /* the GeneralNames SEQUENCE content */
    int has_nc;
    span nc_permitted, nc_excluded; /* nameConstraints GeneralSubtrees contents (empty when absent) */
    span ski, aki;                  /* key identifiers (OCTET STRING contents), empty if absent */
    int unknown_critical;           /* a critical extension we do not understand */
    int self_issued;                /* subject == issuer */
} x509_cert;

int  x509_parse(x509_cert *c, const u8 *der, size_t len);                   /* 0 or TLS_E_CERT_PARSE */
int  x509_check_signature(const x509_cert *c, const x509_cert *issuer);     /* 0 or TLS_E_CERT_SIGNATURE / _UNSUPPORTED / _WEAK */
int  x509_host_is_ipv4(const char *s, u8 ip[4]);                            /* 1 if s is a dotted-quad literal */
int  x509_name_matches(const x509_cert *c, const char *host);               /* 0 or TLS_E_CERT_NAME */
void x509_name_string(span name, char *out, size_t cap);                    /* "C=US, O=Example, CN=host", control characters replaced */
void x509_san_string(const x509_cert *c, char *out, size_t cap);            /* "example.org, *.example.org, 192.168.1.1" */
void x509_describe(const x509_cert *c, TlsCertInfo *out);                   /* fills the public summary */

/* Builds a path from chain[0] (the server's certificate) through candidates chain[1..n-1] to a root of t and checks it.
 * host may be NULL (no name check). Returns 0 or a TLS_E_CERT_* code; on success path[] holds the certificates leaf first, the
 * anchor last (*path_n of them). path may be NULL. */
int  x509_verify_chain(const TlsTrust *t, const x509_cert *chain, int n, const char *host, int64_t now,
                       const x509_cert **path, int *path_n);

/* The signature of the TLS handshake itself (CertificateVerify, ServerKeyExchange) with the public key of cert.
 * scheme is the TLS SignatureScheme; msg is the signed data (it is hashed here). tls13 selects the stricter TLS 1.3 rules. */
int  x509_verify_handshake_signature(const x509_cert *cert, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen);
const char *x509_scheme_name(unsigned scheme);

/* The trust store */
struct TlsTrust {
    x509_cert *anchors;
    u8 **blobs;
    size_t count, cap, skipped;
};
size_t pem_decode_next(const u8 *text, size_t len, const char *label, u8 **der, size_t *derlen);   /* returns the offset after the block, 0 if none */

#endif
