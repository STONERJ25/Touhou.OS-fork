/* A VT100/xterm style terminal emulator core: screen grid, scrollback, escape sequence parser. No window system here. */
#ifndef VT_H
#define VT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VT_DEFAULT 0xFFFFFFFFu             /* "default colour" marker for fg/bg */

enum { A_BOLD = 1, A_DIM = 2, A_UNDERLINE = 4, A_REVERSE = 8, A_INVISIBLE = 16, A_STRIKE = 32, A_ITALIC = 64, A_BLINK = 128 };

typedef struct VtCell {
    uint32_t cp;
    uint32_t fg, bg;                       /* 0x00RRGGBB or VT_DEFAULT */
    uint8_t attr;
} VtCell;

typedef struct VtLine {
    VtCell *cells;
    int len;
} VtLine;

typedef struct Vt {
    int cols, rows;
    VtCell *screen;                        /* rows * cols, the visible grid */
    VtCell *saved_main;                    /* the main screen while the alternate one is shown */
    bool alt;
    VtLine *sb;                            /* scrollback ring */
    int sb_head, sb_count, sb_max;
    int cx, cy, saved_cx, saved_cy;
    uint32_t cur_fg, cur_bg;
    uint8_t cur_attr;
    uint32_t saved_fg, saved_bg;
    uint8_t saved_attr;
    int top, bot;                          /* scroll region (inclusive rows) */
    bool wrap_pending, autowrap, app_cursor, cursor_visible, bracketed_paste, insert_mode, app_keypad;
    int mouse_mode;                        /* 0 off, 1000 press/release, 1002 drag, 1003 all motion */
    bool mouse_sgr;
    uint8_t tabs[512];
    /* parser */
    int state;
    int params[16];
    int nparams;
    bool priv, has_param;
    char inter;
    char osc[256];
    int osc_len;
    uint32_t utf8_cp;
    int utf8_left;
    uint32_t last_cp;
    bool *dirty;                           /* per visible row */
    bool all_dirty;
    int gen;                               /* bumped on every change; lets the view know it must redraw */
    /* callbacks */
    void *user;
    void (*reply)(void *user, const char *data, size_t n);      /* bytes to send back to the program */
    void (*set_title)(void *user, const char *title);
    void (*bell)(void *user);
} Vt;

void vt_init(Vt *v, int cols, int rows, int scrollback);
void vt_free(Vt *v);
void vt_resize(Vt *v, int cols, int rows);
void vt_input(Vt *v, const uint8_t *data, size_t n);
void vt_reset(Vt *v);
const VtCell *vt_cell(const Vt *v, int row, int col);          /* visible grid */
/* Row `row` of the view when scrolled back by `offset` lines (offset 0 = the live screen); NULL cells beyond the line length. */
const VtCell *vt_view_cell(const Vt *v, int offset, int row, int col);
int vt_scrollback_count(const Vt *v);
uint32_t vt_palette(int index);                                 /* the 256 colour xterm palette (first 16 are the Gensokyo scheme) */
void vt_mark_dirty(Vt *v, int row);
void vt_mark_all_dirty(Vt *v);

#endif
