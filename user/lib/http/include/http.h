/*
 * libhttp: a small HTTP/1.1 client for TouhouOS. RFC 9110 (semantics), RFC 9112 (message syntax), RFC 3986 (URLs), RFC 6265
 * (cookies). TLS comes from libtls (<tls.h>), gzip and deflate from libz (<z.h>).
 *
 * Three layers, so that a program with its own event loop (the browser) can take the lower ones:
 *
 *   URLs        http_url_parse, http_url_resolve, http_url_format
 *   messages    http_request_build writes a request; HttpParser is a push parser for responses: feed it the bytes as they
 *               arrive, split anywhere, and it calls back with the status line, each header, the body (de-chunked, and
 *               inflated with HTTP_DECODE) and the end. It never reads a socket itself.
 *   fetching    http_get: name lookup, TCP, TLS with certificate verification, the request, redirects, cookies, the body to a
 *               callback. Blocking, with timeouts.
 *
 *     HttpOptions o = { .on_body = write_out, .ctx = &file };
 *     HttpResult r;
 *     if (http_get("https://example.org/", &o, &r) == 0) printf("%d, %llu bytes\n", r.status, r.body_bytes);
 *     else fprintf(stderr, "%s\n", r.error_text);
 */
#ifndef LIBHTTP_H
#define LIBHTTP_H

#include <stddef.h>
#include <stdint.h>
#include <tls.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HTTP_E_URL = -100,          /* not an http(s) URL this library can use */
    HTTP_E_DNS = -101,          /* the host name did not resolve */
    HTTP_E_CONNECT = -102,      /* no TCP connection */
    HTTP_E_TLS = -103,          /* the TLS handshake failed or the certificate was refused (HttpResult.tls_error says why) */
    HTTP_E_IO = -104,           /* the connection broke */
    HTTP_E_TIMEOUT = -105,
    HTTP_E_PARSE = -106,        /* a malformed response */
    HTTP_E_TOO_BIG = -107,      /* header lines or counts beyond the limits */
    HTTP_E_TRUNCATED = -108,    /* the connection ended before the body was complete (or, over TLS, without close_notify
                                   for a body that only the end of the connection delimits) */
    HTTP_E_DECODE = -109,       /* the gzip/deflate body is corrupt */
    HTTP_E_REDIRECTS = -110,    /* too many redirects */
    HTTP_E_CALLBACK = -111,     /* a callback asked to stop */
    HTTP_E_UNSUPPORTED = -112,  /* a transfer coding other than chunked */
    HTTP_E_NOMEM = -113,
};

/* ---- URLs ---- */
#define HTTP_MAX_HOST 256
#define HTTP_MAX_PATH 4096

typedef struct HttpUrl {
    char scheme[8];             /* "http" or "https" */
    char host[HTTP_MAX_HOST];   /* lower case; a dotted IPv4 address stays as it is */
    int port;                   /* the explicit port or the scheme's default */
    int explicit_port;
    char path[HTTP_MAX_PATH];   /* path and query, "/" at least, no fragment; percent-encoded as needed on the wire */
} HttpUrl;

int  http_url_parse(const char *s, HttpUrl *u);                                  /* 0 or HTTP_E_URL */
int  http_url_resolve(const HttpUrl *base, const char *ref, HttpUrl *out);       /* RFC 3986 5.2: a Location header against the URL */
int  http_url_format(const HttpUrl *u, char *buf, size_t cap);                   /* "https://host[:port]/path"; the length or < 0 */

/* ---- requests ---- */
/* "GET /path HTTP/1.1" with Host, User-Agent, Accept, Accept-Encoding (gzip, deflate when decode), Connection: close, then
 * extra ("Name: value\r\n" lines, may be NULL). Returns the length, or < 0 if it does not fit. */
long http_request_build(char *buf, size_t cap, const char *method, const HttpUrl *u, const char *extra, int decode);

/* ---- responses: the push parser ---- */
#define HTTP_DECODE  0x01       /* inflate Content-Encoding gzip / x-gzip / deflate */
#define HTTP_HEAD    0x02       /* the response to a HEAD request: it has no body whatever its headers say */

typedef struct HttpCallbacks {
    int (*on_status)(void *ctx, int status, const char *reason);         /* every non-zero return stops the parser */
    int (*on_header)(void *ctx, const char *name, const char *value);    /* also for trailers after a chunked body */
    int (*on_headers_done)(void *ctx, int status);                        /* return 1 to discard the body (redirects), < 0 stop */
    int (*on_body)(void *ctx, const uint8_t *data, size_t n);
    void *ctx;
} HttpCallbacks;

enum { HTTP_FRAME_NONE, HTTP_FRAME_LENGTH, HTTP_FRAME_CHUNKED, HTTP_FRAME_CLOSE };

typedef struct HttpParser {
    HttpCallbacks cb;
    int flags;
    int state, error;
    int status, version_minor;
    int64_t content_length;     /* -1 when absent */
    int64_t remaining;          /* body bytes left in this message or chunk */
    int framing;                /* HTTP_FRAME_* */
    int chunked, te_other, discard;
    int encoding;               /* 0 none, 1 gzip, 2 deflate, 3 unknown (passed through) */
    void *z;                    /* the inflater, when decoding */
    int z_started, z_done, z_garbage;
    uint8_t zhead[2], zmagic[2];   /* the first bytes of a deflate body; of what follows a gzip member */
    int zhead_n, zmagic_n;
    char line[8192];
    size_t line_len;
    char held[8192];            /* the last field line (as long as a line may be: real servers send 4.5 KiB Content-Security-Policy fields), held back until the next line shows it is not folded */
    size_t header_bytes;
    int header_count;
    uint64_t body_wire, body_out;   /* bytes as received / as delivered */
    char err[160];
} HttpParser;

void http_parser_init(HttpParser *p, const HttpCallbacks *cb, int flags);
/* Feeds n bytes; *used (may be NULL) says how many belonged to the response. 0 more wanted, 1 the response is complete,
 * < 0 an HTTP_E_* error (p->err has a sentence). Interim 1xx responses are skipped. */
int  http_parser_feed(HttpParser *p, const uint8_t *data, size_t n, size_t *used);
/* The connection ended: 1 if that completes the response (a body delimited by the close), else HTTP_E_TRUNCATED */
int  http_parser_eof(HttpParser *p);
void http_parser_free(HttpParser *p);

/* ---- cookies (an optional jar, kept in memory) ---- */
typedef struct HttpCookieJar HttpCookieJar;
HttpCookieJar *http_jar_new(void);
void http_jar_free(HttpCookieJar *j);
void http_jar_set(HttpCookieJar *j, const HttpUrl *from, const char *set_cookie, int64_t now);   /* one Set-Cookie value */
int  http_jar_header(HttpCookieJar *j, const HttpUrl *to, int64_t now, char *buf, size_t cap);  /* "a=1; b=2" for a Cookie header */
int  http_jar_count(const HttpCookieJar *j);

/* ---- fetching ---- */
typedef struct HttpResult HttpResult;

typedef struct HttpOptions {
    const TlsConfig *tls;       /* for https: NULL means the defaults (verification against the default root store) */
    int max_redirects;          /* 0: 10; < 0: do not follow */
    int timeout_ms;             /* connect, and the longest silence while reading; 0: 30 s */
    int no_decode;              /* do not ask for or inflate gzip/deflate */
    int head;                   /* HEAD instead of GET */
    const char *extra_headers;  /* "Name: value\r\n" lines */
    HttpCookieJar *jar;         /* NULL: no cookies */
    int (*on_body)(void *ctx, const uint8_t *data, size_t n);           /* the final response's body (non-zero stops) */
    void (*on_status)(void *ctx, int status, const char *reason);       /* every response's status line, redirects included */
    int (*on_header)(void *ctx, const char *name, const char *value);   /* every response's headers, redirects included */
    void (*on_event)(void *ctx, const char *line);                      /* progress for -v: connecting, TLS, redirects */
    void (*on_tls)(void *ctx, const TlsInfo *info);                      /* each TLS connection, after its handshake */
    void *ctx;
} HttpOptions;

struct HttpResult {
    int status;                 /* of the final response (0 if none came) */
    char reason[64];
    char url[HTTP_MAX_PATH + 300];   /* the final URL, after redirects */
    int redirects;
    int downgraded;             /* a redirect led from https to http */
    int64_t content_length;     /* -1 when not given */
    uint64_t body_bytes;        /* delivered to on_body (decoded) */
    uint64_t wire_bytes;        /* body bytes as they came */
    char content_type[128];
    char content_encoding[32];
    int tls;                    /* the final response came over TLS */
    int tls_error;              /* TLS_E_* when error == HTTP_E_TLS */
    int error;                  /* 0 or HTTP_E_* */
    char error_text[512];
};

int http_get(const char *url, const HttpOptions *o, HttpResult *res);    /* 0 when a final response arrived (any status) */
const char *http_strerror(int err);

#ifdef __cplusplus
}
#endif
#endif
