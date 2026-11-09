/*
 * fetch and wget: HTTP/1.1 GET over plain TCP or over TLS, redirects followed, chunked bodies decoded.
 * `fetch URL` writes to standard output, `wget URL` saves to a file named after the URL; both take -O FILE ("-" = stdout).
 *
 * http:// is spoken by the code in this file, on its own sockets. https:// goes through libhttp and libtls, the client that
 * https_get(1) uses (and shows more of): the server's certificate chain, its dates and its name are verified against the trust
 * store ($SSL_CERT_FILE, else /etc/ssl/cacert.pem, or the file named by --cacert), and a connection whose certificate is refused is
 * never used. -k (--no-check-certificate) switches the verification off and says so on standard error, whatever -q says.
 * A redirect from http to https continues over TLS; one from https to http continues in the clear after a warning (libhttp's rule).
 * Neither path asks for compressed bodies: what the server sends is what is written.
 *
 * Exit status: 0 done, 1 a protocol error or a local failure, 2 usage, 4 the server could not be reached, 5 no safe TLS connection
 * (certificate refused, no trust store, handshake failed: the number wget uses for a failed verification), 8 the server answered
 * 400 or more.
 */
#include "netutil.h"
#include <getopt.h>
#include <http.h>
#include <tls.h>

typedef struct {
    char host[256];
    unsigned port;
    char path[1024];
    int tls;
} Url;

typedef struct {
    int quiet, include_headers, max_redirects, timeout_s;
    int insecure;                   /* -k: do not verify the server's certificate (https only) */
    const char *ca_file;            /* --cacert: the trusted roots, instead of $SSL_CERT_FILE or the default bundle */
    const char *agent;
    const char *extra_header;
} FetchOpts;

static int parse_url(const char *s, Url *u) {
    unsigned port = 80;
    u->tls = 0;
    if (!strncasecmp(s, "https://", 8)) { u->tls = 1; port = 443; s += 8; }
    else if (!strncasecmp(s, "http://", 7)) s += 7;
    else if (strstr(s, "://")) { kb_error("unsupported URL scheme in '%s'", s); return -1; }
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (hl == 0 || hl >= sizeof u->host) { kb_error("bad host in URL"); return -1; }
    char hostport[256];
    memcpy(hostport, s, hl);
    hostport[hl] = 0;
    u->port = port;
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

/* Decides where the body goes and opens it: -O FILE ("-" is standard output), else for wget a free name made from the last part of
 * the URL path ('.1', '.2', ... rather than overwriting), else standard output. Prints the Length and Saving lines unless quiet.
 * The name chosen ("" for standard output) is copied to dest; returns the descriptor, or -1 after saying why. */
static int open_dest(const char *outname, int to_file_default, const char *urlpath, long long clen, const char *ctype, int quiet,
                     char *dest_out, size_t dest_cap) {
    char autoname[300];
    const char *dest = outname;
    if (!dest && to_file_default) {
        base_name(urlpath, autoname, sizeof autoname);
        char try_[320];
        strlcpy(try_, autoname, sizeof try_);
        for (int k = 1; access(try_, F_OK) == 0 && k < 100; k++) snprintf(try_, sizeof try_, "%s.%d", autoname, k);
        strlcpy(autoname, try_, sizeof autoname);
        dest = autoname;
    }
    int out = 1;
    if (dest && strcmp(dest, "-")) {
        out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { kb_error("%s: %s", dest, strerror(errno)); return -1; }
    }
    if (!quiet) {
        if (clen >= 0) fprintf(stderr, "Length: %lld [%s]\n", clen, ctype[0] ? ctype : "unknown");
        else fprintf(stderr, "Length: unspecified [%s]\n", ctype[0] ? ctype : "unknown");
        if (out != 1) fprintf(stderr, "Saving to: '%s'\n", dest);
    }
    strlcpy(dest_out, out != 1 ? dest : "", dest_cap);
    return out;
}

/* ---------------------------------------------------------------------------------------------------------- https */

#define SEC_HEAD_MAX 16384

typedef struct {
    const FetchOpts *o;
    const char *outname;
    int to_file_default;
    HttpUrl cur, next;              /* the URL of the response being read; where the redirect that was just followed points */
    int have_next;
    int plain_hop;                  /* the connection being made leads to a plain http server (after a redirect from https) */
    int status;                     /* of the response being read (0: none yet) */
    long long clen;
    char ctype[128];
    char *headers;                  /* -i: the status line and header lines of the response being read */
    size_t hlen;
    int out;                        /* the destination; -1 while it is not open yet (it opens at the first byte of a 2xx body) */
    int open_failed, werr;
    char dest[320];
    long long total;
} Secure;

static int sec_open(Secure *s) {
    s->out = open_dest(s->outname, s->to_file_default, s->cur.path, s->clen, s->ctype, s->o->quiet, s->dest, sizeof s->dest);
    if (s->out < 0) { s->open_failed = 1; return -1; }
    if (s->headers) { if (write(s->out, s->headers, s->hlen) < 0 || write(s->out, "\r\n", 2) < 0) { /* the body write below reports it */ } }
    return 0;
}

static void sec_status(void *ctx, int status, const char *reason) {
    Secure *s = ctx;
    if (s->have_next) { s->cur = s->next; s->have_next = 0; }          /* this response comes from the redirect's target */
    if (s->plain_hop && !s->o->quiet) fprintf(stderr, "Connecting to %s:%d... connected.\n", s->cur.host, s->cur.port);
    s->plain_hop = 0;
    s->status = status;
    s->clen = -1;
    s->ctype[0] = 0;
    if (s->headers) s->hlen = (size_t)snprintf(s->headers, SEC_HEAD_MAX, "HTTP/1.1 %d %s\r\n", status, reason);
    if (!s->o->quiet) fprintf(stderr, "HTTP request sent, awaiting response... %d %s\n", status, reason);
}

static int sec_header(void *ctx, const char *name, const char *value) {
    Secure *s = ctx;
    if (s->headers && s->hlen + strlen(name) + strlen(value) + 5 < SEC_HEAD_MAX)
        s->hlen += (size_t)snprintf(s->headers + s->hlen, SEC_HEAD_MAX - s->hlen, "%s: %s\r\n", name, value);
    if (!strcasecmp(name, "Content-Length")) s->clen = atoll(value);
    else if (!strcasecmp(name, "Content-Type")) strlcpy(s->ctype, value, sizeof s->ctype);
    else if (!strcasecmp(name, "Location") && s->status >= 300 && s->status < 400) {
        HttpUrl nx;
        if (http_url_resolve(&s->cur, value, &nx) == 0) { s->next = nx; s->have_next = 1; }
    }
    return 0;
}

static int sec_body(void *ctx, const uint8_t *d, size_t n) {
    Secure *s = ctx;
    if (s->status < 200 || s->status >= 300) return 0;                  /* an error page is not written anywhere */
    if (s->out < 0 && sec_open(s) < 0) return 1;
    while (n) {
        ssize_t w = write(s->out, d, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { s->werr = errno ? errno : EIO; return 1; }
        d += w;
        n -= (size_t)w;
        s->total += w;
    }
    return 0;
}

static void sec_event(void *ctx, const char *line) {
    Secure *s = ctx;
    if (!strncmp(line, "WARNING", 7)) fprintf(stderr, "%s: %s\n", kb_name, line);          /* https to http: not protected any more */
    else if (!strncmp(line, "redirect ", 9) && !s->o->quiet && s->have_next) {
        char next[2048];
        if (http_url_format(&s->next, next, sizeof next) > 0) fprintf(stderr, "Location: %s [following]\n", next);
    } else if (!strncmp(line, "connecting to ", 14)) {                                  /* a TLS connection reports itself in sec_tls */
        const HttpUrl *c = s->have_next ? &s->next : &s->cur;
        s->plain_hop = strcmp(c->scheme, "https") != 0;
    }
}

static void sec_tls(void *ctx, const TlsInfo *in) {
    Secure *s = ctx;
    const HttpUrl *c = s->have_next ? &s->next : &s->cur;               /* a redirect's target is connected before its response arrives */
    if (!s->o->quiet && (in->verified || in->insecure)) {
        fprintf(stderr, "Connecting to %s:%d... connected (%s, %s, %s).\n", c->host, c->port, in->version_name ? in->version_name : "TLS",
                in->suite_name ? in->suite_name : "?", in->verified ? "certificate verified" : "certificate NOT verified");
    }
    if (in->insecure)
        fprintf(stderr, "%s: WARNING: the server's certificate was NOT checked (-k); anyone on the way can read and change this connection%s%s\n",
                kb_name, in->verify_error ? ". Verification would have said: " : "", in->verify_error ? tls_strerror(in->verify_error) : "");
}

static int secure_failure(const Secure *s, const HttpResult *r) {
    if (s->open_failed) return 1;                                       /* open_dest said why */
    if (s->werr) { kb_error("%s: %s", s->dest[0] ? s->dest : "standard output", strerror(s->werr)); return 1; }
    if (s->out >= 0 && r->error != HTTP_E_TLS) {
        kb_error("the transfer ended early after %lld bytes: %s", s->total, r->error_text);
        return 1;
    }
    switch (r->error) {
    case HTTP_E_DNS: case HTTP_E_CONNECT: case HTTP_E_TIMEOUT:
        kb_error("%s", r->error_text);
        return 4;
    case HTTP_E_TLS:
        kb_error("%s", r->error_text);
        return 5;
    case HTTP_E_IO:
        kb_error("%s", r->error_text);
        return s->status ? 1 : 4;                                       /* before any answer the server was not reached */
    default:
        kb_error("%s", r->error_text);
        return 1;
    }
}

/* One URL over TLS, and whatever redirects follow it (libhttp follows them, back to plain http too, at most redirects_left). */
static int fetch_secure(const char *url, const char *outname, int to_file_default, const FetchOpts *o, int redirects_left) {
    Secure *s = calloc(1, sizeof *s);
    if (!s) { kb_error("out of memory"); return 1; }
    s->o = o;
    s->outname = outname;
    s->to_file_default = to_file_default;
    s->out = -1;
    s->clen = -1;
    if (http_url_parse(url, &s->cur) != 0) { kb_error("'%s' is not a URL that can be fetched", url); free(s); return 1; }
    if (o->include_headers) s->headers = malloc(SEC_HEAD_MAX);

    char extra[2400];
    snprintf(extra, sizeof extra, "User-Agent: %s\r\n%s%s", o->agent, o->extra_header ? o->extra_header : "", o->extra_header ? "\r\n" : "");
    TlsConfig tc;
    memset(&tc, 0, sizeof tc);
    if (o->insecure) tc.flags |= TLS_INSECURE;
    tc.ca_file = o->ca_file;
    tc.timeout_ms = o->timeout_s * 1000;
    HttpOptions ho;
    memset(&ho, 0, sizeof ho);
    ho.tls = &tc;
    ho.max_redirects = redirects_left > 0 ? redirects_left : -1;        /* < 0: the answer is the redirect itself */
    ho.timeout_ms = tc.timeout_ms;
    ho.no_decode = 1;
    ho.extra_headers = extra;
    ho.on_body = sec_body;
    ho.on_status = sec_status;
    ho.on_header = sec_header;
    ho.on_event = sec_event;
    ho.on_tls = sec_tls;
    ho.ctx = s;

    HttpResult res;
    int rc = http_get(url, &ho, &res);
    int result, saved = 0;
    if (rc != 0) {
        result = secure_failure(s, &res);
    } else if (res.status >= 400) {
        fprintf(stderr, "%s: server returned %d %s\n", kb_name, res.status, res.reason);
        result = 8;
    } else if (res.status < 200 || res.status >= 300) {
        if (res.status == 301 || res.status == 302 || res.status == 303 || res.status == 307 || res.status == 308) kb_error("too many redirects");
        else kb_error("unexpected status %d", res.status);
        result = 1;
    } else if (s->out < 0 && sec_open(s) < 0) {                          /* an empty body still makes the file */
        result = 1;
    } else {
        saved = 1;
        result = 0;
    }
    if (s->out > 1) close(s->out);
    if (saved && !o->quiet && s->out != 1) fprintf(stderr, "'%s' saved [%lld]\n", s->dest, s->total);
    free(s->headers);
    free(s);
    return result;
}

static int do_fetch(const char *start_url, const char *outname, int to_file_default, const FetchOpts *o) {
    char url[2048];
    strlcpy(url, start_url, sizeof url);
    for (int hop = 0; hop <= o->max_redirects; hop++) {
        Url u;
        if (parse_url(url, &u) < 0) return 1;
        if (u.tls) return fetch_secure(url, outname, to_file_default, o, o->max_redirects - hop);
        if (!o->quiet) fprintf(stderr, "Connecting to %s:%u... ", u.host, u.port);
        int fd = connect_to(&u, o->timeout_s);
        if (fd < 0) return 4;
        if (!o->quiet) fprintf(stderr, "connected.\n");
        char req[2048];
        char hostport[300];
        if (u.port == 80) snprintf(hostport, sizeof hostport, "%s", u.host); else snprintf(hostport, sizeof hostport, "%s:%u", u.host, u.port);
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n", u.path, hostport, o->agent);
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
        char dest[320];
        int out = open_dest(outname, to_file_default, u.path, clen, ctype, o->quiet, dest, sizeof dest);
        if (out < 0) { free(r); free(headers); close(fd); return 1; }
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

#define FETCH_USAGE "usage: %s [-q] [-i] [-k] [-O FILE|-] [-T SECONDS] [-U AGENT] [-H 'Header: v'] [-r REDIRECTS] [--cacert FILE] URL..."

static int fetch_main(int argc, char **argv, int wget_style) {
    FetchOpts o = { .max_redirects = 8, .timeout_s = 30, .agent = "fetch/1.0 (Gensokyo)" };
    static const struct option longopts[] = {
        { "insecure", no_argument, NULL, 'k' },                         /* curl's name */
        { "no-check-certificate", no_argument, NULL, 'k' },             /* wget's */
        { "cacert", required_argument, NULL, 'C' },                     /* curl's */
        { "ca-certificate", required_argument, NULL, 'C' },             /* wget's */
        { "ca", required_argument, NULL, 'C' },                         /* https_get's */
        { NULL, 0, NULL, 0 },
    };
    const char *out = NULL;
    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "qO:o:T:U:H:r:iL4k", longopts, NULL)) != -1) {
        switch (opt) {
        case 'q': o.quiet = 1; break;
        case 'O': case 'o': out = optarg; break;
        case 'T': o.timeout_s = atoi(optarg); if (o.timeout_s < 1) o.timeout_s = 1; break;
        case 'U': o.agent = optarg; break;
        case 'H': o.extra_header = optarg; break;
        case 'r': o.max_redirects = atoi(optarg); break;
        case 'i': o.include_headers = 1; break;
        case 'k': o.insecure = 1; break;
        case 'C': o.ca_file = optarg; break;
        case 'L': case '4': break;
        default: kb_error(FETCH_USAGE, kb_name); return 2;
        }
    }
    if (optind >= argc) { kb_error(FETCH_USAGE, kb_name); return 2; }
    int rc = 0;
    for (int i = optind; i < argc; i++) {
        int r = do_fetch(argv[i], out, wget_style, &o);
        if (r) rc = r;
    }
    return rc;
}

APPLET(fetch) { return fetch_main(argc, argv, 0); }
APPLET(wget) { return fetch_main(argc, argv, 1); }
