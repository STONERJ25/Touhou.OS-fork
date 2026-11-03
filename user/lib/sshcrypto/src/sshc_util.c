/*
 * The small things every part of the client needs: the protocol's data types (RFC 4251, section 5), base64 and hex, bounded string copies, the clock,
 * the random numbers, and where the user's files live.
 */
#include <sshc.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef SSHC_HOST
#include <sys/random.h>
#else
#include <gensokyo.h>
#endif

int sshc_verbose;

/* ------------------------------------------------------------------------------------------------ reading and writing the wire */

void sshc_rd_init(SshcRd *r, const void *p, size_t n) { r->p = p; r->n = n; r->bad = false; }

uint8_t sshc_rd_u8(SshcRd *r) {
    if (r->n < 1) { r->bad = true; return 0; }
    r->n--;
    return *r->p++;
}

uint32_t sshc_rd_u32(SshcRd *r) {
    if (r->n < 4) { r->bad = true; r->n = 0; return 0; }
    uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4;
    r->n -= 4;
    return v;
}

uint64_t sshc_rd_u64(SshcRd *r) {
    uint64_t hi = sshc_rd_u32(r);
    return hi << 32 | sshc_rd_u32(r);
}

bool sshc_rd_bool(SshcRd *r) { return sshc_rd_u8(r) != 0; }

const uint8_t *sshc_rd_str(SshcRd *r, size_t *len) {
    uint32_t n = sshc_rd_u32(r);
    if (r->bad || n > r->n) { r->bad = true; r->n = 0; *len = 0; return r->p; }
    const uint8_t *s = r->p;
    r->p += n;
    r->n -= n;
    *len = n;
    return s;
}

void sshc_rd_skip(SshcRd *r, size_t n) {
    if (r->n < n) { r->bad = true; r->n = 0; return; }
    r->p += n;
    r->n -= n;
}

void sshc_wr_init(SshcWr *w, void *buf, size_t cap) { w->p = buf; w->n = 0; w->cap = cap; w->bad = false; }

void sshc_wr_raw(SshcWr *w, const void *p, size_t n) {
    if (w->bad || n > w->cap - w->n) { w->bad = true; return; }
    if (n) memcpy(w->p + w->n, p, n);
    w->n += n;
}

void sshc_wr_u8(SshcWr *w, uint8_t v) { sshc_wr_raw(w, &v, 1); }

void sshc_wr_u32(SshcWr *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    sshc_wr_raw(w, b, 4);
}

void sshc_wr_u64(SshcWr *w, uint64_t v) {
    sshc_wr_u32(w, (uint32_t)(v >> 32));
    sshc_wr_u32(w, (uint32_t)v);
}

void sshc_wr_bool(SshcWr *w, bool v) { sshc_wr_u8(w, v ? 1 : 0); }

void sshc_wr_str(SshcWr *w, const void *p, size_t n) {
    sshc_wr_u32(w, (uint32_t)n);
    sshc_wr_raw(w, p, n);
}

void sshc_wr_cstr(SshcWr *w, const char *s) { sshc_wr_str(w, s, strlen(s)); }

/* A big-endian magnitude as an mpint (RFC 4251): no leading zero bytes, and one zero byte in front when the top bit would otherwise read as a sign. */
size_t sshc_mpint(uint8_t *out, const uint8_t *be, size_t n) {
    size_t skip = 0;
    while (skip < n && be[skip] == 0) skip++;
    size_t len = n - skip;
    bool pad = len && (be[skip] & 0x80);
    uint32_t total = (uint32_t)(len + pad);
    out[0] = (uint8_t)(total >> 24); out[1] = (uint8_t)(total >> 16); out[2] = (uint8_t)(total >> 8); out[3] = (uint8_t)total;
    size_t o = 4;
    if (pad) out[o++] = 0;
    memcpy(out + o, be + skip, len);
    return o + len;
}

bool sshc_str_is(const uint8_t *p, size_t n, const char *s) { return strlen(s) == n && !memcmp(p, s, n); }

bool sshc_namelist_has(const uint8_t *list, size_t n, const char *name) {
    size_t len = strlen(name), i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && list[j] != ',') j++;
        if (j - i == len && !memcmp(list + i, name, len)) return true;
        i = j + 1;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------------ base64 and hex */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t sshc_b64_encode(const uint8_t *in, size_t n, char *out, bool pad) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        if (i + 1 < n) out[o++] = B64[v >> 6 & 63]; else if (pad) out[o++] = '=';
        if (i + 2 < n) out[o++] = B64[v & 63]; else if (pad) out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

int sshc_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = in[i];
        if (ch == '=') {                                    /* padding: only '=' may follow */
            for (size_t k = i; k < n; k++) if (in[k] != '=') return -1;
            break;
        }
        const char *p = ch ? strchr(B64, ch) : NULL;
        if (!p) return -1;
        acc = acc << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    return (int)o;
}

void sshc_hex(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hex[in[i] >> 4]; out[2 * i + 1] = hex[in[i] & 15]; }
    out[2 * n] = 0;
}

/* ------------------------------------------------------------------------------------------------ text */

void sshc_printable(char *out, size_t cap, const uint8_t *p, size_t n, bool keep_nl) {
    size_t o = 0;
    if (!cap) return;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        uint8_t c = p[i];
        bool ok = (c >= 0x20 && c < 0x7f) || (keep_nl && (c == '\n' || c == '\t'));
        if (keep_nl && c == '\r') continue;                 /* CRLF text prints as LF text */
        out[o++] = ok ? (char)c : '?';
    }
    out[o] = 0;
}

void sshc_cpy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool sshc_fmt(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < cap;
}

void sshc_debug(int level, const char *fmt, ...) {
    if (sshc_verbose < level) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "debug%d: %s\r\n", level, line);               /* \r\n: the terminal may be in raw mode while a session runs */
}

/* ------------------------------------------------------------------------------------------------ clock, randomness, comparison */

long sshc_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

void sshc_random(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        size_t want = n > 256 ? 256 : n;
#ifdef SSHC_HOST
        ssize_t got = getrandom(b, want, 0);
#else
        int got = gap_getrandom(b, want);
#endif
        if (got <= 0) {
            fprintf(stderr, "ssh: the system has no random numbers to give (%d): stopping rather than going on without them\n", (int)got);
            _exit(70);
        }
        b += got;
        n -= (size_t)got;
    }
}

int sshc_ct_equal(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

/* ------------------------------------------------------------------------------------------------ paths */

const char *sshc_home(void) {
    const char *h = getenv("HOME");
    if (h && h[0]) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
    return "/";
}

const char *sshc_local_user(void) {
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name && pw->pw_name[0]) return pw->pw_name;
    const char *u = getenv("USER");
    if (u && u[0]) return u;
    u = getenv("LOGNAME");
    return u && u[0] ? u : "user";
}

bool sshc_expand_path(const char *in, char *out, size_t cap, const char *host, const char *ruser, int port) {
    size_t o = 0;
    if (in[0] == '~' && (in[1] == '/' || in[1] == 0)) {
        const char *h = sshc_home();
        size_t hl = strlen(h);
        if (hl + 1 > cap) return false;
        memcpy(out, h, hl);
        o = hl;
        in++;
    }
    for (; *in; in++) {
        const char *rep = NULL;
        char num[16];
        if (*in == '%') {
            in++;
            switch (*in) {
            case '%': rep = "%"; break;
            case 'd': rep = sshc_home(); break;
            case 'u': rep = sshc_local_user(); break;
            case 'h': rep = host ? host : ""; break;
            case 'r': rep = ruser ? ruser : ""; break;
            case 'p': snprintf(num, sizeof num, "%d", port); rep = num; break;
            default: return false;
            }
        }
        size_t rl = rep ? strlen(rep) : 1;
        if (o + rl + 1 > cap) return false;
        if (rep) memcpy(out + o, rep, rl); else out[o] = *in;
        o += rl;
    }
    out[o] = 0;
    return true;
}

int sshc_mkdir_p(const char *dir) {
    char path[512];
    if (strlen(dir) >= sizeof path) return -1;
    memcpy(path, dir, strlen(dir) + 1);
    for (char *p = path + 1; ; p++) {
        if (*p == '/' || *p == 0) {
            char save = *p;
            *p = 0;
            struct stat st;
            if (stat(path, &st) != 0) {
                if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
            } else if (!S_ISDIR(st.st_mode)) {
                return -1;
            }
            *p = save;
            if (!save) break;
        }
    }
    return 0;
}
