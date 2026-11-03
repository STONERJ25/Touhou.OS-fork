#include "vt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_ESC_INTER, ST_DCS };

static const VtCell g_blank = { ' ', VT_DEFAULT, VT_DEFAULT, 0 };

static VtCell blank(const Vt *v) { VtCell c = { ' ', VT_DEFAULT, v->cur_bg, 0 }; return c; }
static VtCell *cell(Vt *v, int row, int col) { return &v->screen[row * v->cols + col]; }

void vt_mark_dirty(Vt *v, int row) { if (row >= 0 && row < v->rows) v->dirty[row] = true; v->gen++; }
void vt_mark_all_dirty(Vt *v) { v->all_dirty = true; v->gen++; }

static void clear_cells(Vt *v, VtCell *p, int n) {
    VtCell b = blank(v);
    for (int i = 0; i < n; i++) p[i] = b;
}

static void default_tabs(Vt *v) {
    memset(v->tabs, 0, sizeof v->tabs);
    for (int i = 8; i < 512; i += 8) v->tabs[i] = 1;
}

void vt_init(Vt *v, int cols, int rows, int scrollback) {
    memset(v, 0, sizeof *v);
    v->cols = cols;
    v->rows = rows;
    v->screen = malloc((size_t)cols * (size_t)rows * sizeof(VtCell));
    v->dirty = calloc((size_t)rows, sizeof(bool));
    v->sb_max = scrollback;
    v->sb = calloc((size_t)(scrollback > 0 ? scrollback : 1), sizeof(VtLine));
    vt_reset(v);
}

void vt_reset(Vt *v) {
    if (v->alt) { free(v->screen); v->screen = v->saved_main; v->saved_main = NULL; v->alt = false; }
    v->cur_fg = v->cur_bg = VT_DEFAULT;
    v->cur_attr = 0;
    for (int i = 0; i < v->cols * v->rows; i++) v->screen[i] = g_blank;
    v->cx = v->cy = 0;
    v->top = 0;
    v->bot = v->rows - 1;
    v->wrap_pending = false;
    v->autowrap = true;
    v->app_cursor = false;
    v->cursor_visible = true;
    v->bracketed_paste = false;
    v->insert_mode = false;
    v->app_keypad = false;
    v->mouse_mode = 0;
    v->mouse_sgr = false;
    v->state = ST_GROUND;
    v->utf8_left = 0;
    default_tabs(v);
    vt_mark_all_dirty(v);
}

void vt_free(Vt *v) {
    free(v->screen);
    free(v->saved_main);
    for (int i = 0; i < v->sb_max; i++) free(v->sb[i].cells);
    free(v->sb);
    free(v->dirty);
}

/* ---------------------------------------------------------------- scrollback */

static void sb_push(Vt *v, const VtCell *row) {
    if (v->sb_max <= 0 || v->alt) return;
    int idx = (v->sb_head + v->sb_count) % v->sb_max;
    if (v->sb_count == v->sb_max) {
        free(v->sb[v->sb_head].cells);
        idx = v->sb_head;
        v->sb_head = (v->sb_head + 1) % v->sb_max;
    } else v->sb_count++;
    int len = v->cols;
    while (len > 0 && row[len - 1].cp == ' ' && row[len - 1].bg == VT_DEFAULT && !(row[len - 1].attr & A_REVERSE)) len--;   /* trim trailing blanks */
    v->sb[idx].cells = malloc((size_t)(len ? len : 1) * sizeof(VtCell));
    memcpy(v->sb[idx].cells, row, (size_t)len * sizeof(VtCell));
    v->sb[idx].len = len;
}

int vt_scrollback_count(const Vt *v) { return v->sb_count; }

const VtCell *vt_cell(const Vt *v, int row, int col) { return &v->screen[row * v->cols + col]; }

const VtCell *vt_view_cell(const Vt *v, int offset, int row, int col) {
    int line = row - offset;                       /* negative: inside the scrollback */
    if (line >= 0) return line < v->rows ? &v->screen[line * v->cols + col] : &g_blank;
    int back = -line;                              /* 1 = the most recent scrollback line */
    if (back > v->sb_count) return &g_blank;
    const VtLine *l = &v->sb[(v->sb_head + v->sb_count - back) % v->sb_max];
    return col < l->len ? &l->cells[col] : &g_blank;
}

/* ---------------------------------------------------------------- screen operations */

static void scroll_up(Vt *v, int top, int bot, int n) {
    if (n > bot - top + 1) n = bot - top + 1;
    if (n <= 0) return;
    if (top == 0 && !v->alt) for (int i = 0; i < n; i++) sb_push(v, cell(v, i, 0));
    memmove(cell(v, top, 0), cell(v, top + n, 0), (size_t)(bot - top + 1 - n) * (size_t)v->cols * sizeof(VtCell));
    for (int r = bot - n + 1; r <= bot; r++) clear_cells(v, cell(v, r, 0), v->cols);
    for (int r = top; r <= bot; r++) v->dirty[r] = true;
    v->gen++;
}

static void scroll_down(Vt *v, int top, int bot, int n) {
    if (n > bot - top + 1) n = bot - top + 1;
    if (n <= 0) return;
    memmove(cell(v, top + n, 0), cell(v, top, 0), (size_t)(bot - top + 1 - n) * (size_t)v->cols * sizeof(VtCell));
    for (int r = top; r < top + n; r++) clear_cells(v, cell(v, r, 0), v->cols);
    for (int r = top; r <= bot; r++) v->dirty[r] = true;
    v->gen++;
}

static void linefeed(Vt *v) {
    if (v->cy == v->bot) scroll_up(v, v->top, v->bot, 1);
    else if (v->cy < v->rows - 1) v->cy++;
    v->wrap_pending = false;
}

static void put_char(Vt *v, uint32_t cp) {
    if (v->wrap_pending && v->autowrap) {
        v->cx = 0;
        linefeed(v);
    }
    v->wrap_pending = false;
    if (v->insert_mode) {
        memmove(cell(v, v->cy, v->cx + 1), cell(v, v->cy, v->cx), (size_t)(v->cols - v->cx - 1) * sizeof(VtCell));
    }
    VtCell *c = cell(v, v->cy, v->cx);
    c->cp = cp;
    c->fg = v->cur_fg;
    c->bg = v->cur_bg;
    c->attr = v->cur_attr;
    v->dirty[v->cy] = true;
    v->gen++;
    v->last_cp = cp;
    if (v->cx + 1 >= v->cols) v->wrap_pending = true;
    else v->cx++;
}

static void erase_range(Vt *v, int row, int c0, int c1) {          /* [c0, c1) */
    if (c0 < 0) c0 = 0;
    if (c1 > v->cols) c1 = v->cols;
    if (c0 >= c1) return;
    clear_cells(v, cell(v, row, c0), c1 - c0);
    v->dirty[row] = true;
    v->gen++;
}

static int param(Vt *v, int i, int def) {
    if (i >= v->nparams || v->params[i] <= 0) return def;
    return v->params[i];
}

static void reply(Vt *v, const char *s) { if (v->reply) v->reply(v->user, s, strlen(s)); }

static void set_alt(Vt *v, bool on, bool save_cursor) {
    if (on == v->alt) return;
    if (on) {
        if (save_cursor) { v->saved_cx = v->cx; v->saved_cy = v->cy; }
        v->saved_main = v->screen;
        v->screen = malloc((size_t)v->cols * (size_t)v->rows * sizeof(VtCell));
        VtCell b = { ' ', VT_DEFAULT, VT_DEFAULT, 0 };
        for (int i = 0; i < v->cols * v->rows; i++) v->screen[i] = b;
        v->alt = true;
    } else {
        free(v->screen);
        v->screen = v->saved_main;
        v->saved_main = NULL;
        v->alt = false;
        if (save_cursor) { v->cx = v->saved_cx; v->cy = v->saved_cy; }
    }
    vt_mark_all_dirty(v);
}

/* ---------------------------------------------------------------- colours */

static uint32_t xterm256(int n) {
    static const uint32_t ansi[16] = {
        0x1B1D2B, 0xE5545C, 0x7BC86C, 0xE8C75A, 0x6E9BF0, 0xC58AE0, 0x5FCFD0, 0xD7D2C2,
        0x4A4E69, 0xFF7B82, 0xA3E88F, 0xFFE07F, 0x93B7FF, 0xE0AEFF, 0x8CEBEB, 0xFFFBEB,
    };
    if (n < 16) return ansi[n];
    if (n < 232) {
        n -= 16;
        int r = n / 36, g = (n / 6) % 6, b = n % 6;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return (uint32_t)(lv[r] << 16 | lv[g] << 8 | lv[b]);
    }
    int g = 8 + (n - 232) * 10;
    return (uint32_t)(g << 16 | g << 8 | g);
}

uint32_t vt_palette(int n) { return xterm256(n & 255); }

static void sgr(Vt *v) {
    if (v->nparams == 0) { v->cur_fg = v->cur_bg = VT_DEFAULT; v->cur_attr = 0; return; }
    for (int i = 0; i < v->nparams; i++) {
        int p = v->params[i];
        if (p < 0) p = 0;
        if (p == 0) { v->cur_fg = v->cur_bg = VT_DEFAULT; v->cur_attr = 0; }
        else if (p == 1) v->cur_attr |= A_BOLD;
        else if (p == 2) v->cur_attr |= A_DIM;
        else if (p == 3) v->cur_attr |= A_ITALIC;
        else if (p == 4) v->cur_attr |= A_UNDERLINE;
        else if (p == 5 || p == 6) v->cur_attr |= A_BLINK;
        else if (p == 7) v->cur_attr |= A_REVERSE;
        else if (p == 8) v->cur_attr |= A_INVISIBLE;
        else if (p == 9) v->cur_attr |= A_STRIKE;
        else if (p == 22) v->cur_attr &= (uint8_t)~(A_BOLD | A_DIM);
        else if (p == 23) v->cur_attr &= (uint8_t)~A_ITALIC;
        else if (p == 24) v->cur_attr &= (uint8_t)~A_UNDERLINE;
        else if (p == 25) v->cur_attr &= (uint8_t)~A_BLINK;
        else if (p == 27) v->cur_attr &= (uint8_t)~A_REVERSE;
        else if (p == 28) v->cur_attr &= (uint8_t)~A_INVISIBLE;
        else if (p == 29) v->cur_attr &= (uint8_t)~A_STRIKE;
        else if (p >= 30 && p <= 37) v->cur_fg = xterm256(p - 30);
        else if (p == 39) v->cur_fg = VT_DEFAULT;
        else if (p >= 40 && p <= 47) v->cur_bg = xterm256(p - 40);
        else if (p == 49) v->cur_bg = VT_DEFAULT;
        else if (p >= 90 && p <= 97) v->cur_fg = xterm256(p - 90 + 8);
        else if (p >= 100 && p <= 107) v->cur_bg = xterm256(p - 100 + 8);
        else if (p == 38 || p == 48) {
            uint32_t col = VT_DEFAULT;
            if (i + 2 < v->nparams && v->params[i + 1] == 5) { col = xterm256(v->params[i + 2]); i += 2; }
            else if (i + 4 < v->nparams && v->params[i + 1] == 2) {
                col = (uint32_t)((v->params[i + 2] & 255) << 16 | (v->params[i + 3] & 255) << 8 | (v->params[i + 4] & 255));
                i += 4;
            }
            if (p == 38) v->cur_fg = col; else v->cur_bg = col;
        }
    }
}

/* ---------------------------------------------------------------- CSI */

static void set_mode(Vt *v, int mode, bool on) {
    if (v->priv) {
        switch (mode) {
        case 1: v->app_cursor = on; break;
        case 7: v->autowrap = on; break;
        case 25: v->cursor_visible = on; vt_mark_dirty(v, v->cy); break;
        case 47: case 1047: set_alt(v, on, false); break;
        case 1049: set_alt(v, on, true); break;
        case 2004: v->bracketed_paste = on; break;
        case 1000: case 1002: case 1003: v->mouse_mode = on ? mode : 0; break;
        case 1006: v->mouse_sgr = on; break;
        default: break;
        }
    } else if (mode == 4) v->insert_mode = on;
}

static void csi(Vt *v, char final) {
    int n;
    switch (final) {
    case 'A': v->cy -= param(v, 0, 1); if (v->cy < 0) v->cy = 0; v->wrap_pending = false; break;
    case 'B': case 'e': v->cy += param(v, 0, 1); if (v->cy >= v->rows) v->cy = v->rows - 1; v->wrap_pending = false; break;
    case 'C': case 'a': v->cx += param(v, 0, 1); if (v->cx >= v->cols) v->cx = v->cols - 1; v->wrap_pending = false; break;
    case 'D': v->cx -= param(v, 0, 1); if (v->cx < 0) v->cx = 0; v->wrap_pending = false; break;
    case 'E': v->cy += param(v, 0, 1); if (v->cy >= v->rows) v->cy = v->rows - 1; v->cx = 0; v->wrap_pending = false; break;
    case 'F': v->cy -= param(v, 0, 1); if (v->cy < 0) v->cy = 0; v->cx = 0; v->wrap_pending = false; break;
    case 'G': case '`': v->cx = param(v, 0, 1) - 1; if (v->cx >= v->cols) v->cx = v->cols - 1; if (v->cx < 0) v->cx = 0; v->wrap_pending = false; break;
    case 'H': case 'f':
        v->cy = param(v, 0, 1) - 1;
        v->cx = param(v, 1, 1) - 1;
        if (v->cy >= v->rows) v->cy = v->rows - 1;
        if (v->cx >= v->cols) v->cx = v->cols - 1;
        if (v->cy < 0) v->cy = 0;
        if (v->cx < 0) v->cx = 0;
        v->wrap_pending = false;
        break;
    case 'd': v->cy = param(v, 0, 1) - 1; if (v->cy >= v->rows) v->cy = v->rows - 1; if (v->cy < 0) v->cy = 0; v->wrap_pending = false; break;
    case 'J':
        n = v->nparams ? v->params[0] : 0;
        if (n == 0) { erase_range(v, v->cy, v->cx, v->cols); for (int r = v->cy + 1; r < v->rows; r++) erase_range(v, r, 0, v->cols); }
        else if (n == 1) { for (int r = 0; r < v->cy; r++) erase_range(v, r, 0, v->cols); erase_range(v, v->cy, 0, v->cx + 1); }
        else { for (int r = 0; r < v->rows; r++) erase_range(v, r, 0, v->cols); }
        break;
    case 'K':
        n = v->nparams ? v->params[0] : 0;
        if (n == 0) erase_range(v, v->cy, v->cx, v->cols);
        else if (n == 1) erase_range(v, v->cy, 0, v->cx + 1);
        else erase_range(v, v->cy, 0, v->cols);
        break;
    case 'L':
        if (v->cy >= v->top && v->cy <= v->bot) scroll_down(v, v->cy, v->bot, param(v, 0, 1));
        v->cx = 0;
        break;
    case 'M':
        if (v->cy >= v->top && v->cy <= v->bot) scroll_up(v, v->cy, v->bot, param(v, 0, 1));
        v->cx = 0;
        break;
    case '@': {
        int k = param(v, 0, 1);
        if (k > v->cols - v->cx) k = v->cols - v->cx;
        memmove(cell(v, v->cy, v->cx + k), cell(v, v->cy, v->cx), (size_t)(v->cols - v->cx - k) * sizeof(VtCell));
        erase_range(v, v->cy, v->cx, v->cx + k);
        break;
    }
    case 'P': {
        int k = param(v, 0, 1);
        if (k > v->cols - v->cx) k = v->cols - v->cx;
        memmove(cell(v, v->cy, v->cx), cell(v, v->cy, v->cx + k), (size_t)(v->cols - v->cx - k) * sizeof(VtCell));
        erase_range(v, v->cy, v->cols - k, v->cols);
        v->dirty[v->cy] = true;
        break;
    }
    case 'X': erase_range(v, v->cy, v->cx, v->cx + param(v, 0, 1)); break;
    case 'S': scroll_up(v, v->top, v->bot, param(v, 0, 1)); break;
    case 'T': scroll_down(v, v->top, v->bot, param(v, 0, 1)); break;
    case 'b': for (int k = param(v, 0, 1); k > 0 && k < 1000; k--) put_char(v, v->last_cp); break;
    case 'm': sgr(v); break;
    case 'r':
        v->top = param(v, 0, 1) - 1;
        v->bot = param(v, 1, v->rows) - 1;
        if (v->top < 0) v->top = 0;
        if (v->bot >= v->rows) v->bot = v->rows - 1;
        if (v->top >= v->bot) { v->top = 0; v->bot = v->rows - 1; }
        v->cx = v->cy = 0;
        v->wrap_pending = false;
        break;
    case 's': v->saved_cx = v->cx; v->saved_cy = v->cy; v->saved_fg = v->cur_fg; v->saved_bg = v->cur_bg; v->saved_attr = v->cur_attr; break;
    case 'u': v->cx = v->saved_cx; v->cy = v->saved_cy; v->wrap_pending = false; break;
    case 'h': case 'l': for (int i = 0; i < (v->nparams ? v->nparams : 1); i++) set_mode(v, v->params[i], final == 'h'); break;
    case 'g':
        n = v->nparams ? v->params[0] : 0;
        if (n == 0) v->tabs[v->cx] = 0; else if (n == 3) memset(v->tabs, 0, sizeof v->tabs);
        break;
    case 'n':
        n = v->nparams ? v->params[0] : 0;
        if (n == 5) reply(v, "\033[0n");
        else if (n == 6) { char buf[32]; snprintf(buf, sizeof buf, "\033[%d;%dR", v->cy + 1, v->cx + 1); reply(v, buf); }
        break;
    case 'c':
        if (v->inter == '>') reply(v, "\033[>0;1;0c"); else reply(v, "\033[?62;1;6c");
        break;
    default: break;
    }
    v->dirty[v->cy] = true;
    v->gen++;
}

/* ---------------------------------------------------------------- the parser */

static void esc_final(Vt *v, uint8_t c) {
    switch (c) {
    case 'c': vt_reset(v); break;
    case 'D': linefeed(v); break;
    case 'E': v->cx = 0; linefeed(v); break;
    case 'M': if (v->cy == v->top) scroll_down(v, v->top, v->bot, 1); else if (v->cy > 0) v->cy--; v->wrap_pending = false; break;
    case 'H': v->tabs[v->cx] = 1; break;
    case '7': v->saved_cx = v->cx; v->saved_cy = v->cy; v->saved_fg = v->cur_fg; v->saved_bg = v->cur_bg; v->saved_attr = v->cur_attr; break;
    case '8': v->cx = v->saved_cx; v->cy = v->saved_cy; v->cur_fg = v->saved_fg; v->cur_bg = v->saved_bg; v->cur_attr = v->saved_attr; v->wrap_pending = false; break;
    case '=': v->app_keypad = true; break;
    case '>': v->app_keypad = false; break;
    default: break;
    }
    vt_mark_dirty(v, v->cy);
}

static void osc_done(Vt *v) {
    v->osc[v->osc_len] = 0;
    int code = atoi(v->osc);
    char *semi = strchr(v->osc, ';');
    if ((code == 0 || code == 1 || code == 2) && semi && v->set_title) v->set_title(v->user, semi + 1);
}

static void control(Vt *v, uint8_t c) {
    switch (c) {
    case 7: if (v->bell) v->bell(v->user); break;
    case 8: if (v->cx > 0) v->cx--; v->wrap_pending = false; break;
    case 9: {
        int x = v->cx + 1;
        while (x < v->cols - 1 && !v->tabs[x]) x++;
        v->cx = x >= v->cols ? v->cols - 1 : x;
        v->wrap_pending = false;
        break;
    }
    case 10: case 11: case 12: linefeed(v); break;
    case 13: v->cx = 0; v->wrap_pending = false; break;
    default: break;
    }
}

static void feed(Vt *v, uint8_t c) {
    switch (v->state) {
    case ST_GROUND:
        if (c == 0x1B) { v->state = ST_ESC; return; }
        if (c < 0x20 || c == 0x7F) { if (c != 0x7F) control(v, c); return; }
        if (c < 0x80) { put_char(v, c); return; }
        if (v->utf8_left == 0) {
            if ((c & 0xE0) == 0xC0) { v->utf8_cp = c & 0x1F; v->utf8_left = 1; }
            else if ((c & 0xF0) == 0xE0) { v->utf8_cp = c & 0x0F; v->utf8_left = 2; }
            else if ((c & 0xF8) == 0xF0) { v->utf8_cp = c & 0x07; v->utf8_left = 3; }
            else put_char(v, 0xFFFD);
        } else if ((c & 0xC0) == 0x80) {
            v->utf8_cp = (v->utf8_cp << 6) | (c & 0x3F);
            if (--v->utf8_left == 0) put_char(v, v->utf8_cp);
        } else { v->utf8_left = 0; put_char(v, 0xFFFD); }
        return;
    case ST_ESC:
        if (c == '[') { v->state = ST_CSI; v->nparams = 0; v->priv = false; v->has_param = false; v->inter = 0; memset(v->params, 0, sizeof v->params); return; }
        if (c == ']') { v->state = ST_OSC; v->osc_len = 0; return; }
        if (c == 'P' || c == '_' || c == '^' || c == 'X') { v->state = ST_DCS; return; }
        if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%' || c == ' ') { v->state = ST_ESC_INTER; return; }
        if (c < 0x20) { control(v, c); return; }
        v->state = ST_GROUND;
        esc_final(v, c);
        return;
    case ST_ESC_INTER:
        v->state = ST_GROUND;
        return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (v->nparams == 0) v->nparams = 1;
            int i = v->nparams - 1;
            if (!v->has_param) { v->params[i] = 0; v->has_param = true; }
            if (i < 16) v->params[i] = v->params[i] * 10 + (c - '0');
        } else if (c == ';') {
            if (v->nparams == 0) { v->nparams = 1; v->params[0] = 0; }
            if (v->nparams < 16) { v->params[v->nparams++] = 0; }
            v->has_param = true;
            v->params[v->nparams - 1] = 0;
        } else if (c == '?' || c == '>' || c == '<' || c == '=') {
            v->priv = c == '?';
            v->inter = (char)c;
        } else if (c >= 0x20 && c <= 0x2F) {
            v->inter = (char)c;
        } else if (c >= 0x40 && c <= 0x7E) {
            v->state = ST_GROUND;
            if (v->nparams > 0 && !v->has_param) v->nparams = 0;
            csi(v, (char)c);
        } else if (c < 0x20) control(v, c);
        else v->state = ST_GROUND;
        return;
    case ST_OSC:
        if (c == 7) { osc_done(v); v->state = ST_GROUND; }
        else if (c == 0x1B) v->state = ST_OSC_ESC;
        else if (v->osc_len < (int)sizeof v->osc - 1) v->osc[v->osc_len++] = (char)c;
        return;
    case ST_OSC_ESC:
        osc_done(v);
        v->state = c == '\\' ? ST_GROUND : ST_ESC;
        if (c != '\\') feed(v, c);
        return;
    case ST_DCS:
        if (c == 0x1B) v->state = ST_OSC_ESC;
        else if (c == 7) v->state = ST_GROUND;
        return;
    }
}

void vt_input(Vt *v, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) feed(v, data[i]);
}

void vt_resize(Vt *v, int cols, int rows) {
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    if (cols == v->cols && rows == v->rows) return;
    VtCell *nscreen = malloc((size_t)cols * (size_t)rows * sizeof(VtCell));
    for (int i = 0; i < cols * rows; i++) nscreen[i] = g_blank;
    /* keep the bottom of the old screen: if the new one is shorter, the lines that fall off go to the scrollback */
    int drop = v->cy + 1 > rows ? v->cy + 1 - rows : 0;
    for (int r = 0; r < drop; r++) sb_push(v, &v->screen[r * v->cols]);
    for (int r = 0; r < rows && r + drop < v->rows; r++) {
        int n = cols < v->cols ? cols : v->cols;
        memcpy(&nscreen[r * cols], &v->screen[(r + drop) * v->cols], (size_t)n * sizeof(VtCell));
    }
    VtCell *nsaved = NULL;
    if (v->alt && v->saved_main) {
        nsaved = malloc((size_t)cols * (size_t)rows * sizeof(VtCell));
        for (int i = 0; i < cols * rows; i++) nsaved[i] = g_blank;
        for (int r = 0; r < rows && r < v->rows; r++) memcpy(&nsaved[r * cols], &v->saved_main[r * v->cols], (size_t)(cols < v->cols ? cols : v->cols) * sizeof(VtCell));
        free(v->saved_main);
        v->saved_main = nsaved;
    }
    free(v->screen);
    v->screen = nscreen;
    free(v->dirty);
    v->dirty = calloc((size_t)rows, sizeof(bool));
    v->cols = cols;
    v->rows = rows;
    v->cy -= drop;
    if (v->cy < 0) v->cy = 0;
    if (v->cx >= cols) v->cx = cols - 1;
    v->top = 0;
    v->bot = rows - 1;
    v->wrap_pending = false;
    vt_mark_all_dirty(v);
}
