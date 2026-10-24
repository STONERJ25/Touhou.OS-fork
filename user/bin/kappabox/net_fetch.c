/*
 * fetch and wget: HTTP/1.1 and HTTPS GET, redirects followed (also between http and https), chunked bodies decoded.
 * `fetch URL` writes to standard output, `wget URL` saves to a file named after the URL; both take -O FILE ("-" = stdout).
 *
 * HTTPS goes through libtls (user/lib/tls): the certificate chain is checked against /etc/ssl/certs/ca-bundle.pem (or --cacert FILE),
 * the host name against the certificate's subjectAltName, and the dates against the system clock. -k switches the checks off, loudly.
 */
#include "netutil.h"
#include <getopt.h>
#include <time.h>
#include "tls.h"

#define DEFAULT_CA "/etc/ssl/certs/ca-bundle.pem"

typedef struct {
    int https;
    char host[256];
    unsigned port;
    char path[1024];
} Url;

typedef struct {
    int quiet, include_headers, max_redirects, timeout_s, verbose;
    const char *agent;
    const char *extra_header;
    int verify;                                     /* TLS: check the certificate (the default) */
    const char *cacert;
    unsigned tls_flags, tls_suites;
    tls_ctx *ctx;
    int ctx_tried;
} FetchOpts;

typedef struct {
    int fd;
    tls_conn *tls;
    int truncated;                                  /* TLS: the peer closed without close_notify */
    int tls_error;                                  /* TLS: a read failed with this code */
} Conn;

static int parse_url(const char *s, Url *u) {
    memset(u, 0, sizeof *u);
    if (!strncasecmp(s, "https://", 8)) { u->https = 1; s += 8; }
    else if (!strncasecmp(s, "http://", 7)) s += 7;
    else if (strstr(s, "://")) { kb_error("unsupported URL scheme in '%s' (only http and https)", s); return -1; }
    const char *end = s + strcspn(s, "/?#");
    size_t hl = (size_t)(end - s);
    if (hl == 0 || hl >= sizeof u->host) { kb_error("bad host in URL"); return -1; }
    char hostport[256];
    memcpy(hostport, s, hl);
    hostport[hl] = 0;
    if (strchr(hostport, '@')) { kb_error("user names in URLs are not supported"); return -1; }
    if (hostport[0] == '[') { kb_error("IPv6 addresses are not supported"); return -1; }
    u->port = u->https ? 443 : 80;
    char *colon = strrchr(hostport, ':');
    if (colon) { *colon++ = 0; u->port = (unsigned)atoi(colon); if (u->port == 0 || u->port > 65535) { kb_error("bad port in URL"); return -1; } }
    strlcpy(u->host, hostport, sizeof u->host);
    if (*end == '/') strlcpy(u->path, end, sizeof u->path);
    else if (*end == '?') snprintf(u->path, sizeof u->path, "/%s", end);
    else strlcpy(u->path, "/", sizeof u->path);
    char *frag = strchr(u->path, '#');
    if (frag) *frag = 0;
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

/* ---- the transport: a socket, with TLS on top for https ---- */

static ssize_t conn_read(Conn *c, void *buf, size_t n) {
    if (!c->tls) return recv(c->fd, buf, n, 0);
    long r = tls_read(c->tls, buf, n);
    if (r >= 0) return (ssize_t)r;
    if (r == TLS_E_TRUNCATED) { c->truncated = 1; return 0; }
    c->tls_error = (int)r;
    errno = EIO;
    return -1;
}

static int conn_write_all(Conn *c, const char *p, size_t n) {
    if (c->tls) return tls_write(c->tls, p, n) == (long)n ? 0 : -1;
    while (n) {
        ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void conn_close(Conn *c) {
    if (c->tls) tls_close(c->tls);
    close(c->fd);
}

static void utc_text(long long t, char *out, size_t n) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (!gmtime_r(&tt, &tm) || !strftime(out, n, "%Y-%m-%d %H:%M:%S UTC", &tm)) strlcpy(out, "?", n);
}

static tls_ctx *get_ctx(FetchOpts *o) {
    if (o->ctx_tried) return o->ctx;
    o->ctx_tried = 1;
    const char *path = o->cacert ? o->cacert : getenv("SSL_CERT_FILE");
    if (!path) path = DEFAULT_CA;
    tls_ctx *ctx = tls_ctx_new();
    int rc = ctx ? tls_ctx_load_file(ctx, path) : TLS_E_NOMEM;
    if (rc < 0) {
        if (o->verify) kb_error("cannot read the CA certificates in %s: %s", path, rc == TLS_E_IO ? strerror(errno) : tls_strerror(rc));
    } else if (rc == 0) {
        if (o->verify) kb_error("%s holds no usable CA certificates", path);
    } else {
        o->ctx = ctx;
        if (o->verbose) fprintf(stderr, "trusting %zu root certificates from %s\n", tls_ctx_count(ctx), path);
    }
    return o->ctx;
}

static void tls_explain(const char *what, tls_conn *tc, int rc, const Url *u) {
    const tls_info *i = tls_get_info(tc);
    if (rc == TLS_E_ALERT) kb_error("%s %s:%u: the server rejected the handshake (alert %s)", what, u->host, u->port, tls_alert_name(tls_alert_received(tc)));
    else if (rc == TLS_E_IO && errno) kb_error("%s %s:%u: %s (%s)", what, u->host, u->port, tls_strerror(rc), strerror(errno));
    else kb_error("%s %s:%u: %s", what, u->host, u->port, tls_strerror(rc));
    if (rc <= TLS_E_CERT_PARSE && i && i->subject[0]) {
        char from[40], to[40], now[40];
        utc_text(i->not_before, from, sizeof from);
        utc_text(i->not_after, to, sizeof to);
        fprintf(stderr, "  the certificate is for: %s\n  issued by: %s\n  valid from %s to %s\n", i->subject, i->issuer, from, to);
        if (rc == TLS_E_CERT_EXPIRED || rc == TLS_E_CERT_NOT_YET_VALID) {
            utc_text((long long)time(NULL), now, sizeof now);
            fprintf(stderr, "  this computer's clock says %s (if that is wrong, set it with sntp)\n", now);
        }
        if (rc == TLS_E_CERT_NAME) fprintf(stderr, "  but you asked for %s\n", u->host);
        fprintf(stderr, "  use -k to continue anyway (insecure), or --cacert FILE to trust another authority\n");
    }
    if (rc == TLS_E_ENTROPY) fprintf(stderr, "  the kernel's random number generator is not seeded yet (see /proc/random); --weak-random overrides this, at your own risk\n");
}

static int open_tls(Conn *c, const Url *u, FetchOpts *o) {
    tls_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.server_name = u->host;
    cfg.alpn = "http/1.1";
    cfg.flags = o->tls_flags | (o->verify ? 0 : TLS_F_NO_VERIFY);
    cfg.suites = o->tls_suites;
    cfg.ctx = get_ctx(o);                           /* also with -k: it lets the warning say what is really wrong with the certificate */
    if (o->verify && !cfg.ctx) return 5;
    tls_io io;
    tls_io_fd(&io, c->fd);
    c->tls = tls_new(&io, &cfg);
    if (!c->tls) { kb_error("out of memory"); return 1; }
    if (!o->quiet) fprintf(stderr, "TLS handshake... ");
    int rc = tls_handshake(c->tls);
    if (rc) {
        if (!o->quiet) fputc('\n', stderr);
        tls_explain("TLS connection to", c->tls, rc, u);
        if (o->verbose) fprintf(stderr, "  (internal: %s)\n", tls_error_where(c->tls));
        tls_free(c->tls);
        c->tls = NULL;
        return 5;
    }
    const tls_info *i = tls_get_info(c->tls);
    if (!o->quiet) fprintf(stderr, "%s, %s, %s.\n", i->version_name, i->suite_name, i->verified ? "certificate verified" : "certificate NOT checked");
    if (!o->verify) {
        fprintf(stderr, "%s: WARNING: certificate checking is switched off (-k): anyone between you and %s can read and change this connection\n", kb_name, u->host);
        if (i->verify_error) fprintf(stderr, "%s: WARNING: the certificate would have been refused: %s\n", kb_name, tls_strerror(i->verify_error));
    }
    if (o->verbose) {
        char from[40], to[40];
        utc_text(i->not_before, from, sizeof from);
        utc_text(i->not_after, to, sizeof to);
        fprintf(stderr, "  key exchange: %s%s, ALPN: %s, %u certificate(s) in the chain\n", i->group_name, i->hello_retry ? " (after HelloRetryRequest)" : "", i->alpn[0] ? i->alpn : "none", i->chain_length);
        fprintf(stderr, "  subject: %s\n  issuer:  %s\n  valid:   %s to %s\n  SHA-256: ", i->subject, i->issuer, from, to);
        for (int k = 0; k < 32; k++) fprintf(stderr, "%02x%s", i->fingerprint[k], k < 31 ? ":" : "\n");
    }
    return 0;
}

/* Resolves a Location header against the URL it came from. */
static void redirect_target(const Url *from, const char *loc, char *out, size_t n) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) { strlcpy(out, loc, n); return; }
    const char *scheme = from->https ? "https" : "http";
    char hp[300];
    if (from->port == (from->https ? 443u : 80u)) snprintf(hp, sizeof hp, "%s", from->host);
    else snprintf(hp, sizeof hp, "%s:%u", from->host, from->port);
    if (loc[0] == '/' && loc[1] == '/') { snprintf(out, n, "%s:%s", scheme, loc); return; }
    if (loc[0] == '/') { snprintf(out, n, "%s://%s%s", scheme, hp, loc); return; }
    char dir[1024];
    strlcpy(dir, from->path, sizeof dir);
    char *q = strchr(dir, '?');
    if (q) *q = 0;
    char *s = strrchr(dir, '/');
    if (s) s[1] = 0;
    snprintf(out, n, "%s://%s%s%s", scheme, hp, dir, loc);
}

typedef struct {
    Conn *c;
    unsigned char buf[8192];
    size_t pos, len;
    int eof;
} Reader;

static int rd_fill(Reader *r) {
    if (r->pos < r->len) return 1;
    if (r->eof) return 0;
    ssize_t n = conn_read(r->c, r->buf, sizeof r->buf);
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

static int do_fetch(const char *start_url, const char *outname, int to_file_default, FetchOpts *o) {
    char url[2048];
    strlcpy(url, start_url, sizeof url);
    int prev_https = -1;
    for (int hop = 0; hop <= o->max_redirects; hop++) {
        Url u;
        if (parse_url(url, &u) < 0) return 1;
        if (prev_https == 1 && !u.https) fprintf(stderr, "%s: note: redirected from https to http, the next request is not encrypted\n", kb_name);
        prev_https = u.https;
        if (!o->quiet) fprintf(stderr, "Connecting to %s:%u... ", u.host, u.port);
        Conn conn = { connect_to(&u, o->timeout_s), NULL, 0, 0 };
        if (conn.fd < 0) return 4;
        if (!o->quiet) fprintf(stderr, "connected.\n");
        if (u.https) {
            int rc = open_tls(&conn, &u, o);
            if (rc) { close(conn.fd); return rc; }
        }
        char req[2048];
        char hostport[300];
        if (u.port == (u.https ? 443u : 80u)) snprintf(hostport, sizeof hostport, "%s", u.host); else snprintf(hostport, sizeof hostport, "%s:%u", u.host, u.port);
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n", u.path, hostport, o->agent);
        if (o->extra_header) rl += snprintf(req + rl, sizeof req - (size_t)rl, "%s\r\n", o->extra_header);
        rl += snprintf(req + rl, sizeof req - (size_t)rl, "\r\n");
        if (conn_write_all(&conn, req, (size_t)rl) < 0) { kb_error("sending the request failed: %s", strerror(errno)); conn_close(&conn); return 4; }
        if (!o->quiet) fprintf(stderr, "HTTP request sent, awaiting response... ");

        Reader *r = calloc(1, sizeof *r);
        if (!r) { conn_close(&conn); return 1; }
        r->c = &conn;
        char line[2048];
        int n = rd_line(r, line, sizeof line);
        if (n < 12 || strncmp(line, "HTTP/", 5)) {
            if (!o->quiet) fputc('\n', stderr);
            if (n == -1 && conn.tls_error) kb_error("no answer: %s", tls_strerror(conn.tls_error));
            else kb_error(n == -1 ? "no answer: %s" : "the server did not speak HTTP", strerror(errno));
            free(r); conn_close(&conn);
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
            if (n < 0) { kb_error("the connection closed during the headers"); free(r); free(headers); conn_close(&conn); return 1; }
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
            free(r); free(headers); conn_close(&conn);
            continue;
        }
        if (status >= 400) {
            fprintf(stderr, "%s: server returned %d %s\n", kb_name, status, reason);
            free(r); free(headers); conn_close(&conn);
            return 8;
        }
        if (status < 200 || status >= 300) { kb_error("unexpected status %d", status); free(r); free(headers); conn_close(&conn); return 1; }

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
            if (out < 0) { kb_error("%s: %s", dest, strerror(errno)); free(r); free(headers); conn_close(&conn); return 1; }
        }
        if (!o->quiet) {
            if (clen >= 0) fprintf(stderr, "Length: %lld [%s]\n", clen, ctype[0] ? ctype : "unknown");
            else fprintf(stderr, "Length: unspecified [%s]\n", ctype[0] ? ctype : "unknown");
            if (out != 1) fprintf(stderr, "Saving to: '%s'\n", dest);
        }
        if (headers) { if (write(out, headers, hlen) < 0 || write(out, "\r\n", 2) < 0) { /* the body write below reports it */ } }
        long long total = 0;
        int failed = 0, complete = 0;
        errno = 0;
        if (chunked) {
            for (;;) {
                n = rd_line(r, line, sizeof line);
                if (n < 0) { failed = 1; break; }
                if (n == 0) continue;                           /* the CRLF after the previous chunk */
                long long csize = strtoll(line, NULL, 16);
                if (csize <= 0) { complete = 1; break; }
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
            if (clen >= 0) complete = total >= clen;
            else complete = !failed && !conn.truncated;      /* no length given: only a proper close (TLS: close_notify) proves the end */
            if (clen >= 0 && total < clen) failed = 1;
        }
        if (out != 1) close(out);
        if (!failed && !complete) {
            kb_error("the server closed the connection without close_notify and sent no length: the download may be incomplete (%lld bytes)", total);
            failed = 2;
        }
        if (failed == 1) {
            if (conn.tls_error) kb_error("the transfer failed after %lld bytes: %s", total, tls_strerror(conn.tls_error));
            else kb_error("the transfer ended early after %lld bytes: %s", total, errno ? strerror(errno) : "connection closed");
        }
        if (failed) {
            free(r); free(headers); conn_close(&conn);
            return 1;
        }
        if (!o->quiet && out != 1) fprintf(stderr, "'%s' saved [%lld]\n", dest, total);
        free(r); free(headers); conn_close(&conn);
        return 0;
    }
    kb_error("too many redirects");
    return 1;
}

enum { OPT_CACERT = 256, OPT_NOCHECK, OPT_TLS12, OPT_TLS13, OPT_WEAK, OPT_SOFTAES, OPT_CIPHER };

static int fetch_main(int argc, char **argv, int wget_style) {
    FetchOpts o;
    memset(&o, 0, sizeof o);
    o.max_redirects = 8;
    o.timeout_s = 30;
    o.agent = "fetch/1.0 (Gensokyo)";
    o.verify = 1;
    const char *out = NULL;
    static const struct option longopts[] = {
        { "cacert", required_argument, NULL, OPT_CACERT },
        { "ca-certificate", required_argument, NULL, OPT_CACERT },
        { "no-check-certificate", no_argument, NULL, OPT_NOCHECK },
        { "insecure", no_argument, NULL, 'k' },
        { "tls12", no_argument, NULL, OPT_TLS12 },
        { "tls13", no_argument, NULL, OPT_TLS13 },
        { "weak-random", no_argument, NULL, OPT_WEAK },
        { "soft-aes", no_argument, NULL, OPT_SOFTAES },
        { "cipher", required_argument, NULL, OPT_CIPHER },
        { "verbose", no_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };
    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "qO:o:T:U:H:r:iL4kv", longopts, NULL)) != -1) {
        switch (opt) {
        case 'q': o.quiet = 1; break;
        case 'O': case 'o': out = optarg; break;
        case 'T': o.timeout_s = atoi(optarg); if (o.timeout_s < 1) o.timeout_s = 1; break;
        case 'U': o.agent = optarg; break;
        case 'H': o.extra_header = optarg; break;
        case 'r': o.max_redirects = atoi(optarg); break;
        case 'i': o.include_headers = 1; break;
        case 'k': case OPT_NOCHECK: o.verify = 0; break;
        case 'v': o.verbose = 1; break;
        case OPT_CACERT: o.cacert = optarg; break;
        case OPT_TLS12: o.tls_flags |= TLS_F_TLS12_ONLY; break;
        case OPT_TLS13: o.tls_flags |= TLS_F_TLS13_ONLY; break;
        case OPT_WEAK: o.tls_flags |= TLS_F_WEAK_RANDOM; break;
        case OPT_SOFTAES: o.tls_flags |= TLS_F_NO_AESNI; break;
        case OPT_CIPHER:
            if (!strcmp(optarg, "aes128")) o.tls_suites = TLS_S_AES128_GCM;
            else if (!strcmp(optarg, "aes256")) o.tls_suites = TLS_S_AES256_GCM;
            else if (!strcmp(optarg, "chacha20")) o.tls_suites = TLS_S_CHACHA20;
            else { kb_error("--cipher is aes128, aes256 or chacha20"); return 2; }
            break;
        case 'L': case '4': break;
        default:
            kb_error("usage: %s [-qvik] [-O FILE|-] [-T SECONDS] [-U AGENT] [-H 'Header: v'] [-r REDIRECTS] [--cacert FILE] URL...", kb_name);
            return 2;
        }
    }
    if (optind >= argc) { kb_error("usage: %s [-qvik] [-O FILE|-] [-T SECONDS] [--cacert FILE] URL...", kb_name); return 2; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int r = do_fetch(argv[i], out, wget_style, &o);
        if (r) rc = r;
    }
    return rc;
}

APPLET(fetch) { return fetch_main(argc, argv, 0); }
APPLET(wget) { return fetch_main(argc, argv, 1); }
