/*
 * PEM files (RFC 7468) and the set of trusted root certificates (TlsTrust). Reading a file goes through os_io.c, so that this part
 * builds unchanged for the host tests.
 */
#include <stdlib.h>
#include "x509.h"
#include "conn.h"

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes standard base64 (white space is skipped); returns the number of bytes or -1. out needs 3/4 of n. */
static long b64_decode(const u8 *in, size_t n, u8 *out) {
    unsigned acc = 0;
    int bits = 0, pad = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int c = in[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') { pad++; continue; }
        int v = b64_val(c);
        if (v < 0 || pad) return -1;                                 /* data after the padding is an error */
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (u8)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    if (pad > 2 || bits >= 6) return -1;
    return (long)o;
}

static const u8 *find_str(const u8 *text, size_t len, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= len; i++)
        if (text[i] == (u8)needle[0] && memcmp(text + i, needle, nl) == 0) return text + i;
    return NULL;
}

size_t pem_decode_next(const u8 *text, size_t len, const char *label, u8 **der, size_t *derlen) {
    char begin[64], endm[64];
    size_t ll = strlen(label);
    if (ll > 40) return 0;
    memcpy(begin, "-----BEGIN ", 11);
    memcpy(begin + 11, label, ll);
    memcpy(begin + 11 + ll, "-----", 6);
    memcpy(endm, "-----END ", 9);
    memcpy(endm + 9, label, ll);
    memcpy(endm + 9 + ll, "-----", 6);
    const u8 *b = find_str(text, len, begin);
    if (!b) return 0;
    const u8 *body = b + strlen(begin);
    const u8 *e = find_str(body, (size_t)(text + len - body), endm);
    if (!e) return 0;
    size_t consumed = (size_t)(e - text) + strlen(endm);
    *der = NULL;
    *derlen = 0;
    size_t blen = (size_t)(e - body);
    u8 *buf = malloc(blen / 4 * 3 + 4);
    if (!buf) return consumed;
    long got = b64_decode(body, blen, buf);
    if (got <= 0) {
        free(buf);
        return consumed;
    }
    *der = buf;
    *derlen = (size_t)got;
    return consumed;
}

/* ---------------------------------------------------------------------------------------------------- the trust store */

TlsTrust *tls_trust_new(void) { return calloc(1, sizeof(TlsTrust)); }

void tls_trust_free(TlsTrust *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) free(t->blobs[i]);
    free(t->blobs);
    free(t->anchors);
    free(t);
}

size_t tls_trust_count(const TlsTrust *t) { return t ? t->count : 0; }
size_t tls_trust_skipped(const TlsTrust *t) { return t ? t->skipped : 0; }

static int usable_root_key(int key_type) {
    return key_type == X_KEY_RSA || key_type == X_KEY_RSA_PSS || key_type == X_KEY_P256 || key_type == X_KEY_P384 || key_type == X_KEY_ED25519;
}

/* takes ownership of der */
static int trust_add(TlsTrust *t, u8 *der, size_t len) {
    if (t->count == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        x509_cert *a = realloc(t->anchors, cap * sizeof *a);
        if (!a) { free(der); return TLS_E_NOMEM; }
        t->anchors = a;
        u8 **b = realloc(t->blobs, cap * sizeof *b);
        if (!b) { free(der); return TLS_E_NOMEM; }
        t->blobs = b;
        t->cap = cap;
    }
    x509_cert *c = &t->anchors[t->count];
    if (x509_parse(c, der, len) != 0 || !usable_root_key(c->key_type)) {
        t->skipped++;
        free(der);
        return 0;
    }
    for (size_t i = 0; i < t->count; i++)                             /* the same root twice in a bundle: keep one */
        if (t->anchors[i].der_len == len && memcmp(t->anchors[i].der, der, len) == 0) {
            free(der);
            return 0;
        }
    t->blobs[t->count++] = der;
    return 1;
}

int tls_trust_add_der(TlsTrust *t, const void *der, size_t len) {
    if (!t || !der || len == 0) return TLS_E_ARG;
    u8 *copy = malloc(len);
    if (!copy) return TLS_E_NOMEM;
    memcpy(copy, der, len);
    return trust_add(t, copy, len);
}

int tls_trust_load_pem(TlsTrust *t, const void *data, size_t len) {
    if (!t || !data) return TLS_E_ARG;
    const u8 *text = data;
    int added = 0;
    for (;;) {
        u8 *der;
        size_t derlen;
        size_t used = pem_decode_next(text, len, "CERTIFICATE", &der, &derlen);
        if (!used) break;
        text += used;
        len -= used;
        if (!der) { t->skipped++; continue; }
        int rc = trust_add(t, der, derlen);
        if (rc < 0) return rc;
        added += rc;
    }
    return added;
}

int tls_trust_load_file(TlsTrust *t, const char *path) {
    if (!t || !path) return TLS_E_ARG;
    u8 *buf;
    size_t len;
    int rc = tls_os_read_file(path, 16u << 20, &buf, &len);
    if (rc) return rc;
    rc = (len > 0 && buf[0] == 0x30) ? tls_trust_add_der(t, buf, len) : tls_trust_load_pem(t, buf, len);
    free(buf);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------- the default store */

static TlsTrust *g_default;
static int g_default_err;
static char g_default_path[256];

/* Loaded once and kept; a failure is not kept, so that a bundle put in place later is found by the next connection. */
const TlsTrust *tls_trust_default(int *err, const char **path) {
    if (!g_default) {
        g_default_err = 0;
        const char *p = tls_os_getenv("SSL_CERT_FILE");
        if (!p || !*p) p = TLS_DEFAULT_CA_FILE;
        size_t n = strlen(p);
        if (n >= sizeof g_default_path) n = sizeof g_default_path - 1;
        memcpy(g_default_path, p, n);
        g_default_path[n] = 0;
        TlsTrust *t = tls_trust_new();
        if (!t) {
            g_default_err = TLS_E_NOMEM;
        } else {
            int rc = tls_trust_load_file(t, g_default_path);
            if (rc < 0 || t->count == 0) {
                g_default_err = rc < 0 ? rc : TLS_E_NO_TRUST;
                tls_trust_free(t);
            } else {
                g_default = t;
            }
        }
    }
    if (err) *err = g_default ? 0 : g_default_err;
    if (path) *path = g_default_path;
    return g_default;
}
