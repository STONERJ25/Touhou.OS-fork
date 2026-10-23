/* The editor view: layout (optionally with word wrap), drawing, caret and selection, scrolling, keyboard and mouse input.
 *
 * One visual row is 16 pixels high and every character is one 8 pixel cell (tabs advance to the next tab stop). Without word
 * wrap a logical line is exactly one row; with wrap it is split into rows by ed_wrap_count(). The vertical scroll position is a
 * (line, row-inside-line) pair, so nothing ever needs the layout of the whole document: only the lines on screen are measured,
 * which keeps a 100,000 line file as responsive as a ten line one. */
#include "notebook.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CW 8
#define CH 16
#define PADX 4
#define BARW 14

typedef struct { const char *t; size_t s, e; } LV;      /* t[0] is the byte at offset s */

static LV lview(Editor *e, int line) {
    Doc *d = &e->doc;
    LV v;
    v.s = doc_line_start(d, line);
    v.e = doc_line_end(d, line);
    v.t = doc_ptr(d, v.s, v.e);
    return v;
}

static int tabw(const Editor *e) { return e->tab_w < 1 ? 4 : e->tab_w; }

/* ---------------------------------------------------------------- word wrap */

/* Splits the line [s, e) (t[0] = byte at s) into rows of at most `cols` cells, preferring to break after a blank. Returns the
 * number of rows; rs (if not NULL, capacity cap) receives the start offset of every row. */
int ed_wrap_count(const char *t, size_t s, size_t e, int cols, int tw, size_t *rs, int cap) {
    int nrows = 1;
    if (rs && cap > 0) rs[0] = s;
    size_t i = s, row_start = s, brk = 0;
    int col = 0;
    while (i < e) {
        uint32_t cp;
        int n = nb_u8_dec(t + (i - s), t + (e - s), &cp);
        int w = cp == '\t' ? tw - col % tw : 1;
        if (col + w > cols && col > 0) {
            size_t at = brk > row_start ? brk : i;
            nrows++;
            if (rs && nrows <= cap) rs[nrows - 1] = at;
            row_start = at;
            brk = 0;
            col = 0;
            if (at != i) { i = at; continue; }
            w = cp == '\t' ? tw : 1;
        }
        col += w;
        i += (size_t)n;
        if (cp == ' ' || cp == '\t') brk = i;
    }
    return nrows;
}

static WrapEnt *wrap_get(Editor *e, int line) {
    WrapEnt *w = &e->wc[line % WRAP_CACHE];
    if (w->n > 0 && w->line == line && w->ver == e->doc.version && w->cols == e->cols && w->tabw == tabw(e)) return w;
    LV v = lview(e, line);
    int n = ed_wrap_count(v.t, v.s, v.e, e->cols, tabw(e), NULL, 0);
    size_t *rs = realloc(w->rs, (size_t)n * sizeof(size_t));
    if (!rs) {                                                  /* out of memory: pretend the line is one row */
        free(w->rs);
        w->rs = malloc(sizeof(size_t));
        if (w->rs) w->rs[0] = v.s;
        w->n = 1; w->line = line; w->ver = e->doc.version; w->cols = e->cols; w->tabw = tabw(e);
        return w;
    }
    ed_wrap_count(v.t, v.s, v.e, e->cols, tabw(e), rs, n);
    w->rs = rs;
    w->n = n;
    w->line = line;
    w->ver = e->doc.version;
    w->cols = e->cols;
    w->tabw = tabw(e);
    return w;
}

int ed_rows_in_line(Editor *e, int line) {
    if (!e->wrap || line < 0 || line >= e->doc.nls) return 1;
    return wrap_get(e, line)->n;
}

/* Byte range of row `row` of `line` (without the newline). */
static void row_span(Editor *e, int line, int row, size_t *rs, size_t *re) {
    Doc *d = &e->doc;
    size_t ls = doc_line_start(d, line), le = doc_line_end(d, line);
    if (!e->wrap) { *rs = ls; *re = le; return; }
    WrapEnt *w = wrap_get(e, line);
    if (row < 0) row = 0;
    if (row >= w->n) row = w->n - 1;
    *rs = w->rs[row];
    *re = row + 1 < w->n ? w->rs[row + 1] : le;
}

/* Line, row inside the line and visual column (inside the row) of a byte offset. */
static void vis_of(Editor *e, size_t off, int *line, int *row, int *col) {
    Doc *d = &e->doc;
    int l = doc_line_of(d, off);
    size_t le = doc_line_end(d, l);
    int r = 0;
    size_t rs = doc_line_start(d, l);
    if (e->wrap) {
        WrapEnt *w = wrap_get(e, l);
        int lo = 0, hi = w->n - 1;
        while (lo < hi) { int mid = lo + (hi - lo + 1) / 2; if (w->rs[mid] <= off) lo = mid; else hi = mid - 1; }
        r = lo;
        rs = w->rs[r];
    }
    LV v = lview(e, l);
    int c = 0, tw = tabw(e);
    for (size_t i = rs; i < off && i < le;) {
        uint32_t cp;
        int n = nb_u8_dec(v.t + (i - v.s), v.t + (le - v.s), &cp);
        c += cp == '\t' ? tw - c % tw : 1;
        i += (size_t)n;
    }
    *line = l; *row = r; *col = c;
}

/* Offset in row `row` of `line` closest to visual column `target`. */
static size_t off_at_col(Editor *e, int line, int row, int target) {
    Doc *d = &e->doc;
    size_t rs, re;
    row_span(e, line, row, &rs, &re);
    size_t le = doc_line_end(d, line);
    LV v = lview(e, line);
    int c = 0, tw = tabw(e);
    size_t i = rs;
    while (i < re) {
        uint32_t cp;
        int n = nb_u8_dec(v.t + (i - v.s), v.t + (re - v.s), &cp);
        int w = cp == '\t' ? tw - c % tw : 1;
        if (target < c + (w + 1) / 2) return i;
        c += w;
        i += (size_t)n;
    }
    if (re < le) return doc_prev_char(d, re) > rs ? doc_prev_char(d, re) : rs;      /* soft-wrapped row: stay before the break */
    return re;
}

/* Offset of visual column `target` counted from the start of the logical line (ignores wrapping). */
static size_t off_at_line_col(Editor *e, int line, int target) {
    LV v = lview(e, line);
    int c = 0, tw = tabw(e);
    size_t i = v.s;
    while (i < v.e) {
        uint32_t cp;
        int n = nb_u8_dec(v.t + (i - v.s), v.t + (v.e - v.s), &cp);
        int w = cp == '\t' ? tw - c % tw : 1;
        if (target < c + (w + 1) / 2) return i;
        c += w;
        i += (size_t)n;
    }
    return v.e;
}

/* ---------------------------------------------------------------- scrolling */

/* Moves (line, sub) by n rows; returns how many rows it actually moved (fewer at the ends of the document). */
static int step_rows(Editor *e, int *line, int *sub, int n) {
    int nl = doc_nlines(&e->doc), moved = 0;
    while (n > 0) {
        int rows = ed_rows_in_line(e, *line);
        if (*sub + 1 < rows) (*sub)++;
        else if (*line + 1 < nl) { (*line)++; *sub = 0; }
        else break;
        n--; moved++;
    }
    while (n < 0) {
        if (*sub > 0) (*sub)--;
        else if (*line > 0) { (*line)--; *sub = ed_rows_in_line(e, *line) - 1; }
        else break;
        n++; moved++;
    }
    return moved;
}

static void ed_clamp(Editor *e) {
    int nl = doc_nlines(&e->doc);
    if (e->top_line >= nl) { e->top_line = nl - 1; e->top_sub = 0; }
    if (e->top_line < 0) e->top_line = 0;
    int rows = ed_rows_in_line(e, e->top_line);
    if (e->top_sub >= rows) e->top_sub = rows - 1;
    if (e->top_sub < 0) e->top_sub = 0;
    int vr = e->vis_rows < 1 ? 1 : e->vis_rows;
    int l = e->top_line, s = e->top_sub;
    int got = step_rows(e, &l, &s, vr - 1);
    if (got < vr - 1 && (e->top_line > 0 || e->top_sub > 0)) {          /* no blank space below the last row */
        int tl = e->top_line, ts = e->top_sub;
        step_rows(e, &tl, &ts, -(vr - 1 - got));
        e->top_line = tl;
        e->top_sub = ts;
    }
    if (e->wrap || e->scroll_x < 0) e->scroll_x = 0;
}

static void scroll_rows(Editor *e, int n) {
    if (n < 0) {
        int l = e->top_line, s = e->top_sub;
        step_rows(e, &l, &s, n);
        e->top_line = l;
        e->top_sub = s;
        return;
    }
    for (int k = 0; k < n; k++) {
        int l = e->top_line, s = e->top_sub;
        if (step_rows(e, &l, &s, e->vis_rows) < e->vis_rows) break;      /* the last row is already in view */
        l = e->top_line; s = e->top_sub;
        step_rows(e, &l, &s, 1);
        e->top_line = l;
        e->top_sub = s;
    }
}

static void do_reveal(Editor *e) {
    int line, row, col;
    vis_of(e, e->doc.caret, &line, &row, &col);
    int vr = e->vis_rows < 1 ? 1 : e->vis_rows;
    if (line < e->top_line || (line == e->top_line && row < e->top_sub)) {
        e->top_line = line;
        e->top_sub = row;
    } else {
        if (line - e->top_line > vr + 2) { e->top_line = line - vr + 1 > 0 ? line - vr + 1 : 0; e->top_sub = 0; }
        int dist;
        if (!e->wrap) dist = line - e->top_line;
        else {
            int l = e->top_line, s = e->top_sub;
            dist = 0;
            while ((l < line || (l == line && s < row)) && dist <= vr + 2) {
                if (step_rows(e, &l, &s, 1) == 0) break;
                dist++;
            }
        }
        if (dist >= vr) {
            int tl = line, ts = row;
            step_rows(e, &tl, &ts, -(vr - 1));
            e->top_line = tl;
            e->top_sub = ts;
        }
    }
    if (!e->wrap) {
        int vc = e->vis_cols < 4 ? 4 : e->vis_cols;
        int margin = vc > 24 ? 6 : 1;
        if (col < e->scroll_x) e->scroll_x = col > margin ? col - margin : 0;
        else if (col >= e->scroll_x + vc - 1) e->scroll_x = col - vc + 1 + margin;
    } else e->scroll_x = 0;
}

void ed_reveal(Editor *e) { e->reveal = true; }

/* ---------------------------------------------------------------- syntax state per line */

static SynState syn_state_at(Editor *e, int line) {
    Doc *d = &e->doc;
    if (e->syn_ver != d->version || e->syn_lang != e->lang) {
        if (e->syn_lang != e->lang) e->syn_valid = 0;
        else if (d->dirty_line < e->syn_valid) e->syn_valid = d->dirty_line;
        d->dirty_line = INT_MAX;
        e->syn_ver = d->version;
        e->syn_lang = e->lang;
    }
    int nl = doc_nlines(d);
    if (line >= nl) line = nl - 1;
    if (line < 0) line = 0;
    if (e->syn_cap < nl + 2) {
        int nc = nl + 1024;
        uint8_t *p = realloc(e->syn, (size_t)nc);
        if (!p) return 0;
        e->syn = p;
        e->syn_cap = nc;
    }
    if (e->syn_valid > nl - 1) e->syn_valid = nl - 1;
    if (e->syn_valid < 0) e->syn_valid = 0;
    e->syn[0] = 0;
    while (e->syn_valid < line) {
        int l = e->syn_valid;
        LV v = lview(e, l);
        e->syn[l + 1] = syn_scan(e->lang, v.t, v.e - v.s, e->syn[l], NULL);
        e->syn_valid++;
    }
    return e->syn[line];
}

/* ---------------------------------------------------------------- creation and options */

void ed_init(Editor *e) {
    memset(e, 0, sizeof *e);
    doc_init(&e->doc);
    e->numbers = true;
    e->highlight = true;
    e->spaces = true;
    e->tab_w = 4;
    e->want_col = -1;
    e->cols = 80;
    e->vis_rows = 20;
    e->vis_cols = 80;
    e->lang = LANG_PLAIN;
    e->syn_lang = LANG_COUNT;
}

void ed_free(Editor *e) {
    for (int i = 0; i < WRAP_CACHE; i++) free(e->wc[i].rs);
    free(e->syn);
    doc_free(&e->doc);
    memset(e, 0, sizeof *e);
}

void ed_invalidate(Editor *e) {
    for (int i = 0; i < WRAP_CACHE; i++) { e->wc[i].n = 0; e->wc[i].line = -1; }
    e->syn_valid = 0;
    e->syn_ver = 0;
    e->syn_lang = LANG_COUNT;
    e->doc.dirty_line = 0;
}

void ed_reset_view(Editor *e) {
    e->top_line = e->top_sub = e->scroll_x = 0;
    e->want_col = -1;
    e->drag = 0;
    e->reveal = true;
    ed_invalidate(e);
}

bool ed_has_selection(const Editor *e) { return e->doc.caret != e->doc.anchor; }

void ed_selection(const Editor *e, size_t *lo, size_t *hi) {
    size_t a = e->doc.caret, b = e->doc.anchor;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

void ed_set_selection(Editor *e, size_t anchor, size_t caret) {
    size_t len = doc_len(&e->doc);
    e->doc.anchor = anchor > len ? len : anchor;
    e->doc.caret = caret > len ? len : caret;
    e->want_col = -1;
    e->reveal = true;
    doc_break_undo(&e->doc);
}

void ed_caret_pos(Editor *e, int *line, int *col) {
    Doc *d = &e->doc;
    int l = doc_line_of(d, d->caret);
    LV v = lview(e, l);
    int c = 0, tw = tabw(e);
    for (size_t i = v.s; i < d->caret && i < v.e;) {
        uint32_t cp;
        int n = nb_u8_dec(v.t + (i - v.s), v.t + (v.e - v.s), &cp);
        c += cp == '\t' ? tw - c % tw : 1;
        i += (size_t)n;
    }
    *line = l;
    *col = c;
}

void ed_goto(Editor *e, int line, int col) {
    Doc *d = &e->doc;
    int nl = doc_nlines(d);
    line--;
    if (line < 0) line = 0;
    if (line >= nl) line = nl - 1;
    size_t off = col > 1 ? off_at_line_col(e, line, col - 1) : doc_line_start(d, line);
    d->caret = d->anchor = off;
    e->want_col = -1;
    e->reveal = true;
    e->blink_ms = k_now_ms();
    doc_break_undo(d);
}

/* ---------------------------------------------------------------- editing primitives */

static bool deny_edit(void) {
    if (A.readonly) { msg_set("Read-only file: use Save As to keep a copy."); return true; }
    return false;
}

static bool do_replace(Editor *e, size_t pos, size_t del, const char *ins, size_t n, size_t caret_after, int kind) {
    if (deny_edit()) return false;
    if (!doc_edit(&e->doc, pos, del, ins, n, caret_after, kind)) { msg_set("Out of memory: Yuyuko is hungry."); return false; }
    e->want_col = -1;
    e->reveal = true;
    e->blink_ms = k_now_ms();
    return true;
}

bool ed_replace_range(Editor *e, size_t pos, size_t del, const char *ins, size_t n, size_t caret_after) {
    return do_replace(e, pos, del, ins, n, caret_after, DK_OTHER);
}

static void set_caret(Editor *e, size_t pos, bool extend) {
    size_t len = doc_len(&e->doc);
    if (pos > len) pos = len;
    e->doc.caret = pos;
    if (!extend) e->doc.anchor = pos;
    e->want_col = -1;
    e->reveal = true;
    e->blink_ms = k_now_ms();
    doc_break_undo(&e->doc);
}

void ed_select_all(Editor *e) {
    e->doc.anchor = 0;
    e->doc.caret = doc_len(&e->doc);
    e->reveal = false;
    e->blink_ms = k_now_ms();
}

void ed_insert_text(Editor *e, const char *s, size_t n) {
    char *tmp = NULL;
    if (memchr(s, '\r', n) || memchr(s, 0, n)) {                /* pasted CRLF / CR text becomes LF; zero bytes would make the file "binary" */
        tmp = malloc(n + 1);
        if (!tmp) { msg_set("Out of memory: Yuyuko is hungry."); return; }
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\r') { tmp[m++] = '\n'; if (i + 1 < n && s[i + 1] == '\n') i++; }
            else if (s[i] != 0) tmp[m++] = s[i];
        }
        s = tmp;
        n = m;
    }
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    do_replace(e, lo, hi - lo, s, n, lo + n, DK_OTHER);
    free(tmp);
}

bool ed_copy(Editor *e, bool cut) {
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    if (hi == lo) { msg_set("Select some text first."); return false; }
    if (cut && deny_edit()) return false;
    size_t n = hi - lo;
    char *buf = malloc(n + 1);
    if (!buf) { msg_set("Out of memory: Yuyuko is hungry."); return false; }
    doc_get(&e->doc, lo, n, buf);
    buf[n] = 0;
    free(A.clip);
    A.clip = buf;
    A.cliplen = n;
    size_t sys = n > 1000 ? 1000 : n;                           /* the system clipboard holds 1 KiB: pass on the start */
    while (sys > 0 && sys < n && (((unsigned char)buf[sys]) & 0xC0) == 0x80) sys--;
    A.clip_sys_len = sys;
    kapp_clipboard_copy(A.app, buf, sys);
    if (cut) do_replace(e, lo, n, "", 0, lo, DK_OTHER);
    return true;
}

bool ed_paste(Editor *e) {
    if (deny_edit()) return false;
    size_t n = 0;
    const char *c = kapp_clipboard(A.app, &n);
    const char *src = c;
    size_t sn = n;
    if (A.clip && A.cliplen > 0 && n == A.clip_sys_len && !memcmp(c, A.clip, n)) { src = A.clip; sn = A.cliplen; }   /* our own, whole text */
    if (!src || sn == 0) { msg_set("The clipboard is empty."); return false; }
    ed_insert_text(e, src, sn);
    return true;
}

bool ed_undo(Editor *e) {
    if (deny_edit()) return false;
    if (!doc_undo(&e->doc)) { msg_set("Nothing to undo."); return false; }
    e->want_col = -1;
    e->reveal = true;
    e->blink_ms = k_now_ms();
    return true;
}

bool ed_redo(Editor *e) {
    if (deny_edit()) return false;
    if (!doc_redo(&e->doc)) { msg_set("Nothing to redo."); return false; }
    e->want_col = -1;
    e->reveal = true;
    e->blink_ms = k_now_ms();
    return true;
}

/* The lines touched by the selection: [l0, l1]; a selection that ends at the very start of a line does not include it. */
static void sel_lines(Editor *e, int *l0, int *l1) {
    Doc *d = &e->doc;
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    *l0 = doc_line_of(d, lo);
    *l1 = doc_line_of(d, hi);
    if (hi > lo && *l1 > *l0 && hi == doc_line_start(d, *l1)) (*l1)--;
}

static size_t indent_unit(Editor *e, char *out, int col) {
    if (e->spaces) {
        int n = tabw(e) - col % tabw(e);
        memset(out, ' ', (size_t)n);
        return (size_t)n;
    }
    out[0] = '\t';
    return 1;
}

void ed_indent(Editor *e, bool dedent) {
    Doc *d = &e->doc;
    if (deny_edit()) return;
    size_t lo, hi;
    ed_selection(e, &lo, &hi);
    int l0, l1;
    sel_lines(e, &l0, &l1);
    size_t a = doc_line_start(d, l0), b = doc_line_end(d, l1);
    size_t old = b - a;
    int tw = tabw(e);
    char *out = malloc(old + (size_t)(l1 - l0 + 1) * (size_t)tw + 8);
    if (!out) { msg_set("Out of memory: Yuyuko is hungry."); return; }
    const char *src = doc_ptr(d, a, b);
    size_t m = 0;
    for (int l = l0; l <= l1; l++) {
        size_t ls = doc_line_start(d, l) - a, le = doc_line_end(d, l) - a;
        size_t k = ls;
        if (!dedent) {
            if (le > ls) {                                      /* blank lines stay blank */
                if (e->spaces) { memset(out + m, ' ', (size_t)tw); m += (size_t)tw; }
                else out[m++] = '\t';
            }
        } else {
            if (k < le && src[k] == '\t') k++;
            else { int cnt = 0; while (k < le && src[k] == ' ' && cnt < tw) { k++; cnt++; } }
        }
        size_t n = le - k;
        nb_memmove(out + m, src + k, n);
        m += n;
        if (l < l1) out[m++] = '\n';
    }
    bool had_sel = hi > lo;
    if (m == old && !memcmp(out, src, m)) { free(out); return; }
    size_t caret_after = had_sel ? a + m : (size_t)((ptrdiff_t)lo + (ptrdiff_t)m - (ptrdiff_t)old);
    if (!had_sel && (ptrdiff_t)caret_after < (ptrdiff_t)a) caret_after = a;
    if (do_replace(e, a, old, out, m, caret_after, DK_OTHER) && had_sel) {
        d->anchor = a;
        if (d->cur > 0) d->u[d->cur - 1].aa = a;
    }
    free(out);
}

void ed_duplicate_line(Editor *e) {
    Doc *d = &e->doc;
    if (deny_edit()) return;
    int l0, l1;
    sel_lines(e, &l0, &l1);
    size_t a = doc_line_start(d, l0), b = doc_line_end(d, l1);
    size_t n = b - a;
    char *t = malloc(n + 2);
    if (!t) return;
    t[0] = '\n';
    doc_get(d, a, n, t + 1);
    size_t cb = d->caret + n + 1, ab = d->anchor + n + 1;
    if (do_replace(e, b, 0, t, n + 1, cb, DK_OTHER)) {
        d->anchor = ab;
        if (d->cur > 0) d->u[d->cur - 1].aa = ab;
    }
    free(t);
}

void ed_delete_line(Editor *e) {
    Doc *d = &e->doc;
    if (deny_edit()) return;
    int l0, l1;
    sel_lines(e, &l0, &l1);
    int nl = doc_nlines(d);
    size_t a = doc_line_start(d, l0), b = doc_line_end(d, l1);
    if (l1 + 1 < nl) b++;
    else if (l0 > 0) a--;
    do_replace(e, a, b - a, "", 0, a, DK_OTHER);
}

void ed_move_lines(Editor *e, int dir) {
    Doc *d = &e->doc;
    if (deny_edit()) return;
    int l0, l1;
    sel_lines(e, &l0, &l1);
    int nl = doc_nlines(d);
    if ((dir < 0 && l0 == 0) || (dir > 0 && l1 + 1 >= nl)) return;
    size_t gs = doc_line_start(d, l0), ge = doc_line_end(d, l1);
    size_t ns, ne;
    if (dir < 0) { ns = doc_line_start(d, l0 - 1); ne = doc_line_end(d, l0 - 1); }
    else { ns = doc_line_start(d, l1 + 1); ne = doc_line_end(d, l1 + 1); }
    size_t gl = ge - gs, nlen = ne - ns;
    char *buf = malloc(gl + 1);
    char *nb = malloc(nlen + 1);
    char *out = malloc(gl + nlen + 2);
    if (!buf || !nb || !out) { free(buf); free(nb); free(out); return; }
    char *g = buf;
    doc_get(d, gs, gl, g);
    doc_get(d, ns, nlen, nb);
    size_t m = 0;
    if (dir < 0) { nb_memmove(out, g, gl); m = gl; out[m++] = '\n'; nb_memmove(out + m, nb, nlen); m += nlen; }
    else { nb_memmove(out, nb, nlen); m = nlen; out[m++] = '\n'; nb_memmove(out + m, g, gl); m += gl; }
    size_t a = dir < 0 ? ns : gs;
    size_t old = dir < 0 ? ge - ns : ne - gs;
    ptrdiff_t shift = dir < 0 ? -(ptrdiff_t)(nlen + 1) : (ptrdiff_t)(nlen + 1);
    size_t cb = (size_t)((ptrdiff_t)d->caret + shift), ab = (size_t)((ptrdiff_t)d->anchor + shift);
    if (do_replace(e, a, old, out, m, cb, DK_OTHER)) {
        d->anchor = ab;
        if (d->cur > 0) d->u[d->cur - 1].aa = ab;
    }
    free(buf); free(nb); free(out);
}

/* ---------------------------------------------------------------- caret movement */

static void move_vertical(Editor *e, int rows, bool extend) {
    Doc *d = &e->doc;
    int line, row, col;
    vis_of(e, d->caret, &line, &row, &col);
    int want = e->want_col >= 0 ? e->want_col : col;
    int nl = line, nr = row;
    step_rows(e, &nl, &nr, rows);
    size_t np;
    if (nl == line && nr == row) np = rows < 0 ? 0 : doc_len(d);       /* already on the first / last row: go to the edge */
    else np = off_at_col(e, nl, nr, want);
    set_caret(e, np, extend);
    e->want_col = want;
}

static void word_bounds(Editor *e, size_t off, size_t *lo, size_t *hi) {
    Doc *d = &e->doc;
    int l = doc_line_of(d, off);
    size_t ls = doc_line_start(d, l), le = doc_line_end(d, l);
    if (off > le) off = le;
    char tmp[4];
    uint32_t cp = 0;
    int c;
    if (off < le) {
        size_t n = le - off < 4 ? le - off : 4;
        doc_get(d, off, n, tmp);
        nb_u8_dec(tmp, tmp + n, &cp);
        c = nb_char_class(cp);
    } else if (off > ls) {
        size_t p = doc_prev_char(d, off);
        size_t n = off - p;
        doc_get(d, p, n, tmp);
        nb_u8_dec(tmp, tmp + n, &cp);
        c = nb_char_class(cp);
        off = p;
    } else { *lo = *hi = off; return; }
    size_t a = off, b = off;
    while (a > ls) {
        size_t p = doc_prev_char(d, a);
        size_t n = a - p;
        doc_get(d, p, n, tmp);
        nb_u8_dec(tmp, tmp + n, &cp);
        if (nb_char_class(cp) != c) break;
        a = p;
    }
    while (b < le) {
        size_t n = le - b < 4 ? le - b : 4;
        doc_get(d, b, n, tmp);
        int k = nb_u8_dec(tmp, tmp + n, &cp);
        if (nb_char_class(cp) != c) break;
        b += (size_t)k;
    }
    *lo = a;
    *hi = b;
}

static size_t ed_hit(Editor *e, int mx, int my) {
    int vr = my - e->text.y;
    vr = vr < 0 ? -((-vr + CH - 1) / CH) : vr / CH;
    int line = e->top_line, sub = e->top_sub;
    step_rows(e, &line, &sub, vr);
    int px = mx - (e->text.x + PADX) + CW / 2;
    int col = (px < 0 ? 0 : px / CW) + e->scroll_x;
    return off_at_col(e, line, sub, col);
}

/* the byte offset under a window position; clicks on the gutter count as the start of that line */
size_t ed_offset_at(Editor *e, int mx, int my) {
    if (mx < e->text.x) mx = e->text.x + 1;
    return ed_hit(e, mx, my);
}

/* ---------------------------------------------------------------- keyboard */

static bool is_text_key(const KKey *k) {
    return k->ch >= 32 && k->ch != 127 && !(k->mods & (MOD_CTRL | MOD_ALT));
}

bool ed_key(Editor *e, KUI *ui, const KKey *k) {
    (void)ui;
    Doc *d = &e->doc;
    bool shift = (k->mods & MOD_SHIFT) != 0, ctrl = (k->mods & MOD_CTRL) != 0, alt = (k->mods & MOD_ALT) != 0;
    size_t lo, hi, len = doc_len(d);
    ed_selection(e, &lo, &hi);
    bool sel = hi > lo;

    if (ctrl && !alt) {
        switch (k->code) {
        case KEY_A: ed_select_all(e); return true;
        case KEY_C: ed_copy(e, false); return true;
        case KEY_X: ed_copy(e, true); return true;
        case KEY_V: ed_paste(e); return true;
        case KEY_Z: if (shift) ed_redo(e); else ed_undo(e); return true;
        case KEY_Y: ed_redo(e); return true;
        case KEY_HOME: set_caret(e, 0, shift); return true;
        case KEY_END: set_caret(e, len, shift); return true;
        case KEY_LEFT: set_caret(e, doc_word_left(d, d->caret), shift); return true;
        case KEY_RIGHT: set_caret(e, doc_word_right(d, d->caret), shift); return true;
        case KEY_BACKSPACE:
            if (sel) do_replace(e, lo, hi - lo, "", 0, lo, DK_OTHER);
            else { size_t p = doc_word_left(d, d->caret); do_replace(e, p, d->caret - p, "", 0, p, DK_OTHER); }
            return true;
        case KEY_DELETE:
            if (sel) do_replace(e, lo, hi - lo, "", 0, lo, DK_OTHER);
            else { size_t p = doc_word_right(d, d->caret); do_replace(e, d->caret, p - d->caret, "", 0, d->caret, DK_OTHER); }
            return true;
        case KEY_INSERT: ed_copy(e, false); return true;
        case KEY_D: if (shift) { ed_duplicate_line(e); return true; } break;
        case KEY_K: if (shift) { ed_delete_line(e); return true; } break;
        case KEY_LBRACKET: ed_indent(e, true); return true;
        case KEY_RBRACKET: ed_indent(e, false); return true;
        default: break;
        }
        return false;
    }
    if (alt && !ctrl) {
        if (k->code == KEY_UP) { ed_move_lines(e, -1); return true; }
        if (k->code == KEY_DOWN) { ed_move_lines(e, 1); return true; }
        return false;
    }

    switch (k->code) {
    case KEY_LEFT:
        if (sel && !shift) set_caret(e, lo, false); else set_caret(e, doc_prev_char(d, d->caret), shift);
        return true;
    case KEY_RIGHT:
        if (sel && !shift) set_caret(e, hi, false); else set_caret(e, doc_next_char(d, d->caret), shift);
        return true;
    case KEY_UP: move_vertical(e, -1, shift); return true;
    case KEY_DOWN: move_vertical(e, 1, shift); return true;
    case KEY_PAGEUP:
    case KEY_PAGEDOWN: {
        int n = e->vis_rows > 1 ? e->vis_rows - 1 : 1;
        if (k->code == KEY_PAGEUP) n = -n;
        int tl = e->top_line, ts = e->top_sub;
        step_rows(e, &tl, &ts, n);
        e->top_line = tl;
        e->top_sub = ts;
        move_vertical(e, n, shift);
        return true;
    }
    case KEY_HOME: {
        int line, row, col;
        vis_of(e, d->caret, &line, &row, &col);
        size_t np;
        if (e->wrap) { size_t rs, re; row_span(e, line, row, &rs, &re); np = rs; }
        else { size_t ie = doc_indent_end(d, line), ls = doc_line_start(d, line); np = d->caret != ie ? ie : ls; }
        set_caret(e, np, shift);
        return true;
    }
    case KEY_END: {
        int line, row, col;
        vis_of(e, d->caret, &line, &row, &col);
        size_t rs, re;
        row_span(e, line, row, &rs, &re);
        size_t np = re;
        if (e->wrap && re < doc_line_end(d, line)) np = doc_prev_char(d, re);
        set_caret(e, np, shift);
        return true;
    }
    case KEY_BACKSPACE:
        if (sel) do_replace(e, lo, hi - lo, "", 0, lo, DK_OTHER);
        else if (d->caret > 0) {
            size_t p = doc_prev_char(d, d->caret);
            do_replace(e, p, d->caret - p, "", 0, p, DK_BACKSPACE);
        }
        return true;
    case KEY_DELETE:
        if (shift && sel) { ed_copy(e, true); return true; }
        if (sel) do_replace(e, lo, hi - lo, "", 0, lo, DK_OTHER);
        else if (d->caret < len) {
            size_t q = doc_next_char(d, d->caret);
            do_replace(e, d->caret, q - d->caret, "", 0, d->caret, DK_DELFWD);
        }
        return true;
    case KEY_INSERT:
        if (shift) { ed_paste(e); return true; }
        if (ctrl) { ed_copy(e, false); return true; }
        return false;
    case KEY_ENTER:
    case KEY_KPENTER: {
        int line = doc_line_of(d, lo);
        size_t ls = doc_line_start(d, line), ie = doc_indent_end(d, line);
        size_t ind = (ie < lo ? ie : lo) - ls;
        if (ind > 200) ind = 200;
        char buf[512];
        size_t n = 0;
        buf[n++] = '\n';
        doc_get(d, ls, ind, buf + 1);
        n += ind;
        size_t caret_after;
        bool brace = false;
        if (e->lang == LANG_C) {
            size_t p = lo;
            while (p > ls && doc_byte(d, p - 1) == ' ') p--;
            brace = p > ls && doc_byte(d, p - 1) == '{';
        }
        if (brace) {
            if (e->spaces) { memset(buf + n, ' ', (size_t)tabw(e)); n += (size_t)tabw(e); }
            else buf[n++] = '\t';
            caret_after = lo + n;
            if (hi < len && doc_byte(d, hi) == '}') { buf[n++] = '\n'; doc_get(d, ls, ind, buf + n); n += ind; }
        } else caret_after = lo + n;
        do_replace(e, lo, hi - lo, buf, n, caret_after, DK_OTHER);
        return true;
    }
    case KEY_TAB: {
        if (shift) { ed_indent(e, true); return true; }
        int l0, l1;
        sel_lines(e, &l0, &l1);
        if (sel && l1 > l0) { ed_indent(e, false); return true; }
        int line, col;
        ed_caret_pos(e, &line, &col);
        char unit[16];
        size_t n = indent_unit(e, unit, col);
        do_replace(e, lo, hi - lo, unit, n, lo + n, DK_OTHER);
        return true;
    }
    default:
        break;
    }
    if (is_text_key(k)) {
        char enc[4];
        int n = kc_utf8_encode(k->ch, enc);
        if (sel) do_replace(e, lo, hi - lo, enc, (size_t)n, lo + (size_t)n, DK_OTHER);
        else do_replace(e, d->caret, 0, enc, (size_t)n, d->caret + (size_t)n, DK_TYPE);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- scroll bars */

static bool sbar(KUI *ui, KRect r, bool horiz, int *pos, int maxpos, int page, int total, bool live) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    kc_fill(cv, r, kc_shade(th->window_bg, 90));
    if (maxpos <= 0 || total <= 0) return false;
    int track = horiz ? r.w : r.h;
    int tl = (int)((long long)track * page / total);
    if (tl < 18) tl = 18;
    if (tl > track) tl = track;
    int range = track - tl;
    if (*pos < 0) *pos = 0;
    if (*pos > maxpos) *pos = maxpos;
    int tp = (int)((long long)range * (*pos) / maxpos);
    KRect thumb = horiz ? kr(r.x + tp, r.y, tl, r.h) : kr(r.x, r.y + tp, r.w, tl);
    uint32_t id = k_id(horiz ? "nb-hscroll" : "nb-vscroll", r);
    bool changed = false;
    if (!live) { kc_panel(cv, thumb, KB_RAISED, th->button_bg); return false; }
    bool over = k_hover(ui, thumb);
    if (over && ui->pressed[0]) { ui->active = id; ui->drag_dy = horiz ? ui->mx - thumb.x : ui->my - thumb.y; }
    else if (!over && k_hover(ui, r) && ui->pressed[0]) {                /* click on the track: one page */
        bool before = horiz ? ui->mx < thumb.x : ui->my < thumb.y;
        int np = *pos + (before ? -page : page);
        if (np < 0) np = 0;
        if (np > maxpos) np = maxpos;
        if (np != *pos) { *pos = np; changed = true; }
    }
    if (ui->active == id) {
        if (ui->buttons & 1) {
            int rel = (horiz ? ui->mx - r.x : ui->my - r.y) - ui->drag_dy;
            int np = range > 0 ? (int)((long long)rel * maxpos / range) : 0;
            if (np < 0) np = 0;
            if (np > maxpos) np = maxpos;
            if (np != *pos) { *pos = np; changed = true; }
            tp = (int)((long long)range * (*pos) / maxpos);
            thumb = horiz ? kr(r.x + tp, r.y, tl, r.h) : kr(r.x, r.y + tp, r.w, tl);
        } else ui->active = 0;
    }
    kc_panel(cv, thumb, KB_RAISED, th->button_bg);
    return changed;
}

/* ---------------------------------------------------------------- drawing */

typedef struct {
    Editor *e;
    KCanvas *cv;
    const KTheme *th;
    size_t sel_lo, sel_hi, caret;
    int caret_line;
    bool show_caret, blink_on;
    int cls_line;
    uint32_t curline_bg, gutter_bg, match_bg;
    const char *pat;
    size_t plen;
    bool pcase;
} Ctx;

static bool ci_eq(unsigned char a, unsigned char b) {
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    return a == b;
}

static void draw_row(Ctx *c, int y, int line, int sub, size_t rs, size_t re) {
    Editor *e = c->e;
    Doc *d = &e->doc;
    KCanvas *cv = c->cv;
    const KTheme *th = c->th;
    KRect tx = e->text;
    size_t ls = doc_line_start(d, line), le = doc_line_end(d, line);
    bool last_row = re == le;
    bool has_sel = c->sel_hi > c->sel_lo;
    KRect old = cv->clip;

    /* gutter */
    if (e->numbers) {
        KRect g = kr(e->box.x + 2, y, e->gutter_w, CH);
        kc_set_clip(cv, kr_intersect(g, old));
        if (sub == 0) {
            char num[16];
            snprintf(num, sizeof num, "%d", line + 1);
            int w = (int)strlen(num) * CW;
            kc_text(cv, g.x + e->gutter_w - 10 - w, y, num, line == c->caret_line ? th->text : th->text_dim);
        }
    }
    kc_set_clip(cv, kr_intersect(tx, old));
    if (line == c->caret_line && !has_sel) kc_fill(cv, kr(tx.x, y, tx.w, CH), c->curline_bg);

    /* syntax classes of the whole line (computed once for all of its rows) */
    const uint8_t *cls = NULL;
    if (e->highlight && e->lang != LANG_PLAIN && doc_len(d) <= NB_HL_MAXDOC && le - ls <= NB_SYN_MAXLEN) {
        if (c->cls_line != line) {
            SynState st = syn_state_at(e, line);
            LV lv = lview(e, line);
            syn_scan(e->lang, lv.t, le - ls, st, e->cls);
            c->cls_line = line;
        }
        cls = e->cls;
    }
    LV v = lview(e, line);

    /* find matches in this row */
    size_t ms[16], me[16];
    int nm = 0;
    if (c->plen && re >= rs + c->plen) {
        for (size_t i = rs; i + c->plen <= re && nm < 16; i++) {
            const char *p = v.t + (i - v.s);
            bool ok = c->pcase ? (p[0] == c->pat[0]) : ci_eq((unsigned char)p[0], (unsigned char)c->pat[0]);
            if (!ok) continue;
            size_t k = 1;
            if (c->pcase) { while (k < c->plen && p[k] == c->pat[k]) k++; }
            else { while (k < c->plen && ci_eq((unsigned char)p[k], (unsigned char)c->pat[k])) k++; }
            if (k == c->plen) { ms[nm] = i; me[nm] = i + c->plen; nm++; i += c->plen - 1; }
        }
    }

    int x0 = tx.x + PADX - e->scroll_x * CW;
    int col = 0, tw = tabw(e), mi = 0, caret_col = -1;
    size_t i = rs;
    int right = tx.x + tx.w;
    while (i < re) {
        if (i == c->caret) caret_col = col;
        uint32_t cp;
        int n = nb_u8_dec(v.t + (i - v.s), v.t + (re - v.s), &cp);
        int w = cp == '\t' ? tw - col % tw : 1;
        int x = x0 + col * CW;
        if (x >= right) break;
        if (x + w * CW > tx.x) {
            bool sel = i >= c->sel_lo && i < c->sel_hi;
            while (mi < nm && me[mi] <= i) mi++;
            bool hl = mi < nm && i >= ms[mi] && i < me[mi];
            if (sel) kc_fill(cv, kr(x, y, w * CW, CH), th->select_bg);
            else if (hl) kc_fill(cv, kr(x, y, w * CW, CH), c->match_bg);
            if (cp != ' ' && cp != '\t') {
                uint32_t fg = sel ? th->select_text : (cls ? syn_color(cls[i - ls], th) : th->field_text);
                kc_glyph(cv, x, y, (cp < 32 || cp == 127) ? 0xFFFD : cp, fg, 1);
            }
        }
        col += w;
        i += (size_t)n;
    }
    if (i >= re && last_row && c->caret == re) caret_col = col;
    if (last_row && c->sel_lo <= le && c->sel_hi > le && le < doc_len(d)) {      /* the selected line break */
        kc_fill(cv, kr(x0 + col * CW, y, CW, CH), th->select_bg);
    }
    if (c->show_caret && c->blink_on && caret_col >= 0) {
        int x = x0 + caret_col * CW;
        bool on_sel = c->caret >= c->sel_lo && c->caret < c->sel_hi;
        if (x >= tx.x && x < right) kc_fill(cv, kr(x, y, 2, CH), on_sel ? th->select_text : th->field_text);
    }
    cv->clip = old;
}

void ed_draw(Editor *e, KUI *ui, KRect r, bool active, bool blocked) {
    Doc *d = &e->doc;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    e->box = r;
    kc_panel(cv, r, KB_SUNKEN, th->field_bg);
    KRect in = kr_inset(r, 2);
    int nl = doc_nlines(d);
    int digits = 1;
    for (int v = nl; v >= 10; v /= 10) digits++;
    if (digits < 3) digits = 3;
    e->gutter_w = e->numbers ? digits * CW + 20 : 0;

    /* layout: vertical bar always, horizontal bar only when a line is wider than the view (no wrap) */
    int tw_full = in.w - BARW - e->gutter_w;
    if (tw_full < 24) tw_full = 24;
    int cols0 = (tw_full - 2 * PADX) / CW;
    bool hbar = !e->wrap && (int)d->maxline + 2 > cols0;
    int th_h = in.h - (hbar ? BARW : 0);
    if (th_h < CH) th_h = CH;
    e->text = kr(in.x + e->gutter_w, in.y, tw_full, th_h);
    e->vis_rows = th_h / CH;
    if (e->vis_rows < 1) e->vis_rows = 1;
    e->cols = cols0 < 4 ? 4 : cols0;
    e->vis_cols = e->cols;
    KRect vbar = kr(in.x + in.w - BARW, in.y, BARW, th_h);
    KRect hb = kr(in.x + e->gutter_w, in.y + th_h, tw_full, BARW);

    ed_clamp(e);
    if (e->reveal) { do_reveal(e); e->reveal = false; ed_clamp(e); }

    /* mouse */
    KRect gut = kr(in.x, in.y, e->gutter_w, th_h);
    bool hover_text = !blocked && k_hover(ui, e->text);
    bool hover_gut = !blocked && e->gutter_w > 0 && k_hover(ui, gut);
    if (hover_text) ui->cursor_shape = FC_TEXT;
    if (!blocked && ui->pressed[0] && (hover_text || hover_gut)) {
        ui->focus = ED_ID;
        ui->caret_ms = ui->now_ms;
        size_t off = hover_gut ? doc_line_start(d, doc_line_of(d, ed_hit(e, e->text.x + 1, ui->my))) : ed_hit(e, ui->mx, ui->my);
        doc_break_undo(d);
        e->want_col = -1;
        e->blink_ms = ui->now_ms;
        bool triple = !ui->double_click && e->last_dbl_ms && ui->now_ms - e->last_dbl_ms < 600;
        if (hover_gut || triple) {
            int l = doc_line_of(d, off);
            e->drag_lo = doc_line_start(d, l);
            e->drag_hi = l + 1 < nl ? doc_line_start(d, l + 1) : doc_len(d);
            d->anchor = e->drag_lo;
            d->caret = e->drag_hi;
            e->drag = 3;
            e->last_dbl_ms = 0;
        } else if (ui->double_click) {
            size_t lo, hi;
            word_bounds(e, off, &lo, &hi);
            e->drag_lo = lo;
            e->drag_hi = hi;
            d->anchor = lo;
            d->caret = hi;
            e->drag = 2;
            e->last_dbl_ms = ui->now_ms;
        } else {
            if (ui->mods & MOD_SHIFT) d->caret = off; else d->caret = d->anchor = off;
            e->drag = 1;
            e->last_dbl_ms = 0;
        }
        kapp_set_tick(ui->app, 40);
    }
    if (e->drag) {
        if (ui->buttons & 1) {
            if (ui->my < e->text.y) scroll_rows(e, -1);
            else if (ui->my >= e->text.y + e->text.h) scroll_rows(e, 1);
            if (!e->wrap) {
                if (ui->mx < e->text.x) e->scroll_x = e->scroll_x > 0 ? e->scroll_x - 1 : 0;
                else if (ui->mx >= e->text.x + e->text.w) e->scroll_x++;
            }
            size_t off = ed_hit(e, ui->mx, ui->my);
            if (e->drag == 1) d->caret = off;
            else {
                size_t lo, hi;
                if (e->drag == 2) word_bounds(e, off, &lo, &hi);
                else { int l = doc_line_of(d, off); lo = doc_line_start(d, l); hi = l + 1 < nl ? doc_line_start(d, l + 1) : doc_len(d); }
                if (off < e->drag_lo) { d->anchor = e->drag_hi; d->caret = lo; }
                else { d->anchor = e->drag_lo; d->caret = hi; }
            }
            e->want_col = -1;
            ui->redraw_soon = true;
        } else {
            e->drag = 0;
            kapp_set_tick(ui->app, 0);
        }
    }
    if (!blocked && ui->wheel && k_hover(ui, e->box)) {
        if (ui->mods & MOD_SHIFT) { if (!e->wrap) { e->scroll_x -= ui->wheel * 6; if (e->scroll_x < 0) e->scroll_x = 0; } }
        else scroll_rows(e, -ui->wheel * 3);
    }

    /* background of the gutter */
    uint32_t gutter_bg = kc_shade(th->window_bg, 96);
    if (e->numbers) {
        kc_fill(cv, gut, gutter_bg);
        kc_vline(cv, gut.x + gut.w - 1, gut.y, gut.h, kc_shade(th->window_bg, 78));
    }

    /* rows */
    Ctx c;
    memset(&c, 0, sizeof c);
    c.e = e;
    c.cv = cv;
    c.th = th;
    c.cls_line = -1;
    ed_selection(e, &c.sel_lo, &c.sel_hi);
    c.caret = d->caret;
    c.caret_line = doc_line_of(d, d->caret);
    c.show_caret = active && ui->has_focus;
    c.blink_on = ((ui->now_ms > e->blink_ms ? ui->now_ms - e->blink_ms : 0) / 500) % 2 == 0;
    c.curline_bg = kc_mix(th->field_bg, KC_GOLD, 36);
    c.gutter_bg = gutter_bg;
    c.match_bg = 0xF3DC8C;
    if (A.find_open && A.find_buf[0]) { c.pat = A.find_buf; c.plen = strlen(A.find_buf); c.pcase = A.match_case; }
    KRect old = cv->clip;
    kc_set_clip(cv, kr(in.x, in.y, in.w - BARW, th_h));
    int line = e->top_line, sub = e->top_sub, y = in.y;
    for (int vr = 0; vr < e->vis_rows + 1 && line < nl; vr++, y += CH) {
        size_t rs, re;
        row_span(e, line, sub, &rs, &re);
        draw_row(&c, y, line, sub, rs, re);
        sub++;
        if (sub >= ed_rows_in_line(e, line)) { line++; sub = 0; }
    }
    cv->clip = old;

    if (doc_len(d) == 0 && !A.has_path && th_h >= 3 * CH) {                  /* an empty page says what to do */
        kc_set_clip(cv, kr_intersect(e->text, old));
        kc_text(cv, e->text.x + PADX, in.y + 2 * CH, "A blank page. Start typing, or press Ctrl+O to open a file.", th->text_dim);
        cv->clip = old;
    }

    /* scroll bars */
    int pos, maxpos;
    if (e->wrap) {
        int tl = nl - 1, ts = 0;
        step_rows(e, &tl, &ts, -(e->vis_rows - 1));
        maxpos = tl + (ts > 0 ? 1 : 0);
    } else maxpos = nl - e->vis_rows;
    if (maxpos < 0) maxpos = 0;
    pos = e->top_line;
    if (sbar(ui, vbar, false, &pos, maxpos, e->vis_rows, nl, !blocked)) { e->top_line = pos; e->top_sub = 0; ed_clamp(e); }
    if (hbar) {
        int total = (int)d->maxline + 4;
        int hmax = total - e->vis_cols;
        if (hmax < 0) hmax = 0;
        int hp = e->scroll_x;
        if (sbar(ui, hb, true, &hp, hmax, e->vis_cols, total, !blocked)) e->scroll_x = hp;
        kc_fill(cv, kr(in.x + in.w - BARW, in.y + th_h, BARW, BARW), kc_shade(th->window_bg, 90));
        if (e->gutter_w) kc_fill(cv, kr(in.x, in.y + th_h, e->gutter_w, BARW), gutter_bg);
    }
    if (active && ui->has_focus) ui->redraw_soon = true;
}
