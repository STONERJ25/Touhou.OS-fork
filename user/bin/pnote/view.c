/* Layout and drawing: the shrine-red title bar, the text rows with optional line numbers and highlighting, the status
 * row and the two rows of key hints. Only rows that changed since the last frame are sent to the terminal. */
#include "pnote.h"

int cp_width(uint32_t cp, long col) {
    if (cp == '\t') return E.tabsize - (int)(col % E.tabsize);
    if (cp < 0x20 || cp == 0x7F) return 2;
    return 1;
}

long line_dispcol(const Line *L, size_t off) {
    long col = 0;
    size_t i = 0;
    if (off > L->len) off = L->len;
    while (i < off) {
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)L->s + i, L->len - i, &cp);
        col += cp_width(cp, col);
        i += (size_t)l;
    }
    return col;
}

/* Byte offset of the character that covers display column `want` (the end of the line if it is shorter). */
size_t line_off_for_col(const Line *L, long want) {
    long col = 0;
    size_t i = 0;
    while (i < L->len) {
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)L->s + i, L->len - i, &cp);
        int w = cp_width(cp, col);
        if (col + w > want) return i;
        col += w;
        i += (size_t)l;
    }
    return L->len;
}

int gutter_width(void) {
    if (!E.linenums) return 0;
    int digits = 1;
    for (long v = E.n; v >= 10; v /= 10) digits++;
    if (digits < 3) digits = 3;
    return digits + 1;
}

void view_layout(void) {
    PtTerm *t = &E.term;
    E.rows = t->rows;
    E.cols = t->cols;
    int bottom = E.rows >= 12 ? 3 : 1;
    E.text_rows = E.rows - 1 - bottom;
    if (E.text_rows < 1) E.text_rows = 1;
    E.gutter = gutter_width();
    E.text_w = E.cols - E.gutter;
    if (E.text_w < 1) E.text_w = 1;
    if (E.scr.rows != E.rows || E.scr.cols != E.cols) pts_resize(&E.scr, E.rows, E.cols);
}

void view_resize_check(void) {
    if (pt_term_size(&E.term)) pts_invalidate(&E.scr);
}

void view_scroll_to_cursor(void) {
    long rows = E.text_rows;
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.n) E.cy = E.n - 1;
    if (E.cy < E.top || E.cy >= E.top + rows) {
        if (E.cy < E.top - rows || E.cy >= E.top + 2 * rows) E.top = E.cy - rows / 2;
        else if (E.cy < E.top) E.top = E.cy;
        else E.top = E.cy - rows + 1;
    }
    if (E.top < 0) E.top = 0;
    if (E.top >= E.n) E.top = E.n - 1;
    long col = line_dispcol(LN(E.cy), E.cx);
    long w = E.text_w;
    if (w < 8) {
        if (col < E.hscroll) E.hscroll = col;
        else if (col >= E.hscroll + w) E.hscroll = col - w + 1;
    } else if (col < E.hscroll + (E.hscroll > 0 ? 1 : 0)) {
        E.hscroll = col - w / 2;
        if (E.hscroll < 0) E.hscroll = 0;
    } else if (col > E.hscroll + w - 2) {
        E.hscroll = col - w / 2;
    }
}

/* ------------------------------------------------------------------ drawing helpers */

typedef struct { int base; int rev; } Look;

static void set_look(PtRow *row, Look *cur, Look want) {
    if (cur->base == want.base && cur->rev == want.rev) return;
    pr_reset(row);
    const char *codes = want.base ? hl_style_sgr(want.base) : "";
    if (codes[0]) pr_sgr(row, codes);
    if (want.rev) pr_sgr(row, "7");
    *cur = want;
}

static void draw_text_row(PtRow *row, long ln) {
    Line *L = LN(ln);
    Look cur = { 0, 0 };
    if (E.gutter) {
        char nb[32];
        snprintf(nb, sizeof nb, "%*ld ", E.gutter - 1, ln + 1);
        pr_sgr(row, ln == E.cy ? "33" : "90");
        pr_str(row, nb, E.gutter);
        pr_reset(row);
    }
    static unsigned char styles[2048];
    const unsigned char *st = NULL;
    if (E.highlight && E.lang != LANG_NONE && L->len <= sizeof styles) {
        hl_line(E.lang, L->s, L->len, L->hl, styles);
        st = styles;
    }
    long l1 = 0, l2 = 0;
    size_t c1 = 0, c2 = 0;
    int have_sel = sel_range(&l1, &c1, &l2, &c2) && ln >= l1 && ln <= l2;
    size_t sa = 0, sb = 0;
    int sel_nl = 0;
    if (have_sel) {
        sa = ln == l1 ? c1 : 0;
        sb = ln == l2 ? c2 : L->len;
        sel_nl = ln < l2;
    }
    int have_match = E.mh_on && ln == E.mh_ln;

    long col = 0;
    size_t i = 0;
    int used = 0, w = E.text_w, stop = 0;
    int left_hidden = E.hscroll > 0 && L->len > 0;
    while (i < L->len && used < w && !stop) {
        uint32_t cp;
        int l = pt_u8_decode((const unsigned char *)L->s + i, L->len - i, &cp);
        int cw = cp_width(cp, col);
        if (col + cw <= E.hscroll) { col += cw; i += (size_t)l; continue; }
        Look want;
        want.base = st ? st[i] : 0;
        want.rev = (have_sel && i >= sa && i < sb) || (have_match && i >= E.mh_c0 && i < E.mh_c1);
        for (int u = 0; u < cw && used < w; u++) {
            long ucol = col + u;
            if (ucol < E.hscroll) continue;
            uint32_t ch;
            if (cp == '\t') ch = ' ';
            else if (cp < 0x20) ch = u == 0 ? '^' : (uint32_t)(cp + 0x40);
            else if (cp == 0x7F) ch = u == 0 ? '^' : '?';
            else if (pt_is_rawbyte(cp)) ch = '?';
            else ch = cp;
            Look use = want;
            if (cp < 0x20 && cp != '\t') use.rev = 1;
            int more_after = (u + 1 < cw) || (i + (size_t)l < L->len);
            if (used == 0 && left_hidden && w > 2) { use.rev = 1; use.base = 0; ch = '<'; }
            else if (used == w - 1 && more_after && w > 2) { use.rev = 1; use.base = 0; ch = '>'; stop = 1; }
            set_look(row, &cur, use);
            pr_cp(row, ch);
            used++;
            if (stop) break;
        }
        col += cw;
        i += (size_t)l;
    }
    if (sel_nl && used < w) {
        Look use = { 0, 1 };
        set_look(row, &cur, use);
        pr_cp(row, ' ');
    }
    if (cur.base || cur.rev) pr_reset(row);
}

static void draw_title(PtRow *row) {
    int w = E.cols;
    pr_sgr(row, "1;37;41");
    pr_str(row, " Patchouli's Notebook", w);
    char tags[96] = "";
    if (E.view_only) strlcat(tags, "[view only] ", sizeof tags);
    else if (E.file_readonly) strlcat(tags, "[read-only file] ", sizeof tags);
    if (E.n > 0 && LN(0)->cr) strlcat(tags, "[CRLF] ", sizeof tags);
    if (buf_modified()) strlcat(tags, "Modified ", sizeof tags);
    int tl = (int)strlen(tags);
    if (row->w + tl > w) tl = 0;
    const char *name = E.path ? E.path : (E.from_stdin ? "(standard input)" : "New Buffer");
    char shown[320];
    snprintf(shown, sizeof shown, "%s", name);
    int nl = (int)pt_u8_count(shown, strlen(shown));
    int room = w - row->w - tl - 4;
    if (room >= 4) {
        if (nl > room) {                                /* keep the tail of a long path */
            size_t skip = pt_u8_skip(name, strlen(name), (size_t)(nl - room + 1));
            snprintf(shown, sizeof shown, "~%s", name + skip);
            nl = room;
        }
        int gap = (w - tl - nl) / 2 - row->w;
        if (gap < 2) gap = 2;
        pr_spaces(row, gap);
        pr_str(row, shown, nl);
    }
    if (row->w < w - tl) pr_spaces(row, w - tl - row->w);
    if (tl) pr_str(row, tags, tl);
    if (row->w < w) pr_spaces(row, w - row->w);
    pr_reset(row);
}

static const char *const hint_a[] = { "^G", "Help", "^S", "Save", "^X", "Exit", "^W", "Search", "^R", "Replace", "^K", "Cut" };
static const char *const hint_b[] = { "^Z", "Undo", "^Y", "Redo", "^L", "Goto", "^U", "Paste", "^N", "Next", "^T", "Toggle" };

static void draw_hints(PtRow *row, const char *const *h) {
    int per = E.cols / 6;
    if (per < 9) per = 9;
    for (int k = 0; k < 6; k++) {
        int start = k * per;
        if (start + 4 > E.cols) break;
        if (row->w < start) pr_spaces(row, start - row->w);
        pr_sgr(row, "7");
        pr_str(row, h[k * 2], 2);
        pr_reset(row);
        pr_spaces(row, 1);
        int room = per - 3;
        if (room > E.cols - row->w) room = E.cols - row->w;
        if (room > 0) pr_str(row, h[k * 2 + 1], room);
    }
}

static void draw_status(PtRow *row, int *cursor_col) {
    int w = E.cols;
    *cursor_col = -1;
    if (E.prompt_label) {
        pr_sgr(row, "1");
        int used = pr_str(row, E.prompt_label, w);
        size_t tl = strlen(E.prompt_text);
        int room = w - used - 1;
        if (room < 1) room = 1;
        size_t cur_cols = pt_u8_count(E.prompt_text, E.prompt_cur);
        size_t start = 0;
        if ((int)cur_cols >= room) start = pt_u8_skip(E.prompt_text, tl, cur_cols - (size_t)room + 1);
        int shown = pr_text(row, E.prompt_text + start, tl - start, room);
        *cursor_col = used + (int)(cur_cols - pt_u8_count(E.prompt_text, start));
        (void)shown;
        pr_reset(row);
        return;
    }
    if (E.msg[0]) {
        char buf[260];
        snprintf(buf, sizeof buf, "[ %s ]", E.msg);
        int len = (int)pt_u8_count(buf, strlen(buf));
        if (len > w) len = w;
        int pad = (w - len) / 2;
        pr_spaces(row, pad);
        pr_sgr(row, "1;7");
        pr_str(row, buf, w);
        pr_reset(row);
        return;
    }
    char info[96];
    Line *L = LN(E.cy);
    long col = line_dispcol(L, E.cx) + 1;
    int pct = E.n > 1 ? (int)((E.cy * 100) / (E.n - 1)) : 100;
    snprintf(info, sizeof info, "line %ld/%ld (%d%%)  col %ld ", E.cy + 1, E.n, pct, col);
    if (E.highlight && E.lang != LANG_NONE) {
        char tmp[96];
        snprintf(tmp, sizeof tmp, "%s  %s", hl_lang_name(E.lang), info);
        snprintf(info, sizeof info, "%s", tmp);
    }
    int il = (int)strlen(info);
    if (il < w) pr_spaces(row, w - il);
    pr_sgr(row, "90");
    pr_str(row, info, w);
    pr_reset(row);
}

void view_draw(void) {
    view_layout();
    view_scroll_to_cursor();
    hl_prepare(E.top + E.text_rows);
    PtScreen *s = &E.scr;
    draw_title(pts_row(s, 0));
    for (int r = 0; r < E.text_rows; r++) {
        PtRow *row = pts_row(s, 1 + r);
        long ln = E.top + r;
        if (ln < E.n) draw_text_row(row, ln);
        else { pr_sgr(row, "90"); pr_str(row, "~", 1); pr_reset(row); }
    }
    int status_row = E.rows >= 12 ? E.rows - 3 : E.rows - 1;
    int prompt_col;
    draw_status(pts_row(s, status_row), &prompt_col);
    if (E.rows >= 12) {
        draw_hints(pts_row(s, E.rows - 2), hint_a);
        draw_hints(pts_row(s, E.rows - 1), hint_b);
    }
    int crow, ccol;
    if (prompt_col >= 0) { crow = status_row; ccol = prompt_col; }
    else {
        crow = 1 + (int)(E.cy - E.top);
        ccol = E.gutter + (int)(line_dispcol(LN(E.cy), E.cx) - E.hscroll);
        if (ccol >= E.cols) ccol = E.cols - 1;
        if (ccol < 0) ccol = 0;
    }
    pts_flush(s, &E.term, crow, ccol, 1);
}
