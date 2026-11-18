#include "netutil.h"

char *nu_ip(uint32_t ip) {
    static char buf[8][20];
    static int n;
    char *b = buf[n++ & 7];
    snprintf(b, 20, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

char *nu_mac(const uint8_t *mac) {
    static char buf[4][20];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, 20, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return b;
}

uint32_t nu_prefix_mask(int len) { return len <= 0 ? 0 : len >= 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu << (32 - len); }

int nu_mask_len(uint32_t mask) {
    int n = 0;
    while (n < 32 && (mask & (0x80000000u >> n))) n++;
    return (mask << n) == 0 || n == 32 ? n : -1;
}

int nu_parse_cidr(const char *s, uint32_t *ip, int *prefix) {
    char tmp[32];
    strlcpy(tmp, s, sizeof tmp);
    char *slash = strchr(tmp, '/');
    int len = -1;
    if (slash) {
        *slash++ = 0;
        char *end;
        long v = strtol(slash, &end, 10);
        if (*end || end == slash || v < 0 || v > 32) return 0;
        len = (int)v;
    }
    struct in_addr a;
    if (!isdigit((unsigned char)tmp[0]) || !inet_aton(tmp, &a)) return 0;
    *ip = ntohl(a.s_addr);
    if (prefix) *prefix = len;
    return 1;
}

int nu_resolve(const char *host, uint32_t *ip) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc) { kb_error("%s: %s", host, gai_strerror(rc)); return 0; }
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 1;
}

char *nu_ip6(const uint8_t a[16]) {
    static char buf[8][INET6_ADDRSTRLEN];
    static int n;
    char *b = buf[n++ & 7];
    if (!inet_ntop(AF_INET6, a, b, INET6_ADDRSTRLEN)) strcpy(b, "?");
    return b;
}

int nu_parse_cidr6(const char *s, uint8_t addr[16], int *prefix) {
    char tmp[64];
    if (!strchr(s, ':') || strlcpy(tmp, s, sizeof tmp) >= sizeof tmp) return 0;
    char *slash = strchr(tmp, '/');
    int len = -1;
    if (slash) {
        *slash++ = 0;
        char *end;
        long v = strtol(slash, &end, 10);
        if (*end || end == slash || v < 0 || v > 128) return 0;
        len = (int)v;
    }
    if (inet_pton(AF_INET6, tmp, addr) != 1) return 0;
    if (prefix) *prefix = len;
    return 1;
}

const char *nu_scope6(unsigned scope) {
    switch (scope) {
    case G6S_LOOPBACK: return "host";
    case G6S_LINKLOCAL: return "link";
    case G6S_SITELOCAL: return "site";
    default: return "global";
    }
}

int nu_resolve_sa(const char *host, int family, struct sockaddr_storage *out, socklen_t *len) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_DGRAM;
    int rc;
    if (family == AF_UNSPEC) {
        hints.ai_family = strchr(host, ':') ? AF_INET6 : AF_INET;
        rc = getaddrinfo(host, NULL, &hints, &res);
        if (rc == EAI_NONAME && hints.ai_family == AF_INET) { hints.ai_family = AF_INET6; rc = getaddrinfo(host, NULL, &hints, &res); }
    } else {
        hints.ai_family = family;
        rc = getaddrinfo(host, NULL, &hints, &res);
    }
    if (rc) { kb_error("%s: %s", host, gai_strerror(rc)); return 0; }
    memset(out, 0, sizeof *out);
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *len = res->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

const char *nu_sa_str(const struct sockaddr *sa, char *buf, size_t n) {
    socklen_t sl = sa->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
    if (getnameinfo(sa, sl, buf, (socklen_t)n, NULL, 0, NI_NUMERICHOST) != 0) strlcpy(buf, "?", n);
    return buf;
}

long nu_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
