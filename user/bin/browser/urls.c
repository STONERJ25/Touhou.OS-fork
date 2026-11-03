/*
 * Addresses (see urls.h). RFC 3986 (syntax, percent-encoding, fragments), RFC 8089 (file: URLs).
 */
#include "urls.h"
#include "browser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* length of a scheme at the start of s ("http" in "http://x"), 0 when s does not start with scheme syntax followed by ':' */
static size_t scheme_len(const char *s) {
    if (!isalpha((unsigned char)s[0])) return 0;
    size_t n = 1;
    while (isalnum((unsigned char)s[n]) || s[n] == '+' || s[n] == '-' || s[n] == '.') n++;
    return s[n] == ':' ? n : 0;
}

static bool scheme_is(const char *s, size_t n, const char *name) { return strlen(name) == n && strncasecmp(s, name, n) == 0; }

BrUrlKind br_url_kind(const char *url) {
    size_t n = url ? scheme_len(url) : 0;
    if (!n) return BR_URL_NONE;
    if (scheme_is(url, n, "http")) return BR_URL_HTTP;
    if (scheme_is(url, n, "https")) return BR_URL_HTTPS;
    if (scheme_is(url, n, "file")) return BR_URL_FILE;
    if (scheme_is(url, n, "about")) return BR_URL_ABOUT;
    if (scheme_is(url, n, "data")) return BR_URL_DATA;
    return BR_URL_OTHER;
}

/* ---------------------------------------------------------------- percent-encoding */

static bool path_safe(unsigned char c) {
    if (isalnum(c)) return true;
    return strchr("-._~/:@!$&'()*+,;=", c) != NULL && c != 0;
}

/* appends the percent-encoded form of s to out (cap bytes, always terminated) */
static void encode_path(const char *s, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlen(out);
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (path_safe(c)) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

size_t br_percent_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hx[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hx, NULL, 16);
            r += 2;
        } else *w++ = *r;
    }
    *w = 0;
    return (size_t)(w - s);
}

/* ---------------------------------------------------------------- file: URLs */

/* the absolute path with "." and ".." resolved and "//" collapsed, in place */
static void normalize_path(char *p) {
    char *seg = p;
    size_t n = strlen(p);
    char *tmp = malloc(n + 2);
    if (!tmp) return;
    size_t o = 0;
    while (*seg) {
        while (*seg == '/') seg++;
        char *e = seg;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - seg);
        if (l == 0) break;
        if (l == 1 && seg[0] == '.') { /* skip */ }
        else if (l == 2 && seg[0] == '.' && seg[1] == '.') { while (o > 0 && tmp[o - 1] != '/') o--; if (o > 0) o--; }
        else { tmp[o++] = '/'; memcpy(tmp + o, seg, l); o += l; }
        seg = e;
    }
    bool trailing = n > 1 && p[n - 1] == '/';
    if (o == 0) tmp[o++] = '/';
    else if (trailing) tmp[o++] = '/';
    tmp[o] = 0;
    memcpy(p, tmp, o + 1);
    free(tmp);
}

void br_path_to_file_url(const char *path, char *url, size_t cap) {
    char norm[BR_MAX_URL];
    snprintf(norm, sizeof norm, "%s", path && *path == '/' ? path : "/");
    normalize_path(norm);
    snprintf(url, cap, "file://");
    encode_path(norm, url, cap);
}

bool br_file_url_to_path(const char *url, char *path, size_t cap) {
    if (br_url_kind(url) != BR_URL_FILE) return false;
    const char *p = url + 5;
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        const char *slash = strchr(p, '/');
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl != 0 && !(hl == 9 && strncasecmp(p, "localhost", 9) == 0)) return false;
        p = slash ? slash : "/";
    } else if (p[0] != '/') return false;
    size_t n = strcspn(p, "?#");
    if (n + 1 > cap) return false;
    memcpy(path, p, n);
    path[n] = 0;
    br_percent_decode(path);
    return path[0] == '/';
}

/* ---------------------------------------------------------------- what was typed */

static void trim_into(const char *in, char *out, size_t cap) {
    while (*in && isspace((unsigned char)*in)) in++;
    size_t n = strlen(in);
    while (n > 0 && isspace((unsigned char)in[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, in, n);
    out[n] = 0;
}

static void copy_url(const char *s, char *out, size_t cap) {      /* spaces and control characters become %XX, the rest is kept */
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c <= 0x20 || c == 0x7F) { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static bool is_ipv4_host(const char *h, size_t n) {
    int dots = 0;
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) {
        if (h[i] == '.') dots++;
        else if (!isdigit((unsigned char)h[i])) return false;
    }
    return dots == 3;
}

bool br_url_from_input(const char *input, const char *cwd, const char *home, char *out, size_t cap) {
    char t[BR_MAX_URL];
    out[0] = 0;
    trim_into(input ? input : "", t, sizeof t);
    if (!t[0]) { snprintf(out, cap, "about:blank"); return true; }
    size_t sl = scheme_len(t);
    if (sl) {
        const char *rest = t + sl + 1;
        bool known = scheme_is(t, sl, "about") || scheme_is(t, sl, "data") || scheme_is(t, sl, "javascript") || scheme_is(t, sl, "mailto") ||
                     scheme_is(t, sl, "file") || scheme_is(t, sl, "http") || scheme_is(t, sl, "https");
        bool slashes = rest[0] == '/' && rest[1] == '/';
        size_t digits = 0;
        while (isdigit((unsigned char)rest[digits])) digits++;
        bool port = digits > 0 && (rest[digits] == 0 || rest[digits] == '/' || rest[digits] == '?' || rest[digits] == '#');
        if (slashes || (known && !port)) {
            copy_url(t, out, cap);
            if (br_url_kind(out) == BR_URL_FILE && !(out[5] == '/' && out[6] == '/')) {          /* file:/x and file:x are paths */
                char path[BR_MAX_URL];
                snprintf(path, sizeof path, "%s", out + 5);
                br_percent_decode(path);
                if (path[0] == '/') br_path_to_file_url(path, out, cap);
            }
            return true;
        }
    }
    char path[BR_MAX_URL];
    path[0] = 0;
    if (t[0] == '/') snprintf(path, sizeof path, "%s", t);
    else if (t[0] == '~' && (t[1] == '/' || t[1] == 0) && home && *home) snprintf(path, sizeof path, "%s%s", home, t + 1);
    else if ((!strncmp(t, "./", 2) || !strncmp(t, "../", 3) || !strcmp(t, ".") || !strcmp(t, "..")) && cwd && *cwd) snprintf(path, sizeof path, "%s/%s", cwd, t);
    if (path[0]) { br_path_to_file_url(path, out, cap); return true; }
    if (cwd && *cwd && !strchr(t, '/') ) {                          /* a bare name of a file that is here */
        char full[BR_MAX_URL];
        snprintf(full, sizeof full, "%s/%s", cwd, t);
        if (access(full, F_OK) == 0) { br_path_to_file_url(full, out, cap); return true; }
    }
    for (const char *c = t; *c; c++) if (isspace((unsigned char)*c)) return false;
    size_t hl = strcspn(t, "/?#");
    size_t hostlen = hl;
    const char *colon = memchr(t, ':', hl);
    if (colon) hostlen = (size_t)(colon - t);
    bool local = (hostlen == 9 && strncasecmp(t, "localhost", 9) == 0) || is_ipv4_host(t, hostlen);
    bool dotted = memchr(t, '.', hostlen) != NULL;
    char tmp[BR_MAX_URL];
    snprintf(tmp, sizeof tmp, "%s://%s", local || !dotted ? "http" : "https", t);
    copy_url(tmp, out, cap);
    return true;
}

void br_url_split_fragment(const char *url, char *base, size_t cap, const char **frag) {
    const char *h = strchr(url, '#');
    size_t n = h ? (size_t)(h - url) : strlen(url);
    if (n >= cap) n = cap - 1;
    memcpy(base, url, n);
    base[n] = 0;
    if (frag) *frag = h ? h + 1 : NULL;
}

bool br_url_same_document(const char *a, const char *b) {
    char x[BR_MAX_URL], y[BR_MAX_URL];
    br_url_split_fragment(a, x, sizeof x, NULL);
    br_url_split_fragment(b, y, sizeof y, NULL);
    return strcmp(x, y) == 0;
}

void br_url_display(const char *url, char *out, size_t cap, size_t max_chars) {
    const char *s = url;
    BrUrlKind k = br_url_kind(url);
    if (k == BR_URL_HTTP) s = url + 7;
    else if (k == BR_URL_HTTPS) s = url + 8;
    snprintf(out, cap, "%s", s);
    size_t n = strlen(out);
    if (n > 0 && out[n - 1] == '/' && strchr(out, '/') == out + n - 1) out[--n] = 0;      /* "example.org/" -> "example.org" */
    if (max_chars > 3 && n > max_chars && max_chars + 1 <= cap) {
        size_t cut = max_chars - 3;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        strcpy(out + cut, "...");
    }
}
