#include "fbcon.h"
#include "fb.h"
#include "kernel.h"
#include "font8x16.h"

#define MAX_COLS 256
#define MAX_ROWS 100

typedef struct {
    u32 cp;
    u32 fg;
    u32 bg;
} Cell;

static const u32 g_palette[16] = {
    0x2E3436, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x555753, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};

#define DEFAULT_FG 0xD3D7CF
#define DEFAULT_BG 0x0B0B14

static Cell g_cells[MAX_ROWS * MAX_COLS];
static int  g_cols, g_rows;
static int  g_cx, g_cy;
static int  g_saved_cx, g_saved_cy;
static u32  g_fg = DEFAULT_FG, g_bg = DEFAULT_BG;
static bool g_bold, g_reverse;
static bool g_enabled = true;
static bool g_cursor_visible;       /* cursor currently painted on screen */

/* escape sequence parser */
enum { ST_NORMAL, ST_ESC, ST_CSI };
static int  g_state;
static int  g_params[8];
static int  g_nparams;
static bool g_have_param;
static bool g_private;

/* utf-8 decoder */
static u32 g_utf_cp;
static int g_utf_left;

static inline Cell *cell(int col, int row) { return &g_cells[row * MAX_COLS + col]; }

static void draw_cell(int col, int row, bool inverse) {
    if (!g_enabled || !g_fb.ready) return;
    const Cell *c = cell(col, row);
    u32 fg = inverse ? c->bg : c->fg;
    u32 bg = inverse ? c->fg : c->bg;
    fb_draw_glyph(col * FONT_W, row * FONT_H, font8x16_glyph(c->cp), fg, bg);
}

static void cursor_hide(void) {
    if (g_cursor_visible) {
        draw_cell(g_cx, g_cy, false);
        g_cursor_visible = false;
    }
}

static void cursor_show(void) {
    if (g_enabled && g_fb.ready && g_cx < g_cols && g_cy < g_rows) {
        draw_cell(g_cx, g_cy, true);
        g_cursor_visible = true;
    }
}

static void clear_cells(int row, int col0, int col1) {
    for (int c = col0; c < col1; c++) {
        Cell *ce = cell(c, row);
        ce->cp = ' ';
        ce->fg = g_fg;
        ce->bg = g_bg;
    }
    if (g_enabled && g_fb.ready)
        fb_fill_rect(col0 * FONT_W, row * FONT_H, (col1 - col0) * FONT_W, FONT_H, g_bg);
}

static void scroll_up(void) {
    for (int r = 1; r < g_rows; r++)
        memcpy(cell(0, r - 1), cell(0, r), sizeof(Cell) * (size_t)g_cols);
    if (g_enabled && g_fb.ready)
        fb_copy_rect(0, FONT_H, 0, 0, g_cols * FONT_W, (g_rows - 1) * FONT_H);
    clear_cells(g_rows - 1, 0, g_cols);
}

static void newline(void) {
    g_cx = 0;
    if (++g_cy >= g_rows) {
        g_cy = g_rows - 1;
        scroll_up();
    }
}

static void put_glyph(u32 cp) {
    if (g_cx >= g_cols) newline();
    Cell *c = cell(g_cx, g_cy);
    c->cp = cp;
    c->fg = g_reverse ? g_bg : g_fg;
    c->bg = g_reverse ? g_fg : g_bg;
    draw_cell(g_cx, g_cy, false);
    g_cx++;
}

static void apply_sgr(void) {
    if (g_nparams == 0) { g_params[0] = 0; g_nparams = 1; }
    for (int i = 0; i < g_nparams; i++) {
        int p = g_params[i];
        if (p == 0) { g_fg = DEFAULT_FG; g_bg = DEFAULT_BG; g_bold = false; g_reverse = false; }
        else if (p == 1) { g_bold = true; }
        else if (p == 22) { g_bold = false; }
        else if (p == 7) { g_reverse = true; }
        else if (p == 27) { g_reverse = false; }
        else if (p >= 30 && p <= 37) { g_fg = g_palette[(p - 30) + (g_bold ? 8 : 0)]; }
        else if (p == 39) { g_fg = DEFAULT_FG; }
        else if (p >= 40 && p <= 47) { g_bg = g_palette[p - 40]; }
        else if (p == 49) { g_bg = DEFAULT_BG; }
        else if (p >= 90 && p <= 97) { g_fg = g_palette[(p - 90) + 8]; }
        else if (p >= 100 && p <= 107) { g_bg = g_palette[(p - 100) + 8]; }
        else if (p == 38 && i + 4 < g_nparams + 0 && g_params[i + 1] == 2) {
            g_fg = ((u32)g_params[i + 2] << 16) | ((u32)g_params[i + 3] << 8) | (u32)g_params[i + 4];
            i += 4;
        }
        else if (p == 48 && i + 4 < g_nparams + 0 && g_params[i + 1] == 2) {
            g_bg = ((u32)g_params[i + 2] << 16) | ((u32)g_params[i + 3] << 8) | (u32)g_params[i + 4];
            i += 4;
        }
    }
}

static int param(int idx, int def) {
    return (idx < g_nparams && g_params[idx] > 0) ? g_params[idx] : def;
}

static void do_csi(char final) {
    if (g_have_param && g_nparams < 8) g_nparams++;
    switch (final) {
    case 'm': apply_sgr(); break;
    case 'A': g_cy -= param(0, 1); if (g_cy < 0) g_cy = 0; break;
    case 'B': g_cy += param(0, 1); if (g_cy >= g_rows) g_cy = g_rows - 1; break;
    case 'C': g_cx += param(0, 1); if (g_cx >= g_cols) g_cx = g_cols - 1; break;
    case 'D': g_cx -= param(0, 1); if (g_cx < 0) g_cx = 0; break;
    case 'G': g_cx = param(0, 1) - 1; if (g_cx >= g_cols) g_cx = g_cols - 1; if (g_cx < 0) g_cx = 0; break;
    case 'H': case 'f':
        g_cy = param(0, 1) - 1;
        g_cx = param(1, 1) - 1;
        if (g_cy >= g_rows) g_cy = g_rows - 1;
        if (g_cx >= g_cols) g_cx = g_cols - 1;
        if (g_cy < 0) g_cy = 0;
        if (g_cx < 0) g_cx = 0;
        break;
    case 'J': {
        int mode = g_nparams ? g_params[0] : 0;
        if (mode == 2 || mode == 3) {
            for (int r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
            if (mode == 2) { g_cx = 0; g_cy = 0; }
        } else if (mode == 0) {
            clear_cells(g_cy, g_cx, g_cols);
            for (int r = g_cy + 1; r < g_rows; r++) clear_cells(r, 0, g_cols);
        } else if (mode == 1) {
            for (int r = 0; r < g_cy; r++) clear_cells(r, 0, g_cols);
            clear_cells(g_cy, 0, g_cx + 1);
        }
        break;
    }
    case 'K': {
        int mode = g_nparams ? g_params[0] : 0;
        if (mode == 0) clear_cells(g_cy, g_cx, g_cols);
        else if (mode == 1) clear_cells(g_cy, 0, g_cx + 1);
        else clear_cells(g_cy, 0, g_cols);
        break;
    }
    case 's': g_saved_cx = g_cx; g_saved_cy = g_cy; break;
    case 'u': g_cx = g_saved_cx; g_cy = g_saved_cy; break;
    default: break;
    }
}

static void putc_cp(u32 cp) {
    if (g_state == ST_ESC) {
        if (cp == '[') {
            g_state = ST_CSI;
            g_nparams = 0;
            g_have_param = false;
            g_private = false;
            memset(g_params, 0, sizeof g_params);
        } else {
            g_state = ST_NORMAL;
        }
        return;
    }
    if (g_state == ST_CSI) {
        if (cp >= '0' && cp <= '9') {
            if (g_nparams < 8) g_params[g_nparams] = g_params[g_nparams] * 10 + (int)(cp - '0');
            g_have_param = true;
        } else if (cp == ';') {
            if (g_nparams < 7) g_nparams++;
            g_have_param = false;
        } else if (cp == '?') {
            g_private = true;
        } else if (cp >= 0x40 && cp <= 0x7E) {
            if (!g_private) do_csi((char)cp);
            g_state = ST_NORMAL;
        }
        return;
    }

    switch (cp) {
    case 0x1B: g_state = ST_ESC; break;
    case '\n': newline(); break;
    case '\r': g_cx = 0; break;
    case '\b':
        if (g_cx > 0) g_cx--;
        break;
    case '\t': {
        int next = (g_cx + 8) & ~7;
        while (g_cx < next && g_cx < g_cols) put_glyph(' ');
        break;
    }
    case 7: break;
    default:
        if (cp >= 0x20) put_glyph(cp);
        break;
    }
}

void fbcon_init(void) {
    g_cols = g_fb.ready ? (int)(g_fb.width / FONT_W) : 80;
    g_rows = g_fb.ready ? (int)(g_fb.height / FONT_H) : 25;
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
    g_fg = DEFAULT_FG;
    g_bg = DEFAULT_BG;
    g_cx = g_cy = 0;
    g_state = ST_NORMAL;
    fb_fill_rect(0, 0, (int)g_fb.width, (int)g_fb.height, DEFAULT_BG);
    for (int r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
}

void fbcon_write(const char *s, size_t n) {
    if (g_cols == 0) return;
    cursor_hide();
    for (size_t i = 0; i < n; i++) {
        u8 b = (u8)s[i];
        if (g_utf_left) {
            if ((b & 0xC0) == 0x80) {
                g_utf_cp = (g_utf_cp << 6) | (b & 0x3F);
                if (--g_utf_left == 0) putc_cp(g_utf_cp);
                continue;
            }
            g_utf_left = 0;     /* malformed: fall through and treat b as a fresh byte */
        }
        if (b < 0x80) putc_cp(b);
        else if ((b & 0xE0) == 0xC0) { g_utf_cp = b & 0x1F; g_utf_left = 1; }
        else if ((b & 0xF0) == 0xE0) { g_utf_cp = b & 0x0F; g_utf_left = 2; }
        else if ((b & 0xF8) == 0xF0) { g_utf_cp = b & 0x07; g_utf_left = 3; }
        else putc_cp('?');
    }
    cursor_show();
}

void fbcon_clear(void) {
    cursor_hide();
    for (int r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
    g_cx = g_cy = 0;
    cursor_show();
}

void fbcon_set_enabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    g_cursor_visible = false;
    if (on) fbcon_redraw();
}

bool fbcon_enabled(void) { return g_enabled; }

void fbcon_redraw(void) {
    if (!g_enabled || !g_fb.ready) return;
    fb_fill_rect(0, 0, (int)g_fb.width, (int)g_fb.height, DEFAULT_BG);
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            draw_cell(c, r, false);
    g_cursor_visible = false;
    cursor_show();
}

void fbcon_set_fg(u32 rgb) { g_fg = rgb; }

void fbcon_reset_colors(void) {
    g_fg = DEFAULT_FG;
    g_bg = DEFAULT_BG;
    g_bold = g_reverse = false;
}

void fbcon_size(int *cols, int *rows) {
    *cols = g_cols;
    *rows = g_rows;
}
