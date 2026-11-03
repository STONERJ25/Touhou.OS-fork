/*
 * X.509 certificates: parsing, signature checks, host names, name constraints and path validation: the parts of RFC 5280 a TLS
 * client needs, RFC 4055 (RSASSA-PSS parameters), RFC 8410 (Ed25519 keys), RFC 6125 (host names, wildcards).
 *
 * Not implemented, and why the failures are closed rather than open: revocation (CRL, OCSP: nothing is fetched), policy
 * processing (policies are read past, never required), P-521 and Ed448 keys, DSA (refused as unsupported), the Common Name as a
 * host name (only subjectAltName counts, as in current browsers). An unknown *critical* extension makes a certificate unacceptable.
 */
#include <sshcrypto.h>
#include "x509.h"

#define MAX_CHAIN 10
#define PATH_BUDGET 64                      /* signature checks one path search may spend (a hostile chain cannot make it explode) */
#define RSA_MIN_BITS 2048
#define RSA_MAX_BITS 4096

/* ---------------------------------------------------------------------------------------------------- OIDs (DER contents) */
static const u8 OID_RSA_ENC[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
static const u8 OID_RSA_PSS[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0a };
static const u8 OID_MGF1[]       = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x08 };
static const u8 OID_RSA_SHA256[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
static const u8 OID_RSA_SHA384[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
static const u8 OID_RSA_SHA512[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0d };
static const u8 OID_RSA_SHA224[] = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0e };
static const u8 OID_RSA_SHA1[]   = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x05 };
static const u8 OID_RSA_MD5[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x04 };
static const u8 OID_RSA_MD2[]    = { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x02 };
static const u8 OID_SHA1[]       = { 0x2b, 0x0e, 0x03, 0x02, 0x1a };
static const u8 OID_SHA256[]     = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const u8 OID_SHA384[]     = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02 };
static const u8 OID_SHA512[]     = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03 };
static const u8 OID_EC_PUBKEY[]  = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
static const u8 OID_P256[]       = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const u8 OID_P384[]       = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const u8 OID_ECDSA_SHA256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
static const u8 OID_ECDSA_SHA384[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
static const u8 OID_ECDSA_SHA512[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x04 };
static const u8 OID_ECDSA_SHA224[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x01 };
static const u8 OID_ECDSA_SHA1[]   = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x01 };
static const u8 OID_ED25519[]    = { 0x2b, 0x65, 0x70 };
static const u8 OID_BASIC_CONSTRAINTS[] = { 0x55, 0x1d, 0x13 };
static const u8 OID_KEY_USAGE[]  = { 0x55, 0x1d, 0x0f };
static const u8 OID_EKU[]        = { 0x55, 0x1d, 0x25 };
static const u8 OID_SAN[]        = { 0x55, 0x1d, 0x11 };
static const u8 OID_SKI[]        = { 0x55, 0x1d, 0x0e };
static const u8 OID_AKI[]        = { 0x55, 0x1d, 0x23 };
static const u8 OID_NAME_CONSTRAINTS[] = { 0x55, 0x1d, 0x1e };
static const u8 OID_POLICIES[]   = { 0x55, 0x1d, 0x20 };
static const u8 OID_POLICY_MAPPINGS[] = { 0x55, 0x1d, 0x21 };
static const u8 OID_POLICY_CONSTRAINTS[] = { 0x55, 0x1d, 0x24 };
static const u8 OID_INHIBIT_ANY[] = { 0x55, 0x1d, 0x36 };
static const u8 OID_CRLDP[]      = { 0x55, 0x1d, 0x1f };
static const u8 OID_EKU_SERVER[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
static const u8 OID_EKU_ANY[]    = { 0x55, 0x1d, 0x25, 0x00 };

#define OID_IS(span_, arr) asn1_oid_is(span_, arr, sizeof arr)

static int parse_null_or_absent(const u8 *p, const u8 *e) {          /* AlgorithmIdentifier parameters that must be NULL or missing */
    if (p == e) return 0;
    span v;
    return asn1_read_tag(&p, e, 0x05, &v, NULL) == 0 && v.n == 0 && p == e ? 0 : -1;
}

/* HashAlgorithm ::= AlgorithmIdentifier: one of SHA-1/256/384/512 (TLSC_SHA*, 0 for SHA-1), parameters NULL or absent */
static int hash_alg_id(span seq, int *hash) {
    const u8 *p = seq.p, *e = seq.p + seq.n;
    span oid;
    if (asn1_read_tag(&p, e, 0x06, &oid, NULL) || parse_null_or_absent(p, e)) return -1;
    if (OID_IS(oid, OID_SHA256)) *hash = TLSC_SHA256;
    else if (OID_IS(oid, OID_SHA384)) *hash = TLSC_SHA384;
    else if (OID_IS(oid, OID_SHA512)) *hash = TLSC_SHA512;
    else if (OID_IS(oid, OID_SHA1)) *hash = 0;
    else return -1;
    return 0;
}

/* RSASSA-PSS-params (RFC 4055 3.1). Returns 0 with hash (0 = SHA-1, the default) and salt length, -1 if malformed or unusable. */
static int pss_params(span params, int *hash, unsigned *salt) {
    const u8 *p = params.p, *e = params.p + params.n;
    int h = 0, mgf_hash = 0;
    long sl = 20;
    span v;
    if (p < e && *p == 0xa0) {
        span alg;
        if (asn1_read_tag(&p, e, 0xa0, &v, NULL)) return -1;
        const u8 *q = v.p, *qe = v.p + v.n;
        if (asn1_read_tag(&q, qe, 0x30, &alg, NULL) || q != qe || hash_alg_id(alg, &h)) return -1;
    }
    if (p < e && *p == 0xa1) {
        span alg, oid, inner;
        if (asn1_read_tag(&p, e, 0xa1, &v, NULL)) return -1;
        const u8 *q = v.p, *qe = v.p + v.n;
        if (asn1_read_tag(&q, qe, 0x30, &alg, NULL) || q != qe) return -1;
        q = alg.p; qe = alg.p + alg.n;
        if (asn1_read_tag(&q, qe, 0x06, &oid, NULL) || !OID_IS(oid, OID_MGF1)) return -1;
        if (asn1_read_tag(&q, qe, 0x30, &inner, NULL) || q != qe || hash_alg_id(inner, &mgf_hash)) return -1;
    }
    if (p < e && *p == 0xa2) {
        span in, mag;
        if (asn1_read_tag(&p, e, 0xa2, &v, NULL)) return -1;
        const u8 *q = v.p, *qe = v.p + v.n;
        if (asn1_read_tag(&q, qe, 0x02, &in, NULL) || q != qe || asn1_uint(in, &mag) || mag.n > 2) return -1;
        sl = 0;
        for (size_t i = 0; i < mag.n; i++) sl = sl << 8 | mag.p[i];
    }
    if (p < e && *p == 0xa3) {
        span in;
        if (asn1_read_tag(&p, e, 0xa3, &v, NULL)) return -1;
        const u8 *q = v.p, *qe = v.p + v.n;
        if (asn1_read_tag(&q, qe, 0x02, &in, NULL) || q != qe || in.n != 1 || in.p[0] != 1) return -1;   /* trailerFieldBC */
    }
    if (p != e || h != mgf_hash || sl > 64) return -1;               /* MGF1 must use the same hash (as every real signer does) */
    *hash = h;
    *salt = (unsigned)sl;
    return 0;
}

/* The signature AlgorithmIdentifier (content of the SEQUENCE) */
static void sig_alg_from(x509_cert *c, span alg) {
    const u8 *p = alg.p, *e = alg.p + alg.n;
    span oid;
    c->sig_alg = X_SIG_UNSUPPORTED;
    if (asn1_read_tag(&p, e, 0x06, &oid, NULL)) return;
    if (OID_IS(oid, OID_RSA_SHA256) || OID_IS(oid, OID_RSA_SHA384) || OID_IS(oid, OID_RSA_SHA512)) {
        if (parse_null_or_absent(p, e)) return;
        c->sig_alg = X_SIG_RSA_PKCS1;
        c->sig_hash = OID_IS(oid, OID_RSA_SHA256) ? TLSC_SHA256 : OID_IS(oid, OID_RSA_SHA384) ? TLSC_SHA384 : TLSC_SHA512;
    } else if (OID_IS(oid, OID_ECDSA_SHA256) || OID_IS(oid, OID_ECDSA_SHA384) || OID_IS(oid, OID_ECDSA_SHA512)) {
        if (parse_null_or_absent(p, e)) return;                      /* RFC 5758 says absent; a NULL is tolerated */
        c->sig_alg = X_SIG_ECDSA;
        c->sig_hash = OID_IS(oid, OID_ECDSA_SHA256) ? TLSC_SHA256 : OID_IS(oid, OID_ECDSA_SHA384) ? TLSC_SHA384 : TLSC_SHA512;
    } else if (OID_IS(oid, OID_ED25519)) {
        if (p != e) return;                                          /* RFC 8410: the parameters MUST be absent */
        c->sig_alg = X_SIG_ED25519;
    } else if (OID_IS(oid, OID_RSA_PSS)) {
        span params;
        int h;
        unsigned salt;
        if (asn1_read_tag(&p, e, 0x30, &params, NULL) || p != e) {
            c->sig_alg = X_SIG_WEAK;                                 /* no parameters: all defaults, which means SHA-1 */
            return;
        }
        if (pss_params(params, &h, &salt) != 0) return;
        if (h == 0) { c->sig_alg = X_SIG_WEAK; return; }
        c->sig_alg = X_SIG_RSA_PSS;
        c->sig_hash = h;
        c->sig_salt = salt;
    } else if (OID_IS(oid, OID_RSA_SHA1) || OID_IS(oid, OID_RSA_MD5) || OID_IS(oid, OID_RSA_MD2) || OID_IS(oid, OID_ECDSA_SHA1) ||
               OID_IS(oid, OID_RSA_SHA224) || OID_IS(oid, OID_ECDSA_SHA224)) {
        c->sig_alg = X_SIG_WEAK;
    }
}

/* ---------------------------------------------------------------------------------------------------- parsing */

static unsigned bit_length(span mag) {
    if (mag.n == 0) return 0;
    unsigned top = mag.p[0], bits = 0;
    while (top) { bits++; top >>= 1; }
    return (unsigned)(mag.n - 1) * 8 + bits;
}

static int parse_rsa_key(x509_cert *c, span key) {
    const u8 *kp = key.p, *ke = key.p + key.n;
    span seq, ni, ei;
    if (asn1_read_tag(&kp, ke, 0x30, &seq, NULL) || kp != ke) return -1;
    const u8 *sp = seq.p, *se = seq.p + seq.n;
    if (asn1_read_tag(&sp, se, 0x02, &ni, NULL) || asn1_read_tag(&sp, se, 0x02, &ei, NULL) || sp != se) return -1;
    if (asn1_uint(ni, &c->rsa_n) || asn1_uint(ei, &c->rsa_e)) return -1;
    c->key_bits = bit_length(c->rsa_n);
    return 0;
}

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
        if (parse_null_or_absent(ap, ae) || parse_rsa_key(c, key)) return -1;
        c->key_type = X_KEY_RSA;
    } else if (OID_IS(oid, OID_RSA_PSS)) {                           /* a key that may only make PSS signatures (RFC 4055 1.2) */
        span params;
        if (ap < ae) {
            int h;
            unsigned salt;
            if (asn1_read_tag(&ap, ae, 0x30, &params, NULL) || ap != ae || pss_params(params, &h, &salt) || h == 0) return 0;  /* unusable: X_KEY_OTHER */
            c->key_pss_hash = h;
        }
        if (parse_rsa_key(c, key)) return -1;
        c->key_type = X_KEY_RSA_PSS;
    } else if (OID_IS(oid, OID_EC_PUBKEY)) {
        span curve;
        if (asn1_read_tag(&ap, ae, 0x06, &curve, NULL) == 0 && ap == ae) {
            if (OID_IS(curve, OID_P256) && key.n == 65 && key.p[0] == 4) { c->key_type = X_KEY_P256; c->key_bits = 256; }
            else if (OID_IS(curve, OID_P384) && key.n == 97 && key.p[0] == 4) { c->key_type = X_KEY_P384; c->key_bits = 384; }
            c->ec_point = key;
        }
    } else if (OID_IS(oid, OID_ED25519)) {
        if (ap != ae || key.n != 32) return -1;
        c->key_type = X_KEY_ED25519;
        c->key_bits = 256;
        c->ec_point = key;
    }
    return 0;
}

/* GeneralName structure check: [n] tagged, primitive or constructed per RFC 5280's choice */
static int general_names_valid(span names) {
    const u8 *p = names.p, *e = names.p + names.n;
    while (p < e) {
        unsigned tag;
        span v;
        if (asn1_read(&p, e, &tag, &v, NULL)) return -1;
        if ((tag & 0xc0) != 0x80) return -1;                         /* every GeneralName is context-specific */
        if (tag == 0x82 && v.n == 0) return -1;                      /* an empty dNSName (RFC 5280 4.2.1.6) */
        if (tag == 0x87 && v.n != 4 && v.n != 16) return -1;         /* iPAddress is 4 or 16 octets in a SAN */
    }
    return 0;
}

/* GeneralSubtrees content: SEQUENCE OF GeneralSubtree { base GeneralName, minimum [0] DEFAULT 0, maximum [1] absent } */
static int subtrees_valid(span st) {
    const u8 *p = st.p, *e = st.p + st.n;
    if (p == e) return -1;                                           /* SIZE (1..MAX) */
    while (p < e) {
        span sub, v;
        unsigned tag;
        if (asn1_read_tag(&p, e, 0x30, &sub, NULL)) return -1;
        const u8 *q = sub.p, *qe = sub.p + sub.n;
        if (asn1_read(&q, qe, &tag, &v, NULL) || (tag & 0xc0) != 0x80) return -1;
        if (tag == 0x87 && v.n != 8 && v.n != 32) return -1;         /* address and mask */
        if (q < qe && *q == 0x80) {                                  /* minimum: must be 0 */
            if (asn1_read_tag(&q, qe, 0x80, &v, NULL) || v.n != 1 || v.p[0] != 0) return -1;
        }
        if (q != qe) return -1;                                      /* maximum MUST be absent */
    }
    return 0;
}

#define MAX_EXT 40

static int parse_extensions(x509_cert *c, span wrap) {
    const u8 *p = wrap.p, *e = wrap.p + wrap.n;
    span seq;
    if (asn1_read_tag(&p, e, 0x30, &seq, NULL) || p != e) return -1;
    const u8 *sp = seq.p, *se = seq.p + seq.n;
    span seen[MAX_EXT];
    int nseen = 0;
    while (sp < se) {
        span ext, oid, val, flag;
        if (asn1_read_tag(&sp, se, 0x30, &ext, NULL)) return -1;
        const u8 *xp = ext.p, *xe = ext.p + ext.n;
        int critical = 0;
        if (asn1_read_tag(&xp, xe, 0x06, &oid, NULL)) return -1;
        for (int i = 0; i < nseen; i++)                              /* RFC 5280 4.2: one instance of an extension per certificate */
            if (seen[i].n == oid.n && memcmp(seen[i].p, oid.p, oid.n) == 0) return -1;
        if (nseen < MAX_EXT) seen[nseen++] = oid;
        else return -1;
        if (xp < xe && *xp == 0x01) {
            /* DER says TRUE is ff and a FALSE default is left out; an explicit FALSE (00) occurs in real certificates and is read */
            if (asn1_read_tag(&xp, xe, 0x01, &flag, NULL) || flag.n != 1 || (flag.p[0] != 0xff && flag.p[0] != 0)) return -1;
            critical = flag.p[0] == 0xff;
        }
        if (asn1_read_tag(&xp, xe, 0x04, &val, NULL) || xp != xe) return -1;
        const u8 *vp = val.p, *ve = val.p + val.n;
        if (OID_IS(oid, OID_BASIC_CONSTRAINTS)) {
            span body, b, plen;
            if (asn1_read_tag(&vp, ve, 0x30, &body, NULL) || vp != ve) return -1;
            c->has_bc = 1;
            const u8 *bp = body.p, *be = body.p + body.n;
            if (bp < be && *bp == 0x01) {
                if (asn1_read_tag(&bp, be, 0x01, &b, NULL) || b.n != 1 || (b.p[0] != 0xff && b.p[0] != 0)) return -1;
                c->is_ca = b.p[0] == 0xff;
            }
            if (bp < be) {
                span mag;
                if (asn1_read_tag(&bp, be, 0x02, &plen, NULL) || asn1_uint(plen, &mag) || mag.n > 2) return -1;
                c->path_len = mag.n == 0 ? 0 : mag.n == 1 ? mag.p[0] : (mag.p[0] << 8 | mag.p[1]);
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
            if (asn1_read_tag(&vp, ve, 0x30, &list, NULL) || vp != ve || list.n == 0) return -1;
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
            if (asn1_read_tag(&vp, ve, 0x30, &names, NULL) || vp != ve || names.n == 0 || general_names_valid(names)) return -1;
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
        } else if (OID_IS(oid, OID_NAME_CONSTRAINTS)) {
            span body, st;
            if (asn1_read_tag(&vp, ve, 0x30, &body, NULL) || vp != ve) return -1;
            const u8 *bp = body.p, *be = body.p + body.n;
            if (bp < be && *bp == 0xa0) {
                if (asn1_read_tag(&bp, be, 0xa0, &st, NULL) || subtrees_valid(st)) return -1;
                c->nc_permitted = st;
            }
            if (bp < be && *bp == 0xa1) {
                if (asn1_read_tag(&bp, be, 0xa1, &st, NULL) || subtrees_valid(st)) return -1;
                c->nc_excluded = st;
            }
            if (bp != be || (c->nc_permitted.n == 0 && c->nc_excluded.n == 0)) return -1;
            c->has_nc = 1;
        } else if (OID_IS(oid, OID_POLICIES) || OID_IS(oid, OID_POLICY_MAPPINGS) || OID_IS(oid, OID_POLICY_CONSTRAINTS) ||
                   OID_IS(oid, OID_INHIBIT_ANY) || OID_IS(oid, OID_CRLDP)) {
            /* understood well enough to read past: no policy is ever required here and nothing is fetched */
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
    sig_alg_from(c, alg);

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
    /* serial: positive up to 20 octets (RFC 5280 4.1.2.2); zero and negative ones exist in the wild and are tolerated */
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
    if (t < te && *t == 0x81) { span skip; if (c->version < 2 || asn1_read_tag(&t, te, 0x81, &skip, NULL)) return TLS_E_CERT_PARSE; }
    if (t < te && *t == 0x82) { span skip; if (c->version < 2 || asn1_read_tag(&t, te, 0x82, &skip, NULL)) return TLS_E_CERT_PARSE; }
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

static int rsa_key_ok(const x509_cert *k) {
    if (k->key_bits < RSA_MIN_BITS) return TLS_E_CERT_WEAK;
    if (k->key_bits > RSA_MAX_BITS) return TLS_E_CERT_UNSUPPORTED;
    return 0;
}

int x509_check_signature(const x509_cert *c, const x509_cert *iss) {
    u8 digest[64];
    int rc;
    switch (c->sig_alg) {
    case X_SIG_WEAK: return TLS_E_CERT_WEAK;
    case X_SIG_RSA_PKCS1:
        if (iss->key_type != X_KEY_RSA) return iss->key_type == X_KEY_OTHER ? TLS_E_CERT_UNSUPPORTED : TLS_E_CERT_SIGNATURE;
        if ((rc = rsa_key_ok(iss))) return rc;
        tlsc_hash_once(c->sig_hash, c->tbs.p, c->tbs.n, digest);
        return tlsc_rsa_verify_pkcs1(iss->rsa_n.p, iss->rsa_n.n, iss->rsa_e.p, iss->rsa_e.n, c->sig_hash, digest, c->signature.p, c->signature.n) == 0 ? 0 : TLS_E_CERT_SIGNATURE;
    case X_SIG_RSA_PSS:
        if (iss->key_type != X_KEY_RSA && iss->key_type != X_KEY_RSA_PSS) return iss->key_type == X_KEY_OTHER ? TLS_E_CERT_UNSUPPORTED : TLS_E_CERT_SIGNATURE;
        if (iss->key_type == X_KEY_RSA_PSS && iss->key_pss_hash && iss->key_pss_hash != c->sig_hash) return TLS_E_CERT_SIGNATURE;
        if ((rc = rsa_key_ok(iss))) return rc;
        tlsc_hash_once(c->sig_hash, c->tbs.p, c->tbs.n, digest);
        return tlsc_rsa_verify_pss(iss->rsa_n.p, iss->rsa_n.n, iss->rsa_e.p, iss->rsa_e.n, c->sig_hash, digest, c->signature.p, c->signature.n, c->sig_salt) == 0 ? 0 : TLS_E_CERT_SIGNATURE;
    case X_SIG_ECDSA: {
        int curve = iss->key_type == X_KEY_P256 ? TLSC_P256 : iss->key_type == X_KEY_P384 ? TLSC_P384 : 0;
        if (!curve) return iss->key_type == X_KEY_OTHER ? TLS_E_CERT_UNSUPPORTED : TLS_E_CERT_SIGNATURE;
        span r, s;
        if (ecdsa_sig_decode(c->signature.p, c->signature.n, &r, &s) != 0) return TLS_E_CERT_SIGNATURE;
        tlsc_hash_once(c->sig_hash, c->tbs.p, c->tbs.n, digest);
        return tlsc_ecdsa_verify(curve, iss->ec_point.p, iss->ec_point.n, digest, tlsc_hash_len(c->sig_hash), r.p, r.n, s.p, s.n) == 0 ? 0 : TLS_E_CERT_SIGNATURE;
    }
    case X_SIG_ED25519:
        if (iss->key_type != X_KEY_ED25519) return iss->key_type == X_KEY_OTHER ? TLS_E_CERT_UNSUPPORTED : TLS_E_CERT_SIGNATURE;
        if (c->signature.n != 64) return TLS_E_CERT_SIGNATURE;
        return ed25519_verify(c->signature.p, c->tbs.p, c->tbs.n, iss->ec_point.p) == 0 ? 0 : TLS_E_CERT_SIGNATURE;
    default:
        return TLS_E_CERT_UNSUPPORTED;
    }
}

const char *x509_scheme_name(unsigned scheme) {
    switch (scheme) {
    case 0x0403: return "ecdsa_secp256r1_sha256";
    case 0x0503: return "ecdsa_secp384r1_sha384";
    case 0x0804: return "rsa_pss_rsae_sha256";
    case 0x0805: return "rsa_pss_rsae_sha384";
    case 0x0806: return "rsa_pss_rsae_sha512";
    case 0x0809: return "rsa_pss_pss_sha256";
    case 0x080a: return "rsa_pss_pss_sha384";
    case 0x080b: return "rsa_pss_pss_sha512";
    case 0x0807: return "ed25519";
    case 0x0401: return "rsa_pkcs1_sha256";
    case 0x0501: return "rsa_pkcs1_sha384";
    case 0x0601: return "rsa_pkcs1_sha512";
    default: return "unknown";
    }
}

int x509_verify_handshake_signature(const x509_cert *cert, unsigned scheme, int tls13, const u8 *msg, size_t msglen, const u8 *sig, size_t siglen) {
    enum { K_PSS, K_PSS_PSS, K_PKCS1, K_ECDSA, K_ED25519 } kind;
    int hash = 0;
    int want_curve = 0;                                              /* TLS 1.3 ties the curve to the scheme */
    switch (scheme) {
    case 0x0403: kind = K_ECDSA; hash = TLSC_SHA256; want_curve = TLSC_P256; break;
    case 0x0503: kind = K_ECDSA; hash = TLSC_SHA384; want_curve = TLSC_P384; break;
    case 0x0804: kind = K_PSS; hash = TLSC_SHA256; break;
    case 0x0805: kind = K_PSS; hash = TLSC_SHA384; break;
    case 0x0806: kind = K_PSS; hash = TLSC_SHA512; break;
    case 0x0809: kind = K_PSS_PSS; hash = TLSC_SHA256; break;
    case 0x080a: kind = K_PSS_PSS; hash = TLSC_SHA384; break;
    case 0x080b: kind = K_PSS_PSS; hash = TLSC_SHA512; break;
    case 0x0807: kind = K_ED25519; break;
    case 0x0401: kind = K_PKCS1; hash = TLSC_SHA256; break;
    case 0x0501: kind = K_PKCS1; hash = TLSC_SHA384; break;
    case 0x0601: kind = K_PKCS1; hash = TLSC_SHA512; break;
    default: return -1;                                              /* SHA-1, P-521, Ed448: never offered */
    }
    if (tls13 && kind == K_PKCS1) return -1;                         /* RFC 8446 4.2.3: PKCS#1 v1.5 is for certificates only */
    if (kind == K_ED25519) {
        if (cert->key_type != X_KEY_ED25519 || siglen != 64) return -1;
        return ed25519_verify(sig, msg, msglen, cert->ec_point.p) == 0 ? 0 : -1;
    }
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
    if (kind == K_PSS_PSS) {                                         /* the key itself is an RSASSA-PSS key */
        if (cert->key_type != X_KEY_RSA_PSS || (cert->key_pss_hash && cert->key_pss_hash != hash)) return -1;
    } else if (cert->key_type != X_KEY_RSA) {
        return -1;
    }
    if (cert->key_bits > RSA_MAX_BITS) return -1;
    if (kind == K_PKCS1)
        return tlsc_rsa_verify_pkcs1(cert->rsa_n.p, cert->rsa_n.n, cert->rsa_e.p, cert->rsa_e.n, hash, digest, sig, siglen) == 0 ? 0 : -1;
    return tlsc_rsa_verify_pss(cert->rsa_n.p, cert->rsa_n.n, cert->rsa_e.p, cert->rsa_e.n, hash, digest, sig, siglen, dl) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------------------------------------- names */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int x509_host_is_ipv4(const char *s, u8 ip[4]) {
    int part = 0, val = 0, digits = 0;
    for (const char *p = s;; p++) {
        if (*p >= '0' && *p <= '9') {
            if (digits && val == 0) return 0;                        /* "01.2.3.4": leading zeros are octal elsewhere; refuse */
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

static int ieq(const u8 *a, const u8 *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

/* a name as it may appear in a certificate: printable ASCII without spaces (this also rejects an embedded NUL:
 * "good.example\0.evil.example") */
static int printable_name(const u8 *p, size_t n) {
    if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x21 || p[i] > 0x7e) return 0;
    return 1;
}

static int dns_pattern_matches(const u8 *pat, size_t plen, const char *host, size_t hlen) {
    if (plen > 0 && pat[plen - 1] == '.') plen--;
    if (!printable_name(pat, plen)) return 0;
    if (pat[0] == '*') {
        if (plen < 5 || pat[1] != '.') return 0;                     /* only a whole left-most label: "*.example.org" */
        const u8 *rest = pat + 2;
        size_t rlen = plen - 2;
        int dots = 0;
        for (size_t i = 0; i < rlen; i++) {
            if (rest[i] == '*') return 0;
            dots += rest[i] == '.';
        }
        if (dots < 1) return 0;                                      /* "*.com" is not a certificate anybody may have */
        size_t i = 0;
        while (i < hlen && host[i] != '.') i++;                      /* the first label of the host */
        if (i == 0 || i >= hlen) return 0;
        if (hlen - (i + 1) != rlen) return 0;
        return ieq((const u8 *)host + i + 1, rest, rlen);
    }
    for (size_t i = 0; i < plen; i++)
        if (pat[i] == '*') return 0;
    return plen == hlen && ieq(pat, (const u8 *)host, plen);
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
        if (tag == 0x82 && !is_ip) {                                 /* dNSName (an address never matches a DNS name) */
            if (dns_pattern_matches(v.p, v.n, host, hlen)) return 0;
        } else if (tag == 0x87 && is_ip) {                           /* iPAddress */
            if (v.n == 4 && memcmp(v.p, ip, 4) == 0) return 0;
        }
    }
    return TLS_E_CERT_NAME;
}

/* ---------------------------------------------------------------------------------------------------- name constraints */

/* Is the DNS name (or the base of a wildcard) inside the subtree of base? RFC 5280 4.2.1.10: adding labels to the left of base
 * stays inside; a base that starts with '.' (common practice) means strictly below it. Empty base: everything. */
static int dns_in_subtree(const u8 *name, size_t nlen, const u8 *base, size_t blen) {
    if (blen > 0 && base[blen - 1] == '.') blen--;
    if (nlen > 0 && name[nlen - 1] == '.') nlen--;
    if (blen == 0) return 1;
    if (base[0] == '.') return nlen > blen && ieq(name + nlen - blen, base, blen);
    if (nlen == blen) return ieq(name, base, blen);
    return nlen > blen && name[nlen - blen - 1] == '.' && ieq(name + nlen - blen, base, blen);
}

/* wildcard "*.rest": every expansion inside the subtree (permitted) / some expansion inside it (excluded) */
static int dns_name_in(const u8 *name, size_t nlen, const u8 *base, size_t blen, int excluded) {
    if (nlen >= 2 && name[0] == '*' && name[1] == '.') {
        const u8 *rest = name + 2;
        size_t rlen = nlen - 2;
        if (blen > 0 && base[0] == '.') {                            /* ".b": expansions L.rest are below b when rest is b or below it */
            if ((rlen == blen - 1 && ieq(rest, base + 1, rlen)) || dns_in_subtree(rest, rlen, base, blen)) return 1;
            return 0;
        }
        if (dns_in_subtree(rest, rlen, base, blen)) return 1;
        if (excluded) {                                              /* base = "L.rest" for one label L: the wildcard can hit it */
            size_t i = 0;
            while (i < blen && base[i] != '.') i++;
            if (i > 0 && i < blen && blen - i - 1 == rlen && ieq(base + i + 1, rest, rlen)) return 1;
        }
        return 0;
    }
    return dns_in_subtree(name, nlen, base, blen);
}

/* directoryName: the subject's RDNs start with the constraint's RDNs (compared as encoded) */
static int dn_in_subtree(span subject, span base_name) {
    const u8 *sp = subject.p, *se = subject.p + subject.n, *bp = base_name.p, *be = base_name.p + base_name.n;
    span sseq, bseq;
    if (asn1_read_tag(&sp, se, 0x30, &sseq, NULL) || asn1_read_tag(&bp, be, 0x30, &bseq, NULL)) return 0;
    return bseq.n <= sseq.n && memcmp(sseq.p, bseq.p, bseq.n) == 0;
}

enum { NF_DNS = 1, NF_IP = 2, NF_DN = 4 };

/* Does one name of form `form` lie in any subtree of the list? Sets *forms to the forms the list constrains. */
static int in_subtrees(span list, int form, const u8 *name, size_t nlen, span dn, int excluded, int *forms) {
    const u8 *p = list.p, *e = list.p + list.n;
    int hit = 0;
    while (p < e) {
        span sub, v;
        unsigned tag;
        if (asn1_read_tag(&p, e, 0x30, &sub, NULL)) return 0;
        const u8 *q = sub.p, *qe = sub.p + sub.n;
        if (asn1_read(&q, qe, &tag, &v, NULL)) return 0;
        if (tag == 0x82) {
            *forms |= NF_DNS;
            if (form == NF_DNS && dns_name_in(name, nlen, v.p, v.n, excluded)) hit = 1;
        } else if (tag == 0x87) {
            *forms |= NF_IP;
            if (form == NF_IP && v.n == 8 && nlen == 4) {
                int in = 1;
                for (int i = 0; i < 4; i++)
                    if ((name[i] & v.p[4 + i]) != (v.p[i] & v.p[4 + i])) in = 0;
                if (in) hit = 1;
            }
        } else if (tag == 0xa4) {                                    /* directoryName [4] EXPLICIT Name */
            *forms |= NF_DN;
            if (form == NF_DN) {
                span inner;
                const u8 *ip = v.p, *ie = v.p + v.n;
                if (asn1_read_tag(&ip, ie, 0x30, NULL, &inner) == 0 && dn_in_subtree(dn, inner)) hit = 1;
            }
        }
    }
    return hit;
}

/* One name against a CA's constraints: excluded nowhere, and permitted somewhere when the CA permits names of its form. */
static int name_allowed(const x509_cert *ca, int form, const u8 *name, size_t nlen, span dn) {
    int forms = 0;
    if (ca->nc_excluded.n && in_subtrees(ca->nc_excluded, form, name, nlen, dn, 1, &forms)) return 0;
    if (ca->nc_permitted.n) {
        forms = 0;
        int hit = in_subtrees(ca->nc_permitted, form, name, nlen, dn, 0, &forms);
        if ((forms & form) && !hit) return 0;
    }
    return 1;
}

/* The leaf's names (subjectAltName DNS names and addresses, and its subject) against the constraints of a CA above it. */
static int check_name_constraints(const x509_cert *ca, const x509_cert *leaf) {
    span none = { NULL, 0 };
    if (leaf->subject.n > 2 && !name_allowed(ca, NF_DN, NULL, 0, leaf->subject)) return TLS_E_CERT_CONSTRAINTS;
    if (!leaf->has_san) return 0;
    const u8 *p = leaf->san.p, *e = leaf->san.p + leaf->san.n;
    while (p < e) {
        unsigned tag;
        span v;
        if (asn1_read(&p, e, &tag, &v, NULL)) return TLS_E_CERT_CONSTRAINTS;
        if (tag == 0x82 && !name_allowed(ca, NF_DNS, v.p, v.n, none)) return TLS_E_CERT_CONSTRAINTS;
        if (tag == 0x87 && v.n == 4 && !name_allowed(ca, NF_IP, v.p, v.n, none)) return TLS_E_CERT_CONSTRAINTS;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- public helpers */

int tls_cert_matches_host(const void *der, size_t len, const char *host) {
    x509_cert c;
    if (x509_parse(&c, der, len) != 0) return TLS_E_CERT_PARSE;
    return x509_name_matches(&c, host);
}

int tls_verify_chain(const TlsTrust *t, const void *const *der, const size_t *len, int n, const char *host, int64_t now) {
    x509_cert chain[MAX_CHAIN];
    if (n < 1 || n > MAX_CHAIN) return TLS_E_ARG;
    for (int i = 0; i < n; i++) {
        int rc = x509_parse(&chain[i], der[i], len[i]);
        if (rc) return rc;
    }
    return x509_verify_chain(t, chain, n, host, now, NULL, NULL);
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

void x509_san_string(const x509_cert *c, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return;
    out[0] = 0;
    if (!c->has_san) return;
    const u8 *p = c->san.p, *e = c->san.p + c->san.n;
    int first = 1;
    while (p < e) {
        unsigned tag;
        span v;
        if (asn1_read(&p, e, &tag, &v, NULL)) break;
        if (tag != 0x82 && !(tag == 0x87 && v.n == 4)) continue;
        if (!first) put_str(out, cap, &n, ", ");
        first = 0;
        if (tag == 0x82) {
            for (size_t k = 0; k < v.n; k++) put(out, cap, &n, v.p[k] >= 0x20 && v.p[k] < 0x7f ? (char)v.p[k] : '?');
        } else {
            for (int k = 0; k < 4; k++) {
                char tmp[4];
                int len = 0;
                unsigned b = v.p[k];
                do { tmp[len++] = (char)('0' + b % 10); b /= 10; } while (b);
                while (len) put(out, cap, &n, tmp[--len]);
                if (k < 3) put(out, cap, &n, '.');
            }
        }
    }
    out[n] = 0;
}

static const char *hash_name(int h) { return h == TLSC_SHA256 ? "SHA-256" : h == TLSC_SHA384 ? "SHA-384" : h == TLSC_SHA512 ? "SHA-512" : "?"; }

static void put_uint(char *out, size_t cap, size_t *n, unsigned v) {
    char tmp[12];
    int len = 0;
    do { tmp[len++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (len) put(out, cap, n, tmp[--len]);
}

void x509_describe(const x509_cert *c, TlsCertInfo *out) {
    memset(out, 0, sizeof *out);
    x509_name_string(c->subject, out->subject, sizeof out->subject);
    x509_name_string(c->issuer, out->issuer, sizeof out->issuer);
    x509_san_string(c, out->names, sizeof out->names);
    out->not_before = c->not_before;
    out->not_after = c->not_after;
    size_t n = 0;
    switch (c->key_type) {
    case X_KEY_RSA: put_str(out->key, sizeof out->key, &n, "RSA "); put_uint(out->key, sizeof out->key, &n, c->key_bits); break;
    case X_KEY_RSA_PSS: put_str(out->key, sizeof out->key, &n, "RSA-PSS "); put_uint(out->key, sizeof out->key, &n, c->key_bits); break;
    case X_KEY_P256: put_str(out->key, sizeof out->key, &n, "EC P-256"); break;
    case X_KEY_P384: put_str(out->key, sizeof out->key, &n, "EC P-384"); break;
    case X_KEY_ED25519: put_str(out->key, sizeof out->key, &n, "Ed25519"); break;
    default: put_str(out->key, sizeof out->key, &n, "unsupported"); break;
    }
    out->key[n] = 0;
    n = 0;
    switch (c->sig_alg) {
    case X_SIG_RSA_PKCS1: put_str(out->sig, sizeof out->sig, &n, "RSA "); put_str(out->sig, sizeof out->sig, &n, hash_name(c->sig_hash)); break;
    case X_SIG_RSA_PSS: put_str(out->sig, sizeof out->sig, &n, "RSA-PSS "); put_str(out->sig, sizeof out->sig, &n, hash_name(c->sig_hash)); break;
    case X_SIG_ECDSA: put_str(out->sig, sizeof out->sig, &n, "ECDSA "); put_str(out->sig, sizeof out->sig, &n, hash_name(c->sig_hash)); break;
    case X_SIG_ED25519: put_str(out->sig, sizeof out->sig, &n, "Ed25519"); break;
    case X_SIG_WEAK: put_str(out->sig, sizeof out->sig, &n, "weak (SHA-1 or MD5)"); break;
    default: put_str(out->sig, sizeof out->sig, &n, "unsupported"); break;
    }
    out->sig[n] = 0;
    tlsc_hash_once(TLSC_SHA256, c->der, c->der_len, out->sha256);
}

int tls_cert_describe(const void *der, size_t len, TlsCertInfo *out) {
    x509_cert c;
    if (x509_parse(&c, der, len) != 0) return TLS_E_CERT_PARSE;
    x509_describe(&c, out);
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- paths */

static int same_name(span a, span b) { return a.n == b.n && memcmp(a.p, b.p, a.n) == 0; }

/* if both sides carry key identifiers they must agree: it spares signature checks on look-alike candidates */
static int keyid_compatible(const x509_cert *child, const x509_cert *issuer) {
    if (child->aki.n == 0 || issuer->ski.n == 0) return 1;
    return child->aki.n == issuer->ski.n && memcmp(child->aki.p, issuer->ski.p, child->aki.n) == 0;
}

static int date_check(const x509_cert *c, int64_t now) {
    if (now < c->not_before) return TLS_E_CERT_NOT_YET_VALID;
    if (now > c->not_after) return TLS_E_CERT_EXPIRED;
    return 0;
}

typedef struct {
    const TlsTrust *t;
    const x509_cert *chain;
    int n;
    int64_t now;
    int budget;
    int why;                                                         /* the most telling reason a candidate was refused */
    const x509_cert *path[MAX_CHAIN + 1];
    int depth;                                                       /* certificates in path[] */
} build_state;

/* The reasons, from the least to the most informative: a refused candidate that was close to working tells more than none. */
static int why_rank(int rc) {
    switch (rc) {
    case 0: return 0;
    case TLS_E_CERT_UNTRUSTED: return 1;
    case TLS_E_CERT_SIGNATURE: return 2;
    case TLS_E_CERT_USAGE: return 3;
    case TLS_E_CERT_CONSTRAINTS: return 4;
    case TLS_E_CERT_UNSUPPORTED: return 5;
    case TLS_E_CERT_WEAK: return 6;
    case TLS_E_CERT_EXPIRED: case TLS_E_CERT_NOT_YET_VALID: return 7;
    default: return 1;
    }
}

static void note(build_state *st, int rc) {
    if (why_rank(rc) > why_rank(st->why)) st->why = rc;
}

/* May `ca` issue the certificate at position `below` of the path (0 = the leaf)? `anchor`: it comes from the trust store. */
static int issuer_acceptable(build_state *st, const x509_cert *ca, int below, int anchor) {
    int rc;
    if ((rc = date_check(ca, st->now))) return rc;
    if (ca->key_type == X_KEY_OTHER || ca->key_type == X_KEY_NONE) return TLS_E_CERT_UNSUPPORTED;
    if (!anchor) {
        if (ca->unknown_critical) return TLS_E_CERT_UNSUPPORTED;
        if (ca->version != 3 || !ca->has_bc || !ca->is_ca) return TLS_E_CERT_USAGE;
        if (ca->has_eku && !(ca->eku & (X_EKU_SERVER_AUTH | X_EKU_ANY))) return TLS_E_CERT_USAGE;
    } else if (ca->has_bc && !ca->is_ca) {
        return TLS_E_CERT_USAGE;
    }
    if (ca->has_ku && !(ca->ku & X_KU_KEY_CERT_SIGN)) return TLS_E_CERT_USAGE;
    /* pathLenConstraint counts the non-self-issued intermediates between this CA and the leaf */
    if (ca->path_len >= 0) {
        int between = 0;
        for (int i = 1; i <= below; i++)
            if (!st->path[i]->self_issued) between++;
        if (between > ca->path_len) return TLS_E_CERT_USAGE;
    }
    if (ca->has_nc && (rc = check_name_constraints(ca, st->path[0]))) return rc;
    return 0;
}

static int try_issuer(build_state *st, const x509_cert *cur, const x509_cert *cand, int below, int anchor) {
    if (!same_name(cand->subject, cur->issuer) || !keyid_compatible(cur, cand)) return -1;
    int rc = issuer_acceptable(st, cand, below, anchor);
    if (rc) { note(st, rc); return -1; }
    if (st->budget-- <= 0) return -1;
    rc = x509_check_signature(cur, cand);
    if (rc) { note(st, rc); return -1; }
    return 0;
}

/* Depth-first: from path[depth-1] upwards, a trust anchor first, then the certificates the server sent. 1 when a path is found. */
static int find_path(build_state *st, unsigned used) {
    const x509_cert *cur = st->path[st->depth - 1];
    int below = st->depth - 1;
    for (size_t i = 0; st->t && i < st->t->count; i++) {
        const x509_cert *a = &st->t->anchors[i];
        if (try_issuer(st, cur, a, below, 1) == 0) {
            st->path[st->depth++] = a;
            return 1;
        }
    }
    if (st->depth >= MAX_CHAIN) return 0;
    for (int j = 1; j < st->n; j++) {
        if (used & (1u << j)) continue;
        const x509_cert *cand = &st->chain[j];
        if (try_issuer(st, cur, cand, below, 0) != 0) continue;
        st->path[st->depth++] = cand;
        if (find_path(st, used | (1u << j))) return 1;
        st->depth--;
        if (st->budget <= 0) return 0;
    }
    return 0;
}

int x509_verify_chain(const TlsTrust *t, const x509_cert *chain, int n, const char *host, int64_t now, const x509_cert **path, int *path_n) {
    if (n < 1) return TLS_E_CERT_EMPTY;
    if (n > MAX_CHAIN) n = MAX_CHAIN;
    const x509_cert *leaf = &chain[0];
    if (path_n) *path_n = 0;
    if (leaf->unknown_critical) return TLS_E_CERT_UNSUPPORTED;
    if (leaf->key_type == X_KEY_OTHER || leaf->key_type == X_KEY_NONE) return TLS_E_CERT_UNSUPPORTED;
    if ((leaf->key_type == X_KEY_RSA || leaf->key_type == X_KEY_RSA_PSS) && rsa_key_ok(leaf)) return rsa_key_ok(leaf);
    if (leaf->has_ku && !(leaf->ku & X_KU_DIGITAL_SIGNATURE)) return TLS_E_CERT_USAGE;           /* every suite here signs */
    if (leaf->has_eku && !(leaf->eku & (X_EKU_SERVER_AUTH | X_EKU_ANY))) return TLS_E_CERT_USAGE;
    int rc = date_check(leaf, now);
    if (rc) return rc;

    build_state st;
    memset(&st, 0, sizeof st);
    st.t = t;
    st.chain = chain;
    st.n = n;
    st.now = now;
    st.budget = PATH_BUDGET;
    st.path[0] = leaf;
    st.depth = 1;
    /* a server certificate that is itself in the trust store (https-get --ca server.pem) is trusted as it stands */
    int pinned = 0;
    for (size_t i = 0; t && i < t->count; i++) {
        const x509_cert *a = &t->anchors[i];
        if (a->der_len == leaf->der_len && memcmp(a->der, leaf->der, a->der_len) == 0) { pinned = 1; break; }
    }
    if (!pinned && !find_path(&st, 1u)) {
        if (st.why && st.why != TLS_E_CERT_UNTRUSTED) return st.why;
        /* is the best the server offered a self-signed certificate? */
        const x509_cert *top = leaf;
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
    if (host && (rc = x509_name_matches(leaf, host))) return rc;
    if (path) for (int i = 0; i < st.depth; i++) path[i] = st.path[i];
    if (path_n) *path_n = st.depth;
    return 0;
}
