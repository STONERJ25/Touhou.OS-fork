/*
 * The text console on the framebuffer: a cell grid, UTF-8, ANSI SGR colours and basic cursor control.
 *
 * The grid (g_cells) is the truth; the screen is only a picture of it. What the screen currently shows is remembered cell by
 * cell (g_shown), and a flush at the end of every write draws exactly the cells whose picture is out of date: a text row is
 * rendered into a RAM buffer and then pushed to the framebuffer scanline by scanline in one sequential burst each.
 *
 * Why not the classic "scroll by copying pixels up"? On a PC the framebuffer is display memory mapped write-combining (see
 * fb.c). Writing it is fast; READING it is not: every load is its own slow bus round trip, and a full-screen scroll reads
 * 8 MB at 1080p. With the diff the boot log scrolls at the speed of the changed cells only, and nothing here ever reads the
 * screen. The same idea makes a screen full of output (`dmesg`) cost one repaint per write() instead of one per line.
 */
#include "fbcon.h"
#include "fb.h"
#include "kernel.h"
#include "mm.h"
#include "font8x16.h"

#define MAX_COLS 256
#define MAX_ROWS 100

typedef struct {
    u32 cp;
    u32 fg;
    u32 bg;
} Cell;

#define UNKNOWN_CP 0xFFFFFFFFu          /* a code point no cell ever holds: "what is on screen here is not known" forces a redraw */

static const u32 g_palette[16] = {
    0x2E3436, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3465A4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x555753, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};

#define DEFAULT_FG 0xD3D7CF
#define DEFAULT_BG 0x0B0B14

static Cell g_cells[MAX_ROWS * MAX_COLS];       /* what the console contains */
static Cell g_shown[MAX_ROWS * MAX_COLS];       /* what the screen shows (cursor included); UNKNOWN_CP where not known */
static bool g_row_dirty[MAX_ROWS];
static bool g_all_dirty;
static u32  g_rowbuf[FONT_H * MAX_COLS * FONT_W];   /* one text row, rendered in hardware pixel format */
static int  g_cols, g_rows;
static int  g_cx, g_cy;
static int  g_saved_cx, g_saved_cy;
static u32  g_fg = DEFAULT_FG, g_bg = DEFAULT_BG;
static bool g_bold, g_reverse;
static bool g_enabled = true;
static bool g_cursor_on;                        /* the cursor (an inverted cell) is part of the picture right now */

/* A picture somebody else drew over the grid (the boot banner's yin-yang): the cells under it look blank to us, so they have to be
 * repainted when something blanks or scrolls them. Remembered here, applied to g_shown at the next such moment. */
static bool g_sprite_pending;
static int  g_sprite_c0, g_sprite_r0, g_sprite_c1, g_sprite_r1;

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
static inline Cell *shown(int col, int row) { return &g_shown[row * MAX_COLS + col]; }

/* ---------------------------------------------------------------- drawing: the cells that changed, row by row */

static Cell effective(int col, int row) {
    Cell c = *cell(col, row);
    if (g_cursor_on && col == g_cx && row == g_cy) { u32 t = c.fg; c.fg = c.bg; c.bg = t; }
    return c;
}

static void flush_row(int row) {
    int c0 = -1, c1 = -1;
    for (int c = 0; c < g_cols; c++) {
        Cell e = effective(c, row);
        const Cell *s = shown(c, row);
        if (e.cp != s->cp || e.fg != s->fg || e.bg != s->bg) {
            if (c0 < 0) c0 = c;
            c1 = c;
        }
    }
    if (c0 < 0) return;
    int ncells = c1 - c0 + 1, stride = ncells * FONT_W;
    for (int c = c0; c <= c1; c++) {
        Cell e = effective(c, row);
        *shown(c, row) = e;
        const unsigned char *glyph = font8x16_glyph(e.cp);
        u32 f = fb_hw(e.fg), b = fb_hw(e.bg);
        for (int y = 0; y < FONT_H; y++) {
            u32 *d = g_rowbuf + (size_t)y * stride + (c - c0) * FONT_W;
            unsigned bits = glyph[y];
            for (int x = 0; x < FONT_W; x++) d[x] = (bits & (0x80u >> x)) ? f : b;
        }
    }
    for (int y = 0; y < FONT_H; y++) fb_put_span(c0 * FONT_W, row * FONT_H + y, g_rowbuf + (size_t)y * stride, stride);
}

static void flush(void) {
    if (!g_enabled || !g_fb.ready) return;
    for (int r = 0; r < g_rows; r++) {
        if (!g_all_dirty && !g_row_dirty[r]) continue;
        flush_row(r);
        g_row_dirty[r] = false;
    }
    g_all_dirty = false;
    fb_sync();
}

static inline void mark(int row) { g_row_dirty[row] = true; }

static void apply_sprite(void) {
    if (!g_sprite_pending) return;
    g_sprite_pending = false;
    for (int r = g_sprite_r0; r <= g_sprite_r1 && r < g_rows; r++)
        for (int c = g_sprite_c0; c <= g_sprite_c1 && c < g_cols; c++) shown(c, r)->cp = UNKNOWN_CP;
}

void fbcon_taint_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0) return;
    int c0 = x / FONT_W, r0 = y / FONT_H, c1 = (x + w - 1) / FONT_W, r1 = (y + h - 1) / FONT_H;
    if (!g_sprite_pending) { g_sprite_c0 = c0; g_sprite_r0 = r0; g_sprite_c1 = c1; g_sprite_r1 = r1; g_sprite_pending = true; return; }
    if (c0 < g_sprite_c0) g_sprite_c0 = c0;
    if (r0 < g_sprite_r0) g_sprite_r0 = r0;
    if (c1 > g_sprite_c1) g_sprite_c1 = c1;
    if (r1 > g_sprite_r1) g_sprite_r1 = r1;
}

static void cursor_hide(void) {
    if (g_cursor_on) {
        g_cursor_on = false;
        mark(g_cy);
    }
}

static void cursor_show(void) {
    if (g_enabled && g_fb.ready && g_cx < g_cols && g_cy < g_rows) {
        g_cursor_on = true;
        mark(g_cy);
    }
}

static void clear_cells(int row, int col0, int col1) {
    apply_sprite();
    for (int c = col0; c < col1; c++) {
        Cell *ce = cell(c, row);
        ce->cp = ' ';
        ce->fg = g_fg;
        ce->bg = g_bg;
    }
    mark(row);
}

static void scroll_up(void) {
    apply_sprite();
    for (int r = 1; r < g_rows; r++)
        memcpy(cell(0, r - 1), cell(0, r), sizeof(Cell) * (size_t)g_cols);
    g_all_dirty = true;
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
    mark(g_cy);
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

/* ---------------------------------------------------------------- interface */

static void forget_screen(void) {
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++) shown(c, r)->cp = UNKNOWN_CP;
    g_all_dirty = true;
    g_cursor_on = false;
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
    g_cursor_on = false;
    g_sprite_pending = false;
    fb_bench_fill(DEFAULT_BG);                          /* the whole screen is background now, and this is the first speed sample */
    for (int r = 0; r < g_rows; r++) {
        clear_cells(r, 0, g_cols);
        for (int c = 0; c < g_cols; c++) *shown(c, r) = *cell(c, r);       /* blank on blank: already what the screen shows */
        g_row_dirty[r] = false;
    }
    g_all_dirty = false;
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
    flush();
}

void fbcon_clear(void) {
    cursor_hide();
    for (int r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
    g_cx = g_cy = 0;
    cursor_show();
    flush();
}

void fbcon_set_enabled(bool on) {
    if (on == g_enabled) return;
    g_enabled = on;
    g_cursor_on = false;
    if (on) fbcon_redraw();
}

bool fbcon_enabled(void) { return g_enabled; }

/* The whole screen again, from the grid: after the window server gave the screen back, and for a kernel panic that must be seen. */
void fbcon_redraw(void) {
    if (!g_enabled || !g_fb.ready) return;
    int right = g_cols * FONT_W, bottom = g_rows * FONT_H;                  /* the strips the grid does not cover */
    if (right < (int)g_fb.width) fb_fill_rect(right, 0, (int)g_fb.width - right, (int)g_fb.height, DEFAULT_BG);
    if (bottom < (int)g_fb.height) fb_fill_rect(0, bottom, right, (int)g_fb.height - bottom, DEFAULT_BG);
    g_sprite_pending = false;
    forget_screen();
    cursor_show();
    flush();
}

/* A kernel panic has to show up on the screen even if the window server owns it: no serial port to read it from on a PC. */
void fbcon_force_on(void) {
    if (!g_fb.ready || g_cols == 0) return;
    if (!g_enabled) g_enabled = true;
    fbcon_redraw();
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

/* ---------------------------------------------------------------- self test ("selftest" on the kernel command line) */

/* Is every cell of the screen what the grid says, cursor included? Reads the framebuffer back, which is fine in a test. */
static int verify_screen(const char *stage) {
    if (!g_fb.ready) return 0;
    int bad = 0, first_c = -1, first_r = -1;
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            Cell e = effective(c, r);
            const unsigned char *glyph = font8x16_glyph(e.cp);
            u32 f = fb_hw(e.fg), b = fb_hw(e.bg);
            bool ok = true;
            for (int y = 0; y < FONT_H && ok; y++) {
                const u32 *px = g_fb.front + (size_t)(r * FONT_H + y) * g_fb.pitch + (size_t)c * FONT_W;
                unsigned bits = glyph[y];
                for (int x = 0; x < FONT_W; x++)
                    if (px[x] != ((bits & (0x80u >> x)) ? f : b)) { ok = false; break; }
            }
            if (!ok) { if (!bad) { first_c = c; first_r = r; } bad++; }
        }
    }
    if (bad) klog("selftest", "fbcon %s: %d cells differ from the grid (first at column %d row %d)", stage, bad, first_c, first_r);
    return bad ? 1 : 0;
}

static void say(const char *s) { fbcon_write(s, strlen(s)); }

int fbcon_selftest(void) {
    if (!g_fb.ready) { klog("selftest", "fbcon: no framebuffer, skipped"); return 0; }
    klog("selftest", "fbcon: the screen must always show exactly the grid");
    int fails = 0;
    char line[300];

    say("\x1b[0m\x1b[2J\x1b[H");
    fails += verify_screen("after clear");

    for (int i = 0; i < 3 * g_rows + 7; i++) {                       /* several screens of colourful text: many scrolls */
        snprintf(line, sizeof line, "\x1b[%dmline %d \x1b[1;%dmbold\x1b[0m and some more text to make the row long %*s\n",
                 31 + i % 7, i, 31 + (i + 3) % 7, i % 40, "x");
        say(line);
    }
    fails += verify_screen("after many scrolls");

    say("\x1b[3;5Hhello\x1b[K\x1b[10;1H\x1b[1J\x1b[12;20Hab\tc\td\b\bXY\x1b[2K\x1b[14;1H\x1b[7minverse\x1b[27m normal\x1b[20;3H\x1b[1Kbefore");
    say("\x1b[5;1H\xc3\xa9\xe2\x94\x80\xe2\x94\x82\xe2\x96\x88 utf-8\x1b[s\x1b[30;60Hsaved\x1b[u!\x1b[0J");
    fails += verify_screen("after cursor addressing");

    fbcon_set_enabled(false);                                         /* the window server owns the screen: grid changes, nothing is drawn */
    for (int i = 0; i < 20; i++) say("written while the screen belongs to someone else\n");
    fbcon_set_enabled(true);
    fails += verify_screen("after the screen was given back");

    char *big = kmalloc(60000);                                       /* one huge write, like dmesg into the console */
    if (big) {
        size_t n = 0;
        for (int i = 0; i < 1500 && n + 100 < 60000; i++) n += (size_t)snprintf(big + n, 100, "\x1b[%dmbig write line %d\n", 31 + i % 6, i);
        fbcon_write(big, n);
        kfree(big);
        fails += verify_screen("after one big write");
    }

    memset(line, 'W', 290);                                           /* a line wider than the screen wraps */
    line[290] = '\n';
    fbcon_write(line, 291);
    say("\x1b[0m");
    fails += verify_screen("after a wrapping line");

    fbcon_set_enabled(false);                                         /* a panic while the window server owns the screen */
    say("\x1b[1;31mPANIC-TEST\x1b[0m the screen must be taken back\n");
    fbcon_force_on();
    fails += verify_screen("after a forced redraw");
    klog("selftest", "fbcon: %s", fails ? "FAIL" : "ok");
    return fails;
}
