/*
 * X.509 certificates: parsing, signature checks, host names and chain validation (RFC 5280 / RFC 6125, the parts a TLS client needs).
 *
 * Not implemented (and why the failures are closed rather than open): revocation (CRL, OCSP), name constraints and policy
 * processing (a critical extension of those kinds makes the certificate unacceptable), Ed25519 and P-521 keys, RSASSA-PSS
 * signatures on certificates (the TLS handshake itself does use PSS), and the Common Name as a host name (only subjectAltName counts).
 */
#include "x509.h"

#define MAX_CHAIN 10

/* ---------------------------------------------------------------------------------------------------- OIDs (DER contents) */
static const u8 OID_RSA_ENC[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const u8 OID_RSA_SHA256[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const u8 OID_RSA_SHA384[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const u8 OID_RSA_SHA512[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const u8 OID_RSA_SHA1[]   = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05 };
static const u8 OID_RSA_MD5[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x04 };
static const u8 OID_RSA_MD2[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x02 };
static const u8 OID_EC_PUBKEY[]  = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const u8 OID_P256[]       = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const u8 OID_P384[]       = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const u8 OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const u8 OID_ECDSA_SHA384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const u8 OID_ECDSA_SHA512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };
static const u8 OID_ECDSA_SHA1[]   = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01 };
static const u8 OID_ECDSA_SHA224[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x01 };
static const u8 OID_BASIC_CONSTRAINTS[] = { 0x55, 0x1d, 0x13 };
static const u8 OID_KEY_USAGE[]  = { 0x55, 0x1d, 0x0f };
static const u8 OID_EKU[]        = { 0x55, 0x1d, 0x25 };
static const u8 OID_SAN[]        = { 0x55, 0x1d, 0x11 };
static const u8 OID_SKI[]        = { 0x55, 0x1d, 0x0e };
static const u8 OID_AKI[]        = { 0x55, 0x1d, 0x23 };
static const u8 OID_POLICIES[]   = { 0x55, 0x1d, 0x20 };
static const u8 OID_CRLDP[]      = { 0x55, 0x1d, 0x1f };
static const u8 OID_EKU_SERVER[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
static const u8 OID_EKU_ANY[]    = { 0x55, 0x1d, 0x25, 0x00 };

#define OID_IS(span_, arr) asn1_oid_is(span_, arr, sizeof arr)

static int sig_alg_from(span alg) {
    const u8 *p = alg.p, *e = alg.p + alg.n;
    span oid;
    if (asn1_read_tag(&p, e, 0x06, &oid, NULL)) return X_SIG_NONE;
    if (OID_IS(oid, OID_RSA_SHA256)) return X_SIG_RSA_SHA256;
    if (OID_IS(oid, OID_RSA_SHA384)) return X_SIG_RSA_SHA384;
    if (OID_IS(oid, OID_RSA_SHA512)) return X_SIG_RSA_SHA512;
    if (OID_IS(oid, OID_ECDSA_SHA256)) return X_SIG_ECDSA_SHA256;
    if (OID_IS(oid, OID_ECDSA_SHA384)) return X_SIG_ECDSA_SHA384;
    if (OID_IS(oid, OID_ECDSA_SHA512)) return X_SIG_ECDSA_SHA512;
    if (OID_IS(oid, OID_RSA_SHA1) || OID_IS(oid, OID_RSA_MD5) || OID_IS(oid, OID_RSA_MD2) || OID_IS(oid, OID_ECDSA_SHA1) || OID_IS(oid, OID_ECDSA_SHA224)) return X_SIG_WEAK;
    return X_SIG_UNSUPPORTED;
}

/* ---------------------------------------------------------------------------------------------------- parsing */

static int parse_spki(x509_cert *c, span spki) {
    const u8 *p = spki.p, *e = spki.p + spki.n;
    span alg, bits, oid;
    if (asn1_read_tag(&p, e, 0x30, &alg, NULL) || asn1_read_tag(&p, e, 0x03, &bits, NULL) || p != e) return -1;
    if (bits.n < 1 || bits.p[0] != 0) return -1;
    span key = { bits.p + 1, bits.n - 1 };
    const u8 *ap = alg.p, *ae = alg.p + alg.n;
    if (asn1_read_tag(&ap, ae, 0x06, &oid, NULL)) return -1;
    c->key_type = X_KEY_OTHER;
    if (OID_IS(oid, OID_RSA_ENC)) {
        const u8 *kp = key.p, *ke = key.p + key.n;
        span seq, ni, ei;
        if (asn1_read_tag(&kp, ke, 0x30, &seq, NULL) || kp != ke) return -1;
        const u8 *sp = seq.p, *se = seq.p + seq.n;
        if (asn1_read_tag(&sp, se, 0x02, &ni, NULL) || asn1_read_tag(&sp, se, 0x02, &ei, NULL) || sp != se) return -1;
        if (asn1_uint(ni, &c->rsa_n) || asn1_uint(ei, &c->rsa_e)) return -1;
        c->key_type = X_KEY_RSA;
    } else if (OID_IS(oid, OID_EC_PUBKEY)) {
        span curve;
        if (asn1_read_tag(&ap, ae, 0x06, &curve, NULL) == 0) {
            if (OID_IS(curve, OID_P256) && key.n == 65 && key.p[0] == 4) c->key_type = X_KEY_P256;
            else if (OID_IS(curve, OID_P384) && key.n == 97 && key.p[0] == 4) c->key_type = X_KEY_P384;
            c->ec_point = key;
        }
    }
    return 0;
}

static int parse_extensions(x509_cert *c, span wrap) {
    const u8 *p = wrap.p, *e = wrap.p + wrap.n;
    span seq;
    if (asn1_read_tag(&p, e, 0x30, &seq, NULL) || p != e) return -1;
    const u8 *sp = seq.p, *se = seq.p + seq.n;
    while (sp < se) {
        span ext, oid, val, flag;
        if (asn1_read_tag(&sp, se, 0x30, &ext, NULL)) return -1;
        const u8 *xp = ext.p, *xe = ext.p + ext.n;
        int critical = 0;
        if (asn1_read_tag(&xp, xe, 0x06, &oid, NULL)) return -1;
        if (xp < xe && *xp == 0x01) {
            if (asn1_read_tag(&xp, xe, 0x01, &flag, NULL) || flag.n != 1) return -1;
            critical = flag.p[0] != 0;
        }
        if (asn1_read_tag(&xp, xe, 0x04, &val, NULL) || xp != xe) return -1;
        const u8 *vp = val.p, *ve = val.p + val.n;
        if (OID_IS(oid, OID_BASIC_CONSTRAINTS)) {
            span body, b, plen;
            if (asn1_read_tag(&vp, ve, 0x30, &body, NULL) || vp != ve) return -1;
            c->has_bc = 1;
            const u8 *bp = body.p, *be = body.p + body.n;
            if (bp < be && *bp == 0x01) {
                if (asn1_read_tag(&bp, be, 0x01, &b, NULL) || b.n != 1) return -1;
                c->is_ca = b.p[0] != 0;
            }
            if (bp < be) {
                if (asn1_read_tag(&bp, be, 0x02, &plen, NULL) || plen.n == 0 || plen.n > 2 || (plen.p[0] & 0x80)) return -1;
                c->path_len = plen.n == 1 ? plen.p[0] : (plen.p[0] << 8 | plen.p[1]);
            }
            if (bp != be) return -1;
        } else if (OID_IS(oid, OID_KEY_USAGE)) {
            span bits;
            if (asn1_read_tag(&vp, ve, 0x03, &bits, NULL) || vp != ve || bits.n < 2 || bits.p[0] > 7) return -1;
            c->has_ku = 1;
            c->ku = 0;
            for (size_t i = 0; i < bits.n - 1 && i < 2; i++)
                for (int j = 0; j < 8; j++)
                    if ((bits.p[1 + i] >> (7 - j)) & 1) c->ku |= 1u << (8 * i + j);
        } else if (OID_IS(oid, OID_EKU)) {
            span list, o;
            if (asn1_read_tag(&vp, ve, 0x30, &list, NULL) || vp != ve) return -1;
            c->has_eku = 1;
            const u8 *lp = list.p, *le = list.p + list.n;
            while (lp < le) {
                if (asn1_read_tag(&lp, le, 0x06, &o, NULL)) return -1;
                if (OID_IS(o, OID_EKU_SERVER)) c->eku |= X_EKU_SERVER_AUTH;
                else if (OID_IS(o, OID_EKU_ANY)) c->eku |= X_EKU_ANY;
                else c->eku |= X_EKU_OTHER;
            }
        } else if (OID_IS(oid, OID_SAN)) {
            span names;
            if (asn1_read_tag(&vp, ve, 0x30, &names, NULL) || vp != ve) return -1;
            c->has_san = 1;
            c->san = names;
        } else if (OID_IS(oid, OID_SKI)) {
            span k;
            if (asn1_read_tag(&vp, ve, 0x04, &k, NULL) || vp != ve) return -1;
            c->ski = k;
        } else if (OID_IS(oid, OID_AKI)) {
            span body, k;
            if (asn1_read_tag(&vp, ve, 0x30, &body, NULL) || vp != ve) return -1;
            const u8 *bp = body.p, *be = body.p + body.n;
            if (bp < be && *bp == 0x80 && asn1_read_tag(&bp, be, 0x80, &k, NULL) == 0) c->aki = k;
        } else if (OID_IS(oid, OID_POLICIES) || OID_IS(oid, OID_CRLDP)) {
            /* understood well enough to ignore: we neither evaluate policies nor fetch CRLs */
        } else if (critical) {
            c->unknown_critical = 1;
        }
    }
    return 0;
}

int x509_parse(x509_cert *c, const u8 *der, size_t len) {
    memset(c, 0, sizeof *c);
    c->der = der;
    c->der_len = len;
    c->path_len = -1;
    const u8 *p = der, *end = der + len;
    span cert, tbs, tbs_whole, alg, alg_whole, sigbits;
    if (asn1_read_tag(&p, end, 0x30, &cert, NULL) || p != end) return TLS_E_CERT_PARSE;
    const u8 *q = cert.p, *qe = cert.p + cert.n;
    if (asn1_read_tag(&q, qe, 0x30, &tbs, &tbs_whole) || asn1_read_tag(&q, qe, 0x30, &alg, &alg_whole)) return TLS_E_CERT_PARSE;
    if (asn1_read_tag(&q, qe, 0x03, &sigbits, NULL) || q != qe || sigbits.n < 1 || sigbits.p[0] != 0) return TLS_E_CERT_PARSE;
    c->signature.p = sigbits.p + 1;
    c->signature.n = sigbits.n - 1;
    c->tbs = tbs_whole;
    c->sig_alg = sig_alg_from(alg);

    const u8 *t = tbs.p, *te = tbs.p + tbs.n;
    span v, serial, ialg_whole, validity, spki;
    c->version = 1;
    if (t < te && *t == 0xa0) {
        span wrap;
        if (asn1_read_tag(&t, te, 0xa0, &wrap, NULL)) return TLS_E_CERT_PARSE;
        const u8 *vp = wrap.p, *ve = wrap.p + wrap.n;
        if (asn1_read_tag(&vp, ve, 0x02, &v, NULL) || vp != ve || v.n != 1 || v.p[0] > 2) return TLS_E_CERT_PARSE;
        c->version = v.p[0] + 1;
    }
    if (asn1_read_tag(&t, te, 0x02, &serial, NULL) || serial.n == 0 || serial.n > 21) return TLS_E_CERT_PARSE;
    if (asn1_read_tag(&t, te, 0x30, NULL, &ialg_whole)) return TLS_E_CERT_PARSE;
    if (ialg_whole.n != alg_whole.n || memcmp(ialg_whole.p, alg_whole.p, alg_whole.n) != 0) return TLS_E_CERT_PARSE;   /* the two copies must agree */
    if (asn1_read_tag(&t, te, 0x30, NULL, &c->issuer)) return TLS_E_CERT_PARSE;
    if (asn1_read_tag(&t, te, 0x30, &validity, NULL)) return TLS_E_CERT_PARSE;
    {
        const u8 *vp = validity.p, *ve = validity.p + validity.n;
        unsigned tag;
        span tv;
        if (asn1_read(&vp, ve, &tag, &tv, NULL) || asn1_time(tag, tv, &c->not_before)) return TLS_E_CERT_PARSE;
        if (asn1_read(&vp, ve, &tag, &tv, NULL) || asn1_time(tag, tv, &c->not_after) || vp != ve) return TLS_E_CERT_PARSE;
    }
    if (asn1_read_tag(&t, te, 0x30, NULL, &c->subject)) return TLS_E_CERT_PARSE;
    if (asn1_read_tag(&t, te, 0x30, &spki, NULL)) return TLS_E_CERT_PARSE;
    if (parse_spki(c, spki) != 0) return TLS_E_CERT_PARSE;
    if (t < te && *t == 0x81) { span skip; if (asn1_read_tag(&t, te, 0x81, &skip, NULL)) return TLS_E_CERT_PARSE; }
    if (t < te && *t == 0x82) { span skip; if (asn1_read_tag(&t, te, 0x82, &skip, NULL)) return TLS_E_CERT_PARSE; }
    if (t < te) {
        span wrap;
        if (c->version != 3 || asn1_read_tag(&t, te, 0xa3, &wrap, NULL)) return TLS_E_CERT_PARSE;
        if (parse_extensions(c, wrap) != 0) return TLS_E_CERT_PARSE;
    }
    if (t != te) return TLS_E_CERT_PARSE;
    c->self_issued = c->issuer.n == c->subject.n && memcmp(c->issuer.p, c->subject.p, c->issuer.n) == 0;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- signatures */

/* ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER } */
static int ecdsa_sig_decode(const u8 *sig, size_t len, span *r, span *s) {
    const u8 *p = sig, *e = sig + len;
    span seq, ri, si;
    if (asn1_read_tag(&p, e, 0x30, &seq, NULL) || p != e) return -1;
    const u8 *sp = seq.p, *se = seq.p + seq.n;
    if (asn1_read_tag(&sp, se, 0x02, &ri, NULL) || asn1_read_tag(&sp, se, 0x02, &si, NULL) || sp != se) return -1;
    if (asn1_uint(ri, r) || asn1_uint(si, s)) return -1;
    return 0;
}

static int hash_of_sig(int sig_alg, int *hash, int *ec) {
    switch (sig_alg) {
    case X_SIG_RSA_SHA256: *hash = TLSC_SHA256; *ec = 0; return 0;
    case X_SIG_RSA_SHA384: *hash = TLSC_SHA384; *ec = 0; return 0;
    case X_SIG_RSA_SHA512: *hash = TLSC_SHA512; *ec = 0; return 0;
    case X_SIG_ECDSA_SHA256: *hash = TLSC_SHA256; *ec = 1; return 0;
    case X_SIG_ECDSA_SHA384: *hash = TLSC_SHA384; *ec = 1; return 0;
    case X_SIG_ECDSA_SHA512: *hash = TLSC_SHA512; *ec = 1; return 0;
    default: return -1;
    }
}

int x509_check_signature(const x509_cert *c, const x509_cert *iss) {
    int hash, ec;
    if (hash_of_sig(c->sig_alg, &hash, &ec) != 0) return TLS_E_CERT_UNSUPPORTED;
    u8 digest[64];
    tlsc_hash_once(hash, c->tbs.p, c->tbs.n, digest);
    size_t dl = tlsc_hash_len(hash);
    if (!ec) {
        if (iss->key_type == X_KEY_OTHER || iss->key_type == X_KEY_NONE) return TLS_E_CERT_UNSUPPORTED;
        if (iss->key_type != X_KEY_RSA) return TLS_E_CERT_SIGNATURE;
        return tlsc_rsa_verify_pkcs1(iss->rsa_n.p, iss->rsa_n.n, iss->rsa_e.p, iss->rsa_e.n, hash, digest, c->signature.p, c->signature.n) == 0 ? 0 : TLS_E_CERT_SIGNATURE;
    }
    int curve = iss->key_type == X_KEY_P256 ? TLSC_P256 : iss->key_type == X_KEY_P384 ? TLSC_P384 : 0;
    if (!curve) return iss->key_type == X_KEY_OTHER ? TLS_E_CERT_UNSUPPORTED : TLS_E_CERT_SIGNATURE;
    span r, s;
    if (ecdsa_sig_decode(c->signature.p, c->signature.n, &r, &s) != 0) return TLS_E_CERT_SIGNATURE;
    return tlsc_ecdsa_verify(curve, iss->ec_point.p, iss->ec_point.n, digest, dl, r.p, r.n, s.p, s.n) == 0 ? 0 : TLS_E_CERT_SIGNATURE;
}

int x509_verify_handshake_signature(const x509_cert *cert, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen) {
    enum { K_PSS, K_PKCS1, K_ECDSA } kind;
    int hash;
    int want_curve = 0;                                              /* TLS 1.3 fixes the curve in the scheme */
    switch (scheme) {
    case 0x0403: kind = K_ECDSA; hash = TLSC_SHA256; want_curve = TLSC_P256; break;
    case 0x0503: kind = K_ECDSA; hash = TLSC_SHA384; want_curve = TLSC_P384; break;
    case 0x0804: kind = K_PSS; hash = TLSC_SHA256; break;
    case 0x0805: kind = K_PSS; hash = TLSC_SHA384; break;
    case 0x0806: kind = K_PSS; hash = TLSC_SHA512; break;
    case 0x0401: kind = K_PKCS1; hash = TLSC_SHA256; break;
    case 0x0501: kind = K_PKCS1; hash = TLSC_SHA384; break;
    case 0x0601: kind = K_PKCS1; hash = TLSC_SHA512; break;
    case 0x0603: case 0x0807: case 0x0808: return -1;                /* P-521 and EdDSA: not supported */
    default: return -1;
    }
    if (tls13 && kind == K_PKCS1) return -1;                         /* RFC 8446 4.2.3: PKCS#1 v1.5 is for certificates only */
    u8 digest[64];
    tlsc_hash_once(hash, msg, msglen, digest);
    size_t dl = tlsc_hash_len(hash);
    if (kind == K_ECDSA) {
        int curve = cert->key_type == X_KEY_P256 ? TLSC_P256 : cert->key_type == X_KEY_P384 ? TLSC_P384 : 0;
        if (!curve) return -1;
        if (tls13 && curve != want_curve) return -1;
        span r, s;
        if (ecdsa_sig_decode(sig, siglen, &r, &s) != 0) return -1;
        return tlsc_ecdsa_verify(curve, cert->ec_point.p, cert->ec_point.n, digest, dl, r.p, r.n, s.p, s.n) == 0 ? 0 : -1;
    }
    if (cert->key_type != X_KEY_RSA) return -1;
    if (kind == K_PSS)
        return tlsc_rsa_verify_pss(cert->rsa_n.p, cert->rsa_n.n, cert->rsa_e.p, cert->rsa_e.n, hash, digest, sig, siglen, dl) == 0 ? 0 : -1;
    return tlsc_rsa_verify_pkcs1(cert->rsa_n.p, cert->rsa_n.n, cert->rsa_e.p, cert->rsa_e.n, hash, digest, sig, siglen) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------------------------------------- names */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int x509_host_is_ipv4(const char *s, u8 ip[4]) {
    int part = 0, val = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (*p == '.' || *p == 0) {
            if (!digits || part > 3) return 0;
            ip[part++] = (u8)val;
            val = digits = 0;
            if (*p == 0) break;
        } else {
            return 0;
        }
    }
    return part == 4;
}

static int dns_pattern_matches(const u8 *pat, size_t plen, const char *host, size_t hlen) {
    if (plen > 0 && pat[plen - 1] == '.') plen--;
    if (plen == 0 || plen > 253) return 0;
    for (size_t i = 0; i < plen; i++)
        if (pat[i] < 0x21 || pat[i] > 0x7e) return 0;                /* also rejects an embedded NUL: "good.example\0.evil.example" */
    if (pat[0] == '*') {
        if (plen < 5 || pat[1] != '.') return 0;                     /* only a whole left-most label */
        const u8 *rest = pat + 2;
        size_t rlen = plen - 2;
        for (size_t i = 0; i < rlen; i++)
            if (rest[i] == '*') return 0;
        int dots = 0;
        for (size_t i = 0; i < rlen; i++) dots += rest[i] == '.';
        if (dots < 1) return 0;                                      /* "*.com" is not a certificate anybody may have */
        size_t i = 0;
        while (i < hlen && host[i] != '.') i++;                      /* the first label of the host */
        if (i == 0 || i >= hlen) return 0;
        if (hlen - (i + 1) != rlen) return 0;
        for (size_t k = 0; k < rlen; k++)
            if (lower(host[i + 1 + k]) != lower(rest[k])) return 0;
        return 1;
    }
    for (size_t i = 0; i < plen; i++)
        if (pat[i] == '*') return 0;
    if (plen != hlen) return 0;
    for (size_t i = 0; i < plen; i++)
        if (lower(pat[i]) != lower((u8)host[i])) return 0;
    return 1;
}

int x509_name_matches(const x509_cert *c, const char *host) {
    if (!host || !*host || !c->has_san) return TLS_E_CERT_NAME;
    size_t hlen = strlen(host);
    if (hlen > 0 && host[hlen - 1] == '.') hlen--;
    if (hlen == 0 || hlen > 253) return TLS_E_CERT_NAME;
    u8 ip[4];
    int is_ip = x509_host_is_ipv4(host, ip);
    const u8 *p = c->san.p, *e = c->san.p + c->san.n;
    while (p < e) {
        unsigned tag;
        span v;
        if (asn1_read(&p, e, &tag, &v, NULL)) return TLS_E_CERT_NAME;
        if (tag == 0x82 && !is_ip) {                                 /* dNSName */
            if (dns_pattern_matches(v.p, v.n, host, hlen)) return 0;
        } else if (tag == 0x87 && is_ip) {                           /* iPAddress */
            if (v.n == 4 && memcmp(v.p, ip, 4) == 0) return 0;
        }
    }
    return TLS_E_CERT_NAME;
}

int tls_x509_match_name(const uint8_t *der, size_t len, const char *host) {
    x509_cert c;
    if (x509_parse(&c, der, len) != 0) return TLS_E_CERT_PARSE;
    return x509_name_matches(&c, host);
}

int tls_x509_verify_chain(const tls_ctx *ctx, const uint8_t *const *der, const size_t *len, int n, const char *host, int64_t now) {
    x509_cert chain[MAX_CHAIN];
    if (n < 1 || n > MAX_CHAIN) return TLS_E_ARG;
    for (int i = 0; i < n; i++) {
        int rc = x509_parse(&chain[i], der[i], len[i]);
        if (rc) return rc;
    }
    return x509_verify_chain(ctx, chain, n, host, now);
}

int tls_x509_self_check(const uint8_t *der, size_t len) {
    x509_cert c;
    int rc = x509_parse(&c, der, len);
    return rc ? rc : x509_check_signature(&c, &c);
}

/* "C=US, O=Example Inc, CN=example.org": values are cut down to printable ASCII, a certificate must not be able to inject control characters */
static void put(char *out, size_t cap, size_t *n, char ch) {
    if (*n + 1 < cap) out[(*n)++] = ch;
}

static void put_str(char *out, size_t cap, size_t *n, const char *s) {
    while (*s) put(out, cap, n, *s++);
}

/* an object identifier as dotted decimal text */
static void oid_text(span oid, char *out, size_t cap) {
    size_t n = 0;
    unsigned long acc = 0;
    int first = 1;
    for (size_t k = 0; k < oid.n; k++) {
        acc = (acc << 7) | (oid.p[k] & 0x7f);
        if (oid.p[k] & 0x80) continue;
        unsigned long parts[2];
        int count = 1;
        parts[0] = acc;
        if (first) {                                                 /* the first byte carries two arcs: 40 * a + b */
            parts[0] = acc < 80 ? acc / 40 : 2;
            parts[1] = acc < 80 ? acc % 40 : acc - 80;
            count = 2;
            first = 0;
        } else if (n + 1 < cap) {
            out[n++] = '.';
        }
        for (int i = 0; i < count; i++) {
            char tmp[24];
            int len = 0;
            unsigned long v = parts[i];
            do { tmp[len++] = (char)('0' + v % 10); v /= 10; } while (v);
            if (i && n + 1 < cap) out[n++] = '.';
            while (len) {
                if (n + 1 < cap) out[n++] = tmp[len - 1];
                len--;
            }
        }
        acc = 0;
    }
    out[n] = 0;
}

void x509_name_string(span name, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return;
    const u8 *p = name.p, *e = name.p + name.n;
    span seq;
    if (asn1_read_tag(&p, e, 0x30, &seq, NULL)) { out[0] = 0; return; }
    const u8 *rp = seq.p, *re = seq.p + seq.n;
    int first = 1;
    while (rp < re) {
        span set;
        if (asn1_read_tag(&rp, re, 0x31, &set, NULL)) break;
        const u8 *ap = set.p, *ae = set.p + set.n;
        while (ap < ae) {
            span atv, oid, val;
            unsigned vtag;
            if (asn1_read_tag(&ap, ae, 0x30, &atv, NULL)) break;
            const u8 *tp = atv.p, *te = atv.p + atv.n;
            if (asn1_read_tag(&tp, te, 0x06, &oid, NULL) || asn1_read(&tp, te, &vtag, &val, NULL)) break;
            if (!first) put_str(out, cap, &n, ", ");
            first = 0;
            static const struct { u8 oid[3]; const char *name; } known[] = {
                { { 0x55, 0x04, 0x03 }, "CN" }, { { 0x55, 0x04, 0x06 }, "C" }, { { 0x55, 0x04, 0x07 }, "L" }, { { 0x55, 0x04, 0x08 }, "ST" },
                { { 0x55, 0x04, 0x0a }, "O" }, { { 0x55, 0x04, 0x0b }, "OU" },
            };
            const char *label = NULL;
            for (size_t k = 0; k < TLSC_ARRAY_SIZE(known); k++)
                if (oid.n == 3 && memcmp(oid.p, known[k].oid, 3) == 0) label = known[k].name;
            if (label) {
                put_str(out, cap, &n, label);
            } else {
                char buf[64];
                oid_text(oid, buf, sizeof buf);
                put_str(out, cap, &n, buf);
            }
            put(out, cap, &n, '=');
            if (vtag == 0x1e) {                                      /* BMPString: UTF-16BE */
                for (size_t k = 0; k + 1 < val.n; k += 2) put(out, cap, &n, val.p[k] == 0 && val.p[k + 1] >= 0x20 && val.p[k + 1] < 0x7f ? (char)val.p[k + 1] : '?');
            } else {
                for (size_t k = 0; k < val.n; k++) put(out, cap, &n, val.p[k] >= 0x20 && val.p[k] < 0x7f ? (char)val.p[k] : '?');
            }
        }
    }
    out[n] = 0;
}

/* ---------------------------------------------------------------------------------------------------- chains */

static int same_name(span a, span b) { return a.n == b.n && memcmp(a.p, b.p, a.n) == 0; }

/* if both sides carry key identifiers they must agree: it spares signature checks on look-alike candidates */
static int keyid_compatible(const x509_cert *child, const x509_cert *issuer) {
    if (child->aki.n == 0 || issuer->ski.n == 0) return 1;
    return child->aki.n == issuer->ski.n && memcmp(child->aki.p, issuer->ski.p, child->aki.n) == 0;
}

typedef struct {
    const tls_ctx *ctx;
    const x509_cert *chain;
    int n;
    int why;                                                         /* the most telling reason a candidate was refused */
} build_state;

static int find_path(build_state *st, unsigned used, const x509_cert *cur, int depth, int *path, const x509_cert **anchor) {
    for (size_t i = 0; st->ctx && i < st->ctx->count; i++) {
        const x509_cert *a = &st->ctx->anchors[i];
        if (!same_name(a->subject, cur->issuer) || !keyid_compatible(cur, a)) continue;
        int rc = x509_check_signature(cur, a);
        if (rc == 0) { *anchor = a; return depth; }
        if (rc == TLS_E_CERT_UNSUPPORTED || st->why == 0) st->why = rc;
    }
    if (depth >= MAX_CHAIN - 2) return -1;
    for (int j = 1; j < st->n; j++) {
        if (used & (1u << j)) continue;
        const x509_cert *cand = &st->chain[j];
        if (!same_name(cand->subject, cur->issuer) || !keyid_compatible(cur, cand)) continue;
        int rc = x509_check_signature(cur, cand);
        if (rc != 0) {
            if (rc == TLS_E_CERT_UNSUPPORTED || st->why == 0) st->why = rc;
            continue;
        }
        path[depth] = j;
        int r = find_path(st, used | (1u << j), cand, depth + 1, path, anchor);
        if (r >= 0) return r;
    }
    return -1;
}

int x509_verify_chain(const tls_ctx *ctx, const x509_cert *chain, int n, const char *host, int64_t now) {
    if (n < 1) return TLS_E_CERT_EMPTY;
    if (n > MAX_CHAIN) return TLS_E_CERT_UNSUPPORTED;
    /* a server certificate that is itself in the trust store (--cacert server.pem) is trusted as it stands */
    int pinned = 0;
    for (size_t i = 0; ctx && i < ctx->count; i++) {
        const x509_cert *a = &ctx->anchors[i];
        if (a->der_len == chain[0].der_len && memcmp(a->der, chain[0].der, a->der_len) == 0) { pinned = 1; break; }
    }
    const x509_cert *seq[MAX_CHAIN + 1];
    int m = 0;
    seq[m++] = &chain[0];
    if (!pinned) {
        build_state st = { ctx, chain, n, 0 };
        int path[MAX_CHAIN];
        const x509_cert *anchor = NULL;
        int nint = find_path(&st, 1u, &chain[0], 0, path, &anchor);
        if (nint < 0) {
            if (st.why == TLS_E_CERT_UNSUPPORTED || st.why == TLS_E_CERT_SIGNATURE) return st.why;
            /* is the best the server offered a self-signed certificate? */
            const x509_cert *top = &chain[0];
            for (int guard = 0; guard < n; guard++) {
                if (top->self_issued) return TLS_E_CERT_SELF_SIGNED;
                const x509_cert *next = NULL;
                for (int j = 1; j < n; j++)
                    if (&chain[j] != top && same_name(chain[j].subject, top->issuer)) next = &chain[j];
                if (!next) break;
                top = next;
            }
            return TLS_E_CERT_UNTRUSTED;
        }
        for (int d = 0; d < nint; d++) seq[m++] = &chain[path[d]];
        seq[m++] = anchor;
    }

    for (int i = 0; i < m; i++) {
        const x509_cert *c = seq[i];
        int is_anchor = i == m - 1 && !pinned;
        if (!is_anchor && c->unknown_critical) return TLS_E_CERT_UNSUPPORTED;
        if (now < c->not_before) return TLS_E_CERT_NOT_YET_VALID;
        if (now > c->not_after) return TLS_E_CERT_EXPIRED;
        if (!is_anchor && c->key_type == X_KEY_RSA && c->rsa_n.n < 256) return TLS_E_CERT_UNSUPPORTED;      /* RSA below 2048 bits */
        if (i > 0) {                                                 /* a CA */
            if (!is_anchor && (c->version != 3 || !c->has_bc || !c->is_ca)) return TLS_E_CERT_USAGE;
            if (is_anchor && c->has_bc && !c->is_ca) return TLS_E_CERT_USAGE;
            if (c->has_ku && !(c->ku & X_KU_KEY_CERT_SIGN)) return TLS_E_CERT_USAGE;
            if (c->path_len >= 0 && i - 1 > c->path_len) return TLS_E_CERT_USAGE;        /* certificates between this CA and the leaf */
        }
        if (!is_anchor && c->has_eku && !(c->eku & (X_EKU_SERVER_AUTH | X_EKU_ANY))) return TLS_E_CERT_USAGE;
    }
    if (chain[0].has_ku && !(chain[0].ku & X_KU_DIGITAL_SIGNATURE)) return TLS_E_CERT_USAGE;
    if (host) {
        int rc = x509_name_matches(&chain[0], host);
        if (rc != 0) return rc;
    }
    return 0;
}
