/* Address conversion: inet_aton / inet_addr / inet_ntoa, and inet_pton / inet_ntop for IPv4 and IPv6 (RFC 4291 section 2.2 for the forms that are read, RFC 5952 for the form that is written; tests/host/test_net6.py
 * compares both with the C library of the machine it runs on). */
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

/* a.b.c.d, a.b.c (c is 16 bits), a.b (b is 24 bits) or a (32 bits); each part decimal, 0x hex or 0 octal */
int inet_aton(const char *s, struct in_addr *out) {
    unsigned long parts[4];
    int n = 0;
    while (1) {
        if (!isdigit((unsigned char)*s)) return 0;
        unsigned long v = 0;
        int base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0' && isdigit((unsigned char)s[1])) { base = 8; s++; }
        int digits = 0;
        while (1) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (base == 16 && ((*s | 32) >= 'a' && (*s | 32) <= 'f')) d = (*s | 32) - 'a' + 10;
            else break;
            if (d >= base) return 0;
            v = v * (unsigned long)base + (unsigned long)d;
            if (v > 0xFFFFFFFFul) return 0;
            s++;
            digits++;
        }
        if (!digits && base != 8) return 0;
        parts[n++] = v;
        if (*s == '.') { if (n == 4) return 0; s++; continue; }
        break;
    }
    if (*s && !isspace((unsigned char)*s)) return 0;
    unsigned long addr;
    switch (n) {
    case 1: addr = parts[0]; break;
    case 2: if (parts[0] > 255 || parts[1] > 0xFFFFFF) return 0; addr = parts[0] << 24 | parts[1]; break;
    case 3: if (parts[0] > 255 || parts[1] > 255 || parts[2] > 0xFFFF) return 0; addr = parts[0] << 24 | parts[1] << 16 | parts[2]; break;
    default: if (parts[0] > 255 || parts[1] > 255 || parts[2] > 255 || parts[3] > 255) return 0;
             addr = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3]; break;
    }
    if (out) out->s_addr = htonl((uint32_t)addr);
    return 1;
}

in_addr_t inet_addr(const char *s) {
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[INET_ADDRSTRLEN];
    uint32_t a = ntohl(in.s_addr);
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
    return buf;
}

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;

/* A dotted quad in strict form (four decimal numbers of one to three digits, no leading zeros, none above 255), the whole of [s, s+len). */
static int pton4(const char *s, size_t len, unsigned char out[4]) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (i >= len || s[i] < '0' || s[i] > '9') return 0;
        unsigned v = 0;
        size_t digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (unsigned)(s[i] - '0');
            if (++digits > 3 || v > 255) return 0;
            i++;
        }
        if (digits > 1 && s[i - digits] == '0') return 0;                        /* leading zeros are not allowed in the strict form */
        out[part] = (unsigned char)v;
        if (part < 3) { if (i >= len || s[i] != '.') return 0; i++; }
    }
    return i == len;
}

/* The groups of an IPv6 address between s and e: "h:h:h", each of one to four hex digits (RFC 4291 section 2.2). The last token may be a dotted quad (it counts as two groups) when
 * `tail` says this run is the end of the whole address. Returns the number of groups stored, or -1 if the text is not that. */
static int groups6(const char *s, const char *e, uint16_t *out, int max, int tail) {
    int n = 0;
    if (s == e) return 0;
    for (;;) {
        const char *te = s;
        while (te < e && *te != ':') te++;
        size_t len = (size_t)(te - s);
        if (len == 0) return -1;                                                /* a single colon at the start or end, or three in a row */
        int last = te == e;
        if (last && tail && memchr(s, '.', len)) {
            unsigned char q[4];
            if (!pton4(s, len, q) || n + 2 > max) return -1;
            out[n++] = (uint16_t)(q[0] << 8 | q[1]);
            out[n++] = (uint16_t)(q[2] << 8 | q[3]);
            return n;
        }
        if (len > 4) return -1;
        unsigned v = 0;
        for (size_t i = 0; i < len; i++) {
            int c = (unsigned char)s[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            v = v << 4 | (unsigned)d;
        }
        if (n >= max) return -1;
        out[n++] = (uint16_t)v;
        if (last) return n;
        s = te + 1;
    }
}

static int pton6(const char *src, unsigned char *dst) {
    size_t len = strlen(src);
    uint16_t g[8];
    const char *dc = strstr(src, "::");
    if (!dc) {
        if (groups6(src, src + len, g, 8, 1) != 8) return 0;
    } else {
        uint16_t l[8], r[8];
        if (strstr(dc + 1, "::")) return 0;                                     /* a second "::" (and ":::") */
        int nl = groups6(src, dc, l, 8, 0), nr = groups6(dc + 2, src + len, r, 8, 1);
        if (nl < 0 || nr < 0 || nl + nr > 7) return 0;                          /* "::" stands for one group or more */
        for (int i = 0; i < 8; i++) g[i] = 0;
        for (int i = 0; i < nl; i++) g[i] = l[i];
        for (int i = 0; i < nr; i++) g[8 - nr + i] = r[i];
    }
    for (int i = 0; i < 8; i++) { dst[2 * i] = (unsigned char)(g[i] >> 8); dst[2 * i + 1] = (unsigned char)g[i]; }
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af == AF_INET6) {
        unsigned char tmp[16];
        if (!pton6(src, tmp)) return 0;
        memcpy(dst, tmp, 16);
        return 1;
    }
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    unsigned char tmp[4];
    if (!pton4(src, strlen(src), tmp)) return 0;
    memcpy(dst, tmp, 4);
    return 1;
}

/* RFC 5952 section 4: lower case, no leading zeros, the longest run of two or more zero groups (the first of equal ones) written "::"; ::a.b.c.d and ::ffff:a.b.c.d keep their dotted
 * quad, as the common implementations print them. */
static void ntop6(const unsigned char *a, char *out) {
    uint16_t w[8];
    for (int i = 0; i < 8; i++) w[i] = (uint16_t)(a[2 * i] << 8 | a[2 * i + 1]);
    int best_base = -1, best_len = 0, cur_base = -1, cur_len = 0;
    for (int i = 0; i < 8; i++) {
        if (w[i] == 0) {
            if (cur_base < 0) { cur_base = i; cur_len = 1; } else cur_len++;
        } else if (cur_base >= 0) {
            if (best_base < 0 || cur_len > best_len) { best_base = cur_base; best_len = cur_len; }
            cur_base = -1;
        }
    }
    if (cur_base >= 0 && (best_base < 0 || cur_len > best_len)) { best_base = cur_base; best_len = cur_len; }
    if (best_base >= 0 && best_len < 2) best_base = -1;
    char *o = out;
    for (int i = 0; i < 8; i++) {
        if (best_base >= 0 && i >= best_base && i < best_base + best_len) {
            if (i == best_base) *o++ = ':';
            continue;
        }
        if (i != 0) *o++ = ':';
        if (i == 6 && best_base == 0 && (best_len == 6 || (best_len == 5 && w[5] == 0xFFFF))) {
            o += sprintf(o, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
            *o = 0;
            return;
        }
        o += sprintf(o, "%x", w[i]);
    }
    if (best_base >= 0 && best_base + best_len == 8) *o++ = ':';
    *o = 0;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    char tmp[INET6_ADDRSTRLEN];
    if (af == AF_INET6) {
        ntop6(src, tmp);
    } else if (af == AF_INET) {
        const unsigned char *a = src;
        snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (strlen(tmp) + 1 > size) { errno = ENOSPC; return NULL; }
    strcpy(dst, tmp);
    return dst;
}
