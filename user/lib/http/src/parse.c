/*
 * The response parser of libhttp: a push parser for HTTP/1.1 responses (RFC 9112), fed bytes in pieces of any size.
 *
 *   status line, header fields (an obsolete folded line is joined to the one before, RFC 9112 5.2), then the body framed by
 *   (RFC 9112 6.3, in this order): no body for HEAD, 1xx, 204 and 304; Transfer-Encoding chunked; Content-Length; the end of
 *   the connection. Interim 1xx responses (100 Continue, 103 Early Hints) are read and skipped.
 *   Content-Encoding gzip, x-gzip and deflate are inflated with libz when HTTP_DECODE is given ("deflate" is zlib-wrapped by the
 *   standard and raw from some servers: the first two bytes decide). Any other coding is passed through as it came.
 *
 * Limits: a line (and so a field) of 8 KiB, 64 KiB and 256 fields of header, chunk sizes below 2^60. Every number is checked for overflow.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <z.h>
#include <http.h>

enum { S_STATUS, S_HEADERS, S_BODY, S_CHUNK_SIZE, S_CHUNK_DATA, S_CHUNK_CRLF, S_TRAILERS, S_DONE, S_ERROR };

#define MAX_HEADER_BYTES (64 * 1024)
#define MAX_HEADERS 256

static int fail(HttpParser *p, int err, const char *msg) {
    if (!p->error) {
        p->error = err;
        snprintf(p->err, sizeof p->err, "%s", msg);
    }
    p->state = S_ERROR;
    return err;
}

void http_parser_init(HttpParser *p, const HttpCallbacks *cb, int flags) {
    memset(p, 0, sizeof *p);
    if (cb) p->cb = *cb;
    p->flags = flags;
    p->content_length = -1;
    p->remaining = -1;
    p->state = S_STATUS;
}

void http_parser_free(HttpParser *p) {
    if (p->z) {
        z_inflate_end(p->z);
        free(p->z);
        p->z = NULL;
    }
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* ---------------------------------------------------------------------------------------------------- the body */

static int emit(HttpParser *p, const uint8_t *d, size_t n) {
    p->body_out += n;
    if (n && p->cb.on_body && p->cb.on_body(p->cb.ctx, d, n)) return fail(p, HTTP_E_CALLBACK, "the receiver of the body stopped");
    return 0;
}

/* Runs the inflater over d[0..n): returns the bytes it took (all of them, unless the stream ended among them), or < 0. */
static long z_run(HttpParser *p, const uint8_t *d, size_t n) {
    ZStream *z = p->z;
    uint8_t out[16384];
    size_t taken = 0;
    for (;;) {
        z->next_in = d + taken;
        z->avail_in = n - taken;
        z->next_out = out;
        z->avail_out = sizeof out;
        int rc = z_inflate(z);
        size_t used = (n - taken) - z->avail_in, got = sizeof out - z->avail_out;
        taken += used;
        z->avail_in = 0;
        if (got && emit(p, out, got)) return p->error;
        if (rc == Z_STREAM_END) {
            p->z_done = 1;
            return (long)taken;
        }
        if (rc < 0 && rc != Z_BUF_ERROR) {
            char msg[100];
            snprintf(msg, sizeof msg, "the compressed body is corrupt (%s)", z_strerror(rc));
            return fail(p, HTTP_E_DECODE, msg);
        }
        if (got == sizeof out) continue;                             /* the inflater may hold more output */
        if (taken == n) return (long)taken;                          /* all taken: more input is needed */
        if (used == 0 && got == 0) return fail(p, HTTP_E_DECODE, "the compressed body makes no progress");
    }
}

static int inflate_some(HttpParser *p, const uint8_t *d, size_t n) {
    if (!p->z_started) {
        int fmt = Z_GZIP;
        if (p->encoding == 2) {                                      /* deflate: zlib header (CM 8, FCHECK) or raw DEFLATE */
            while (n && p->zhead_n < 2) { p->zhead[p->zhead_n++] = *d++; n--; }
            if (p->zhead_n < 2) return 0;
            unsigned cmf = p->zhead[0], flg = p->zhead[1];
            fmt = (cmf & 0x0f) == 8 && ((cmf << 8) | flg) % 31 == 0 ? Z_ZLIB : Z_RAW;
        }
        ZStream *z = calloc(1, sizeof *z);
        if (!z) return fail(p, HTTP_E_NOMEM, "out of memory for the inflater");
        if (z_inflate_init(z, fmt) != Z_OK) { free(z); return fail(p, HTTP_E_NOMEM, "the inflater did not start"); }
        p->z = z;
        p->z_started = 1;
        if (p->zhead_n) {
            uint8_t head[2] = { p->zhead[0], p->zhead[1] };
            p->zhead_n = 0;
            long r = z_run(p, head, 2);
            if (r < 0) return (int)r;
        }
    }
    while (n) {
        if (p->z_done) {
            /* after a stream: another gzip member (it starts 1f 8b, possibly split over two feeds), or bytes to ignore */
            if (p->encoding != 1 || p->z_garbage) return 0;
            while (n && p->zmagic_n < 2) { p->zmagic[p->zmagic_n++] = *d++; n--; }
            if (p->zmagic_n < 2) return 0;
            p->zmagic_n = 0;
            if (p->zmagic[0] != 0x1f || p->zmagic[1] != 0x8b) { p->z_garbage = 1; return 0; }
            z_inflate_reset(p->z);
            p->z_done = 0;
            long r = z_run(p, p->zmagic, 2);
            if (r < 0) return (int)r;
            continue;
        }
        long r = z_run(p, d, n);
        if (r < 0) return (int)r;
        d += r;
        n -= (size_t)r;
    }
    return 0;
}

static int deliver(HttpParser *p, const uint8_t *d, size_t n) {
    p->body_wire += n;
    if (p->discard || n == 0) return 0;
    if (p->encoding == 1 || p->encoding == 2) return inflate_some(p, d, n);
    return emit(p, d, n);
}

static int body_done(HttpParser *p) {
    if (!p->discard && (p->encoding == 1 || p->encoding == 2) && (p->zhead_n || (p->z_started && !p->z_done)))
        return fail(p, HTTP_E_DECODE, "the compressed body ends before its stream does");
    p->state = S_DONE;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------- lines */

static int status_line(HttpParser *p, char *s) {
    if (strncmp(s, "HTTP/1.", 7) != 0 || s[7] < '0' || s[7] > '9' || (s[8] != ' ' && s[8] != 0))
        return fail(p, HTTP_E_PARSE, "the response does not start with an HTTP/1.x status line");
    p->version_minor = s[7] - '0';
    const char *c = s + 9;
    if (s[8] == 0 || c[0] < '1' || c[0] > '5' || c[1] < '0' || c[1] > '9' || c[2] < '0' || c[2] > '9' || (c[3] != ' ' && c[3] != 0))
        return fail(p, HTTP_E_PARSE, "a malformed status code");
    p->status = (c[0] - '0') * 100 + (c[1] - '0') * 10 + (c[2] - '0');
    const char *reason = c[3] ? c + 4 : "";
    if (p->cb.on_status && p->cb.on_status(p->cb.ctx, p->status, reason)) return fail(p, HTTP_E_CALLBACK, "stopped at the status line");
    p->state = S_HEADERS;
    return 0;
}

static int parse_length(const char *v, int64_t *out) {
    int64_t n = -1;
    for (;;) {                                                       /* "42" or "42, 42" (a repeated identical value) */
        while (*v == ' ' || *v == '\t') v++;
        if (*v < '0' || *v > '9') return -1;
        int64_t x = 0;
        while (*v >= '0' && *v <= '9') {
            if (x > (INT64_MAX - 9) / 10) return -1;
            x = x * 10 + (*v++ - '0');
        }
        if (n >= 0 && x != n) return -1;
        n = x;
        while (*v == ' ' || *v == '\t') v++;
        if (*v == 0) break;
        if (*v++ != ',') return -1;
    }
    *out = n;
    return 0;
}

static int is_tchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'*+-.^_`|~", c));
}

/* one complete field line "name: value" (folded continuation lines already joined) */
static int field(HttpParser *p, char *s) {
    char *colon = strchr(s, ':');
    if (!colon || colon == s) return fail(p, HTTP_E_PARSE, "a header line without a field name");
    for (char *q = s; q < colon; q++)
        if (!is_tchar(*q)) return fail(p, HTTP_E_PARSE, "a field name with a space or a bad character in it");
    *colon = 0;
    char *v = colon + 1;
    while (*v == ' ' || *v == '\t') v++;
    size_t vl = strlen(v);
    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
    if (p->state == S_TRAILERS) return 0;                            /* trailers are read and left alone */
    if (++p->header_count > MAX_HEADERS) return fail(p, HTTP_E_TOO_BIG, "more than 256 header fields");
    if (ieq(s, "Content-Length")) {
        int64_t n;
        if (parse_length(v, &n) != 0 || (p->content_length >= 0 && n != p->content_length)) return fail(p, HTTP_E_PARSE, "an invalid Content-Length");
        p->content_length = n;
    } else if (ieq(s, "Transfer-Encoding")) {
        /* the codings in order; only a final "chunked" is understood (anything before it would need decoding too) */
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", v);
        char *save = NULL, *last = NULL;
        int count = 0;
        for (char *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ' || *t == '\t') t++;
            size_t tl = strlen(t);
            while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t')) t[--tl] = 0;
            last = t;
            count++;
        }
        if (last && ieq(last, "chunked") && count == 1 && !p->chunked) p->chunked = 1;
        else p->te_other = 1;
    } else if (ieq(s, "Content-Encoding") && (p->flags & HTTP_DECODE)) {
        if (ieq(v, "gzip") || ieq(v, "x-gzip")) p->encoding = 1;
        else if (ieq(v, "deflate")) p->encoding = 2;
        else if (!ieq(v, "identity") && v[0]) p->encoding = 3;
    }
    if (p->cb.on_header && p->cb.on_header(p->cb.ctx, s, v)) return fail(p, HTTP_E_CALLBACK, "stopped at a header");
    return 0;
}

static int headers_done(HttpParser *p) {
    if (p->status >= 100 && p->status < 200 && p->status != 101) {  /* interim: the real response follows */
        HttpCallbacks cb = p->cb;
        int flags = p->flags;
        http_parser_init(p, &cb, flags);
        return 0;
    }
    int rc = p->cb.on_headers_done ? p->cb.on_headers_done(p->cb.ctx, p->status) : 0;
    if (rc < 0) return fail(p, HTTP_E_CALLBACK, "stopped after the header");
    p->discard = rc == 1;
    if ((p->flags & HTTP_HEAD) || p->status == 204 || p->status == 304 || p->status < 200) {
        p->framing = HTTP_FRAME_NONE;
        p->state = S_DONE;
        return 1;
    }
    if (p->te_other) return fail(p, HTTP_E_UNSUPPORTED, "the body uses a transfer coding other than chunked");
    if (p->chunked) {
        p->framing = HTTP_FRAME_CHUNKED;
        p->state = S_CHUNK_SIZE;
        return 0;
    }
    if (p->content_length >= 0) {
        p->framing = HTTP_FRAME_LENGTH;
        p->remaining = p->content_length;
        p->state = S_BODY;
        return p->remaining == 0 ? body_done(p) : 0;
    }
    p->framing = HTTP_FRAME_CLOSE;
    p->remaining = -1;
    p->state = S_BODY;
    return 0;
}

static int chunk_size(HttpParser *p, const char *s) {
    uint64_t n = 0;
    int digits = 0;
    while (*s == ' ' || *s == '\t') s++;
    for (; *s; s++, digits++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (v < 0) break;
        if (n >> 59) return fail(p, HTTP_E_PARSE, "a chunk size beyond any reason");
        n = n << 4 | (unsigned)v;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (!digits || (*s && *s != ';')) return fail(p, HTTP_E_PARSE, "a malformed chunk size line");
    if (n == 0) {
        p->state = S_TRAILERS;
        return 0;
    }
    p->remaining = (int64_t)n;
    p->state = S_CHUNK_DATA;
    return 0;
}

/* a whole line (without CR LF) in a line state */
static int line(HttpParser *p, char *s, size_t len) {
    switch (p->state) {
    case S_STATUS:
        if (len == 0) return 0;                                      /* tolerated: empty lines before the status line */
        return status_line(p, s);
    case S_HEADERS:
    case S_TRAILERS:
        return 0;                                                    /* handled in fold_or_field */
    case S_CHUNK_SIZE:
        return chunk_size(p, s);
    case S_CHUNK_CRLF:
        if (len != 0) return fail(p, HTTP_E_PARSE, "chunk data longer than its size");
        p->state = S_CHUNK_SIZE;
        return 0;
    default:
        return 0;
    }
}

int http_parser_feed(HttpParser *p, const uint8_t *d, size_t n, size_t *used) {
    size_t i = 0;
    while (i < n && p->state != S_DONE && p->state != S_ERROR) {
        if (p->state == S_BODY || p->state == S_CHUNK_DATA) {
            size_t take = n - i;
            if (p->remaining >= 0 && (int64_t)take > p->remaining) take = (size_t)p->remaining;
            if (deliver(p, d + i, take)) break;
            i += take;
            if (p->remaining >= 0) {
                p->remaining -= (int64_t)take;
                if (p->remaining == 0) {
                    if (p->state == S_CHUNK_DATA) p->state = S_CHUNK_CRLF;
                    else body_done(p);
                }
            }
            continue;
        }
        const uint8_t *nl = memchr(d + i, '\n', n - i);
        size_t take = nl ? (size_t)(nl - (d + i)) + 1 : n - i;
        if (p->line_len + take >= sizeof p->line) {
            fail(p, HTTP_E_TOO_BIG, "a line of the response is longer than 8 KiB");
            break;
        }
        int in_header = p->state == S_STATUS || p->state == S_HEADERS || p->state == S_TRAILERS;
        if (in_header && (p->header_bytes += take) > MAX_HEADER_BYTES) {
            fail(p, HTTP_E_TOO_BIG, "the response header is larger than 64 KiB");
            break;
        }
        memcpy(p->line + p->line_len, d + i, take);
        p->line_len += take;
        i += take;
        if (!nl) continue;
        size_t len = p->line_len - 1;                                /* without the LF; a CR before it goes too */
        if (len && p->line[len - 1] == '\r') len--;
        p->line[len] = 0;
        if (p->state == S_HEADERS || p->state == S_TRAILERS) {
            /* a field line is held back until the next line shows whether it continues it (obs-fold, joined with a space) */
            p->line_len = 0;
            if (len && (p->line[0] == ' ' || p->line[0] == '\t')) {
                if (!p->held[0]) { fail(p, HTTP_E_PARSE, "a folded header line with nothing before it"); break; }
                char *v = p->line;
                while (*v == ' ' || *v == '\t') v++;
                size_t cur = strlen(p->held);
                if (cur + 1 + strlen(v) >= sizeof p->held) { fail(p, HTTP_E_TOO_BIG, "a folded header field longer than 8 KiB"); break; }
                p->held[cur] = ' ';
                strcpy(p->held + cur + 1, v);
                continue;
            }
            if (p->held[0]) {
                char tmp[sizeof p->held];
                memcpy(tmp, p->held, sizeof tmp);
                p->held[0] = 0;
                if (field(p, tmp)) break;
            }
            if (len == 0) {
                if (p->state == S_TRAILERS) { body_done(p); break; }
                if (headers_done(p) < 0) break;
                continue;
            }
            if (len >= sizeof p->held) { fail(p, HTTP_E_TOO_BIG, "a header field longer than 8 KiB"); break; }
            memcpy(p->held, p->line, len + 1);
            continue;
        }
        p->line_len = 0;
        if (line(p, p->line, len) < 0) break;
    }
    if (used) *used = i;
    return p->state == S_ERROR ? p->error : p->state == S_DONE ? 1 : 0;
}

int http_parser_eof(HttpParser *p) {
    if (p->state == S_DONE) return 1;
    if (p->state == S_ERROR) return p->error;
    if (p->state == S_BODY && p->framing == HTTP_FRAME_CLOSE) return body_done(p);
    if (p->state == S_STATUS && p->line_len == 0 && p->header_bytes == 0) return fail(p, HTTP_E_TRUNCATED, "the server closed the connection without answering");
    return fail(p, HTTP_E_TRUNCATED, p->state == S_HEADERS || p->state == S_STATUS ? "the connection ended in the response header" : "the connection ended before the body was complete");
}
