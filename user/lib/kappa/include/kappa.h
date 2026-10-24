/*
 * Kappa toolkit: the drawing and widget library every Gensokyo desktop program is built from (Nitori would be proud).
 *
 *  - KCanvas: software 2D drawing on a 0x00RRGGBB pixel buffer (rectangles, bevels, gradients, text, icons, blits).
 *  - KUI: immediate-mode widgets. The program describes its whole window on every frame:
 *
 *        static void draw(KApp *app, KUI *ui, void *user) {
 *            k_clear(ui);
 *            if (k_button(ui, kr(10, 10, 80, 24), "Hello")) puts("clicked");
 *        }
 *        int main(void) { KApp *a = kapp_new("Hello", 300, 200, 0); kapp_set_draw(a, draw, NULL); return kapp_run(a); }
 *
 *  - KApp: connects to the window server, owns the window and a private back buffer, turns events into KUI input and
 *    calls the draw function after every batch of events (and on timers).
 */
#ifndef KAPPA_H
#define KAPPA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <fairy.h>

/* ---------------------------------------------------------------- geometry and colour */

typedef struct { int x, y, w, h; } KRect;

static inline KRect kr(int x, int y, int w, int h) { KRect r = { x, y, w, h }; return r; }
static inline bool kr_contains(KRect r, int px, int py) { return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h; }
static inline bool kr_empty(KRect r) { return r.w <= 0 || r.h <= 0; }
KRect kr_intersect(KRect a, KRect b);
KRect kr_union(KRect a, KRect b);
KRect kr_inset(KRect r, int d);
KRect kr_grow(KRect r, int d);

#define KRGB(r, g, b) ((uint32_t)(((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))
uint32_t kc_mix(uint32_t a, uint32_t b, int t256);              /* a + (b - a) * t / 256 */
uint32_t kc_shade(uint32_t c, int percent);                      /* 100 = unchanged, 50 = half as bright, 150 = brighter */

/* The Gensokyo palette */
enum {
    KC_SHRINE   = 0xC8102E,         /* shrine red */
    KC_SHRINE_D = 0x8E0B21,
    KC_PAPER    = 0xECE6D6,         /* washi paper: window bodies */
    KC_PAPER_D  = 0xD6CFBB,
    KC_INK      = 0x1A1A24,         /* text */
    KC_NIGHT    = 0x0E1330,         /* deep night blue */
    KC_NIGHT_L  = 0x1D2A5C,
    KC_GOLD     = 0xD9A441,
    KC_MOON     = 0xF4EFD8,
    KC_WHITE    = 0xFFFFFF,
    KC_BLACK    = 0x000000,
};

/* ---------------------------------------------------------------- canvas */

/* Coordinates, sizes and the clip rectangle are in logical pixels. A canvas with scale N > 1 (a window on a hi-dpi screen, see
 * fairy_scale()) has N x N pixels in px for each logical pixel, and stride counts those: every kc_* function and every widget
 * draws at that finer resolution by itself (text and bevels N times as thick, icons, circles and gradients drawn with the extra
 * pixels). Code that writes px directly must take the scale into account (kc_scale()), or ask for 1x windows with
 * kapp_set_hidpi(false) before opening any. scale 0 means 1, so canvases set up by hand need not mention it. */
typedef struct KCanvas {
    uint32_t *px;
    int w, h, stride;               /* w, h: logical size; stride in pixels of px */
    KRect clip;
    int scale;                      /* pixels of px per logical pixel; 0 or 1: the same thing */
} KCanvas;

static inline int kc_scale(const KCanvas *c) { return c->scale > 1 ? c->scale : 1; }

typedef enum { KB_RAISED, KB_SUNKEN, KB_FLAT, KB_ETCHED, KB_FRAME } KBevel;
typedef enum { KA_LEFT, KA_CENTER, KA_RIGHT } KAlign;

void kc_init(KCanvas *c, uint32_t *px, int w, int h, int stride);
void kc_init_scaled(KCanvas *c, uint32_t *px, int w, int h, int stride, int scale);   /* w, h logical; px holds w*scale x h*scale pixels */
void kc_set_clip(KCanvas *c, KRect r);                           /* intersected with the canvas bounds */
KCanvas kc_native_view(const KCanvas *c);                         /* the same pixels as a 1x canvas: coordinates in pixels of px */
void kc_pixel(KCanvas *c, int x, int y, uint32_t rgb);
void kc_blend_pixel(KCanvas *c, int x, int y, uint32_t rgb, int alpha);    /* alpha 0..255 */
void kc_fill(KCanvas *c, KRect r, uint32_t rgb);
void kc_fill_alpha(KCanvas *c, KRect r, uint32_t rgb, int alpha);
void kc_outline(KCanvas *c, KRect r, uint32_t rgb);
void kc_hline(KCanvas *c, int x, int y, int len, uint32_t rgb);
void kc_vline(KCanvas *c, int x, int y, int len, uint32_t rgb);
void kc_line(KCanvas *c, int x0, int y0, int x1, int y1, uint32_t rgb);
void kc_gradient_v(KCanvas *c, KRect r, uint32_t top, uint32_t bottom);
void kc_gradient_h(KCanvas *c, KRect r, uint32_t left, uint32_t right);
void kc_circle_fill(KCanvas *c, int cx, int cy, int radius, uint32_t rgb);
void kc_circle(KCanvas *c, int cx, int cy, int radius, uint32_t rgb);
void kc_ellipse_fill(KCanvas *c, int cx, int cy, int rx, int ry, uint32_t rgb);
void kc_bevel(KCanvas *c, KRect r, KBevel kind);                 /* draws a 2 px border (1 px for KB_FLAT/KB_ETCHED) */
void kc_panel(KCanvas *c, KRect r, KBevel kind, uint32_t fill);  /* fill + bevel */
void kc_blit(KCanvas *dst, int x, int y, const uint32_t *src, int sw, int sh, int sstride);
void kc_blit_part(KCanvas *dst, int x, int y, const uint32_t *src, int sstride, KRect part);
void kc_blit_argb(KCanvas *dst, int x, int y, const uint32_t *src, int sw, int sh, int sstride);   /* alpha in bits 24..31 */
/* The kc_blit functions take a picture of sw x sh logical pixels (enlarged on a scaled canvas). kc_blit_native takes one that is
 * already at the canvas' resolution (sw x sh pixels of px, e.g. a wallpaper rendered for the whole screen) and puts its top-left
 * corner at logical (x, y). On a 1x canvas the two are the same. */
void kc_blit_native(KCanvas *dst, int x, int y, const uint32_t *src, int sw, int sh, int sstride);
void kc_scroll(KCanvas *c, KRect r, int dy);                     /* moves the pixels inside r up (dy < 0) or down */

/* Text: the built-in 8x16 bitmap font. Strings are UTF-8; a code point is one 8 px cell. */
int  kc_text(KCanvas *c, int x, int y, const char *s, uint32_t fg);                  /* returns the advance in pixels */
int  kc_text_n(KCanvas *c, int x, int y, const char *s, size_t bytes, uint32_t fg);
int  kc_text_bold(KCanvas *c, int x, int y, const char *s, uint32_t fg);
int  kc_text_scaled(KCanvas *c, int x, int y, const char *s, uint32_t fg, int scale);
int  kc_text_shadow(KCanvas *c, int x, int y, const char *s, uint32_t fg, uint32_t shadow);
void kc_text_in(KCanvas *c, KRect r, const char *s, uint32_t fg, KAlign align);       /* vertically centred, clipped with "..." */
int  kc_text_width(const char *s);
int  kc_glyph(KCanvas *c, int x, int y, uint32_t cp, uint32_t fg, int scale);          /* single code point, transparent background */
int  kc_utf8_next(const char *s, uint32_t *cp);                                        /* bytes consumed; 0 at the end */
size_t kc_utf8_len(const char *s);                                                     /* code points */
size_t kc_utf8_offset(const char *s, size_t cp_index);                                 /* byte offset of the n-th code point */
int  kc_utf8_encode(uint32_t cp, char *out);                                           /* writes 1..4 bytes, returns the count */

/* ---------------------------------------------------------------- icons */

typedef enum {
    KI_FOLDER, KI_FOLDER_OPEN, KI_FILE, KI_TEXT, KI_IMAGE, KI_TERMINAL, KI_CALC, KI_CLOCK, KI_GEAR, KI_SPELLCARD, KI_NEWS,
    KI_NOTEBOOK, KI_TRASH, KI_HOME, KI_DRIVE, KI_YINYANG, KI_POWER, KI_SEARCH, KI_STAR, KI_UP, KI_BACK, KI_FORWARD, KI_PLUS,
    KI_CLOSE, KI_MINIMIZE, KI_MAXIMIZE, KI_RESTORE, KI_CHECK, KI_ARROW_DOWN, KI_ARROW_RIGHT, KI_REFRESH, KI_GAME, KI_MONITOR,
    KI_INFO, KI_WARNING, KI_ERROR, KI_QUESTION, KI_TORII, KI_BAMBOO, KI_PETAL, KI_MOON, KI_LOCK, KI_USER, KI_SKETCH, KI_COUNT
} KIcon;

void k_icon(KCanvas *c, KIcon icon, int x, int y, int size);     /* procedural, crisp at 16..64 px */
const char *k_icon_name(KIcon icon);

/* ---------------------------------------------------------------- UI context */

typedef struct KTheme {
    uint32_t window_bg, text, text_dim, text_on_accent;
    uint32_t accent, accent_dark, accent_light;
    uint32_t button_bg, field_bg, field_text, select_bg, select_text, border_dark, border_light;
    uint32_t title_active_a, title_active_b, title_inactive_a, title_inactive_b, title_text, title_text_inactive;
} KTheme;

extern const KTheme k_theme_shrine;     /* the default */

typedef struct KKey { uint8_t code, mods, down; uint32_t ch; } KKey;

#define KUI_MAX_KEYS 64

typedef struct KUI {
    KCanvas cv;
    const KTheme *theme;
    /* pointer */
    int mx, my;
    uint8_t buttons;                /* bit0 left, bit1 right, bit2 middle */
    bool pressed[3], released[3];   /* edges since the previous frame */
    bool double_click;              /* left button double click happened this frame */
    int wheel;                      /* detents since the previous frame, positive = up */
    bool mouse_inside;
    /* keyboard */
    KKey keys[KUI_MAX_KEYS];
    int nkeys;
    uint8_t mods;
    bool has_focus;
    bool key_down[256];             /* keys currently held, by KEY_* code (for games) */
    /* time */
    uint64_t now_ms;
    /* widget state */
    uint32_t hot, active, focus;    /* widget ids */
    int drag_dx, drag_dy;
    uint32_t cursor_shape;          /* requested mouse cursor, applied at the end of the frame */
    bool redraw_soon;               /* a widget wants another frame (blinking caret, animation) */
    int caret, sel_anchor;          /* text field editing state (the focused field) */
    uint64_t caret_ms;
    struct KApp *app;
    struct KWin *window;            /* the window this UI draws into */
    int menu_open;                  /* 1-based index of the open menu, 0 = none; widgets ignore the pointer while a menu is open */
    int menu_item_hover;
    bool input_blocked;
    const char *tooltip;
    int tooltip_x, tooltip_y;
    KRect damage;                   /* set by programs that redraw only part of the window (terminals); default: everything */
    bool has_damage;
    char scratch[64];
} KUI;

static inline void k_damage(KUI *ui, KRect r) {
    ui->damage = ui->has_damage ? kr_union(ui->damage, r) : r;
    ui->has_damage = true;
}

uint32_t k_id(const char *label, KRect r);
void k_clear(KUI *ui);                                           /* fills the whole canvas with the window background */

bool k_key_pressed(KUI *ui, uint8_t code);                       /* consumes a press (or repeat) of that key if present */
bool k_hover(KUI *ui, KRect r);
bool k_clicked(KUI *ui, KRect r);                                /* left click released inside r (not consumed) */

bool k_button(KUI *ui, KRect r, const char *label);
bool k_button_icon(KUI *ui, KRect r, KIcon icon, const char *label);
bool k_button_flat(KUI *ui, KRect r, const char *label, bool selected);      /* toolbar/tab style */
bool k_icon_button(KUI *ui, KRect r, KIcon icon, bool selected);
void k_label(KUI *ui, int x, int y, const char *text);
void k_label_col(KUI *ui, int x, int y, const char *text, uint32_t color);
void k_label_in(KUI *ui, KRect r, const char *text, KAlign align);
void k_heading(KUI *ui, int x, int y, const char *text);          /* bold, accent coloured */
bool k_checkbox(KUI *ui, int x, int y, const char *label, bool *value);
bool k_radio(KUI *ui, int x, int y, const char *label, int *group, int value);
bool k_textfield(KUI *ui, KRect r, char *buf, size_t cap);        /* true when Enter was pressed */
bool k_textfield_pw(KUI *ui, KRect r, char *buf, size_t cap);     /* shows bullets */
bool k_slider(KUI *ui, KRect r, int *value, int min, int max);
void k_progress(KUI *ui, KRect r, int percent);
void k_separator(KUI *ui, KRect r);
void k_group(KUI *ui, KRect r, const char *title);
int  k_tabs(KUI *ui, KRect r, const char *const *labels, int n, int *selected);   /* draws the tab strip in r, returns selected */
void k_scrollbar(KUI *ui, KRect r, int *offset, int content, int view);          /* vertical */
void k_tooltip_text(KUI *ui, const char *text);                   /* drawn near the pointer at the end of the frame */

typedef struct KListState {
    int scroll;                     /* first visible row */
    int selected;                   /* -1 none */
    int activated;                  /* row that was double-clicked / Enter-ed this frame, else -1 */
    int hover;
} KListState;
void k_list_init(KListState *st);
/* Draws rows with optional icons (icons may be NULL). Returns true when the selection changed. */
bool k_list(KUI *ui, KRect r, const char *const *items, const KIcon *icons, int n, KListState *st);

typedef struct KMenuItem { const char *label; KIcon icon; bool separator; bool disabled; const char *shortcut; } KMenuItem;
/* Immediate-mode menu bar: call with the bar rectangle; returns the (menu << 8 | item + 1) chosen this frame or 0. */
int  k_menubar(KUI *ui, KRect r, const char *const *titles, const KMenuItem *const *menus, const int *counts, int nmenus);

/* A multi-line text area with scrolling and selection; used by the notebook and the terminal's scrollback viewers. */
typedef struct KTextArea {
    char *text; size_t len, cap;
    size_t caret, anchor;
    int scroll_y, scroll_x;
    bool readonly, wrap;
    bool modified;
    size_t *ls;                     /* line start offsets (rebuilt lazily) */
    int nls, ls_cap;
    bool ls_valid;
} KTextArea;
void   kta_init(KTextArea *t, size_t cap);
void   kta_set(KTextArea *t, const char *s, size_t len);
void   kta_free(KTextArea *t);
bool   k_textarea(KUI *ui, KRect r, KTextArea *t);                /* true when the text changed */
void   kta_insert(KTextArea *t, const char *s, size_t n);
void   kta_goto_line(KTextArea *t, int line);
int    kta_line_count(const KTextArea *t);

/* ---------------------------------------------------------------- application loop */

typedef struct KApp KApp;
typedef void (*KDrawFn)(KApp *app, KUI *ui, void *user);
typedef void (*KFdFn)(KApp *app, int fd, void *user);
typedef void (*KKeyFn)(KApp *app, const KKey *key, void *user);     /* raw key hook before the widgets see it; return via app_consume */

/* A KApp can own several windows (the main one plus popups and dialogs), all sharing one event loop. */
typedef struct KWin KWin;
KWin *kwin_new(KApp *app, int x, int y, int w, int h, const char *title, unsigned fairy_flags, KDrawFn draw, void *user);
void  kwin_close(KWin *w);                                        /* closes at the end of the current iteration */
void  kwin_redraw(KWin *w);
void  kwin_set_close_handler(KWin *w, void (*fn)(KWin *w, void *user), void *user);   /* the server asked it to close (popups: click outside) */
KUI  *kwin_ui(KWin *w);
int   kwin_width(KWin *w);
int   kwin_height(KWin *w);
FairyWindow *kwin_fairy(KWin *w);
void  kwin_move(KWin *w, int x, int y, int content_w, int content_h);
void  kwin_set_title(KWin *w, const char *title);
KWin *kapp_main(KApp *app);
KUI  *kapp_ui(KApp *app);
void  kapp_unwatch_fd(KApp *app, int fd);

/* Hi-dpi: on a screen with a ui scale above 1 (fairy_scale()) KApp windows get buffers at that scale and Kappa draws everything
 * at the finer resolution (FW_HIDPI). A program that writes pixels into its canvas itself calls kapp_set_hidpi(false) before
 * opening its first window: its windows are then 1x and the window server enlarges them. Sizes and coordinates are logical
 * either way. */
void  kapp_set_hidpi(bool enabled);
int   kapp_scale(KApp *app);                                      /* the ui scale of the screen (1, 2, ...) */

KApp *kapp_new_screen(const char *title, unsigned fairy_flags);   /* window the size of the screen */
KApp *kapp_new(const char *title, int w, int h, unsigned fairy_flags);
KApp *kapp_new_at(const char *title, int x, int y, int w, int h, unsigned fairy_flags);
void  kapp_set_draw(KApp *app, KDrawFn fn, void *user);
void  kapp_set_tick(KApp *app, int interval_ms);                  /* redraw every interval_ms even without input (0 = off) */
void  kapp_watch_fd(KApp *app, int fd, KFdFn fn, void *user);     /* call fn when fd is readable, then redraw */
void  kapp_set_key_hook(KApp *app, KKeyFn fn, void *user);
void  kapp_set_close_handler(KApp *app, bool (*fn)(KApp *app, void *user), void *user);   /* return true to let the window close */
void  kapp_redraw(KApp *app);
void  kapp_quit(KApp *app, int code);
int   kapp_run(KApp *app);                                        /* returns the exit code */
void  kapp_set_title(KApp *app, const char *title);
void  kapp_set_cursor(KApp *app, unsigned shape);
void  kapp_set_minsize(KApp *app, int w, int h);
void  kapp_move(KApp *app, int x, int y, int w, int h);
void  kapp_set_state(KApp *app, unsigned state);
FairyDisplay *kapp_display(KApp *app);
FairyWindow  *kapp_window(KApp *app);
int   kapp_width(KApp *app);
int   kapp_height(KApp *app);
void  kapp_set_event_hook(KApp *app, void (*fn)(KApp *app, const FairyEvent *ev, void *user), void *user);   /* sees every event first */
void  kapp_destroy(KApp *app);

/* Run a function on the clipboard text (copy: returns immediately; paste: the text arrives as a FEV_CLIPBOARD event
 * which KApp stores; read it with kapp_clipboard()). */
void  kapp_clipboard_copy(KApp *app, const char *text, size_t len);
const char *kapp_clipboard(KApp *app, size_t *len);
void  kapp_clipboard_fetch(KApp *app);

/* Non-blocking dialogs: they open a window in `app`, call back once the user decides, and close themselves.
 * The callbacks receive NULL / false when the dialog was cancelled. */
typedef void (*KTextCb)(const char *text, void *user);
typedef void (*KFileCb)(const char *path, void *user);
typedef void (*KConfirmCb)(bool yes, void *user);
void k_input_dialog(KApp *app, const char *title, const char *prompt, const char *initial, KTextCb cb, void *user);
void k_confirm_dialog(KApp *app, const char *title, const char *message, KIcon icon, const char *yes, const char *no, KConfirmCb cb, void *user);
/* mode: 0 open an existing file, 1 save (asks for a name), 2 choose a folder */
void k_file_dialog(KApp *app, int mode, const char *title, const char *start_dir, const char *default_name, KFileCb cb, void *user);
void k_focus_textfield(KUI *ui, const char *buf);                 /* give the field editing `buf` the keyboard focus */

/* Modal message box (blocking, own small window): returns the button index chosen (0-based) or -1 if closed. */
int   k_messagebox(FairyDisplay *d, const char *title, const char *message, KIcon icon, const char *const *buttons, int nbuttons);

/* ---------------------------------------------------------------- scenery */
/* The default wallpaper (moon, bamboo, torii) rendered into any pixel buffer; used by the desktop and the login screen. */
void k_scenery_night(uint32_t *px, int w, int h, int stride);
void k_scenery_night_scaled(uint32_t *px, int w, int h, int stride, int scale);     /* w, h logical; px holds w*scale x h*scale */

/* Round avatar with the resident's initial; ring: 0 plain, 1 hovered, 2 selected. */
void k_avatar(KCanvas *cv, int cx, int cy, const char *name, int ring);
void k_avatar_colors(const char *name, uint32_t *bg, uint32_t *fg);

/* ---------------------------------------------------------------- misc helpers */
uint64_t k_now_ms(void);
const char *k_basename(const char *path);

#endif
