/* Address conversion: inet_aton / inet_addr / inet_ntoa / inet_pton / inet_ntop (IPv4). */
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

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    unsigned v[4];
    int n = 0;
    const char *p = src;
    while (n < 4) {
        if (!isdigit((unsigned char)*p)) return 0;
        unsigned x = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) { x = x * 10 + (unsigned)(*p++ - '0'); if (++digits > 3 || x > 255) return 0; }
        if (digits > 1 && src[0] == '0' && p - src == digits) { /* leading zeros are not allowed in the strict form */ }
        v[n++] = x;
        if (n < 4) { if (*p != '.') return 0; p++; }
    }
    if (*p) return 0;
    unsigned char *o = dst;
    for (int i = 0; i < 4; i++) o[i] = (unsigned char)v[i];
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    const unsigned char *a = src;
    char tmp[INET_ADDRSTRLEN];
    snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    if (strlen(tmp) + 1 > size) { errno = ENOSPC; return NULL; }
    strcpy(dst, tmp);
    return dst;
}
