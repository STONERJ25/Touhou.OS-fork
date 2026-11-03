/* Cursor movement and the editing commands. Every change to the text goes through edit_insert / edit_delete so that it
 * can be undone. */
#include "pnote.h"

static Line *cl(void) { return LN(E.cy); }

void clamp_cursor(void) {
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.n) E.cy = E.n - 1;
    Line *L = cl();
    if (E.cx > L->len) E.cx = L->len;
    while (E.cx > 0 && E.cx < L->len && ((unsigned char)L->s[E.cx] & 0xC0) == 0x80) E.cx--;
}

void remember_col(void) { E.want_col = line_dispcol(cl(), E.cx); }

/* Called before every command. Typing, backspacing and deleting in a row share one undo step; everything else starts a
 * new one. */
void begin_cmd(int kind) {
    E.prev_kind = E.last_kind;
    if (kind == K_OTHER || kind == K_CUT || kind != E.last_kind) undo_boundary();
    E.last_kind = kind;
}

int edit_allowed(void) {
    if (E.view_only) {
        set_msg("This notebook is open for reading only (started with -R)");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ selection */

int sel_range(long *l1, size_t *c1, long *l2, size_t *c2) {
    if (!E.mark_on) return 0;
    long ml = E.mark_ln;
    size_t mc = E.mark_col;
    if (ml >= E.n) { ml = E.n - 1; mc = LN(ml)->len; }
    if (mc > LN(ml)->len) mc = LN(ml)->len;
    if (ml == E.cy && mc == E.cx) return 0;
    if (ml < E.cy || (ml == E.cy && mc < E.cx)) { *l1 = ml; *c1 = mc; *l2 = E.cy; *c2 = E.cx; }
    else { *l1 = E.cy; *c1 = E.cx; *l2 = ml; *c2 = mc; }
    return 1;
}

void cmd_toggle_mark(void) {
    if (E.mark_on) { E.mark_on = 0; set_msg("Selection cleared"); return; }
    E.mark_on = 1;
    E.mark_ln = E.cy;
    E.mark_col = E.cx;
    set_msg("Selection started: move the cursor, then ^K cuts it, Alt-6 copies it, Alt-A cancels");
}

void cmd_clear_mark(void) { E.mark_on = 0; }

/* The text between two positions, with "\r\n" for CR LF terminators. */
static void range_text(PtBuf *out, long l1, size_t c1, long l2, size_t c2) {
    for (long i = l1; i <= l2; i++) {
        Line *L = LN(i);
        size_t a = i == l1 ? c1 : 0, b = i == l2 ? c2 : L->len;
        pb_add(out, L->s + a, b - a);
        if (i < l2) {
            if (L->cr) pb_addc(out, '\r');
            pb_addc(out, '\n');
        }
    }
}

static int delete_selection(void) {
    long l1, l2;
    size_t c1, c2;
    if (!sel_range(&l1, &c1, &l2, &c2)) { E.mark_on = 0; return 0; }
    free(edit_delete(l1, c1, l2, c2, NULL, 0));
    E.cy = l1;
    E.cx = c1;
    E.mark_on = 0;
    return 1;
}

/* ------------------------------------------------------------------ movement */

void cmd_move_left(void) {
    Line *L = cl();
    if (E.cx > 0) E.cx = pt_u8_prev(L->s, E.cx);
    else if (E.cy > 0) { E.cy--; E.cx = cl()->len; }
    remember_col();
}

void cmd_move_right(void) {
    Line *L = cl();
    if (E.cx < L->len) E.cx += (size_t)pt_u8_next(L->s, L->len, E.cx);
    else if (E.cy < E.n - 1) { E.cy++; E.cx = 0; }
    remember_col();
}

void cmd_move_up(void) {
    if (E.want_col < 0) remember_col();
    if (E.cy > 0) { E.cy--; E.cx = line_off_for_col(cl(), E.want_col); }
    else E.cx = 0;
}

void cmd_move_down(void) {
    if (E.want_col < 0) remember_col();
    if (E.cy < E.n - 1) { E.cy++; E.cx = line_off_for_col(cl(), E.want_col); }
    else { E.cx = cl()->len; remember_col(); }
}

void cmd_home(void) { E.cx = 0; remember_col(); }
void cmd_end(void) { E.cx = cl()->len; remember_col(); }

void cmd_page(int dir) {
    if (E.want_col < 0) remember_col();
    long step = E.text_rows - 1;
    if (step < 1) step = 1;
    long ny = E.cy + dir * step;
    if (ny < 0) ny = 0;
    if (ny >= E.n) ny = E.n - 1;
    E.top += (ny - E.cy);
    if (E.top < 0) E.top = 0;
    E.cy = ny;
    E.cx = line_off_for_col(cl(), E.want_col);
}

static int word_after(void) {
    Line *L = cl();
    if (E.cx >= L->len) return 0;
    uint32_t cp;
    pt_u8_decode((const unsigned char *)L->s + E.cx, L->len - E.cx, &cp);
    return pt_is_word_cp(cp);
}

static int word_before(void) {
    Line *L = cl();
    if (E.cx == 0) return 0;
    size_t p = pt_u8_prev(L->s, E.cx);
    uint32_t cp;
    pt_u8_decode((const unsigned char *)L->s + p, E.cx - p, &cp);
    return pt_is_word_cp(cp);
}

static int at_eof(void) { return E.cy == E.n - 1 && E.cx >= cl()->len; }
static int at_bof(void) { return E.cy == 0 && E.cx == 0; }

void cmd_word(int dir) {
    if (dir > 0) {
        while (!at_eof() && !word_after()) cmd_move_right();
        while (!at_eof() && word_after()) cmd_move_right();
    } else {
        while (!at_bof() && !word_before()) cmd_move_left();
        while (!at_bof() && word_before()) cmd_move_left();
    }
    remember_col();
}

void cmd_top(void) { E.cy = 0; E.cx = 0; remember_col(); }
void cmd_bottom(void) { E.cy = E.n - 1; E.cx = cl()->len; remember_col(); }

void cmd_goto(long line, long col) {
    if (line < 1) line = 1;
    if (line > E.n) line = E.n;
    E.cy = line - 1;
    E.cx = col > 1 ? line_off_for_col(cl(), col - 1) : 0;
    remember_col();
}

/* ------------------------------------------------------------------ editing */

void cmd_insert_cp(uint32_t cp) {
    if (!edit_allowed()) return;
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || pt_is_rawbyte(cp)) return;
    if (E.mark_on) {
        long l1, l2;
        size_t c1, c2;
        if (sel_range(&l1, &c1, &l2, &c2)) { undo_boundary(); delete_selection(); }
        E.mark_on = 0;
    }
    char u[4];
    int l = pt_u8_encode(cp, u);
    long eln;
    size_t ecol;
    edit_insert(E.cy, E.cx, u, (size_t)l, &eln, &ecol, 1);
    E.cy = eln;
    E.cx = ecol;
    remember_col();
}

void cmd_newline(void) {
    if (!edit_allowed()) return;
    if (E.mark_on) delete_selection();
    Line *L = cl();
    PtBuf nl = { 0 };
    if (L->cr) pb_addc(&nl, '\r');
    pb_addc(&nl, '\n');
    if (E.autoindent) {
        size_t k = 0;
        while (k < L->len && k < E.cx && (L->s[k] == ' ' || L->s[k] == '\t')) k++;
        pb_add(&nl, L->s, k);
    }
    long eln;
    size_t ecol;
    edit_insert(E.cy, E.cx, nl.p, nl.len, &eln, &ecol, 0);
    pb_free(&nl);
    E.cy = eln;
    E.cx = ecol;
    remember_col();
}

void cmd_backspace(void) {
    if (!edit_allowed()) return;
    if (E.mark_on && delete_selection()) { remember_col(); return; }
    E.mark_on = 0;
    Line *L = cl();
    if (E.cx > 0) {
        size_t p = pt_u8_prev(L->s, E.cx);
        free(edit_delete(E.cy, p, E.cy, E.cx, NULL, 1));
        E.cx = p;
    } else if (E.cy > 0) {
        size_t plen = LN(E.cy - 1)->len;
        free(edit_delete(E.cy - 1, plen, E.cy, 0, NULL, 0));
        E.cy--;
        E.cx = plen;
    }
    remember_col();
}

void cmd_delete(void) {
    if (!edit_allowed()) return;
    if (E.mark_on && delete_selection()) { remember_col(); return; }
    E.mark_on = 0;
    Line *L = cl();
    if (E.cx < L->len) {
        size_t n = (size_t)pt_u8_next(L->s, L->len, E.cx);
        free(edit_delete(E.cy, E.cx, E.cy, E.cx + n, NULL, 1));
    } else if (E.cy < E.n - 1) {
        free(edit_delete(E.cy, L->len, E.cy + 1, 0, NULL, 0));
    }
    remember_col();
}

void cmd_tab(void) {
    if (!edit_allowed()) return;
    if (E.mark_on) { undo_boundary(); delete_selection(); }
    char sp[64];
    size_t n = 1;
    sp[0] = '\t';
    if (E.tabs_to_spaces) {
        long col = line_dispcol(cl(), E.cx);
        n = (size_t)(E.tabsize - (int)(col % E.tabsize));
        if (n > sizeof sp) n = sizeof sp;
        memset(sp, ' ', n);
    }
    long eln;
    size_t ecol;
    edit_insert(E.cy, E.cx, sp, n, &eln, &ecol, 1);
    E.cy = eln;
    E.cx = ecol;
    remember_col();
}

/* Shift-Tab: take one level of indentation off the current line. */
void cmd_unindent(void) {
    if (!edit_allowed()) return;
    Line *L = cl();
    size_t k = 0;
    if (L->len && L->s[0] == '\t') k = 1;
    else while (k < L->len && k < (size_t)E.tabsize && L->s[k] == ' ') k++;
    if (!k) { set_msg("Nothing to unindent"); return; }
    free(edit_delete(E.cy, 0, E.cy, k, NULL, 0));
    E.cx = E.cx > k ? E.cx - k : 0;
    remember_col();
}

void cmd_cut(void) {
    if (!edit_allowed()) return;
    long l1, l2;
    size_t c1, c2;
    if (sel_range(&l1, &c1, &l2, &c2)) {
        size_t n;
        char *t = edit_delete(l1, c1, l2, c2, &n, 0);
        pb_clear(&E.clip);
        pb_add(&E.clip, t, n);
        free(t);
        E.clip_lines = 0;
        E.cy = l1;
        E.cx = c1;
        E.mark_on = 0;
        remember_col();
        set_msg("Cut %zu bytes", n);
        return;
    }
    E.mark_on = 0;
    Line *L = cl();
    size_t llen = L->len;
    PtBuf text = { 0 };
    pb_add(&text, L->s, llen);
    if (L->cr) pb_addc(&text, '\r');
    pb_addc(&text, '\n');
    long ln = E.cy;
    if (E.n == 1) {
        free(edit_delete(0, 0, 0, llen, NULL, 0));
        E.cx = 0;
    } else if (ln < E.n - 1) {
        free(edit_delete(ln, 0, ln + 1, 0, NULL, 0));
        E.cx = 0;
    } else {
        size_t plen = LN(ln - 1)->len;
        free(edit_delete(ln - 1, plen, ln, llen, NULL, 0));
        E.cy = ln - 1;
        E.cx = 0;
    }
    if (E.prev_kind == K_CUT && E.clip_lines) pb_add(&E.clip, text.p, text.len);       /* successive ^K collect lines */
    else { pb_clear(&E.clip); pb_add(&E.clip, text.p, text.len); }
    E.clip_lines = 1;
    pb_free(&text);
    remember_col();
}

void cmd_copy(void) {
    long l1, l2;
    size_t c1, c2;
    if (!sel_range(&l1, &c1, &l2, &c2)) {
        set_msg("Nothing is selected (Alt-A starts a selection)");
        return;
    }
    pb_clear(&E.clip);
    range_text(&E.clip, l1, c1, l2, c2);
    E.clip_lines = 0;
    E.mark_on = 0;
    set_msg("Copied %zu bytes", E.clip.len);
}

void insert_text_at_cursor(const char *text, size_t n) {
    long eln;
    size_t ecol;
    edit_insert(E.cy, E.cx, text, n, &eln, &ecol, 0);
    E.cy = eln;
    E.cx = ecol;
    remember_col();
}

void cmd_paste(void) {
    if (!edit_allowed()) return;
    if (!E.clip.len) { set_msg("The cut buffer is empty"); return; }
    if (E.mark_on) delete_selection();
    if (E.clip_lines) {
        size_t keep = E.cx;
        long eln;
        size_t ecol;
        edit_insert(E.cy, 0, E.clip.p, E.clip.len, &eln, &ecol, 0);
        E.cy = eln;
        E.cx = keep;
        clamp_cursor();
        remember_col();
    } else {
        insert_text_at_cursor(E.clip.p, E.clip.len);
    }
}
