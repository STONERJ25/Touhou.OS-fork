/*
 * The strict parser of the update manifest, format 1 (docs/UPDATE.md, "The manifest, format 1"; the grammar that tools/serve-update.py writes and
 * checks). It refuses every byte sequence the format does not allow. See updmanifest.h for why it is a file of its own.
 */
#include "updmanifest.h"
#include <stdio.h>
#include <string.h>

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

int upd_lower_hex(const char *s, size_t n, uint8_t *out) {       /* exactly n lowercase hex digits; out gets n/2 bytes (out may be NULL) */
    for (size_t i = 0; i < n; i++)
        if (hexval(s[i]) < 0) return -1;
    if (out)
        for (size_t i = 0; i < n / 2; i++) out[i] = (uint8_t)(hexval(s[2 * i]) * 16 + hexval(s[2 * i + 1]));
    return 0;
}

/* ------------------------------------------------------------------------------------------------ the manifest, format 1 (strict) */

static int is_alnum(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

/* "0" or [1-9][0-9]{0,maxdig-1} */
int upd_number_ok(const char *s, size_t n, size_t maxdig) {
    if (n == 0 || n > maxdig || (s[0] == '0' && n > 1)) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

int upd_path_ok(const char *s, size_t n) {
    if (n < 1 || n > 128) return 0;
    int comps = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && s[j] != '/') j++;
        size_t len = j - i;
        if (len < 1 || len > 64 || !(is_alnum(s[i]) || s[i] == '_')) return 0;
        for (size_t k = i + 1; k < j; k++)
            if (!(is_alnum(s[k]) || s[k] == '.' || s[k] == '_' || s[k] == '-')) return 0;
        if (++comps > 8) return 0;
        if (j == n) break;
        i = j + 1;
        if (i == n) return 0;                                   /* a trailing '/' */
    }
    return 1;
}

static int field(const char *line, size_t n, const char *name, const char **val, size_t *vn) {
    size_t k = strlen(name);
    if (n <= k + 1 || memcmp(line, name, k) || line[k] != ' ') return 0;
    *val = line + k + 1;
    *vn = n - k - 1;
    return 1;
}

/* 0, or -1 with why: the parse of docs/UPDATE.md's grammar, the same that parse_body() and split_manifest() of tools/serve-update.py accept */
int upd_parse_manifest(const uint8_t *d, size_t n, Manifest *m, char *why, size_t wn) {
#define BAD(...) do { snprintf(why, wn, __VA_ARGS__); return -1; } while (0)
    memset(m, 0, sizeof *m);
    if (n > MAX_MANIFEST) BAD("larger than %d bytes", MAX_MANIFEST);
    if (n == 0 || d[n - 1] != '\n') BAD("it does not end with a newline");
    for (size_t i = 0; i < n; i++)
        if (d[i] != '\n' && (d[i] < 0x20 || d[i] > 0x7e)) BAD("a byte that is not printable ASCII or LF at offset %zu", i);
    size_t cut = n - 1;
    while (cut > 0 && d[cut - 1] != '\n') cut--;
    const char *last = (const char *)d + cut;
    size_t lastn = n - 1 - cut;
    if (cut == 0 || lastn != 10 + 128 || memcmp(last, "signature ", 10) || upd_lower_hex(last + 10, 128, m->sig)) BAD("the last line is not 'signature <128 hex digits>'");
    m->body_len = cut;
    const char *line[64];
    size_t len[64];
    int nl = 0;
    for (size_t i = 0; i < cut;) {
        size_t j = i;
        while (d[j] != '\n') j++;
        if (nl == 64) BAD("too many lines");
        line[nl] = (const char *)d + i;
        len[nl++] = j - i;
        i = j + 1;
    }
    if (nl < 6) BAD("too few lines");
    if (len[0] != 15 || memcmp(line[0], "touhou-update 1", 15)) BAD("not a touhou-update 1 manifest");
    const char *v;
    size_t vn;
    if (!field(line[1], len[1], "version", &v, &vn) || vn > 64 || !is_alnum(v[0])) BAD("expected a valid 'version' line");
    for (size_t i = 1; i < vn; i++)
        if (!(is_alnum(v[i]) || strchr("._+-", v[i]))) BAD("expected a valid 'version' line");
    memcpy(m->version, v, vn);
    if (!field(line[2], len[2], "issued", &v, &vn) || !upd_number_ok(v, vn, 16)) BAD("expected a valid 'issued' line");
    for (size_t i = 0; i < vn; i++) m->issued = m->issued * 10 + (v[i] - '0');
    if (!field(line[3], len[3], "key", &v, &vn) || vn != 16 || upd_lower_hex(v, 16, NULL)) BAD("expected a valid 'key' line");
    memcpy(m->key_id, v, 16);
    if (len[nl - 1] != 3 || memcmp(line[nl - 1], "end", 3)) BAD("the last body line is not 'end'");
    for (int li = 4; li < nl - 1; li++) {
        const char *s = line[li], *e = s + len[li], *p[4];
        size_t pl[4];
        int parts = 0;
        for (const char *q = s;; ) {
            const char *sp = memchr(q, ' ', (size_t)(e - q));
            if (parts == 4) BAD("expected 'file PATH SIZE SHA256'");
            p[parts] = q;
            pl[parts++] = (size_t)((sp ? sp : e) - q);
            if (!sp) break;
            q = sp + 1;
        }
        if (parts != 4 || pl[0] != 4 || memcmp(p[0], "file", 4)) BAD("expected 'file PATH SIZE SHA256'");
        if (!upd_path_ok(p[1], pl[1])) BAD("refused path '%.*s'", (int)(pl[1] > 80 ? 80 : pl[1]), p[1]);
        if (!upd_number_ok(p[2], pl[2], 12)) BAD("bad size for %.*s", (int)pl[1], p[1]);
        if (pl[3] != 64 || upd_lower_hex(p[3], 64, NULL)) BAD("bad sha256 for %.*s", (int)pl[1], p[1]);
        if (m->n == MAX_MFILES) BAD("a manifest names 1 to %d files", MAX_MFILES);
        MEntry *f = &m->f[m->n];
        memcpy(f->path, p[1], pl[1]);
        f->path[pl[1]] = 0;
        if (m->n && strcmp(m->f[m->n - 1].path, f->path) >= 0) BAD("file lines must be in strictly ascending byte order of their paths");
        for (size_t i = 0; i < pl[2]; i++) f->size = f->size * 10 + (uint64_t)(p[2][i] - '0');
        upd_lower_hex(p[3], 64, f->sha);
        m->n++;
    }
    if (m->n < 1) BAD("a manifest names 1 to %d files", MAX_MFILES);
    return 0;
#undef BAD
}
