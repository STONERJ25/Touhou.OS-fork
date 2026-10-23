/*
 * Ran's Calculator: the window. A dark display on top, a keypad (standard, plus a function pad in scientific mode) and the
 * history tape on the right. Everything also works from the keyboard: the expression line is a small text editor.
 */
#include <kappa.h>
#include <gensokyo.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "engine.h"
#include "ui.h"

#define EXPR_MAX 240
#define HIST_MAX 120
#define MENU_H 22

#define COL_LCD      0x141B42
#define COL_LCD_TEXT 0xB9C2F0
#define COL_LCD_DIM  0x6C78B0
#define COL_LCD_BIG  0xF4EFD8
#define COL_LCD_ERR  0xFF9A86
#define COL_LCD_TAG  0xD9A441

typedef struct HistEntry {
    char expr[EXPR_MAX];
    char result[48];
} HistEntry;

typedef struct Calc {
    KApp *app;
    char expr[EXPR_MAX];
    int len, caret, scroll_cp;
    bool evaluated;                     /* the expression was committed with Enter / '=': the big number is final */
    bool has_error;
    CalcError err;
    bool preview_ok;
    cnum preview;
    char preview_text[64];
    char value_text[64];
    CalcContext ctx;
    bool has_ans;
    cnum mem;
    bool has_mem;
    bool scientific, show_history;
    HistEntry hist[HIST_MAX];
    int nhist, hist_scroll, hist_nav, hist_hover;
    bool hist_stick;
    KWin *about;
    bool trace;
    char status[64];
    uint64_t status_until;
    uint64_t caret_ms;
} Calc;

static Calc C;

/* ---------------------------------------------------------------- small helpers */

static void trace(const char *fmt, ...) {
    if (!C.trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void flash(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(C.status, sizeof C.status, fmt, ap);
    va_end(ap);
    C.status_until = k_now_ms() + 2200;
}

static size_t u8_prev(const char *s, size_t i) { if (!i) return 0; i--; while (i && (s[i] & 0xC0) == 0x80) i--; return i; }
static size_t u8_next(const char *s, size_t i) { if (!s[i]) return i; i++; while (s[i] && (s[i] & 0xC0) == 0x80) i++; return i; }

static int cp_index(const char *s, int byte_off) {                  /* code points before a byte offset */
    int n = 0;
    for (int i = 0; i < byte_off && s[i]; i = (int)u8_next(s, (size_t)i)) n++;
    return n;
}

static void clear_error(void) { C.has_error = false; C.hist_nav = -1; }

static void expr_set(const char *s) {
    snprintf(C.expr, sizeof C.expr, "%s", s);
    C.len = (int)strlen(C.expr);
    C.caret = C.len;
    C.evaluated = false;
    clear_error();
    C.caret_ms = k_now_ms();
}

static void clear_entry(void) { expr_set(""); C.scroll_cp = 0; }

static bool starts_with_binary_operator(const char *s) {
    if (!s[0]) return false;
    if (strchr("+-*/^%!", s[0])) return true;
    return !strncmp(s, "\xC3\x97", 2) || !strncmp(s, "\xC3\xB7", 2) || !strncmp(s, "\xC2\xB2", 2) || !strncmp(s, "\xC2\xB3", 2) ||
           !strncmp(s, " mod", 4);
}

static void expr_insert(const char *s) {
    if (C.evaluated) {                                              /* an operator continues from the answer, anything else starts over */
        bool cont = starts_with_binary_operator(s) && C.has_ans;
        expr_set(cont ? "ans" : "");
    }
    clear_error();
    int n = (int)strlen(s);
    if (C.len + n >= EXPR_MAX) { flash("That expression is long enough already"); return; }
    memmove(C.expr + C.caret + n, C.expr + C.caret, (size_t)(C.len - C.caret + 1));
    memcpy(C.expr + C.caret, s, (size_t)n);
    C.len += n;
    C.caret += n;
    C.caret_ms = k_now_ms();
}

static void expr_backspace(void) {
    if (C.evaluated) { C.evaluated = false; }
    clear_error();
    if (C.caret == 0) return;
    size_t p = u8_prev(C.expr, (size_t)C.caret);
    memmove(C.expr + p, C.expr + C.caret, (size_t)(C.len - C.caret + 1));
    C.len -= C.caret - (int)p;
    C.caret = (int)p;
    C.caret_ms = k_now_ms();
}

static void expr_delete(void) {
    C.evaluated = false;
    clear_error();
    if (C.caret >= C.len) return;
    size_t n = u8_next(C.expr, (size_t)C.caret);
    memmove(C.expr + C.caret, C.expr + n, (size_t)(C.len - (int)n + 1));
    C.len -= (int)n - C.caret;
    C.caret_ms = k_now_ms();
}

static void caret_move(int to) {
    if (to < 0) to = 0;
    if (to > C.len) to = C.len;
    C.caret = to;
    C.evaluated = false;
    C.caret_ms = k_now_ms();
}

/* The number the caret is in or right after: [start, end) in bytes, or start == end when there is none. */
static void number_at_caret(int *start, int *end) {
    int e = C.caret, s = C.caret;
    while (s > 0 && ((C.expr[s - 1] >= '0' && C.expr[s - 1] <= '9') || C.expr[s - 1] == '.')) s--;
    *start = s;
    *end = e;
}

static void negate(void) {
    if (C.evaluated) { expr_set(C.has_ans ? "-ans" : "-"); return; }
    clear_error();
    if (C.len == 0) { expr_insert("-"); return; }
    int s, e;
    number_at_caret(&s, &e);
    if (s == e) {                                                   /* not on a number: negate the whole expression */
        if (!strncmp(C.expr, "-(", 2) && C.expr[C.len - 1] == ')') {
            char tmp[EXPR_MAX];
            snprintf(tmp, sizeof tmp, "%.*s", C.len - 3, C.expr + 2);
            expr_set(tmp);
        } else {
            char tmp[EXPR_MAX];
            snprintf(tmp, sizeof tmp, "-(%s)", C.expr);
            expr_set(tmp);
        }
        return;
    }
    bool unary_minus = s > 0 && C.expr[s - 1] == '-' && (s == 1 || strchr("+-*/^(,", C.expr[s - 2]) != NULL);
    if (unary_minus) {
        memmove(C.expr + s - 1, C.expr + s, (size_t)(C.len - s + 1));
        C.len--;
        C.caret--;
    } else if (C.len + 1 < EXPR_MAX) {
        memmove(C.expr + s + 1, C.expr + s, (size_t)(C.len - s + 1));
        C.expr[s] = '-';
        C.len++;
        C.caret++;
    }
}

static void wrap_with(const char *prefix, const char *suffix) {
    if (C.len == 0) { expr_insert(prefix); return; }
    char tmp[EXPR_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s%s%s", prefix, C.expr, suffix);
    if (strlen(tmp) >= EXPR_MAX) { flash("That expression is long enough already"); return; }
    expr_set(tmp);
}

/* ---------------------------------------------------------------- history */

static void history_add(const char *expr, const char *result) {
    if (C.nhist == HIST_MAX) {
        memmove(&C.hist[0], &C.hist[1], (size_t)(HIST_MAX - 1) * sizeof C.hist[0]);
        C.nhist--;
    }
    HistEntry *h = &C.hist[C.nhist++];
    snprintf(h->expr, sizeof h->expr, "%s", expr);
    snprintf(h->result, sizeof h->result, "%s", result);
    C.hist_stick = true;
}

/* ---------------------------------------------------------------- actions */

static void commit(void) {
    if (C.len == 0) return;
    cnum v;
    CalcError err;
    if (calc_eval(&C.ctx, C.expr, &v, &err)) {
        char text[64];
        calc_format(v, text, sizeof text);
        history_add(C.expr, text);
        C.ctx.ans = v;
        C.has_ans = true;
        snprintf(C.value_text, sizeof C.value_text, "%s", text);
        C.evaluated = true;
        C.has_error = false;
        C.hist_nav = -1;
        trace("calc: %s = %s", C.expr, text);
    } else {
        C.has_error = true;
        C.err = err;
        if (err.pos >= 0 && err.pos <= C.len) C.caret = err.pos;
        trace("calc: %s -> error: %s", C.expr, err.msg);
    }
}

static void refresh_preview(void) {
    C.preview_ok = false;
    if (C.len > 0 && !C.evaluated) {
        CalcError e;
        if (calc_eval(&C.ctx, C.expr, &C.preview, &e)) {
            C.preview_ok = true;
            calc_format(C.preview, C.preview_text, sizeof C.preview_text);
        }
    }
}

/* The number the display currently stands for: the live value of the expression, else the last answer. */
static bool current_value(cnum *out) {
    refresh_preview();
    if (C.len > 0) {
        if (C.evaluated) { *out = C.ctx.ans; return true; }
        if (C.preview_ok) { *out = C.preview; return true; }
        return false;
    }
    if (C.has_ans) { *out = C.ctx.ans; return true; }
    *out = 0;
    return true;
}

static void copy_result(void) {
    char text[64];
    cnum v;
    refresh_preview();
    if (C.len > 0 && !C.evaluated && !C.preview_ok) {                /* nothing valid to copy: take the expression itself */
        kapp_clipboard_copy(C.app, C.expr, (size_t)C.len);
        flash("Copied the expression");
        trace("calc: copied %s", C.expr);
        return;
    }
    if (!current_value(&v)) return;
    calc_format(v, text, sizeof text);
    kapp_clipboard_copy(C.app, text, strlen(text));
    flash("Copied %s", text);
    trace("calc: copied %s", text);
}

static void paste(void) {
    size_t n;
    const char *clip = kapp_clipboard(C.app, &n);
    if (!clip || !n) { flash("The clipboard is empty"); return; }
    char clean[EXPR_MAX];
    size_t m = calc_sanitize(clip, n, clean, sizeof clean);
    if (!m) { flash("Nothing to paste"); return; }
    expr_insert(clean);
    trace("calc: pasted %s", clean);
}

static void mem_store(void) {
    cnum v;
    if (!current_value(&v)) { flash("Nothing to store yet"); return; }
    C.mem = v;
    C.has_mem = true;
    flash("Memory set");
}

static void mem_add(bool plus) {
    cnum v;
    if (!current_value(&v)) { flash("Nothing to add yet"); return; }
    C.mem = plus ? C.mem + v : C.mem - v;
    C.has_mem = true;
    flash("Memory %s", plus ? "added to" : "subtracted from");
}

static void mem_recall(void) {
    if (!C.has_mem) { flash("Memory is empty"); return; }
    char text[64], wrapped[72];
    calc_format(C.mem, text, sizeof text);
    if (text[0] == '-') { snprintf(wrapped, sizeof wrapped, "(%s)", text); expr_insert(wrapped); }
    else expr_insert(text);
}

static void set_degrees(bool deg) { C.ctx.degrees = deg; }

static void set_mode(bool sci, KApp *app) {
    C.scientific = sci;
    kapp_set_minsize(app, sci ? 560 : 420, 350);
}

static void history_nav(int dir) {
    if (C.nhist == 0) return;
    int i = C.hist_nav;
    if (dir < 0) i = i < 0 ? C.nhist - 1 : i - 1;
    else i = i < 0 ? -1 : i + 1;
    if (i >= C.nhist) i = -1;
    if (i < -1) i = 0;
    if (i < 0) { clear_entry(); return; }
    expr_set(C.hist[i].expr);
    C.hist_nav = i;
}

/* ---------------------------------------------------------------- keypad */

typedef enum {
    K_TEXT, K_EQ, K_CLR, K_BACK, K_NEG, K_RECIP, K_MC, K_MR, K_MPLUS, K_MMINUS, K_MS, K_DEG, K_RAD
} KeyKind;

typedef enum { S_DIGIT, S_OP, S_FUNC, S_MEM, S_CLEAR, S_EQUALS } KeyStyle;

typedef struct Key {
    const char *label;
    KeyKind kind;
    const char *text;
    KeyStyle style;
} Key;

#define KEY_COLS 5
#define KEY_ROWS 6
static const Key g_main_keys[KEY_ROWS * KEY_COLS] = {
    { "MC", K_MC, NULL, S_MEM }, { "MR", K_MR, NULL, S_MEM }, { "M+", K_MPLUS, NULL, S_MEM }, { "M-", K_MMINUS, NULL, S_MEM }, { "MS", K_MS, NULL, S_MEM },
    { "C", K_CLR, NULL, S_CLEAR }, { "(", K_TEXT, "(", S_OP }, { ")", K_TEXT, ")", S_OP }, { "%", K_TEXT, "%", S_OP }, { "\xE2\x86\x90", K_BACK, NULL, S_CLEAR },
    { "7", K_TEXT, "7", S_DIGIT }, { "8", K_TEXT, "8", S_DIGIT }, { "9", K_TEXT, "9", S_DIGIT }, { "\xC3\xB7", K_TEXT, "/", S_OP }, { "sqrt", K_TEXT, "sqrt(", S_FUNC },
    { "4", K_TEXT, "4", S_DIGIT }, { "5", K_TEXT, "5", S_DIGIT }, { "6", K_TEXT, "6", S_DIGIT }, { "\xC3\x97", K_TEXT, "*", S_OP }, { "x\xC2\xB2", K_TEXT, "\xC2\xB2", S_FUNC },
    { "1", K_TEXT, "1", S_DIGIT }, { "2", K_TEXT, "2", S_DIGIT }, { "3", K_TEXT, "3", S_DIGIT }, { "\xE2\x88\x92", K_TEXT, "-", S_OP }, { "1/x", K_RECIP, NULL, S_FUNC },
    { "\xC2\xB1", K_NEG, NULL, S_OP }, { "0", K_TEXT, "0", S_DIGIT }, { ".", K_TEXT, ".", S_DIGIT }, { "+", K_TEXT, "+", S_OP }, { "=", K_EQ, NULL, S_EQUALS },
};

#define SCI_COLS 3
static const Key g_sci_keys[KEY_ROWS * SCI_COLS] = {
    { "Deg", K_DEG, NULL, S_MEM }, { "Rad", K_RAD, NULL, S_MEM }, { "mod", K_TEXT, " mod ", S_OP },
    { "sin", K_TEXT, "sin(", S_FUNC }, { "cos", K_TEXT, "cos(", S_FUNC }, { "tan", K_TEXT, "tan(", S_FUNC },
    { "asin", K_TEXT, "asin(", S_FUNC }, { "acos", K_TEXT, "acos(", S_FUNC }, { "atan", K_TEXT, "atan(", S_FUNC },
    { "ln", K_TEXT, "ln(", S_FUNC }, { "log", K_TEXT, "log(", S_FUNC }, { "exp", K_TEXT, "exp(", S_FUNC },
    { "x^y", K_TEXT, "^", S_OP }, { "cbrt", K_TEXT, "cbrt(", S_FUNC }, { "n!", K_TEXT, "!", S_OP },
    { "pi", K_TEXT, "pi", S_DIGIT }, { "e", K_TEXT, "e", S_DIGIT }, { "ans", K_TEXT, "ans", S_DIGIT },
};

static void run_key(const Key *k) {
    switch (k->kind) {
    case K_TEXT: expr_insert(k->text); break;
    case K_EQ: commit(); break;
    case K_CLR: clear_entry(); break;
    case K_BACK: expr_backspace(); break;
    case K_NEG: negate(); break;
    case K_RECIP: wrap_with("1/(", ")"); break;
    case K_MC: C.mem = 0; C.has_mem = false; flash("Memory cleared"); break;
    case K_MR: mem_recall(); break;
    case K_MPLUS: mem_add(true); break;
    case K_MMINUS: mem_add(false); break;
    case K_MS: mem_store(); break;
    case K_DEG: set_degrees(true); break;
    case K_RAD: set_degrees(false); break;
    }
}

static uint32_t style_color(KeyStyle s) {
    switch (s) {
    case S_DIGIT: return 0xF7F3E6;
    case S_OP: return 0xD8D1BB;
    case S_FUNC: return 0xCDD3E4;
    case S_MEM: return 0xE0DAC6;
    case S_CLEAR: return 0xE6BDB4;
    case S_EQUALS: return 0xC8102E;
    }
    return 0xE4DECB;
}

static bool key_button(KUI *ui, KRect r, const Key *k, bool selected) {
    uint32_t id = k_id(k->label, r);
    bool hover = k_hover(ui, r);
    if (hover) ui->hot = id;
    if (hover && ui->pressed[0]) ui->active = id;
    bool clicked = false;
    if (ui->active == id && ui->released[0]) { clicked = hover; ui->active = 0; }
    bool down = ui->active == id && (ui->buttons & 1) && hover;
    uint32_t bg = style_color(k->style);
    if (selected) bg = 0xB9C6E8;
    if (down) bg = kc_shade(bg, 90);
    else if (hover) bg = kc_shade(bg, 104);
    KCanvas *cv = &ui->cv;
    kc_panel(cv, r, down || selected ? KB_SUNKEN : KB_RAISED, bg);
    int off = down ? 1 : 0;
    uint32_t fg = k->style == S_EQUALS ? 0xFFFFFF : KC_INK;
    if (k->style == S_MEM) fg = 0x3A4470;
    KRect t = kr(r.x + off, r.y + off, r.w, r.h);
    int cells = (int)kc_utf8_len(k->label);
    int tw = cells * 8, tx = t.x + (t.w - tw) / 2, ty = t.y + (t.h - 16) / 2;
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, kr_inset(r, 2)));
    if (k->style == S_DIGIT || k->style == S_EQUALS || k->style == S_OP) kc_text_bold(cv, tx, ty, k->label, fg);
    else kc_text(cv, tx, ty, k->label, fg);
    cv->clip = old;
    return clicked;
}

/* ---------------------------------------------------------------- display */

static void draw_flat_tag(KUI *ui, KRect r, const char *label, bool on, bool *clicked) {
    bool hover = k_hover(ui, r);
    uint32_t id = k_id(label, r);
    if (hover && ui->pressed[0]) ui->active = id;
    if (ui->active == id && ui->released[0]) { *clicked = hover; ui->active = 0; }
    kc_fill(&ui->cv, r, on ? COL_LCD_TAG : (hover ? 0x2A356E : 0x1D2754));
    kc_text_in(&ui->cv, r, label, on ? 0x141B42 : COL_LCD_TEXT, KA_CENTER);
}

static void draw_display(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_panel(cv, r, KB_SUNKEN, COL_LCD);
    KRect in = kr_inset(r, 4);
    kc_fill(cv, kr(in.x, in.y, in.w, 1), 0x0A0F2A);
    uint64_t now = ui->now_ms;

    /* indicator row */
    int ty = in.y + 3;
    bool c1 = false, c2 = false, c3 = false;
    int tx = in.x + 4;
    draw_flat_tag(ui, kr(tx, ty, 32, 16), C.ctx.degrees ? "DEG" : "RAD", true, &c1);
    if (c1) set_degrees(!C.ctx.degrees);
    tx += 38;
    if (C.has_mem) {
        kc_text_bold(cv, tx, ty, "M", COL_LCD_TAG);
        tx += 16;
    }
    int rx = in.x + in.w - 4;
    rx -= 32; draw_flat_tag(ui, kr(rx, ty, 32, 16), "SCI", C.scientific, &c3);
    rx -= 36; draw_flat_tag(ui, kr(rx, ty, 32, 16), "STD", !C.scientific, &c2);
    if (c2) set_mode(false, C.app);
    if (c3) set_mode(true, C.app);
    if (C.status[0] && now < C.status_until) {
        int w = (int)kc_utf8_len(C.status) * 8;
        int sx = tx + 8;
        if (sx + w < rx - 8) kc_text(cv, rx - 12 - w, ty, C.status, COL_LCD_TAG);
        ui->redraw_soon = true;
    }

    /* the expression line: a small editor with horizontal scrolling */
    KRect line = kr(in.x + 4, in.y + 22, in.w - 8, 18);
    int cols = line.w / 8;
    int ncp = (int)kc_utf8_len(C.expr);
    int ccp = cp_index(C.expr, C.caret);
    if (ccp < C.scroll_cp) C.scroll_cp = ccp;
    if (ccp >= C.scroll_cp + cols) C.scroll_cp = ccp - cols + 1;
    if (C.scroll_cp > 0 && ncp - C.scroll_cp < cols - 1) C.scroll_cp = ncp - cols + 1 > 0 ? ncp - cols + 1 : 0;
    if (C.scroll_cp < 0) C.scroll_cp = 0;
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, line));
    size_t off = kc_utf8_offset(C.expr, (size_t)C.scroll_cp);
    kc_text(cv, line.x, line.y, C.expr + off, COL_LCD_TEXT);
    if (C.has_error && C.err.pos >= 0) {
        int a = cp_index(C.expr, C.err.pos) - C.scroll_cp;
        int b = cp_index(C.expr, C.err.pos + (C.err.len > 0 ? C.err.len : 1)) - C.scroll_cp;
        if (b <= a) b = a + 1;
        for (int x = a * 8; x < b * 8; x += 2) {                       /* a zig-zag underline under the offending piece */
            kc_pixel(cv, line.x + x, line.y + 16 + ((x / 2) & 1), COL_LCD_ERR);
            kc_pixel(cv, line.x + x + 1, line.y + 16 + ((x / 2) & 1), COL_LCD_ERR);
        }
    }
    if (ui->has_focus && ((now - C.caret_ms) / 500) % 2 == 0) kc_vline(cv, line.x + (ccp - C.scroll_cp) * 8, line.y, 16, COL_LCD_BIG);
    cv->clip = old;
    if (k_hover(ui, line)) {
        ui->cursor_shape = FC_TEXT;
        if (ui->pressed[0]) {                                           /* click to move the caret */
            int col = (ui->mx - line.x + 4) / 8 + C.scroll_cp;
            caret_move((int)kc_utf8_offset(C.expr, (size_t)(col < 0 ? 0 : col)));
        }
    }

    /* the big number */
    KRect res = kr(in.x + 4, in.y + 44, in.w - 8, 34);
    const char *text = "0";
    uint32_t col = COL_LCD_DIM;
    char buf[64];
    if (C.has_error) {
        /* the error message, wrapped over two lines */
        const char *m = C.err.msg;
        int maxc = res.w / 8;
        int len = (int)strlen(m);
        int l1 = len <= maxc ? len : maxc;
        if (l1 < len) { int sp = l1; while (sp > 0 && m[sp] != ' ') sp--; if (sp > maxc / 2) l1 = sp; }
        char one[96];
        snprintf(one, sizeof one, "%.*s", l1, m);
        kc_text_bold(cv, res.x, res.y + 1, one, COL_LCD_ERR);
        if (l1 < len) {
            const char *rest = m + l1 + (m[l1] == ' ' ? 1 : 0);
            kc_text_in(cv, kr(res.x, res.y + 17, res.w, 16), rest, COL_LCD_ERR, KA_LEFT);
        }
        return;
    }
    if (C.evaluated) { text = C.value_text; col = COL_LCD_BIG; }
    else if (C.len > 0 && C.preview_ok) {
        snprintf(buf, sizeof buf, "%s", C.preview_text);
        text = buf;
        col = COL_LCD_DIM;
        if (!strcmp(C.preview_text, C.expr)) text = "";
    } else if (C.len == 0) { text = "0"; col = COL_LCD_DIM; }
    else text = "";
    int cells = (int)kc_utf8_len(text);
    int scale = cells * 16 <= res.w ? 2 : 1;
    int w = cells * 8 * scale;
    if (col == COL_LCD_BIG && scale == 1) kc_text_bold(cv, res.x + res.w - w - 1, res.y + 8, text, col);
    else {
        int ty = res.y + (scale == 2 ? 0 : 8);
        if (col == COL_LCD_BIG) kc_text_scaled(cv, res.x + res.w - w - 1, ty, text, col, scale);      /* a final answer is set in heavier type */
        kc_text_scaled(cv, res.x + res.w - w, ty, text, col, scale);
    }
}

/* ---------------------------------------------------------------- history panel */

static void draw_history(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_text_bold(cv, r.x + 2, r.y + 3, "History", ui->theme->accent_dark);
    KRect clear_btn = kr(r.x + r.w - 54, r.y, 54, 22);
    if (k_button(ui, clear_btn, "Clear")) { C.nhist = 0; C.hist_scroll = 0; }
    KRect list = kr(r.x, r.y + 26, r.w, r.h - 26);
    kc_panel(cv, list, KB_SUNKEN, ui->theme->field_bg);
    KRect in = kr_inset(list, 2);
    const int rowh = 38;
    bool bar = C.nhist * rowh > in.h;
    KRect body = bar ? kr(in.x, in.y, in.w - 14, in.h) : in;
    int total = C.nhist * rowh;
    if (C.hist_stick) { C.hist_scroll = total > body.h ? total - body.h : 0; C.hist_stick = false; }
    if (k_hover(ui, list) && ui->wheel) C.hist_scroll -= ui->wheel * rowh;
    int maxs = total > body.h ? total - body.h : 0;
    if (C.hist_scroll > maxs) C.hist_scroll = maxs;
    if (C.hist_scroll < 0) C.hist_scroll = 0;

    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, body));
    if (C.nhist == 0) {
        kc_text_in(cv, kr(body.x + 6, body.y + 8, body.w - 12, 16), "Nothing calculated yet.", ui->theme->text_dim, KA_LEFT);
        kc_text_in(cv, kr(body.x + 6, body.y + 26, body.w - 12, 16), "Results appear here.", ui->theme->text_dim, KA_LEFT);
    }
    int hover = -1;
    for (int i = 0; i < C.nhist; i++) {
        int y = body.y + i * rowh - C.hist_scroll;
        if (y + rowh < body.y || y > body.y + body.h) continue;
        KRect rr = kr(body.x, y, body.w, rowh);
        bool hv = k_hover(ui, rr) && k_hover(ui, body);
        if (hv) { hover = i; kc_fill(cv, rr, kc_mix(ui->theme->field_bg, ui->theme->accent_light, 55)); }
        kc_hline(cv, rr.x + 4, rr.y + rowh - 1, rr.w - 8, kc_shade(ui->theme->field_bg, 90));
        kc_text_in(cv, kr(rr.x + 6, rr.y + 3, rr.w - 12, 16), C.hist[i].expr, ui->theme->text_dim, KA_LEFT);
        char line[64];
        snprintf(line, sizeof line, "= %s", C.hist[i].result);
        KRect rl = kr(rr.x + 6, rr.y + 19, rr.w - 12, 16);
        kc_set_clip(cv, kr_intersect(kr_intersect(old, body), rl));
        kc_text_bold(cv, rl.x, rl.y, line, ui->theme->text);
        kc_set_clip(cv, kr_intersect(old, body));
        if (hv && ui->released[0]) { expr_set(C.hist[i].expr); }
        if (hv && ui->released[1]) { expr_insert(C.hist[i].result); }
    }
    cv->clip = old;
    C.hist_hover = hover;
    if (hover >= 0) k_tooltip_text(ui, "Click: reuse the expression - right click: use the result");
    if (bar) k_scrollbar(ui, kr(in.x + in.w - 14, in.y, 14, in.h), &C.hist_scroll, total, body.h);
}

/* ---------------------------------------------------------------- about */

static void about_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_gradient_v(cv, kr(0, 0, cv->w, 74), 0x0E1330, 0x2A3A78);
    k_icon(cv, KI_CALC, 18, 12, 48);
    kc_text_bold(cv, 80, 14, "Ran's Calculator", 0xFFF6E0);
    kc_text(cv, 80, 38, "Standard and scientific calculator", 0xB9C2F0);
    kc_text(cv, 80, 54, "for HakureiDE, version 1.0", 0xB9C2F0);
    char ans[64] = "42";
    if (C.has_ans) calc_format(C.ctx.ans, ans, sizeof ans);
    char line[160];
    snprintf(line, sizeof line, "Ran says the answer is %s.", ans);
    kc_text_bold(cv, 20, 90, line, ui->theme->accent_dark);
    const char *quip;
    if (!C.has_ans) quip = "She would like to see the question first.";
    else if (C.ctx.ans == 0) quip = "Chen insists that zero is a perfectly good number.";
    else if (C.ctx.ans < 0) quip = "Yukari borrowed the other side of the number line.";
    else if (C.ctx.ans == 42) quip = "Everyone nods. Nobody asks which question.";
    else quip = "She double-checked it with nine tails.";
    kc_text(cv, 20, 110, quip, ui->theme->text);
    kc_text(cv, 20, 140, "Arithmetic is done in extended precision and", ui->theme->text_dim);
    kc_text(cv, 20, 156, "shown with up to 15 significant digits.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 38, 84, 26), "OK") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) {
        kwin_close(C.about);
        C.about = NULL;
    }
}

static void about_closed(KWin *w, void *user) { (void)w; (void)user; C.about = NULL; }

static void open_about(KApp *app) {
    if (C.about) return;
    C.about = kwin_new(app, -1, -1, 400, 214, "About Calculator", FW_DIALOG | FW_CENTER, about_draw, NULL);
    if (C.about) kwin_set_close_handler(C.about, about_closed, NULL);
}

/* ---------------------------------------------------------------- keyboard */

static bool g_quit_requested;

static void handle_keys(KUI *ui) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool ctrl = k->mods & MOD_CTRL, alt = k->mods & MOD_ALT, shift = k->mods & MOD_SHIFT;
        bool used = true;
        if (ctrl && !alt) {
            switch (k->code) {
            case KEY_C: copy_result(); break;
            case KEY_V: paste(); break;
            case KEY_X: if (C.len) { kapp_clipboard_copy(C.app, C.expr, (size_t)C.len); clear_entry(); flash("Cut the expression"); } break;
            case KEY_L: run_key(&g_main_keys[0]); break;
            case KEY_R: run_key(&g_main_keys[1]); break;
            case KEY_P: mem_add(!shift); break;
            case KEY_M: mem_store(); break;
            case KEY_Q: case KEY_W: g_quit_requested = true; break;
            case KEY_A: caret_move(C.len); break;
            default: used = false; break;
            }
        } else if (alt && !ctrl) {
            if (k->code == KEY_1) set_mode(false, C.app);
            else if (k->code == KEY_2) set_mode(true, C.app);
            else used = false;
        } else {
            switch (k->code) {
            case KEY_ENTER: case KEY_KPENTER: commit(); break;
            case KEY_BACKSPACE: expr_backspace(); break;
            case KEY_DELETE: expr_delete(); break;
            case KEY_LEFT: caret_move((int)u8_prev(C.expr, (size_t)C.caret)); break;
            case KEY_RIGHT: caret_move((int)u8_next(C.expr, (size_t)C.caret)); break;
            case KEY_HOME: caret_move(0); break;
            case KEY_END: caret_move(C.len); break;
            case KEY_ESC: clear_entry(); break;
            case KEY_UP: history_nav(-1); break;
            case KEY_DOWN: history_nav(1); break;
            case KEY_PAGEUP: C.hist_scroll -= 76; break;
            case KEY_PAGEDOWN: C.hist_scroll += 76; break;
            case KEY_F3: set_degrees(true); break;
            case KEY_F4: set_degrees(false); break;
            default:
                if (k->ch == '=') commit();
                else if (k->ch >= 32 && k->ch < 127) { char s[2] = { (char)k->ch, 0 }; expr_insert(s); }
                else used = false;
                break;
            }
        }
        if (used) { k->down = 0; k->code = 0; }
    }
}

/* ---------------------------------------------------------------- the window */

static void draw_menus(KApp *app, KUI *ui) {
    static char l_std[40], l_sci[40], l_deg[40], l_rad[40], l_hist[40];
    const char *on = "\xE2\x9C\x93 ", *off = "  ";
    snprintf(l_std, sizeof l_std, "%sStandard", C.scientific ? off : on);
    snprintf(l_sci, sizeof l_sci, "%sScientific", C.scientific ? on : off);
    snprintf(l_deg, sizeof l_deg, "%sDegrees", C.ctx.degrees ? on : off);
    snprintf(l_rad, sizeof l_rad, "%sRadians", C.ctx.degrees ? off : on);
    snprintf(l_hist, sizeof l_hist, "%sHistory tape", C.show_history ? on : off);
    const KMenuItem file[] = { { "Quit", 0, false, false, "Ctrl+Q" } };
    const KMenuItem edit[] = {
        { "Copy result", 0, false, false, "Ctrl+C" }, { "Paste expression", 0, false, false, "Ctrl+V" }, { NULL, 0, true, false, NULL },
        { "Clear entry", 0, false, false, "Esc" }, { "Clear history", 0, false, false, NULL },
    };
    const KMenuItem view[] = {
        { l_std, 0, false, false, "Alt+1" }, { l_sci, 0, false, false, "Alt+2" }, { NULL, 0, true, false, NULL },
        { l_deg, 0, false, false, "F3" }, { l_rad, 0, false, false, "F4" }, { NULL, 0, true, false, NULL },
        { l_hist, 0, false, false, NULL },
    };
    const KMenuItem help[] = { { "About Ran's Calculator", 0, false, false, NULL } };
    static const char *const titles[] = { "File", "Edit", "View", "Help" };
    const KMenuItem *const menus[] = { file, edit, view, help };
    const int counts[] = { 1, 5, 7, 1 };
    int sel = k_menubar(ui, kr(0, 0, ui->cv.w, MENU_H), titles, menus, counts, 4);
    if (!sel) return;
    int m = sel >> 8, it = (sel & 255) - 1;
    if (m == 1) g_quit_requested = true;
    else if (m == 2) {
        if (it == 0) copy_result();
        else if (it == 1) paste();
        else if (it == 3) clear_entry();
        else if (it == 4) { C.nhist = 0; C.hist_scroll = 0; }
    } else if (m == 3) {
        if (it == 0) set_mode(false, app);
        else if (it == 1) set_mode(true, app);
        else if (it == 3) set_degrees(true);
        else if (it == 4) set_degrees(false);
        else if (it == 6) C.show_history = !C.show_history;
    } else if (m == 4) open_about(app);
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    int W = cv->w, H = cv->h;

    if (ui->menu_open == 0) handle_keys(ui);
    if (g_quit_requested) { kapp_quit(app, 0); return; }

    refresh_preview();                                              /* live preview of the expression being typed */

    /* geometry */
    const int pad = 8, gap = 4;
    int hist_w = 0;
    bool hist_visible = C.show_history && W >= 470;
    int cols = KEY_COLS + (C.scientific ? SCI_COLS : 0);
    int sep = C.scientific ? 10 : 0;                                  /* room between the function pad and the main pad */
    int min_pad_w = cols * 40 + (cols - 1) * gap + sep;
    if (hist_visible) {
        hist_w = W - pad * 3 - min_pad_w;
        if (hist_w > 260) hist_w = 260;
        if (hist_w < 150) { hist_visible = false; hist_w = 0; }
    }
    int left_w = W - pad * 2 - (hist_visible ? hist_w + pad : 0);
    int top = MENU_H + 6;
    int disp_h = 86;
    KRect disp = kr(pad, top, left_w, disp_h);
    int pad_y = top + disp_h + 8;
    int pad_h = H - pad_y - pad;
    int bw = (left_w - sep - (cols - 1) * gap) / cols;
    int bh = (pad_h - (KEY_ROWS - 1) * gap) / KEY_ROWS;
    if (bw > 84) bw = 84;
    if (bw < 24) bw = 24;
    if (bh > 52) bh = 52;
    if (bh < 20) bh = 20;

    draw_display(ui, disp);

    /* keypad */
    int x0 = pad;
    if (C.scientific) {
        for (int r = 0; r < KEY_ROWS; r++)
            for (int c = 0; c < SCI_COLS; c++) {
                const Key *k = &g_sci_keys[r * SCI_COLS + c];
                KRect kb = kr(x0 + c * (bw + gap), pad_y + r * (bh + gap), bw, bh);
                bool sel = (k->kind == K_DEG && C.ctx.degrees) || (k->kind == K_RAD && !C.ctx.degrees);
                if (key_button(ui, kb, k, sel)) run_key(k);
            }
        x0 += SCI_COLS * (bw + gap) - gap + sep;
        kc_vline(cv, x0 - sep / 2 - 1, pad_y, KEY_ROWS * (bh + gap) - gap, kc_shade(ui->theme->window_bg, 80));
        kc_vline(cv, x0 - sep / 2, pad_y, KEY_ROWS * (bh + gap) - gap, 0xFFFFFF);
    }
    for (int r = 0; r < KEY_ROWS; r++)
        for (int c = 0; c < KEY_COLS; c++) {
            const Key *k = &g_main_keys[r * KEY_COLS + c];
            KRect kb = kr(x0 + c * (bw + gap), pad_y + r * (bh + gap), bw, bh);
            if (key_button(ui, kb, k, false)) run_key(k);
        }

    if (hist_visible) draw_history(ui, kr(pad + left_w + pad, top - 2, hist_w, H - top - pad + 2));

    if (ui->has_focus) ui->redraw_soon = true;
    draw_menus(app, ui);
}

static bool on_close(KApp *app, void *user) { (void)app; (void)user; return true; }

int calc_gui(int argc, char **argv) {
    memset(&C, 0, sizeof C);
    C.ctx.degrees = true;
    C.show_history = true;
    C.hist_nav = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) C.trace = true;
        else if (!strcmp(argv[i], "--rad")) C.ctx.degrees = false;
        else if (!strcmp(argv[i], "--sci")) C.scientific = true;
    }
    KApp *app = kapp_new("Calculator", 680, 420, FW_RESIZABLE);
    if (!app) return 1;
    C.app = app;
    C.caret_ms = k_now_ms();
    kapp_set_draw(app, draw, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    set_mode(C.scientific, app);
    int rc = kapp_run(app);
    kapp_destroy(app);
    return rc;
}
