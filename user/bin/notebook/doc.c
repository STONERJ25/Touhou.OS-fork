/* The document: a gap buffer holding the text, an incrementally maintained index of line starts and a multi-level undo history.
 *
 * Everything is measured in bytes of UTF-8 text with '\n' as the only line break (the file layer converts CRLF on the way in
 * and out). The gap sits where the last edit happened, so typing is O(1); anything that needs contiguous text asks doc_ptr(),
 * which moves the gap out of the way (cheap for one line, linear for the whole document). */
#include "notebook.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------- helpers */

/* libgap's memmove is "rep movsb", painfully slow when the machine is emulated; this one moves eight bytes at a time. */
void nb_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || !n) return;
    if (d < s || d >= s + n) {
        while (n >= 32) {
            uint64_t a, b, c, e;
            __builtin_memcpy(&a, s, 8); __builtin_memcpy(&b, s + 8, 8); __builtin_memcpy(&c, s + 16, 8); __builtin_memcpy(&e, s + 24, 8);
            __builtin_memcpy(d, &a, 8); __builtin_memcpy(d + 8, &b, 8); __builtin_memcpy(d + 16, &c, 8); __builtin_memcpy(d + 24, &e, 8);
            d += 32; s += 32; n -= 32;
        }
        while (n >= 8) { uint64_t a; __builtin_memcpy(&a, s, 8); __builtin_memcpy(d, &a, 8); d += 8; s += 8; n -= 8; }
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n >= 32) {
            uint64_t a, b, c, e;
            d -= 32; s -= 32;
            __builtin_memcpy(&a, s, 8); __builtin_memcpy(&b, s + 8, 8); __builtin_memcpy(&c, s + 16, 8); __builtin_memcpy(&e, s + 24, 8);
            __builtin_memcpy(d, &a, 8); __builtin_memcpy(d + 8, &b, 8); __builtin_memcpy(d + 16, &c, 8); __builtin_memcpy(d + 24, &e, 8);
            n -= 32;
        }
        while (n >= 8) { uint64_t a; d -= 8; s -= 8; __builtin_memcpy(&a, s, 8); __builtin_memcpy(d, &a, 8); n -= 8; }
        while (n--) *--d = *--s;
    }
}

int nb_u8_dec(const char *p, const char *end, uint32_t *cp) {
    if (p >= end) { *cp = 0; return 0; }
    unsigned c = (unsigned char)p[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t avail = (size_t)(end - p);
    if (c >= 0xC2 && c <= 0xDF && avail >= 2 && (((unsigned char)p[1]) & 0xC0) == 0x80) {
        *cp = ((c & 0x1Fu) << 6) | ((unsigned char)p[1] & 0x3Fu);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF && avail >= 3) {
        unsigned c1 = (unsigned char)p[1], c2 = (unsigned char)p[2];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0)) {
            *cp = ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
            return 3;
        }
    }
    if (c >= 0xF0 && c <= 0xF4 && avail >= 4) {
        unsigned c1 = (unsigned char)p[1], c2 = (unsigned char)p[2], c3 = (unsigned char)p[3];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 && !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90)) {
            *cp = ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
            return 4;
        }
    }
    *cp = 0xFFFD;
    return 1;
}

bool nb_u8_valid(const char *p, size_t n) {
    const char *end = p + n;
    while (p < end) {
        if ((unsigned char)*p < 0x80) { p++; continue; }
        uint32_t cp;
        int k = nb_u8_dec(p, end, &cp);
        if (k == 1) return false;                       /* a lone high byte: invalid */
        p += k;
    }
    return true;
}

int nb_char_class(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0 || cp == 0x3000) return 0;
    if (cp < 128) return ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') ? 2 : 1;
    return 2;
}

static void oom_exit(void) {
    static const char m[] = "notebook: out of memory (Yuyuko is hungry)\n";
    if (write(2, m, sizeof m - 1) < 0) { }
    _exit(1);
}

/* ---------------------------------------------------------------- creation and the gap */

void doc_init(Doc *d) {
    memset(d, 0, sizeof *d);
    d->cap = 4096;
    d->buf = calloc(1, d->cap + NB_PAD);
    d->capls = 256;
    d->ls = malloc((size_t)d->capls * sizeof(size_t));
    if (!d->buf || !d->ls) oom_exit();
    d->ge = d->cap;
    d->ls[0] = 0;
    d->nls = 1;
    d->dirty_line = INT_MAX;
}

static void urec_free(UndoRec *r) { free(r->del); free(r->ins); r->del = r->ins = NULL; }

static void undo_clear(Doc *d) {
    for (int i = 0; i < d->nu; i++) urec_free(&d->u[i]);
    d->nu = d->cur = 0;
    d->ubytes = 0;
    d->save_cur = 0;
    d->save_lost = false;
    d->depth = 0;
}

void doc_free(Doc *d) {
    undo_clear(d);
    free(d->u);
    free(d->buf);
    free(d->ls);
    memset(d, 0, sizeof *d);
}

static void gap_move(Doc *d, size_t pos) {
    if (pos == d->gs) return;
    size_t glen = d->ge - d->gs;
    if (pos < d->gs) {
        size_t n = d->gs - pos;
        nb_memmove(d->buf + d->ge - n, d->buf + pos, n);
    } else {
        size_t n = pos - d->gs;
        nb_memmove(d->buf + d->gs, d->buf + d->ge, n);
    }
    d->gs = pos;
    d->ge = pos + glen;
}

static bool gap_ensure(Doc *d, size_t need) {
    size_t glen = d->ge - d->gs;
    if (glen >= need) return true;
    size_t len = doc_len(d);
    size_t ncap = d->cap * 2;
    if (ncap < len + need + 4096) ncap = len + need + 4096;
    char *nb = realloc(d->buf, ncap + NB_PAD);
    if (!nb) return false;
    d->buf = nb;
    size_t tail = d->cap - d->ge;
    nb_memmove(d->buf + (ncap - tail), d->buf + d->ge, tail);
    d->ge = ncap - tail;
    d->cap = ncap;
    memset(d->buf + ncap, 0, NB_PAD);
    return true;
}

/* ---------------------------------------------------------------- line index */

static bool ls_reserve(Doc *d, int n) {
    if (n <= d->capls) return true;
    int nc = d->capls * 2;
    if (nc < n + 64) nc = n + 64;
    size_t *p = realloc(d->ls, (size_t)nc * sizeof(size_t));
    if (!p) return false;
    d->ls = p;
    d->capls = nc;
    return true;
}

static void lines_rebuild(Doc *d) {
    size_t len = doc_len(d);
    gap_move(d, len);
    const char *t = d->buf;
    int n = 1;
    size_t maxl = 0, last = 0, i = 0;
    d->ls[0] = 0;
    while (i < len) {
        const char *p = memchr(t + i, '\n', len - i);
        if (!p) break;
        size_t off = (size_t)(p - t);
        if (!ls_reserve(d, n + 1)) oom_exit();
        d->ls[n++] = off + 1;
        if (off - last > maxl) maxl = off - last;
        last = off + 1;
        i = off + 1;
    }
    if (len - last > maxl) maxl = len - last;
    d->nls = n;
    d->maxline = maxl;
}

size_t doc_line_start(const Doc *d, int line) {
    if (line < 0) return 0;
    if (line >= d->nls) return doc_len(d);
    return d->ls[line];
}

size_t doc_line_end(const Doc *d, int line) {
    if (line < 0) line = 0;
    if (line + 1 < d->nls) return d->ls[line + 1] - 1;
    return doc_len(d);
}

int doc_line_of(const Doc *d, size_t off) {
    int lo = 0, hi = d->nls - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (d->ls[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Brings the line index in line with an edit that replaces `del` bytes at `pos` by `n` bytes of `ins`. Call before the text
 * changes (it only needs the old index). Returns false if memory ran out (index untouched). */
static bool lines_update(Doc *d, size_t pos, size_t del, const char *ins, size_t n) {
    int a = doc_line_of(d, pos) + 1;
    int b = a;
    size_t endold = pos + del;
    while (b < d->nls && d->ls[b] <= endold) b++;
    int m = 0;
    for (size_t i = 0; i < n; i++) if (ins[i] == '\n') m++;
    ptrdiff_t delta = (ptrdiff_t)n - (ptrdiff_t)del;
    int newn = d->nls - (b - a) + m;
    if (!ls_reserve(d, newn)) return false;
    if (m != b - a) nb_memmove(&d->ls[a + m], &d->ls[b], (size_t)(d->nls - b) * sizeof(size_t));
    int k = a;
    size_t seg = 0, maxseg = 0;
    for (size_t i = 0; i < n; i++) {
        seg++;
        if (ins[i] == '\n') {
            d->ls[k++] = pos + i + 1;
            if (seg > maxseg) maxseg = seg;
            seg = 0;
        }
    }
    if (delta) for (int j = a + m; j < newn; j++) d->ls[j] += (size_t)delta;
    d->nls = newn;
    if (a - 1 < d->dirty_line) d->dirty_line = a - 1;
    if (maxseg > d->maxline) d->maxline = maxseg;
    return true;
}

/* ---------------------------------------------------------------- reading the text */

void doc_get(const Doc *d, size_t pos, size_t n, char *out) {
    size_t len = doc_len(d);
    if (pos >= len) return;
    if (n > len - pos) n = len - pos;
    if (pos + n <= d->gs) { nb_memmove(out, d->buf + pos, n); return; }
    if (pos >= d->gs) { nb_memmove(out, d->buf + pos + (d->ge - d->gs), n); return; }
    size_t first = d->gs - pos;
    nb_memmove(out, d->buf + pos, first);
    nb_memmove(out + first, d->buf + d->ge, n - first);
}

const char *doc_ptr(Doc *d, size_t from, size_t to) {
    size_t len = doc_len(d);
    if (from > len) from = len;
    if (to > len) to = len;
    if (to < from) to = from;
    if (to <= d->gs) return d->buf + from;
    if (from >= d->gs) return d->buf + from + (d->ge - d->gs);
    if (d->gs - from <= to - d->gs) gap_move(d, from); else gap_move(d, to);
    return doc_ptr(d, from, to);
}

const char *doc_all(Doc *d) {
    gap_move(d, doc_len(d));
    return d->buf;
}

static uint32_t cp_at(const Doc *d, size_t off, int *n) {
    char tmp[4];
    size_t len = doc_len(d);
    if (off >= len) { *n = 0; return 0; }
    size_t cnt = len - off < 4 ? len - off : 4;
    doc_get(d, off, cnt, tmp);
    uint32_t cp;
    *n = nb_u8_dec(tmp, tmp + cnt, &cp);
    return cp;
}

size_t doc_next_char(const Doc *d, size_t off) {
    int n;
    cp_at(d, off, &n);
    return n ? off + (size_t)n : off;
}

size_t doc_prev_char(const Doc *d, size_t off) {
    if (off == 0) return 0;
    size_t len = doc_len(d);
    if (off > len) off = len;
    size_t start = off - 1, lim = off >= 4 ? off - 4 : 0;
    while (start > lim && (doc_byte(d, start) & 0xC0) == 0x80) start--;
    char tmp[4];
    size_t cnt = off - start;
    doc_get(d, start, cnt, tmp);
    uint32_t cp;
    int k = nb_u8_dec(tmp, tmp + cnt, &cp);
    return (size_t)k == cnt ? start : off - 1;
}

size_t doc_word_left(const Doc *d, size_t off) {
    int n;
    while (off > 0) {
        size_t p = doc_prev_char(d, off);
        if (nb_char_class(cp_at(d, p, &n)) != 0) break;
        off = p;
    }
    if (off == 0) return 0;
    int c = nb_char_class(cp_at(d, doc_prev_char(d, off), &n));
    while (off > 0) {
        size_t p = doc_prev_char(d, off);
        if (nb_char_class(cp_at(d, p, &n)) != c) break;
        off = p;
    }
    return off;
}

size_t doc_word_right(const Doc *d, size_t off) {
    size_t len = doc_len(d);
    int n;
    if (off >= len) return len;
    int c = nb_char_class(cp_at(d, off, &n));
    if (c != 0)
        while (off < len && nb_char_class(cp_at(d, off, &n)) == c) off = doc_next_char(d, off);
    while (off < len && nb_char_class(cp_at(d, off, &n)) == 0) off = doc_next_char(d, off);
    return off;
}

size_t doc_indent_end(const Doc *d, int line) {
    size_t s = doc_line_start(d, line), e = doc_line_end(d, line);
    while (s < e) {
        unsigned char c = doc_byte(d, s);
        if (c != ' ' && c != '\t') break;
        s++;
    }
    return s;
}

/* ---------------------------------------------------------------- the one low-level edit */

static bool raw_replace(Doc *d, size_t pos, size_t del, const char *ins, size_t n) {
    size_t len = doc_len(d);
    if (pos > len) pos = len;
    if (del > len - pos) del = len - pos;
    gap_move(d, pos);                                           /* neither of these changes the text, so failing here is harmless */
    if (n > del && !gap_ensure(d, n - del)) return false;
    if (!lines_update(d, pos, del, ins, n)) return false;
    d->ge += del;
    nb_memmove(d->buf + d->gs, ins, n);
    d->gs += n;
    d->version++;
    /* longest line bookkeeping for the horizontal scroll bar */
    int l = doc_line_of(d, pos + n);
    size_t ll = doc_line_end(d, l) - doc_line_start(d, l);
    if (ll > d->maxline) d->maxline = ll;
    l = doc_line_of(d, pos);
    ll = doc_line_end(d, l) - doc_line_start(d, l);
    if (ll > d->maxline) d->maxline = ll;
    return true;
}

bool doc_set_text(Doc *d, const char *text, size_t len) {
    size_t cap = len + len / 16 + 4096;
    char *nb = malloc(cap + NB_PAD);
    if (!nb) return false;
    nb_memmove(nb, text, len);
    memset(nb + cap, 0, NB_PAD);
    free(d->buf);
    d->buf = nb;
    d->cap = cap;
    d->gs = len;
    d->ge = cap;
    undo_clear(d);
    d->caret = d->anchor = 0;
    lines_rebuild(d);
    d->version++;
    d->dirty_line = 0;
    d->undo_break = true;
    return true;
}

/* ---------------------------------------------------------------- undo history */

static void undo_trim(Doc *d) {
    while (d->ubytes > NB_UNDO_BYTES || d->nu > NB_UNDO_RECORDS) {
        uint32_t g = d->u[0].group;
        int k = 0;
        while (k < d->nu && d->u[k].group == g) k++;
        if (k >= d->cur) return;                                /* never drop the newest group */
        for (int i = 0; i < k; i++) { d->ubytes -= d->u[i].del_len + d->u[i].ins_len; urec_free(&d->u[i]); }
        memmove(d->u, d->u + k, (size_t)(d->nu - k) * sizeof(UndoRec));
        d->nu -= k;
        d->cur -= k;
        d->save_cur -= k;
        if (d->save_cur < 0) { d->save_cur = 0; d->save_lost = true; }
    }
}

static bool word_char(unsigned char c) { return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool typing_mergeable(const UndoRec *last, const char *ins) {
    unsigned char c = (unsigned char)ins[0];
    unsigned char p = last->ins_len ? (unsigned char)last->ins[last->ins_len - 1] : 0;
    if (c == '\n' || p == '\n') return false;
    if (word_char(c) && (p == ' ' || p == '\t')) return false;      /* a new word starts a new undo step */
    return true;
}

void doc_group_begin(Doc *d) {
    if (d->depth++ == 0) d->cur_group = ++d->next_group;
}

void doc_group_end(Doc *d) {
    if (d->depth > 0) d->depth--;
    d->undo_break = true;
}

void doc_break_undo(Doc *d) { d->undo_break = true; }

bool doc_modified(const Doc *d) { return d->cur != d->save_cur || d->save_lost; }

void doc_mark_saved(Doc *d) {
    d->save_cur = d->cur;
    d->save_lost = false;
    d->undo_break = true;
}

bool doc_edit(Doc *d, size_t pos, size_t del, const char *ins, size_t n, size_t caret_after, int kind) {
    size_t len = doc_len(d);
    if (pos > len) pos = len;
    if (del > len - pos) del = len - pos;
    if (!del && !n) {
        d->caret = d->anchor = caret_after > len ? len : caret_after;
        return true;
    }
    /* what is about to disappear, for the history */
    char *dtxt = NULL;
    bool record = true;
    if (del) {
        dtxt = malloc(del);
        if (dtxt) doc_get(d, pos, del, dtxt); else record = false;
    }
    size_t cb = d->caret, ab = d->anchor;
    uint32_t now = (uint32_t)k_now_ms();
    if (!raw_replace(d, pos, del, ins, n)) { free(dtxt); return false; }
    size_t newlen = doc_len(d);
    if (caret_after > newlen) caret_after = newlen;

    if (record) {
        if (d->cur < d->nu) {                                   /* a new edit discards the redo history */
            for (int i = d->cur; i < d->nu; i++) { d->ubytes -= d->u[i].del_len + d->u[i].ins_len; urec_free(&d->u[i]); }
            d->nu = d->cur;
            if (d->save_cur > d->cur) d->save_lost = true;
        }
        UndoRec *last = d->cur > 0 ? &d->u[d->cur - 1] : NULL;
        bool merged = false;
        if (last && d->depth == 0 && !d->undo_break && kind != DK_OTHER && (last->flags & UF_TYPING) && d->cur != d->save_cur &&
            (uint32_t)(now - last->t) < 3000) {
            if (kind == DK_TYPE && del == 0 && last->del_len == 0 && last->ins_len && last->pos + last->ins_len == pos &&
                last->ins_len + n < 2000 && typing_mergeable(last, ins)) {
                char *ni = realloc(last->ins, last->ins_len + n);
                if (ni) { nb_memmove(ni + last->ins_len, ins, n); last->ins = ni; last->ins_len += n; d->ubytes += n; merged = true; }
            } else if (kind == DK_BACKSPACE && n == 0 && last->ins_len == 0 && last->del_len && pos + del == last->pos && last->del_len + del < 2000) {
                char *nd = realloc(last->del, last->del_len + del);
                if (nd) {
                    nb_memmove(nd + del, nd, last->del_len);
                    nb_memmove(nd, dtxt, del);
                    last->del = nd; last->del_len += del; last->pos = pos; d->ubytes += del; merged = true;
                }
            } else if (kind == DK_DELFWD && n == 0 && last->ins_len == 0 && last->del_len && pos == last->pos && last->del_len + del < 2000) {
                char *nd = realloc(last->del, last->del_len + del);
                if (nd) { nb_memmove(nd + last->del_len, dtxt, del); last->del = nd; last->del_len += del; d->ubytes += del; merged = true; }
            }
            if (merged) { last->ca = last->aa = caret_after; last->t = now; free(dtxt); dtxt = NULL; }
        }
        if (!merged) {
            if (d->nu >= d->capu) {
                int nc = d->capu ? d->capu * 2 : 64;
                UndoRec *nu = realloc(d->u, (size_t)nc * sizeof(UndoRec));
                if (!nu) record = false; else { d->u = nu; d->capu = nc; }
            }
            char *icopy = NULL;
            if (record && n) { icopy = malloc(n); if (!icopy) record = false; else nb_memmove(icopy, ins, n); }
            if (record) {
                UndoRec *r = &d->u[d->nu++];
                r->pos = pos; r->del = dtxt; r->del_len = del; r->ins = icopy; r->ins_len = n;
                r->cb = cb; r->ab = ab; r->ca = r->aa = caret_after;
                r->group = d->depth ? d->cur_group : ++d->next_group;
                r->t = now;
                r->flags = kind != DK_OTHER ? UF_TYPING : 0;
                d->cur = d->nu;
                d->ubytes += del + n;
                dtxt = NULL;
            } else free(icopy);
        }
    }
    if (!record) {                                              /* out of memory for the history: forget it rather than lie */
        undo_clear(d);
        d->save_lost = true;
        free(dtxt);
    }
    d->undo_break = false;
    d->caret = d->anchor = caret_after;
    undo_trim(d);
    return true;
}

bool doc_undo(Doc *d) {
    if (d->cur == 0) return false;
    uint32_t g = d->u[d->cur - 1].group;
    size_t cb = d->caret, ab = d->anchor;
    while (d->cur > 0 && d->u[d->cur - 1].group == g) {
        UndoRec *r = &d->u[--d->cur];
        if (!raw_replace(d, r->pos, r->ins_len, r->del, r->del_len)) { d->cur++; break; }
        cb = r->cb; ab = r->ab;
    }
    size_t len = doc_len(d);
    d->caret = cb > len ? len : cb;
    d->anchor = ab > len ? len : ab;
    d->undo_break = true;
    return true;
}

bool doc_redo(Doc *d) {
    if (d->cur >= d->nu) return false;
    uint32_t g = d->u[d->cur].group;
    size_t ca = d->caret, aa = d->anchor;
    while (d->cur < d->nu && d->u[d->cur].group == g) {
        UndoRec *r = &d->u[d->cur];
        if (!raw_replace(d, r->pos, r->del_len, r->ins, r->ins_len)) break;
        d->cur++;
        ca = r->ca; aa = r->aa;
    }
    size_t len = doc_len(d);
    d->caret = ca > len ? len : ca;
    d->anchor = aa > len ? len : aa;
    d->undo_break = true;
    return true;
}
