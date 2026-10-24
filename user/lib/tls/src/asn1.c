/* A strict DER reader: just enough ASN.1 for X.509 and ECDSA signatures. */
#include "x509.h"

int asn1_read(const u8 **cur, const u8 *end, unsigned *tag, span *content, span *whole) {
    const u8 *p = *cur;
    if (end - p < 2) return -1;
    unsigned t = p[0];
    if ((t & 0x1f) == 0x1f) return -1;                               /* multi-byte tag numbers do not occur in X.509 */
    size_t len = p[1];
    size_t hdr = 2;
    if (len & 0x80) {
        size_t nb = len & 0x7f;
        if (nb == 0 || nb > 4 || (size_t)(end - p) < 2 + nb) return -1;     /* indefinite lengths are BER, not DER */
        len = 0;
        for (size_t i = 0; i < nb; i++) len = (len << 8) | p[2 + i];
        if (p[2] == 0 || len < 0x80) return -1;                     /* not the shortest encoding */
        hdr = 2 + nb;
    }
    if (len > (size_t)(end - p) - hdr) return -1;
    if (tag) *tag = t;
    if (content) { content->p = p + hdr; content->n = len; }
    if (whole) { whole->p = p; whole->n = hdr + len; }
    *cur = p + hdr + len;
    return 0;
}

int asn1_read_tag(const u8 **cur, const u8 *end, unsigned tag, span *content, span *whole) {
    const u8 *save = *cur;
    unsigned t;
    if (asn1_read(cur, end, &t, content, whole) != 0 || t != tag) {
        *cur = save;
        return -1;
    }
    return 0;
}

int asn1_oid_is(span oid, const u8 *bytes, size_t n) { return oid.n == n && memcmp(oid.p, bytes, n) == 0; }

int asn1_uint(span v, span *mag) {
    if (v.n == 0 || (v.p[0] & 0x80)) return -1;
    if (v.n > 1 && v.p[0] == 0 && !(v.p[1] & 0x80)) return -1;       /* superfluous leading zero */
    if (v.p[0] == 0 && v.n > 1) { mag->p = v.p + 1; mag->n = v.n - 1; }
    else { mag->p = v.p; mag->n = v.n; }
    return 0;
}

/* days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm) */
int64_t asn1_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int digits(const u8 *p, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

int asn1_time(unsigned tag, span v, int64_t *out) {
    int year, off;
    if (tag == 0x17) {                                               /* UTCTime YYMMDDHHMMSSZ */
        if (v.n != 13 || v.p[12] != 'Z') return -1;
        int yy = digits(v.p, 2);
        if (yy < 0) return -1;
        year = yy >= 50 ? 1900 + yy : 2000 + yy;
        off = 2;
    } else if (tag == 0x18) {                                        /* GeneralizedTime YYYYMMDDHHMMSSZ */
        if (v.n != 15 || v.p[14] != 'Z') return -1;
        year = digits(v.p, 4);
        if (year < 0) return -1;
        off = 4;
    } else {
        return -1;
    }
    int mo = digits(v.p + off, 2), da = digits(v.p + off + 2, 2), hh = digits(v.p + off + 4, 2), mi = digits(v.p + off + 6, 2), ss = digits(v.p + off + 8, 2);
    if (mo < 1 || mo > 12 || da < 1 || da > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) return -1;
    static const int mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (da > mdays[mo - 1]) return -1;
    *out = asn1_days_from_civil(year, (unsigned)mo, (unsigned)da) * 86400 + hh * 3600 + mi * 60 + (ss == 60 ? 59 : ss);
    return 0;
}
