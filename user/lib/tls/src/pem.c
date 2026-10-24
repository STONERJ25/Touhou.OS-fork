/* PEM files and the set of trusted root certificates (tls_ctx). File reading is in os_io.c so that this part also builds for the host tests. */
#include <stdlib.h>
#include "x509.h"

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

tls_ctx *tls_ctx_new(void) { return calloc(1, sizeof(tls_ctx)); }

void tls_ctx_free(tls_ctx *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->count; i++) free(ctx->blobs[i]);
    free(ctx->blobs);
    free(ctx->anchors);
    free(ctx);
}

size_t tls_ctx_count(const tls_ctx *ctx) { return ctx ? ctx->count : 0; }
size_t tls_ctx_skipped(const tls_ctx *ctx) { return ctx ? ctx->skipped : 0; }

/* takes ownership of der */
static int ctx_add(tls_ctx *ctx, u8 *der, size_t len) {
    if (ctx->count == ctx->cap) {
        size_t cap = ctx->cap ? ctx->cap * 2 : 64;
        x509_cert *a = realloc(ctx->anchors, cap * sizeof *a);
        if (!a) { free(der); return TLS_E_NOMEM; }
        ctx->anchors = a;
        u8 **b = realloc(ctx->blobs, cap * sizeof *b);
        if (!b) { free(der); return TLS_E_NOMEM; }
        ctx->blobs = b;
        ctx->cap = cap;
    }
    x509_cert *c = &ctx->anchors[ctx->count];
    if (x509_parse(c, der, len) != 0 || (c->key_type != X_KEY_RSA && c->key_type != X_KEY_P256 && c->key_type != X_KEY_P384)) {
        ctx->skipped++;
        free(der);
        return 0;
    }
    ctx->blobs[ctx->count++] = der;
    return 1;
}

int tls_ctx_load_der(tls_ctx *ctx, const void *data, size_t len) {
    if (!ctx || !data || len == 0) return TLS_E_ARG;
    u8 *copy = malloc(len);
    if (!copy) return TLS_E_NOMEM;
    memcpy(copy, data, len);
    return ctx_add(ctx, copy, len);
}

int tls_ctx_load_pem(tls_ctx *ctx, const void *data, size_t len) {
    if (!ctx || !data) return TLS_E_ARG;
    const u8 *text = data;
    int added = 0;
    for (;;) {
        u8 *der;
        size_t derlen;
        size_t used = pem_decode_next(text, len, "CERTIFICATE", &der, &derlen);
        if (!used) break;
        text += used;
        len -= used;
        if (!der) { ctx->skipped++; continue; }
        int rc = ctx_add(ctx, der, derlen);
        if (rc < 0) return rc;
        added += rc;
    }
    return added;
}
