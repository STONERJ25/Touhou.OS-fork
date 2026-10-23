/* Marisa's Sketchbook: a small paint program. Private header shared by the files in this directory.
 *
 *   image.c    the picture, undo/redo history (tile based, so big canvases stay cheap), whole-image operations
 *   raster.c   span sinks, lines, rectangles, ellipses, text, flood fill; the same code draws previews and results
 *   fileio.c   BMP and PPM readers and writers
 *   canvas.c   zoom, scrolling, rendering of the picture and its overlays, pointer queue
 *   tools.c    what every tool does with the pointer, selections, text, clipboard
 *   panels.c   toolbox, options, palette, colour mixer, status bar
 *   dialogs.c  new/resize dialogs, message and confirmation dialogs, About
 *   icons.c    procedural tool icons
 *   main.c     menus, shortcuts, commands, application life cycle
 */
#ifndef SKETCHBOOK_H
#define SKETCHBOOK_H

#include <kappa.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SK_MAX_DIM  4096
#define SK_DEF_W    640
#define SK_DEF_H    480
#define SK_TILE     64

typedef enum {
    T_PENCIL, T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICKER, T_TEXT, T_SELECT, T_SPRAY, T_COUNT
} Tool;

enum { S_OUTLINE = 0, S_FILLED = 1, S_BOTH = 2 };          /* rectangle / ellipse styles */

static inline int sk_min(int a, int b) { return a < b ? a : b; }
static inline int sk_max(int a, int b) { return a > b ? a : b; }
static inline int sk_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int sk_fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* ---------------------------------------------------------------- the picture (image.c) */

typedef struct Doc { int w, h; uint32_t *px; } Doc;       /* 0x00RRGGBB */
extern Doc g_doc;

bool  doc_new(int w, int h, uint32_t fill);               /* replaces the picture and forgets the history */
bool  doc_adopt(uint32_t *px, int w, int h);              /* takes ownership of px (from a loader) */
void  doc_free(void);

/* History. Painting happens inside hist_begin/hist_commit; every pixel write is announced with hist_touch() first (the
 * raster sinks do that), which saves the 64x64 tiles it covers. Whole-image operations are their own entries. */
void  hist_begin(const char *label);
void  hist_touch(int x0, int y0, int x1, int y1);         /* inclusive rectangle in image coordinates */
void  hist_commit(void);
void  hist_abort(void);                                    /* undo the pending entry without recording it */
bool  hist_pending(void);
bool  hist_pending_dirty(void);                            /* the pending entry has already saved something */
bool  hist_undo(void);
bool  hist_redo(void);
const char *hist_undo_label(void);
const char *hist_redo_label(void);
uint32_t hist_version(void);                               /* changes whenever the picture changes; 0 = pristine */
void  hist_clear(void);
enum { HOP_FLIP_H, HOP_FLIP_V, HOP_ROT_CW, HOP_ROT_CCW, HOP_INVERT };
bool  hist_op(int op, const char *label);                  /* applies a whole-image operation and records it */
bool  hist_replace(const char *label, uint32_t *newpx, int nw, int nh);   /* swaps in a new picture, keeps the old one for undo */
uint32_t *img_make_resized(int nw, int nh, uint32_t fill, bool stretch);
uint32_t *img_make_crop(int x, int y, int w, int h);

/* ---------------------------------------------------------------- rasterising (raster.c) */

typedef struct Sink Sink;
struct Sink { void (*span)(Sink *s, int x0, int x1, int y); uint32_t color; int alpha; };
typedef struct { Sink base; KCanvas *cv; int ox, oy, z; } ScreenSink;

extern Sink g_isink;                                       /* writes into the picture (and the history) */
void  ssink_init(ScreenSink *s, KCanvas *cv, int ox, int oy, int z);

void  rs_stamp(Sink *s, int cx, int cy, int size, bool square);
void  rs_line(Sink *s, int x0, int y0, int x1, int y1, int size, bool square);
void  rs_rect(Sink *s, int x0, int y0, int x1, int y1, int thick, uint32_t prim, uint32_t sec, int style);
void  rs_ellipse(Sink *s, int x0, int y0, int x1, int y1, int thick, uint32_t prim, uint32_t sec, int style);
void  rs_text(Sink *s, int x, int y, const char *text, int scale, uint32_t fg, bool opaque, uint32_t bgc);
void  rs_spray(Sink *s, int cx, int cy, int radius, int dots);
void  rs_fill_rect(Sink *s, int x0, int y0, int x1, int y1);
bool  fill_flood(int x, int y, uint32_t color);
void  text_metrics(const char *text, int *cols, int *lines);

/* ---------------------------------------------------------------- files (fileio.c) */

enum { FMT_UNKNOWN = 0, FMT_BMP, FMT_PPM };
int          file_format_for(const char *path);
const char  *file_load(const char *path, uint32_t **px, int *w, int *h);   /* NULL = ok, else an error message */
const char  *file_save(const char *path, const Doc *d);

/* ---------------------------------------------------------------- application state */

typedef struct Sk {
    KApp *app;
    char path[1100];                   /* "" = never named */
    char name[160];                    /* what the title bar shows */
    uint32_t saved_version;
    bool saved_once;
    /* view */
    int zoom;                          /* 1..8 */
    int scroll_x, scroll_y;            /* offsets into the padded picture, in screen pixels */
    bool grid;
    KRect view;                        /* the canvas viewport this frame (window coordinates) */
    int ox, oy;                        /* window position of picture pixel (0,0) this frame */
    /* colours and tool options */
    uint32_t fg, bg;
    int mix_target;                    /* 0 = foreground, 1 = background */
    Tool tool;
    int size[T_COUNT];
    int shape_style;
    int text_scale;
    bool text_opaque;
    /* pointer */
    int cur_x, cur_y;                  /* picture coordinates under the pointer */
    bool cur_inside;
    char hint[200];
    /* misc */
    int modal;                         /* open dialogs of ours */
    uint32_t iter;
    bool quit_after_save;
    uint64_t now;
} Sk;
extern Sk S;

/* ---------------------------------------------------------------- canvas.c */

#define CANVAS_PAD 6
void  canvas_init(void);
void  canvas_frame(KUI *ui, KRect frame);                  /* input, scrollbars, rendering */
void  canvas_event_hook(KApp *app, const FairyEvent *ev, void *user);
void  canvas_set_zoom(int z, bool at_pointer);
void  canvas_center(void);
void  canvas_scroll_to_show(int x, int y, int w, int h);
int   canvas_to_ix(int sx);
int   canvas_to_iy(int sy);
void  canvas_reset_view(void);
void  canvas_blit_image(KCanvas *cv, const uint32_t *src, int sw, int sh, int ix, int iy);   /* a picture-sized image at picture coords, magnified */

/* ---------------------------------------------------------------- tools.c */

const char *tool_name(Tool t);
const char *tool_hint(Tool t);
int   tool_key(Tool t);                                    /* shortcut letter */
void  tool_set(Tool t);
void  tools_init(void);
void  tools_pointer_press(KUI *ui, int button, int x, int y, int mods);
void  tools_pointer_move(KUI *ui, int x, int y, int mods);
void  tools_pointer_release(KUI *ui, int button, int x, int y, int mods);
void  tools_frame(KUI *ui);                                /* per-frame work: spray, text keys, hints */
void  tools_draw_overlays(KUI *ui, KCanvas *cv);           /* previews, selection, text box, brush outline */
bool  tools_dragging(void);
void  tools_cancel_drag(void);
void  tools_commit_all(void);                              /* anchor a floating selection and finish text */
bool  tools_cancel_overlays(void);                         /* drop a floating selection or unfinished text; true if there was one */
unsigned tools_cursor(int ix, int iy);                     /* mouse cursor shape over picture pixel (ix, iy) */
void  tools_nudge(int dx, int dy);                         /* move the selection with the arrow keys */
bool  tools_text_active(void);
void  tools_text_keys(KUI *ui);
bool  tools_has_selection(void);
bool  tools_float_active(void);
void  tools_reset(void);                                   /* new picture: forget selection and text */
void  cmd_copy(void);
void  cmd_cut(void);
void  cmd_paste(void);
void  cmd_delete(void);
void  cmd_select_all(void);
void  cmd_deselect(void);
void  cmd_crop_to_selection(void);
bool  clip_has_content(void);
void  tools_get_selection(int *x, int *y, int *w, int *h);

/* ---------------------------------------------------------------- panels.c */

#define MENU_H     24
#define STATUS_H   22
#define PALETTE_H  66
#define TOOLBOX_W  76
void  panels_init(void);
void  panels_toolbox(KUI *ui, KRect r);
void  panels_palette(KUI *ui, KRect r);
void  panels_status(KUI *ui, KRect r);
void  color_set(int which, uint32_t c);                    /* 0 = fg, 1 = bg */
void  color_swap(void);

/* ---------------------------------------------------------------- dialogs.c */

void  dlg_gc(void);
void  dlg_new_image(void);
void  dlg_resize_canvas(void);
void  dlg_about(void);
void  dlg_shortcuts(void);
void  dlg_message(const char *title, const char *text, KIcon icon);
typedef void (*AskCb)(int choice, void *user);             /* 0 first button, 1 second, 2 third (or cancel) */
void  dlg_ask(const char *title, const char *text, KIcon icon, const char *b0, const char *b1, const char *b2, AskCb cb, void *user);
void  dlg_modal_enter(void);
void  dlg_modal_leave(void);

/* ---------------------------------------------------------------- icons.c */

void  icon_tool(KCanvas *cv, Tool t, int x, int y);        /* 20x20 */
void  icon_swap(KCanvas *cv, int x, int y);                /* 14x14 */

/* ---------------------------------------------------------------- main.c */

void  app_update_title(void);
bool  app_modified(void);
void  app_new_picture(int w, int h);                       /* after the user confirmed */
void  app_resize_canvas(int w, int h, bool stretch);
void  app_open_path(const char *path, bool is_arg);
void  app_cmd_new(void);
void  app_cmd_open(void);
void  app_cmd_save(void);
void  app_cmd_save_as(void);
void  app_cmd_quit(void);
void  app_cmd_undo(void);
void  app_cmd_redo(void);
void  app_cmd_op(int op, const char *label);
void  app_cmd_clear(void);
void  app_cmd_open_screenshot(void);
void  app_guard(void (*then)(void *user), void *user);     /* asks about unsaved changes, then runs `then` */

#endif
