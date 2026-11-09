/*
 * URLs for libhttp: the http and https schemes (RFC 9110 4.2) in the generic syntax of RFC 3986, reference resolution
 * (RFC 3986 5.2, for Location headers), and the request message (RFC 9112 3).
 *
 * Not handled: IPv6 literals ("[...]"), user information in the authority ("user@host" is refused rather than half-supported),
 * internationalized host names (give the xn-- form).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <http.h>

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* bytes that may stand in a request target as they are; everything else is percent-encoded (a '%' that already starts an
 * escape is kept) */
static int keep_in_path(unsigned char c) {
    if (c <= 0x20 || c >= 0x7f) return 0;
    return strchr("\"<>\\^`{|}", c) == NULL;
}

static int put_path(char *out, size_t cap, const char *p, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (keep_in_path(c)) {
            if (k + 1 >= cap) return -1;
            out[k++] = (char)c;
        } else {
            if (k + 3 >= cap) return -1;
            out[k++] = '%';
            out[k++] = hex[c >> 4];
            out[k++] = hex[c & 15];
        }
    }
    out[k] = 0;
    return 0;
}

static int default_port(const char *scheme) { return strcmp(scheme, "https") == 0 ? 443 : 80; }

int http_url_parse(const char *s, HttpUrl *u) {
    memset(u, 0, sizeof *u);
    if (!s) return HTTP_E_URL;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) len--;
    size_t i = 0;
    while (i < len && i < 7 && s[i] != ':') {
        u->scheme[i] = (char)lower(s[i]);
        i++;
    }
    u->scheme[i] = 0;
    if (i >= len || s[i] != ':' || (strcmp(u->scheme, "http") && strcmp(u->scheme, "https"))) return HTTP_E_URL;
    i++;
    if (i + 2 > len || s[i] != '/' || s[i + 1] != '/') return HTTP_E_URL;
    i += 2;
    size_t a = i;
    while (i < len && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
    size_t alen = i - a;
    const char *auth = s + a;
    if (memchr(auth, '@', alen) || (alen && auth[0] == '[')) return HTTP_E_URL;
    size_t hlen = alen;
    const char *colon = memchr(auth, ':', alen);
    u->port = default_port(u->scheme);
    if (colon) {
        hlen = (size_t)(colon - auth);
        size_t plen = alen - hlen - 1;
        if (plen) {
            long port = 0;
            for (size_t k = 0; k < plen; k++) {
                char c = colon[1 + k];
                if (c < '0' || c > '9' || k > 5) return HTTP_E_URL;
                port = port * 10 + (c - '0');
            }
            if (port < 1 || port > 65535) return HTTP_E_URL;
            u->port = (int)port;
            u->explicit_port = 1;
        }
    }
    if (hlen == 0 || hlen >= sizeof u->host) return HTTP_E_URL;
    for (size_t k = 0; k < hlen; k++) {
        char c = (char)lower(auth[k]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')) return HTTP_E_URL;
        u->host[k] = c;
    }
    u->host[hlen] = 0;
    size_t pend = i;
    while (pend < len && s[pend] != '#') pend++;                    /* the fragment never goes to the server */
    char raw[HTTP_MAX_PATH];
    size_t plen = pend - i;
    if (plen + 2 > sizeof raw) return HTTP_E_URL;
    size_t k = 0;
    if (plen == 0 || s[i] != '/') raw[k++] = '/';                   /* "http://host" and "http://host?q" mean "/" */
    memcpy(raw + k, s + i, plen);
    k += plen;
    if (put_path(u->path, sizeof u->path, raw, k) != 0) return HTTP_E_URL;
    return 0;
}

int http_url_format(const HttpUrl *u, char *buf, size_t cap) {
    int n;
    if (u->explicit_port && u->port != default_port(u->scheme)) n = snprintf(buf, cap, "%s://%s:%d%s", u->scheme, u->host, u->port, u->path);
    else n = snprintf(buf, cap, "%s://%s%s", u->scheme, u->host, u->path);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* RFC 3986 5.2.4, step by step (A to E of the RFC): removes "." and ".." segments */
static void remove_dots(const char *in, char *out, size_t cap) {
    size_t o = 0;
    const char *p = in;
    while (*p) {
        if (strncmp(p, "../", 3) == 0) p += 3;                       /* A */
        else if (strncmp(p, "./", 2) == 0) p += 2;
        else if (strncmp(p, "/./", 3) == 0) p += 2;                  /* B: "/./x" becomes "/x" */
        else if (strcmp(p, "/.") == 0) p = "/";
        else if (strncmp(p, "/../", 4) == 0 || strcmp(p, "/..") == 0) {      /* C: up one, dropping the last output segment */
            p = p[3] ? p + 3 : "/";
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
        } else if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {     /* D */
            break;
        } else {                                                     /* E: one segment to the output */
            do {
                if (o + 1 < cap) out[o++] = *p;
                p++;
            } while (*p && *p != '/');
        }
    }
    out[o] = 0;
}

int http_url_resolve(const HttpUrl *base, const char *ref, HttpUrl *out) {
    if (!ref) return HTTP_E_URL;
    while (*ref == ' ' || *ref == '\t') ref++;
    size_t i = 0;
    while (ref[i] && ref[i] != ':' && ref[i] != '/' && ref[i] != '?' && ref[i] != '#') i++;
    if (ref[i] == ':' && i > 0) return http_url_parse(ref, out);    /* absolute: its own scheme */
    char buf[HTTP_MAX_PATH + 600], path[HTTP_MAX_PATH], merged[HTTP_MAX_PATH * 2];
    if (ref[0] == '/' && ref[1] == '/') {
        if (snprintf(buf, sizeof buf, "%s:%s", base->scheme, ref) >= (int)sizeof buf) return HTTP_E_URL;
        return http_url_parse(buf, out);
    }
    char auth[HTTP_MAX_HOST + 16];
    if (base->explicit_port) snprintf(auth, sizeof auth, "%s:%d", base->host, base->port);
    else snprintf(auth, sizeof auth, "%s", base->host);
    /* the reference without its fragment, split at '?' */
    size_t rlen = strcspn(ref, "#");
    size_t qpos = strcspn(ref, "?");
    if (qpos > rlen) qpos = rlen;
    const char *bq = strchr(base->path, '?');
    size_t bplen = bq ? (size_t)(bq - base->path) : strlen(base->path);
    if (rlen >= sizeof path) return HTTP_E_URL;
    if (qpos == 0) {                                                 /* "" or "?query": the base path */
        if (rlen == 0) snprintf(merged, sizeof merged, "%s", base->path);
        else snprintf(merged, sizeof merged, "%.*s%.*s", (int)bplen, base->path, (int)rlen, ref);
        if (snprintf(buf, sizeof buf, "%s://%s%s", base->scheme, auth, merged) >= (int)sizeof buf) return HTTP_E_URL;
        return http_url_parse(buf, out);
    }
    if (ref[0] == '/') {
        snprintf(merged, sizeof merged, "%.*s", (int)qpos, ref);
    } else {                                                         /* merge with the base path's directory */
        size_t dir = bplen;
        while (dir > 0 && base->path[dir - 1] != '/') dir--;
        snprintf(merged, sizeof merged, "%.*s%.*s", (int)dir, base->path, (int)qpos, ref);
    }
    remove_dots(merged, path, sizeof path);
    if (path[0] != '/') {
        memmove(path + 1, path, strlen(path) + 1 < sizeof path - 1 ? strlen(path) + 1 : sizeof path - 2);
        path[0] = '/';
    }
    if (snprintf(buf, sizeof buf, "%s://%s%s%.*s", base->scheme, auth, path, (int)(rlen - qpos), ref + qpos) >= (int)sizeof buf) return HTTP_E_URL;
    return http_url_parse(buf, out);
}

/* True when one of the "Name: value\r\n" lines in extra starts with name (given with its colon, e.g. "user-agent:"); the value of
 * another header that merely mentions the name does not count. */
static int has_header(const char *extra, const char *name) {
    size_t n = strlen(name);
    for (const char *p = extra; p && *p;) {
        if (strncasecmp(p, name, n) == 0) return 1;
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

long http_request_build(char *buf, size_t cap, const char *method, const HttpUrl *u, const char *extra, int decode) {
    char hostport[HTTP_MAX_HOST + 8];
    if (u->port != default_port(u->scheme)) snprintf(hostport, sizeof hostport, "%s:%d", u->host, u->port);
    else snprintf(hostport, sizeof hostport, "%s", u->host);
    /* a caller that sends its own User-Agent (fetch -U) replaces ours: two of them is a request some servers refuse */
    const char *agent = extra && has_header(extra, "user-agent:") ? "" : "User-Agent: libhttp/1.0 (TouhouOS)\r\n";
    int n = snprintf(buf, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n%sAccept: */*\r\n%sConnection: close\r\n%s\r\n",
                     method, u->path, hostport, agent, decode ? "Accept-Encoding: gzip, deflate\r\n" : "", extra ? extra : "");
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

const char *http_strerror(int err) {
    switch (err) {
    case 0: return "no error";
    case HTTP_E_URL: return "not an http:// or https:// URL that can be used here";
    case HTTP_E_DNS: return "the host name could not be resolved";
    case HTTP_E_CONNECT: return "could not connect";
    case HTTP_E_TLS: return "the secure connection could not be made";
    case HTTP_E_IO: return "the connection broke";
    case HTTP_E_TIMEOUT: return "the server did not answer in time";
    case HTTP_E_PARSE: return "the server's response is malformed";
    case HTTP_E_TOO_BIG: return "the response's header is too big";
    case HTTP_E_TRUNCATED: return "the response ended early";
    case HTTP_E_DECODE: return "the compressed body is corrupt";
    case HTTP_E_REDIRECTS: return "too many redirects";
    case HTTP_E_CALLBACK: return "stopped";
    case HTTP_E_UNSUPPORTED: return "the response uses a transfer coding other than chunked";
    case HTTP_E_NOMEM: return "out of memory";
    default: return "unknown error";
    }
}
