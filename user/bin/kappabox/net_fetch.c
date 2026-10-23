/*
 * fetch and wget: HTTP/1.1 GET over plain TCP (no TLS), redirects followed, chunked bodies decoded.
 * `fetch URL` writes to standard output, `wget URL` saves to a file named after the URL; both take -O FILE ("-" = stdout).
 */
#include "netutil.h"

typedef struct {
    char host[256];
    unsigned port;
    char path[1024];
} Url;

typedef struct {
    int quiet, include_headers, max_redirects, timeout_s;
    const char *agent;
    const char *extra_header;
} FetchOpts;

static int parse_url(const char *s, Url *u) {
    if (!strncasecmp(s, "https://", 8)) { kb_error("https is not supported (no TLS here); try http://"); return -1; }
    if (!strncasecmp(s, "http://", 7)) s += 7;
    else if (strstr(s, "://")) { kb_error("unsupported URL scheme in '%s'", s); return -1; }
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (hl == 0 || hl >= sizeof u->host) { kb_error("bad host in URL"); return -1; }
    char hostport[256];
    memcpy(hostport, s, hl);
    hostport[hl] = 0;
    u->port = 80;
    char *colon = strrchr(hostport, ':');
    if (colon) { *colon++ = 0; u->port = (unsigned)atoi(colon); if (u->port == 0 || u->port > 65535) { kb_error("bad port in URL"); return -1; } }
    strlcpy(u->host, hostport, sizeof u->host);
    if (slash) strlcpy(u->path, slash, sizeof u->path); else strlcpy(u->path, "/", sizeof u->path);
    return 0;
}

static int connect_to(const Url *u, int timeout_s) {
    uint32_t ip;
    if (!nu_resolve(u->host, &ip)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { kb_error("socket: %s", strerror(errno)); return -1; }
    struct timeval tv = { timeout_s, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)u->port);
    sa.sin_addr.s_addr = htonl(ip);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        kb_error("connecting to %s:%u failed: %s", u->host, u->port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Resolves a Location header against the URL it came from. */
static void redirect_target(const Url *from, const char *loc, char *out, size_t n) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) { strlcpy(out, loc, n); return; }
    if (loc[0] == '/') { snprintf(out, n, "http://%s:%u%s", from->host, from->port, loc); return; }
    char dir[1024];
    strlcpy(dir, from->path, sizeof dir);
    char *q = strchr(dir, '?');
    if (q) *q = 0;
    char *s = strrchr(dir, '/');
    if (s) s[1] = 0;
    snprintf(out, n, "http://%s:%u%s%s", from->host, from->port, dir, loc);
}

typedef struct {
    int fd;
    unsigned char buf[8192];
    size_t pos, len;
    int eof;
} Reader;

static int rd_fill(Reader *r) {
    if (r->pos < r->len) return 1;
    if (r->eof) return 0;
    ssize_t n = recv(r->fd, r->buf, sizeof r->buf, 0);
    if (n < 0) return -1;
    if (n == 0) { r->eof = 1; return 0; }
    r->pos = 0;
    r->len = (size_t)n;
    return 1;
}

static int rd_byte(Reader *r) {
    int f = rd_fill(r);
    if (f <= 0) return f == 0 ? -2 : -1;
    return r->buf[r->pos++];
}

static int rd_line(Reader *r, char *out, size_t cap) {
    size_t n = 0;
    for (;;) {
        int c = rd_byte(r);
        if (c < 0) return c;
        if (c == '\n') break;
        if (n + 1 < cap) out[n++] = (char)c;
    }
    while (n && out[n - 1] == '\r') n--;
    out[n] = 0;
    return (int)n;
}

static const char *base_name(const char *path, char *buf, size_t n) {
    char tmp[1024];
    strlcpy(tmp, path, sizeof tmp);
    char *q = strpbrk(tmp, "?#");
    if (q) *q = 0;
    char *s = strrchr(tmp, '/');
    const char *b = s ? s + 1 : tmp;
    strlcpy(buf, *b ? b : "index.html", n);
    return buf;
}

static int do_fetch(const char *start_url, const char *outname, int to_file_default, const FetchOpts *o) {
    char url[2048];
    strlcpy(url, start_url, sizeof url);
    for (int hop = 0; hop <= o->max_redirects; hop++) {
        Url u;
        if (parse_url(url, &u) < 0) return 1;
        if (!o->quiet) fprintf(stderr, "Connecting to %s:%u... ", u.host, u.port);
        int fd = connect_to(&u, o->timeout_s);
        if (fd < 0) return 4;
        if (!o->quiet) fprintf(stderr, "connected.\n");
        char req[2048];
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s%s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n", u.path, u.host,
                          u.port == 80 ? "" : "", o->agent);
        if (u.port != 80) rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n", u.path, u.host, u.port, o->agent);
        if (o->extra_header) rl += snprintf(req + rl, sizeof req - (size_t)rl, "%s\r\n", o->extra_header);
        rl += snprintf(req + rl, sizeof req - (size_t)rl, "\r\n");
        if (send_all(fd, req, (size_t)rl) < 0) { kb_error("sending the request failed: %s", strerror(errno)); close(fd); return 4; }
        if (!o->quiet) fprintf(stderr, "HTTP request sent, awaiting response... ");

        Reader *r = calloc(1, sizeof *r);
        if (!r) { close(fd); return 1; }
        r->fd = fd;
        char line[2048];
        int n = rd_line(r, line, sizeof line);
        if (n < 12 || strncmp(line, "HTTP/", 5)) {
            kb_error(n == -1 ? "no answer: %s" : "the server did not speak HTTP", strerror(errno));
            free(r); close(fd);
            return 1;
        }
        int status = atoi(line + 9);
        const char *reason = line + 9;
        while (*reason && *reason != ' ') reason++;
        if (*reason == ' ') reason++;
        if (!o->quiet) fprintf(stderr, "%d %s\n", status, reason);
        long long clen = -1;
        int chunked = 0;
        char location[1024] = "", ctype[128] = "";
        char *headers = o->include_headers ? malloc(16384) : NULL;
        size_t hlen = 0;
        if (headers) hlen = (size_t)snprintf(headers, 16384, "%s\r\n", line);
        for (;;) {
            n = rd_line(r, line, sizeof line);
            if (n < 0) { kb_error("the connection closed during the headers"); free(r); free(headers); close(fd); return 1; }
            if (n == 0) break;
            if (headers && hlen + (size_t)n + 3 < 16384) hlen += (size_t)snprintf(headers + hlen, 16384 - hlen, "%s\r\n", line);
            if (!strncasecmp(line, "Content-Length:", 15)) clen = atoll(line + 15);
            else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line + 18, "chunked")) chunked = 1;
            else if (!strncasecmp(line, "Location:", 9)) { const char *v = line + 9; while (*v == ' ') v++; strlcpy(location, v, sizeof location); }
            else if (!strncasecmp(line, "Content-Type:", 13)) { const char *v = line + 13; while (*v == ' ') v++; strlcpy(ctype, v, sizeof ctype); }
        }
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location[0]) {
            char next[2048];
            redirect_target(&u, location, next, sizeof next);
            if (!o->quiet) fprintf(stderr, "Location: %s [following]\n", next);
            strlcpy(url, next, sizeof url);
            free(r); free(headers); close(fd);
            continue;
        }
        if (status >= 400) {
            fprintf(stderr, "%s: server returned %d %s\n", kb_name, status, reason);
            free(r); free(headers); close(fd);
            return 8;
        }
        if (status < 200 || status >= 300) { kb_error("unexpected status %d", status); free(r); free(headers); close(fd); return 1; }

        /* where the body goes */
        char autoname[300];
        const char *dest = outname;
        if (!dest && to_file_default) {
            base_name(u.path, autoname, sizeof autoname);
            char try_[320];
            strlcpy(try_, autoname, sizeof try_);
            for (int k = 1; access(try_, F_OK) == 0 && k < 100; k++) snprintf(try_, sizeof try_, "%s.%d", autoname, k);
            strlcpy(autoname, try_, sizeof autoname);
            dest = autoname;
        }
        int out = 1;
        if (dest && strcmp(dest, "-")) {
            out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out < 0) { kb_error("%s: %s", dest, strerror(errno)); free(r); free(headers); close(fd); return 1; }
        }
        if (!o->quiet) {
            if (clen >= 0) fprintf(stderr, "Length: %lld [%s]\n", clen, ctype[0] ? ctype : "unknown");
            else fprintf(stderr, "Length: unspecified [%s]\n", ctype[0] ? ctype : "unknown");
            if (out != 1) fprintf(stderr, "Saving to: '%s'\n", dest);
        }
        if (headers) { if (write(out, headers, hlen) < 0 || write(out, "\r\n", 2) < 0) { /* the body write below reports it */ } }
        long long total = 0;
        int failed = 0;
        if (chunked) {
            for (;;) {
                n = rd_line(r, line, sizeof line);
                if (n < 0) { failed = 1; break; }
                if (n == 0) continue;                           /* the CRLF after the previous chunk */
                long long csize = strtoll(line, NULL, 16);
                if (csize <= 0) break;
                while (csize > 0) {
                    int f = rd_fill(r);
                    if (f <= 0) { failed = 1; break; }
                    size_t take = r->len - r->pos;
                    if ((long long)take > csize) take = (size_t)csize;
                    if (write(out, r->buf + r->pos, take) != (ssize_t)take) { failed = 1; break; }
                    r->pos += take;
                    csize -= (long long)take;
                    total += (long long)take;
                }
                if (failed) break;
            }
        } else {
            while (clen < 0 || total < clen) {
                int f = rd_fill(r);
                if (f < 0) { failed = 1; break; }
                if (f == 0) break;
                size_t take = r->len - r->pos;
                if (clen >= 0 && (long long)take > clen - total) take = (size_t)(clen - total);
                if (write(out, r->buf + r->pos, take) != (ssize_t)take) { failed = 1; break; }
                r->pos += take;
                total += (long long)take;
            }
            if (clen >= 0 && total < clen) failed = 1;
        }
        if (out != 1) close(out);
        if (failed) {
            kb_error("the transfer ended early after %lld bytes: %s", total, errno ? strerror(errno) : "connection closed");
            free(r); free(headers); close(fd);
            return 1;
        }
        if (!o->quiet && out != 1) fprintf(stderr, "'%s' saved [%lld]\n", dest, total);
        free(r); free(headers); close(fd);
        return 0;
    }
    kb_error("too many redirects");
    return 1;
}

static int fetch_main(int argc, char **argv, int wget_style) {
    FetchOpts o = { 0, 0, 8, 30, "fetch/1.0 (Gensokyo)", NULL };
    const char *out = NULL;
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "qO:o:T:U:H:r:iL4")) != -1) {
        switch (opt) {
        case 'q': o.quiet = 1; break;
        case 'O': case 'o': out = optarg; break;
        case 'T': o.timeout_s = atoi(optarg); if (o.timeout_s < 1) o.timeout_s = 1; break;
        case 'U': o.agent = optarg; break;
        case 'H': o.extra_header = optarg; break;
        case 'r': o.max_redirects = atoi(optarg); break;
        case 'i': o.include_headers = 1; break;
        case 'L': case '4': break;
        default: kb_error("usage: %s [-q] [-O FILE|-] [-T SECONDS] [-U AGENT] [-H 'Header: v'] [-r REDIRECTS] [-i] URL...", kb_name); return 2;
        }
    }
    if (optind >= argc) { kb_error("usage: %s [-q] [-O FILE|-] [-T SECONDS] URL...", kb_name); return 2; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int r = do_fetch(argv[i], out, wget_style, &o);
        if (r) rc = r;
    }
    return rc;
}

APPLET(fetch) { return fetch_main(argc, argv, 0); }
APPLET(wget) { return fetch_main(argc, argv, 1); }
