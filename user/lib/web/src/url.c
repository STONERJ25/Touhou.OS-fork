/*
 * URL resolution: the parts of the WHATWG URL standard a document needs to turn href and src attributes into absolute URLs
 * against its base URL (the document URL, or <base href>).
 *
 * Handled: schemes (special: http, https, ws, wss, ftp, file), "//" scheme-relative, "/" path-absolute, "?" and "#" only,
 * relative paths merged with the base and their "." and ".." segments removed, backslashes as slashes in special URLs,
 * tab and newline removal, leading and trailing control/space trimming, lowercase scheme and host, default ports dropped,
 * percent-encoding of spaces, controls and non-ASCII bytes in paths, queries and fragments, opaque URLs (mailto:, data:,
 * javascript:) kept as they are. Not handled: IDNA (non-ASCII hosts stay UTF-8), IPv6 normalization, the "is a valid
 * domain" checks. The result is NUL-terminated in the arena given.
 */
#include "webint.h"

typedef struct {
    const char *s;
    size_t scheme_len;          /* without ':' */
    size_t auth_start, auth_len;  /* after "//" */
    bool has_auth;
    size_t path_start, path_len;
    size_t query_start, query_len;   /* after '?'; query_start = 0 means none */
    bool has_query;
} Parts;

static bool is_special(const char *s, size_t n) {
    static const char *const sp[] = { "http", "https", "ws", "wss", "ftp", "file" };
    for (size_t i = 0; i < 6; i++)
        if (strlen(sp[i]) == n && strncasecmp(s, sp[i], n) == 0) return true;
    return false;
}

/* the length of "scheme" when s starts with "scheme:", else 0 */
static size_t scheme_of(const char *s, size_t n) {
    if (!n || !((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z')) return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (c == ':') return i;
        if (!((c | 0x20) >= 'a' && (c | 0x20) <= 'z') && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') return 0;
    }
    return 0;
}

static void split(const char *s, size_t n, Parts *p) {
    memset(p, 0, sizeof *p);
    p->s = s;
    p->scheme_len = scheme_of(s, n);
    size_t i = p->scheme_len ? p->scheme_len + 1 : 0;
    bool special = p->scheme_len && is_special(s, p->scheme_len);
    bool file = p->scheme_len == 4 && strncasecmp(s, "file", 4) == 0;
    if (special && !file) {
        /* "special authority ignore slashes": any number of slashes, then the authority */
        p->has_auth = true;
        while (i < n && (s[i] == '/' || s[i] == '\\')) i++;
        p->auth_start = i;
        while (i < n && s[i] != '/' && s[i] != '\\' && s[i] != '?' && s[i] != '#') i++;
        p->auth_len = i - p->auth_start;
    } else if (i + 1 < n && (s[i] == '/' || s[i] == '\\') && (s[i + 1] == '/' || s[i + 1] == '\\')) {
        p->has_auth = true;
        i += 2;
        p->auth_start = i;
        while (i < n && s[i] != '/' && s[i] != '\\' && s[i] != '?' && s[i] != '#') i++;
        p->auth_len = i - p->auth_start;
    }
    p->path_start = i;
    while (i < n && s[i] != '?' && s[i] != '#') i++;
    p->path_len = i - p->path_start;
    if (i < n && s[i] == '?') {
        p->has_query = true;
        p->query_start = ++i;
        while (i < n && s[i] != '#') i++;
        p->query_len = i - p->query_start;
    }
}

static void put_encoded(WebBuf *o, const char *s, size_t n, bool special, bool query) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && special && !query) c = '/';
        if (c <= 0x20 || c >= 0x7F || c == '"' || (!query && (c == '<' || c == '>' || c == '`')) || (query && (c == '<' || c == '>'))) {
            buf_putc(o, '%');
            buf_putc(o, hex[c >> 4]);
            buf_putc(o, hex[c & 15]);
        } else buf_putc(o, (char)c);
    }
}

/* Writes an absolute path with its "." and ".." segments resolved (the WHATWG path state: ".." drops the previous segment,
 * "%2e" counts as a dot, a final "." or ".." leaves a trailing slash). */
static void put_path(WebBuf *o, const char *path, size_t n, bool special) {
    typedef struct { uint32_t s, n; } Seg;
    VEC(Seg) segs = { NULL, 0, 0 };
    size_t i = 0;
    if (n && (path[0] == '/' || (special && path[0] == '\\'))) i = 1;
    for (;;) {
        size_t s = i;
        while (i < n && !(path[i] == '/' || (special && path[i] == '\\'))) i++;
        const char *seg = path + s;
        size_t sl = i - s;
        bool last = i >= n;
        bool dot = (sl == 1 && seg[0] == '.') || (sl == 3 && strncasecmp(seg, "%2e", 3) == 0);
        bool dotdot = (sl == 2 && seg[0] == '.' && seg[1] == '.') ||
                      (sl == 4 && (strncasecmp(seg, ".%2e", 4) == 0 || strncasecmp(seg, "%2e.", 4) == 0)) ||
                      (sl == 6 && strncasecmp(seg, "%2e%2e", 6) == 0);
        Seg empty = { 0, 0 }, this = { (uint32_t)s, (uint32_t)sl };
        if (dotdot) {
            if (segs.n) segs.n--;
            if (last) vec_push(o->a, segs, empty);
        } else if (dot) {
            if (last) vec_push(o->a, segs, empty);
        } else {
            vec_push(o->a, segs, this);
        }
        if (last) break;
        i++;
    }
    buf_putc(o, '/');
    for (uint32_t k = 0; k < segs.n; k++) {
        if (k) buf_putc(o, '/');
        put_encoded(o, path + segs.v[k].s, segs.v[k].n, special, false);
    }
    vec_free(o->a, segs);
}

static void put_authority(WebBuf *o, const char *a, size_t n, const char *scheme, size_t sn) {
    /* userinfo@host:port, host lowercased; the default port of the scheme is dropped */
    size_t at = n;
    for (size_t i = n; i > 0; i--)
        if (a[i - 1] == '@') { at = i - 1; break; }
    size_t hs = 0;
    if (at < n) {
        buf_put(o, a, at + 1);
        hs = at + 1;
    }
    size_t he = n, ps = n;
    if (hs < n && a[hs] == '[') {
        for (size_t i = hs; i < n; i++) if (a[i] == ']') { he = i + 1; break; }
        if (he < n && a[he] == ':') ps = he + 1;
    } else {
        for (size_t i = hs; i < n; i++) if (a[i] == ':') { he = i; ps = i + 1; break; }
    }
    for (size_t i = hs; i < he; i++) {
        char c = a[i];
        buf_putc(o, (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
    }
    if (ps < n) {
        const char *port = a + ps;
        size_t pl = n - ps;
        while (pl > 1 && port[0] == '0') { port++; pl--; }
        bool deflt = (sn == 4 && strncasecmp(scheme, "http", 4) == 0 && pl == 2 && memcmp(port, "80", 2) == 0) ||
                     (sn == 5 && strncasecmp(scheme, "https", 5) == 0 && pl == 3 && memcmp(port, "443", 3) == 0) ||
                     (sn == 2 && strncasecmp(scheme, "ws", 2) == 0 && pl == 2 && memcmp(port, "80", 2) == 0) ||
                     (sn == 3 && strncasecmp(scheme, "wss", 3) == 0 && pl == 3 && memcmp(port, "443", 3) == 0) ||
                     (sn == 3 && strncasecmp(scheme, "ftp", 3) == 0 && pl == 2 && memcmp(port, "21", 2) == 0);
        if (pl && !deflt) {
            buf_putc(o, ':');
            buf_put(o, port, pl);
        }
    }
}

static void put_scheme(WebBuf *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        buf_putc(o, (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
    }
    buf_putc(o, ':');
}

/* Canonical form of an absolute URL (special or not). */
static void put_absolute(WebBuf *o, const char *s, size_t n) {
    Parts p;
    split(s, n, &p);
    bool special = is_special(s, p.scheme_len);
    put_scheme(o, s, p.scheme_len);
    if (!special && !p.has_auth) {                           /* opaque: mailto:x, data:..., javascript:... */
        buf_put(o, s + p.scheme_len + 1, n - p.scheme_len - 1);
        return;
    }
    buf_puts(o, "//");
    if (p.has_auth) put_authority(o, s + p.auth_start, p.auth_len, s, p.scheme_len);
    if (special || p.path_len) put_path(o, s + p.path_start, p.path_len, special);
    if (p.has_query) {
        buf_putc(o, '?');
        put_encoded(o, s + p.query_start, p.query_len, special, true);
    }
    const char *hash = memchr(s, '#', n);
    if (hash) {
        buf_putc(o, '#');
        put_encoded(o, hash + 1, (size_t)(s + n - hash - 1), special, true);
    }
}

char *url_resolve(WebArena *a, const char *base, const char *rel0, size_t n0) {
    /* strip leading/trailing C0 controls and spaces, and every tab and newline */
    WebBuf rb;
    buf_init(&rb, a);
    size_t s = 0, e = n0;
    while (s < e && (unsigned char)rel0[s] <= 0x20) s++;
    while (e > s && (unsigned char)rel0[e - 1] <= 0x20) e--;
    for (size_t i = s; i < e; i++)
        if (rel0[i] != '\t' && rel0[i] != '\n' && rel0[i] != '\r') buf_putc(&rb, rel0[i]);
    const char *rel = rb.p ? rb.p : "";
    size_t n = rb.n;
    WebBuf o;
    buf_init(&o, a);
    size_t sl = scheme_of(rel, n);
    Parts bp;
    size_t bn = base ? strlen(base) : 0;
    split(base ? base : "", bn, &bp);
    bool bspecial = is_special(base ? base : "", bp.scheme_len);
    if (sl) {
        bool special = is_special(rel, sl);
        bool same = base && bp.scheme_len == sl && strncasecmp(rel, base, sl) == 0;
        bool two_slashes = sl + 2 < n && (rel[sl + 1] == '/' || rel[sl + 1] == '\\') && (rel[sl + 2] == '/' || rel[sl + 2] == '\\');
        if (!(special && same && !two_slashes)) {
            put_absolute(&o, rel, n);
            goto done;
        }
        rel += sl + 1;                                       /* "http:foo" or "http:/foo" against an http base: relative */
        n -= sl + 1;
    }
    if (!base || !bp.scheme_len) goto fail;
    if (!bspecial && !bp.has_auth) {                         /* an opaque base: only fragments resolve */
        if (n && rel[0] == '#') {
            const char *hash = memchr(base, '#', bn);
            buf_put(&o, base, hash ? (size_t)(hash - base) : bn);
            buf_put(&o, rel, n);
            goto done;
        }
        goto fail;
    }
    put_scheme(&o, base, bp.scheme_len);
    if (n >= 2 && (rel[0] == '/' || (bspecial && rel[0] == '\\')) && (rel[1] == '/' || (bspecial && rel[1] == '\\'))) {
        WebBuf full;
        buf_init(&full, a);
        buf_put(&full, base, bp.scheme_len + 1);
        buf_put(&full, rel, n);
        o.n = 0;
        put_absolute(&o, full.p, full.n);
        buf_free(&full);
        goto done;
    }
    buf_puts(&o, "//");
    if (bp.has_auth) put_authority(&o, base + bp.auth_start, bp.auth_len, base, bp.scheme_len);
    Parts rp;
    split(rel, n, &rp);                                      /* no scheme, no authority: path, query, fragment */
    const char *hash = memchr(rel, '#', n);
    if (rp.path_len == 0) {
        put_path(&o, base + bp.path_start, bp.path_len, bspecial);
        if (rp.has_query) {
            buf_putc(&o, '?');
            put_encoded(&o, rel + rp.query_start, rp.query_len, bspecial, true);
        } else if (bp.has_query) {
            buf_putc(&o, '?');
            buf_put(&o, base + bp.query_start, bp.query_len);
        }
    } else {
        WebBuf path;
        buf_init(&path, a);
        if (rel[0] == '/' || (bspecial && rel[0] == '\\')) {
            buf_put(&path, rel, rp.path_len);
        } else {
            const char *bpath = base + bp.path_start;
            size_t cut = 0;
            for (size_t i = 0; i < bp.path_len; i++)
                if (bpath[i] == '/' || (bspecial && bpath[i] == '\\')) cut = i + 1;
            if (!cut) buf_putc(&path, '/');
            buf_put(&path, bpath, cut);
            buf_put(&path, rel, rp.path_len);
        }
        put_path(&o, path.p, path.n, bspecial);
        buf_free(&path);
        if (rp.has_query) {
            buf_putc(&o, '?');
            put_encoded(&o, rel + rp.query_start, rp.query_len, bspecial, true);
        }
    }
    if (hash) {
        buf_putc(&o, '#');
        put_encoded(&o, hash + 1, (size_t)(rel + n - hash - 1), bspecial, true);
    }
done:
    buf_free(&rb);
    {
        char *r = wa_strndup(a, o.p ? o.p : "", o.n);
        buf_free(&o);
        return r;
    }
fail:
    buf_free(&rb);
    buf_free(&o);
    return NULL;
}

const char *doc_resolve_url(WebDoc *d, const char *rel, size_t n, WebArena *a) {
    return url_resolve(a, d->base_url, rel, n);
}
