/* The data types of the SSH protocol (RFC 4251, section 5) and base64. */
#include "sshd.h"
#include <string.h>

void rd_init(Rd *r, const uint8_t *p, size_t n) { r->p = p; r->n = n; r->bad = false; }

uint8_t rd_u8(Rd *r) {
    if (r->n < 1) { r->bad = true; return 0; }
    r->n--;
    return *r->p++;
}

uint32_t rd_u32(Rd *r) {
    if (r->n < 4) { r->bad = true; r->n = 0; return 0; }
    uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4;
    r->n -= 4;
    return v;
}

bool rd_bool(Rd *r) { return rd_u8(r) != 0; }

const uint8_t *rd_str(Rd *r, size_t *len) {
    uint32_t n = rd_u32(r);
    if (r->bad || n > r->n) { r->bad = true; *len = 0; return r->p; }
    const uint8_t *s = r->p;
    r->p += n;
    r->n -= n;
    *len = n;
    return s;
}

void rd_skip(Rd *r, size_t n) {
    if (r->n < n) { r->bad = true; r->n = 0; return; }
    r->p += n;
    r->n -= n;
}

void wr_init(Wr *w, uint8_t *buf, size_t cap) { w->p = buf; w->n = 0; w->cap = cap; w->bad = false; }

void wr_raw(Wr *w, const void *p, size_t n) {
    if (w->bad || n > w->cap - w->n) { w->bad = true; return; }
    memcpy(w->p + w->n, p, n);
    w->n += n;
}

void wr_u8(Wr *w, uint8_t v) { wr_raw(w, &v, 1); }

void wr_u32(Wr *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    wr_raw(w, b, 4);
}

void wr_bool(Wr *w, bool v) { wr_u8(w, v ? 1 : 0); }

void wr_str(Wr *w, const void *p, size_t n) {
    wr_u32(w, (uint32_t)n);
    wr_raw(w, p, n);
}

void wr_cstr(Wr *w, const char *s) { wr_str(w, s, strlen(s)); }

/* A big-endian magnitude as an mpint: no leading zero bytes, and one zero byte in front when the top bit would otherwise read as a sign. */
size_t mpint_encode(uint8_t *out, const uint8_t *be, size_t n) {
    size_t skip = 0;
    while (skip < n && be[skip] == 0) skip++;
    size_t len = n - skip;
    bool pad = len && (be[skip] & 0x80);
    uint32_t total = (uint32_t)(len + pad);
    out[0] = (uint8_t)(total >> 24); out[1] = (uint8_t)(total >> 16); out[2] = (uint8_t)(total >> 8); out[3] = (uint8_t)total;
    size_t o = 4;
    if (pad) out[o++] = 0;
    memcpy(out + o, be + skip, len);
    return o + len;
}

bool str_is(const uint8_t *p, size_t n, const char *s) { return strlen(s) == n && !memcmp(p, s, n); }

/* Name-lists are comma-separated names in one string. */
bool namelist_has(const uint8_t *list, size_t n, const char *name) {
    size_t len = strlen(name), i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == len && !memcmp(list + i, name, len)) return true;
        i = j + 1;
    }
    return false;
}

const char *namelist_pick(const uint8_t *client, size_t n, const char *const *ours) {
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && client[j] != ',') j++;
        for (int k = 0; ours[k]; k++)
            if (strlen(ours[k]) == j - i && !memcmp(client + i, ours[k], j - i)) return ours[k];
        i = j + 1;
    }
    return NULL;
}

void printable(char *out, size_t cap, const uint8_t *p, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) out[o++] = p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '?';
    out[o] = 0;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out, bool pad) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        if (i + 1 < n) out[o++] = B64[v >> 6 & 63]; else if (pad) out[o++] = '=';
        if (i + 2 < n) out[o++] = B64[v & 63]; else if (pad) out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

int b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = in[i];
        if (ch == '=') break;
        const char *p = ch ? strchr(B64, ch) : NULL;
        if (!p) return -1;
        acc = acc << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}
