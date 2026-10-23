/* notebook: Patchouli's Notebook, the text editor of the Gensokyo desktop. Declarations shared by all source files.
 *
 *   doc.c       the document: gap buffer, line index, multi-level undo/redo
 *   syntax.c    light syntax highlighting (C, shell, Markdown)
 *   editor.c    the editor view: layout, word wrap, drawing, caret and selection, scrolling, input
 *   find.c      searching, the find/replace bar
 *   fileio.c    reading and writing files, line endings, recent files, preferences
 *   dialogs.c   message, choice, About and shortcut dialogs (non-blocking, built on kwin_new)
 *   main.c      the application: menus, status bar, shortcuts, file flows, window handling
 *   selftest.c  `notebook --selftest`: randomised tests of the document model (no window server needed)
 */
#ifndef NOTEBOOK_H
#define NOTEBOOK_H

#include <kappa.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define NB_VERSION      "1.0"
#define NB_MAX_FILE     (64u * 1024u * 1024u)       /* larger files are refused */
#define NB_PATH_MAX     1024
#define NB_RECENT_MAX   10
#define NB_SYN_MAXLEN   4096                        /* lines longer than this are not highlighted */
#define NB_HL_MAXDOC    (8u * 1024u * 1024u)        /* documents larger than this are not highlighted */
#define NB_UNDO_BYTES   (48u * 1024u * 1024u)       /* memory the undo history may hold */
#define NB_UNDO_RECORDS 200000
#define NB_PAD          16                          /* zero bytes after the buffer so decoders never run off the end */
#define ED_ID           0x4E4F5445u                 /* the editor's widget id ("NOTE") */
#define WRAP_CACHE      256

typedef enum { EOL_LF, EOL_CRLF, EOL_CR } EolStyle;
typedef enum { LANG_PLAIN, LANG_C, LANG_SHELL, LANG_MARKDOWN, LANG_COUNT } Lang;

/* ---------------------------------------------------------------- doc.c */

enum { DK_OTHER = 0, DK_TYPE, DK_BACKSPACE, DK_DELFWD };       /* edit kinds: the last three may merge into one undo step */
enum { UF_TYPING = 1 };

typedef struct UndoRec {
    size_t pos;
    char *del, *ins;                    /* text removed / inserted at pos */
    size_t del_len, ins_len;
    size_t cb, ab, ca, aa;              /* caret and anchor before / after */
    uint32_t group;                     /* records of one group are undone together */
    uint32_t t;                         /* time of the last merge (ms) */
    uint8_t flags;
} UndoRec;

typedef struct Doc {
    char *buf;                          /* gap buffer: text = buf[0, gs) + buf[ge, cap) */
    size_t cap, gs, ge;
    size_t *ls;                         /* line starts; ls[0] = 0, one entry per line */
    int nls, capls;
    size_t maxline;                     /* upper bound for the longest line, in bytes */
    uint32_t version;                   /* bumped by every change of the text */
    size_t caret, anchor;               /* selection: the caret moves, the anchor stays */
    int dirty_line;                     /* lowest line changed since the highlighter last looked (INT_MAX: none) */
    UndoRec *u;                         /* history: u[0, cur) can be undone, u[cur, nu) redone */
    int nu, capu, cur;
    size_t ubytes;
    int save_cur;                       /* value of cur when the text was last saved */
    bool save_lost;                     /* the saved state is no longer reachable by undo/redo */
    uint32_t next_group, cur_group;
    int depth;                          /* >0 inside doc_group_begin/end */
    bool undo_break;                    /* the next typed edit must not merge into the previous step */
} Doc;

void   nb_memmove(void *dst, const void *src, size_t n);
int    nb_u8_dec(const char *p, const char *end, uint32_t *cp);   /* bytes consumed (>= 1 when p < end), invalid bytes give U+FFFD */
bool   nb_u8_valid(const char *p, size_t n);
int    nb_char_class(uint32_t cp);                                /* 0 blank/newline, 1 punctuation, 2 word */

void   doc_init(Doc *d);
void   doc_free(Doc *d);
bool   doc_set_text(Doc *d, const char *text, size_t len);        /* replaces everything, clears the history */
static inline size_t doc_len(const Doc *d) { return d->cap - (d->ge - d->gs); }
static inline int    doc_nlines(const Doc *d) { return d->nls; }
static inline unsigned char doc_byte(const Doc *d, size_t off) { return (unsigned char)d->buf[off < d->gs ? off : off + (d->ge - d->gs)]; }
size_t doc_line_start(const Doc *d, int line);
size_t doc_line_end(const Doc *d, int line);                      /* offset of the newline (or the end of the text) */
int    doc_line_of(const Doc *d, size_t off);
void   doc_get(const Doc *d, size_t pos, size_t n, char *out);
const char *doc_ptr(Doc *d, size_t from, size_t to);              /* contiguous bytes [from, to); valid until the next edit or doc_ptr of another line */
const char *doc_all(Doc *d);                                      /* the whole text, contiguous */
size_t doc_next_char(const Doc *d, size_t off);
size_t doc_prev_char(const Doc *d, size_t off);
size_t doc_word_left(const Doc *d, size_t off);
size_t doc_word_right(const Doc *d, size_t off);
size_t doc_indent_end(const Doc *d, int line);                    /* offset of the first non-blank character of the line */

/* The caret ends up at caret_after (anchor too). Returns false when memory ran out (nothing changed). */
bool   doc_edit(Doc *d, size_t pos, size_t del, const char *ins, size_t n, size_t caret_after, int kind);
void   doc_group_begin(Doc *d);
void   doc_group_end(Doc *d);
bool   doc_undo(Doc *d);
bool   doc_redo(Doc *d);
static inline bool doc_can_undo(const Doc *d) { return d->cur > 0; }
static inline bool doc_can_redo(const Doc *d) { return d->cur < d->nu; }
bool   doc_modified(const Doc *d);
void   doc_mark_saved(Doc *d);
void   doc_break_undo(Doc *d);

/* ---------------------------------------------------------------- syntax.c */

typedef uint8_t SynState;
enum { SC_NORMAL, SC_COMMENT, SC_STRING, SC_NUMBER, SC_KEYWORD, SC_TYPE, SC_PREPROC, SC_HEADING, SC_EMPHASIS, SC_CODE, SC_LINK,
       SC_MARKER, SC_COUNT };

Lang        syn_detect(const char *path, const char *first_line, size_t len);
const char *syn_name(Lang l);
/* Scans one line (without its newline). Returns the state for the start of the next line; fills cls[0..len) if not NULL. */
SynState    syn_scan(Lang lang, const char *s, size_t len, SynState st, uint8_t *cls);
uint32_t    syn_color(int cls, const KTheme *th);

/* ---------------------------------------------------------------- editor.c */

typedef struct WrapEnt { int line; uint32_t ver; int cols, tabw; int n; size_t *rs; } WrapEnt;

typedef struct Editor {
    Doc doc;
    bool wrap, numbers, highlight, spaces;      /* view options; spaces: Tab inserts spaces */
    int tab_w;
    Lang lang;
    int top_line, top_sub, scroll_x;            /* first visible row (line + row inside a wrapped line), horizontal scroll in columns */
    int want_col;                               /* column kept while moving up and down (-1: none) */
    bool reveal;                                /* scroll the caret into view at the next draw */
    KRect box, text;                            /* geometry of the last draw */
    int gutter_w, vis_rows, vis_cols, cols;
    int drag;                                   /* 0 none, 1 characters, 2 words, 3 lines */
    size_t drag_lo, drag_hi;
    uint64_t last_dbl_ms, blink_ms;
    WrapEnt wc[WRAP_CACHE];
    uint8_t *syn; int syn_cap, syn_valid; uint32_t syn_ver; Lang syn_lang;
    uint8_t cls[NB_SYN_MAXLEN];
    int hscroll_max;
} Editor;

void   ed_init(Editor *e);
void   ed_free(Editor *e);
void   ed_reset_view(Editor *e);
void   ed_draw(Editor *e, KUI *ui, KRect r, bool active, bool blocked);
bool   ed_key(Editor *e, KUI *ui, const KKey *k);                 /* true if the key was used */
void   ed_reveal(Editor *e);
void   ed_select_all(Editor *e);
bool   ed_copy(Editor *e, bool cut);
bool   ed_paste(Editor *e);
bool   ed_undo(Editor *e);
bool   ed_redo(Editor *e);
void   ed_goto(Editor *e, int line, int col);                     /* 1-based; col 0 = start of line */
void   ed_caret_pos(Editor *e, int *line, int *col);              /* 0-based line, visual column from the start of the line */
bool   ed_has_selection(const Editor *e);
void   ed_selection(const Editor *e, size_t *lo, size_t *hi);
void   ed_set_selection(Editor *e, size_t anchor, size_t caret);
void   ed_insert_text(Editor *e, const char *s, size_t n);        /* replaces the selection; handles line endings */
void   ed_indent(Editor *e, bool dedent);
void   ed_duplicate_line(Editor *e);
void   ed_delete_line(Editor *e);
void   ed_move_lines(Editor *e, int dir);
void   ed_invalidate(Editor *e);                                  /* options changed: drop layout and highlight caches */
int    ed_rows_in_line(Editor *e, int line);
size_t ed_offset_at(Editor *e, int mx, int my);                     /* text offset under a window position (as of the last draw) */
bool   ed_replace_range(Editor *e, size_t pos, size_t del, const char *ins, size_t n, size_t caret_after);   /* one undo step */
int    ed_wrap_count(const char *t, size_t s, size_t e, int cols, int tw, size_t *rs, int cap);             /* word wrap rules (tested by --selftest) */

/* ---------------------------------------------------------------- find.c */

size_t nb_find(Doc *d, const char *pat, size_t plen, size_t from, bool match_case, bool backward);   /* SIZE_MAX if none */
int    nb_count_matches(Doc *d, const char *pat, size_t plen, bool match_case, int limit);
bool   find_next(bool backward, bool quiet);
bool   replace_current(void);
int    replace_all(void);
void   find_open(bool replace);
void   find_close(void);
int    findbar_height(void);
void   find_apply_focus(KUI *ui);
void   findbar_draw(KUI *ui, KRect r);

/* ---------------------------------------------------------------- fileio.c */

typedef struct FileInfo {
    EolStyle eol;
    bool mixed, bom, utf8_ok;
    size_t crlf, lf, cr;
} FileInfo;

int    nb_read_all(const char *path, char **data, size_t *len);          /* 0 or an errno value */
long   nb_find_nul(const char *data, size_t len);                          /* offset of the first NUL byte, -1 if none */
size_t nb_normalize(char *data, size_t len, FileInfo *info);              /* strips a BOM, converts line endings to LF in place */
int    nb_write_doc(const char *path, Doc *d, EolStyle eol, bool bom);   /* 0 or an errno value */
const char *nb_eol_name(EolStyle e);
void   nb_abspath(const char *in, char *out, size_t cap);
void   recent_load(void);
void   recent_add(const char *path);
void   recent_remove(const char *path);
void   recent_clear(void);
void   config_load(void);
void   config_save(void);

/* ---------------------------------------------------------------- dialogs.c */

typedef void (*ChoiceCb)(int choice, void *user);                         /* choice index, or the cancel index when closed */
void   dlg_choice(KApp *app, const char *title, const char *msg, KIcon icon, const char *const *labels, int n, int cancel, ChoiceCb cb, void *user);
void   dlg_message(KApp *app, const char *title, const char *msg, KIcon icon);
void   dlg_about(KApp *app);
void   dlg_help(KApp *app);
void   nb_modal_enter(void);
void   nb_modal_leave(void);

/* ---------------------------------------------------------------- main.c */

typedef struct App {
    KApp *app;
    KUI *mainui;                        /* the main window's UI, valid after the first frame */
    Editor ed;
    char path[NB_PATH_MAX];
    bool has_path;
    EolStyle eol;
    bool eol_mixed, bom, utf8_ok, readonly;
    int64_t mtime;
    char name[256];
    bool show_status, pref_spaces;
    char msg[200];
    uint64_t msg_until;
    char title[320];
    int modal;                          /* dialogs of ours that are open: the editor ignores input meanwhile */
    bool quitting, trace, in_draw, focus_init;
    int children;                       /* windows spawned by "New Window" that still need reaping */
    bool find_open, repl_open;
    char find_buf[256], repl_buf[256], find_prev[256];
    bool match_case;
    int match_count;
    bool count_valid;
    uint32_t count_ver;
    uint64_t count_due;
    bool want_find_focus, want_repl_focus, want_editor_focus, want_find_keep;
    char recent[NB_RECENT_MAX][NB_PATH_MAX];
    int nrecent;
    char *clip;
    size_t cliplen, clip_sys_len;
    uint64_t frame_ms;
} App;

extern App A;

void msg_set(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nb_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  selftest_run(void);

#endif
