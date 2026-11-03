/*
 * pnote -- Patchouli's Notebook, a small full-screen text editor.
 *
 * Source map:
 *   buf.c      the text: a gap array of lines, edit primitives, undo/redo, loading and saving
 *   edit.c     cursor movement and editing commands (typing, cut/paste, indentation, selection)
 *   view.c     layout and drawing: title bar, text rows, line numbers, hints, scrolling
 *   prompt.c   status messages, one-line prompts, yes/no questions, the help screen
 *   search.c   search, search again, replace
 *   hl.c       the modest syntax highlighter (C, shell, markdown)
 *   main.c     command line, key bindings, main loop
 */
#ifndef PNOTE_H
#define PNOTE_H

#include <ctype.h>
#include <regex.h>
#include <sys/stat.h>
#include "../ptlib/pt_term.h"

#define PNOTE_VERSION "1.0"

/* One line of text without its terminator. Lines read from a file borrow their bytes from the file image (cap == 0)
 * until they are first edited, so loading a very large file costs one pass over it and no per-line allocation. */
typedef struct {
    char *s;
    uint32_t len;
    uint32_t cap : 27;          /* bytes allocated for s; 0 = borrowed from the file image */
    uint32_t cr : 1;            /* the line was terminated by CR LF */
    uint32_t hl : 4;            /* highlighter state at the start of the line */
} Line;

enum { OP_INS = 1, OP_DEL = 2 };

typedef struct {
    int type;
    long ln;
    size_t col;
    char *text;                 /* the inserted / deleted text, "\r\n" marking a CR LF terminator */
    size_t len;
    long group;
} UndoOp;

enum { LANG_NONE = 0, LANG_C, LANG_SH, LANG_MD, LANG_COUNT };

/* kinds of command, for grouping typing runs into one undo step */
enum { K_OTHER = 0, K_TYPE, K_BS, K_DELCH, K_CUT };

typedef struct {
    /* text: gap array of lines */
    Line *a;
    long cap, gap, gaplen, n;
    char *blob;                 /* the file image that unedited lines point into */

    /* file */
    char *path;
    int final_newline;          /* the last line is terminated */
    int was_empty;              /* the buffer started out empty (new file): a terminator is added when text appears */
    int file_readonly;          /* the file exists but cannot be written */
    int new_file;               /* the path does not exist yet */
    int view_only;              /* -R: refuse edits */
    int from_stdin;             /* loaded from `pnote -` */
    int filter;                 /* `... | pnote - | ...`: the result goes to standard output */

    /* cursor and view */
    long cy;
    size_t cx;
    long want_col;              /* column to return to when moving vertically, -1 = unset */
    long top;                   /* first line shown */
    long hscroll;               /* columns scrolled off to the left */

    /* undo */
    UndoOp *ops;
    long nops, opcap, undo_pos, save_pos, group;
    size_t undo_bytes;
    int last_kind, prev_kind;
    int force_modified;

    /* clipboard */
    PtBuf clip;
    int clip_lines;
    int mark_on;
    long mark_ln;
    size_t mark_col;

    /* settings */
    int tabsize, tabs_to_spaces, autoindent, linenums, highlight, lang, lang_auto;

    /* search */
    char sbuf[256], rbuf[256];
    int s_regex, s_case;        /* s_case: 0 smartcase, 1 case-sensitive, 2 ignore case */
    int mh_on;                  /* highlight the last match */
    long mh_ln;
    size_t mh_c0, mh_c1;

    /* highlight cache */
    long hl_valid;              /* lines 0..hl_valid have a valid start state */

    /* terminal */
    PtTerm term;
    PtScreen scr;
    int rows, cols, text_rows, text_w, gutter;
    char msg[240];
    const char *prompt_label;   /* set while a one-line prompt is open */
    const char *prompt_text;
    size_t prompt_cur;
    int quit;
} Ed;

extern Ed E;

/* buf.c */
static inline Line *LN(long i) { return &E.a[i < E.gap ? i : i + E.gaplen]; }
void   buf_reset_empty(void);
int    buf_load_fd(int fd, const char *what);          /* 0 on success, -1 with errno */
size_t buf_insert_raw(long ln, size_t col, const char *text, size_t n, long *eln, size_t *ecol);
char  *buf_delete_raw(long l1, size_t c1, long l2, size_t c2, size_t *outlen);
void   edit_insert(long ln, size_t col, const char *text, size_t n, long *eln, size_t *ecol, int merge);
char  *edit_delete(long l1, size_t c1, long l2, size_t c2, size_t *outlen, int merge);
void   text_end_pos(long ln, size_t col, const char *text, size_t n, long *eln, size_t *ecol);
void   undo_boundary(void);
int    do_undo(void);
int    do_redo(void);
int    buf_modified(void);
void   buf_mark_saved(void);
int    buf_write_fd(int fd);
size_t buf_total_bytes(void);

/* edit.c */
void   cmd_move_left(void);
void   cmd_move_right(void);
void   cmd_move_up(void);
void   cmd_move_down(void);
void   cmd_home(void);
void   cmd_end(void);
void   cmd_page(int dir);
void   cmd_word(int dir);
void   cmd_top(void);
void   cmd_bottom(void);
void   cmd_goto(long line, long col);
void   cmd_insert_cp(uint32_t cp);
void   cmd_newline(void);
void   cmd_backspace(void);
void   cmd_delete(void);
void   cmd_tab(void);
void   cmd_unindent(void);
void   cmd_cut(void);
void   cmd_copy(void);
void   cmd_paste(void);
void   cmd_toggle_mark(void);
void   cmd_clear_mark(void);
int    edit_allowed(void);
void   clamp_cursor(void);
void   remember_col(void);
void   begin_cmd(int kind);
int    sel_range(long *l1, size_t *c1, long *l2, size_t *c2);
void   insert_text_at_cursor(const char *text, size_t n);

/* view.c */
void   view_layout(void);
void   view_scroll_to_cursor(void);
void   view_draw(void);
long   line_dispcol(const Line *L, size_t off);
size_t line_off_for_col(const Line *L, long col);
int    cp_width(uint32_t cp, long col);
int    gutter_width(void);
void   view_resize_check(void);

/* prompt.c */
void   set_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    prompt_line(char *label, size_t labcap, char *buf, size_t cap, int (*onkey)(int key, char *label, size_t labcap));
int    ask_key(const char *question, const char *keys);   /* returns the lower-case key pressed, or 0 for cancel */
void   show_help(void);
int    next_key(void);                                     /* blocks, repaints on idle, handles resizes */

/* search.c */
void   cmd_search(void);
void   cmd_search_again(int dir);
void   cmd_replace(void);

/* hl.c */
int    hl_line(int lang, const char *s, size_t n, int state_in, unsigned char *styles);
int    hl_detect(const char *path, const char *first_line, size_t n);
const char *hl_style_sgr(int style);
const char *hl_lang_name(int lang);
void   hl_prepare(long upto);
void   hl_invalidate(long from_line);

/* main.c */
int    save_buffer(int ask_name);          /* 1 on success */
void   do_exit(void);
void   toggle_menu(void);

#endif
