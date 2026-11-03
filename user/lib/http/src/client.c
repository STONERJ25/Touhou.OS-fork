/*
 * http_get: the blocking fetcher of libhttp. One request per connection ("Connection: close"), redirects followed
 * (301, 302, 303, 307, 308; only GET and HEAD are made, so nothing changes method), cookies kept when a jar is given.
 *
 * End of a response over TLS: a body framed by Content-Length or chunked encoding is complete when its framing says so, and a
 * missing close_notify after that does not matter. A body that only the end of the connection delimits is complete only if
 * the server said close_notify; otherwise http_get returns HTTP_E_TRUNCATED (the bytes have gone to on_body already), because
 * someone who can cut the connection could have cut the body.
 */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <http.h>

typedef struct {
    const HttpOptions *o;
    HttpResult *r;
    HttpUrl url;
    int follow;                                                      /* redirects are followed */
    int redirect;                                                    /* this response is one, and is being followed */
    char location[HTTP_MAX_PATH + 300];
} fetch;

static void event(const HttpOptions *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void event(const HttpOptions *o, const char *fmt, ...) {
    if (!o->on_event) return;
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    o->on_event(o->ctx, buf);
}

static int fail(HttpResult *r, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(HttpResult *r, int err, const char *fmt, ...) {
    r->error = err;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error_text, sizeof r->error_text, fmt, ap);
    va_end(ap);
    return err;
}

static int ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

/* ---------------------------------------------------------------------------------------------------- the parser's callbacks */

static int cb_status(void *ctx, int status, const char *reason) {
    fetch *f = ctx;
    f->r->status = status;
    snprintf(f->r->reason, sizeof f->r->reason, "%s", reason);
    f->r->content_length = -1;
    f->r->content_type[0] = f->r->content_encoding[0] = 0;
    f->location[0] = 0;
    if (f->o->on_status) f->o->on_status(f->o->ctx, status, reason);
    return 0;
}

static int cb_header(void *ctx, const char *name, const char *value) {
    fetch *f = ctx;
    if (ieq(name, "Location")) snprintf(f->location, sizeof f->location, "%s", value);
    else if (ieq(name, "Content-Type")) snprintf(f->r->content_type, sizeof f->r->content_type, "%s", value);
    else if (ieq(name, "Content-Encoding")) snprintf(f->r->content_encoding, sizeof f->r->content_encoding, "%s", value);
    else if (ieq(name, "Content-Length")) f->r->content_length = strtoll(value, NULL, 10);
    else if (ieq(name, "Set-Cookie") && f->o->jar) http_jar_set(f->o->jar, &f->url, value, (int64_t)time(NULL));
    if (f->o->on_header) return f->o->on_header(f->o->ctx, name, value);
    return 0;
}

static int is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

static int cb_headers_done(void *ctx, int status) {
    fetch *f = ctx;
    f->redirect = f->follow && is_redirect(status) && f->location[0];
    return f->redirect ? 1 : 0;                                      /* a redirect's body is thrown away */
}

static int cb_body(void *ctx, const uint8_t *d, size_t n) {
    fetch *f = ctx;
    f->r->body_bytes += n;
    return f->o->on_body ? f->o->on_body(f->o->ctx, d, n) : 0;
}

/* ---------------------------------------------------------------------------------------------------- connections */

typedef struct {
    int fd;
    TlsConn *tls;
    int timeout_ms;
} conn;

static int tcp_open(fetch *f, int timeout_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port[8];
    snprintf(port, sizeof port, "%d", f->url.port);
    int rc = getaddrinfo(f->url.host, port, &hints, &res);
    if (rc != 0 || !res) return fail(f->r, HTTP_E_DNS, "%s: the name does not resolve (%s)", f->url.host, gai_strerror(rc));
    int fd = -1, err = 0;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) { err = errno; continue; }
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);    /* bounds connect() */
        char addr[32];
        const unsigned char *ip = (const unsigned char *)&((struct sockaddr_in *)a->ai_addr)->sin_addr;
        snprintf(addr, sizeof addr, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        event(f->o, "connecting to %s (%s) port %d", f->url.host, addr, f->url.port);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        err = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (err == ETIMEDOUT || err == EAGAIN) return fail(f->r, HTTP_E_TIMEOUT, "%s port %d: no answer to the connection attempt", f->url.host, f->url.port);
        return fail(f->r, HTTP_E_CONNECT, "%s port %d: %s", f->url.host, f->url.port, strerror(err));
    }
    struct timeval none = { 0, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int wait_fd(int fd, short ev, int timeout_ms) {
    struct pollfd p = { fd, ev, 0 };
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r > 0) return 0;
        if (r == 0) return HTTP_E_TIMEOUT;
        if (errno != EINTR) return HTTP_E_IO;
    }
}

static int conn_write(conn *c, const char *d, size_t n) {
    if (c->tls) return tls_write(c->tls, d, n) == (long)n ? 0 : HTTP_E_IO;
    while (n) {
        if (wait_fd(c->fd, POLLOUT, c->timeout_ms)) return HTTP_E_TIMEOUT;
        ssize_t w = send(c->fd, d, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return HTTP_E_IO;
        d += w;
        n -= (size_t)w;
    }
    return 0;
}

/* > 0 bytes, 0 the end (for TLS: close_notify), HTTP_E_TRUNCATED (TLS without close_notify), < 0 error */
static long conn_read(conn *c, uint8_t *buf, size_t n) {
    if (c->tls) {
        long r = tls_read(c->tls, buf, n);
        if (r >= 0) return r;
        if (r == TLS_E_TRUNCATED) return HTTP_E_TRUNCATED;
        if (r == TLS_E_TIMEOUT) return HTTP_E_TIMEOUT;
        return HTTP_E_IO;
    }
    for (;;) {
        if (wait_fd(c->fd, POLLIN, c->timeout_ms)) return HTTP_E_TIMEOUT;
        ssize_t r = recv(c->fd, buf, n, 0);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? HTTP_E_IO : (long)r;
    }
}

static void conn_close(conn *c) {
    if (c->tls) tls_close(c->tls);
    if (c->fd >= 0) close(c->fd);
    c->tls = NULL;
    c->fd = -1;
}

/* ---------------------------------------------------------------------------------------------------- one exchange */

static int exchange(fetch *f, int timeout_ms) {
    const HttpOptions *o = f->o;
    HttpResult *r = f->r;
    conn c = { -1, NULL, timeout_ms };
    int https = strcmp(f->url.scheme, "https") == 0;
    c.fd = tcp_open(f, timeout_ms);
    if (c.fd < 0) return c.fd;
    if (https) {
        TlsConfig cfg;
        if (o->tls) cfg = *o->tls;
        else memset(&cfg, 0, sizeof cfg);
        if (!cfg.alpn) cfg.alpn = "http/1.1";
        if (!cfg.timeout_ms) cfg.timeout_ms = timeout_ms;
        cfg.flags &= ~(unsigned)TLS_NONBLOCK;
        c.tls = tls_connect(c.fd, f->url.host, &cfg);
        if (!c.tls) { conn_close(&c); return fail(r, HTTP_E_NOMEM, "out of memory for TLS"); }
        int st = tls_status(c.tls);
        if (o->on_tls) o->on_tls(o->ctx, tls_info(c.tls));
        if (st < 0) {
            r->tls_error = st;
            fail(r, HTTP_E_TLS, "%s: %s", f->url.host, tls_error_string(c.tls));
            conn_close(&c);
            return HTTP_E_TLS;
        }
        const TlsInfo *ti = tls_info(c.tls);
        event(o, "%s, %s, %s; certificate %s", ti->version_name, ti->suite_name, ti->group_name,
              ti->insecure ? "NOT VERIFIED (--insecure)" : "verified");
        r->tls = 1;
    } else {
        r->tls = 0;
    }
    char cookie[4096], extra[6000];
    extra[0] = 0;
    if (o->jar && http_jar_header(o->jar, &f->url, (int64_t)time(NULL), cookie, sizeof cookie) > 0)
        snprintf(extra, sizeof extra, "Cookie: %s\r\n", cookie);
    if (o->extra_headers) strncat(extra, o->extra_headers, sizeof extra - strlen(extra) - 1);
    char req[HTTP_MAX_PATH + 8000];
    long rl = http_request_build(req, sizeof req, o->head ? "HEAD" : "GET", &f->url, extra, !o->no_decode);
    if (rl < 0) { conn_close(&c); return fail(r, HTTP_E_URL, "the request does not fit"); }
    event(o, "%s %s", o->head ? "HEAD" : "GET", f->url.path);
    if (conn_write(&c, req, (size_t)rl)) { conn_close(&c); return fail(r, HTTP_E_IO, "%s: sending the request failed", f->url.host); }

    HttpCallbacks cb = { cb_status, cb_header, cb_headers_done, cb_body, f };
    HttpParser p;
    http_parser_init(&p, &cb, (o->no_decode ? 0 : HTTP_DECODE) | (o->head ? HTTP_HEAD : 0));
    uint8_t buf[16384];
    int rc = 0;
    for (;;) {
        long n = conn_read(&c, buf, sizeof buf);
        if (n > 0) {
            rc = http_parser_feed(&p, buf, (size_t)n, NULL);
            if (rc != 0) break;
            continue;
        }
        if (n == 0 || n == HTTP_E_TRUNCATED) {
            int framed_by_close = p.framing == HTTP_FRAME_CLOSE;
            rc = http_parser_eof(&p);
            if (rc == 1 && n == HTTP_E_TRUNCATED && framed_by_close && !f->redirect) {
                rc = fail(r, HTTP_E_TRUNCATED, "%s closed the TLS connection without close_notify, and the body had no length: it may be incomplete", f->url.host);
            } else if (rc < 0) {
                rc = fail(r, rc, "%s: %s", f->url.host, p.err);
            }
            break;
        }
        if (n == HTTP_E_TIMEOUT) rc = fail(r, HTTP_E_TIMEOUT, "%s sent nothing for %d s", f->url.host, timeout_ms / 1000);
        else if (c.tls) rc = fail(r, HTTP_E_IO, "%s: %s", f->url.host, tls_error_string(c.tls));
        else rc = fail(r, HTTP_E_IO, "%s: the connection broke", f->url.host);
        break;
    }
    if (rc < 0 && !r->error) fail(r, rc, "%s: %s", f->url.host, p.err[0] ? p.err : http_strerror(rc));
    r->wire_bytes = p.body_wire;
    http_parser_free(&p);
    conn_close(&c);
    return rc < 0 ? rc : 0;
}

int http_get(const char *url, const HttpOptions *o, HttpResult *r) {
    static const HttpOptions none;
    if (!o) o = &none;
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    fetch f;
    memset(&f, 0, sizeof f);
    f.o = o;
    f.r = r;
    if (http_url_parse(url, &f.url) != 0) return fail(r, HTTP_E_URL, "'%s' is not an http:// or https:// URL this client can use", url);
    int max = o->max_redirects == 0 ? 10 : o->max_redirects;
    f.follow = max > 0;
    int timeout = o->timeout_ms > 0 ? o->timeout_ms : 30000;
    for (;;) {
        http_url_format(&f.url, r->url, sizeof r->url);
        f.redirect = 0;
        int rc = exchange(&f, timeout);
        if (rc < 0) return rc;
        if (!f.redirect) return 0;
        if (r->redirects >= max) return fail(r, HTTP_E_REDIRECTS, "more than %d redirects (the last one to %s)", max, f.location);
        HttpUrl next;
        if (http_url_resolve(&f.url, f.location, &next) != 0) return fail(r, HTTP_E_URL, "a redirect to '%s', which is not an http(s) URL", f.location);
        if (!strcmp(f.url.scheme, "https") && strcmp(next.scheme, "https")) {
            r->downgraded = 1;
            event(o, "WARNING: redirected from https to plain http (%s): what follows is not protected", next.host);
        }
        r->redirects++;
        event(o, "redirect %d (%d) to %s", r->redirects, r->status, f.location);
        f.url = next;
    }
}
