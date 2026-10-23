/*
 * vi: a modal vi/vim-lite editor for TouhouOS.
 *
 *   buf.c     lines, undo, registers, marks, file reading and writing
 *   input.c   key decoding, type-ahead queue, macros, mappings
 *   screen.c  painting (wrapping lines, numbers, status line), command-line input
 *   motion.c  motions, text objects, searching
 *   normal.c  normal / visual / insert mode and the operators
 *   ex.c      the ":" commands
 *   main.c    start-up, argument list, signals
 *
 * Text is bytes; the cursor sits on a byte offset that always starts a UTF-8 code point (ptlib/pt_utf8.h). Bytes that are
 * not valid UTF-8 are single "raw byte" code points, so they survive a load and save unchanged.
 */
#ifndef VI_H
#define VI_H

#include "../ptlib/pt_term.h"
#include <ctype.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAXPATH 1024

typedef struct { char *s; unsigned len, cap; unsigned char gm; } Line;   /* cap == 0: the bytes belong to the file image (or the line is empty); gm: :global mark */
typedef struct { long l; int c; } Pos;                  /* line, byte offset */

/* A span of text. Charwise: [a, b) with b possibly at the end of a line (or at column 0 of a later line to take the newline);
 * linewise: lines a.l .. b.l; blockwise: lines a.l .. b.l, display columns vc1 .. vc2 (inclusive; toeol: up to the line end). */
typedef struct { Pos a, b; int type; int vc1, vc2, toeol; } Range;

typedef struct { char *s; size_t len; int type; } Reg;   /* type 'c', 'l' or 'b' */

enum { M_NORMAL, M_INSERT, M_REPLACE, M_VISUAL, M_VLINE, M_VBLOCK };

/* key codes: code points, 1..26 are Ctrl-A..Z, 27 is Esc; special keys come from pt_term.h (PT_K_*) */
#define K_ESC 27
#define K_ENTER 13
#define K_BS 127
#define K_NONE (-2)
#define K_EOF  (-1)
#define K_NOMAP 0x40000000                              /* flag on keys that came from a mapping (not mapped again) */

typedef struct {
    int number, rnu, list, ic, scs, ws, hls, is, wrap, et, ai, ro, showmode, ruler, cursorline, magic;
    int ts, sw, sts, tw;
} Opts;

/* ---- globals (defined in main.c) */
extern Line *L;                                         /* the buffer: L[0..nl) */
extern long nl, lcap;
extern char fname[MAXPATH];
extern int noeol, buf_empty, modified, file_ro, view_mode;
extern Opts o;
extern Pos cur;                                         /* cursor */
extern int curswant;                                    /* desired display column for j/k; -1 = end of line */
extern int mode;
extern long topline;                                    /* first line shown */
extern Pos marks[26], mark_ctx, mark_lt, mark_gt, mark_lb, mark_rb, mark_dot, mark_ins;
extern PtTerm term;
extern char msg[512];
extern int msg_err;
extern int quit_flag, exit_status;
extern int vis_mode;                                    /* M_VISUAL / M_VLINE / M_VBLOCK when selecting, else 0 */
extern Pos vis_start;
extern int recording;                                   /* register being recorded, or 0 */
extern char **argfiles;
extern int nargfiles, argidx;
extern int cmdline_active;
extern char cmdline_prompt[8];
extern PtBuf cmdline_text;
extern int cmdline_pos;
extern char last_search[256];
extern int last_search_dir;
extern char last_sub_pat[256], last_sub_rep[256], last_sub_flags[16];
extern int in_global;

/* ---- buf.c */
void  buf_clear(void);
void  buf_new_empty(void);
int   buf_load(const char *path, int *nlines, long *nbytes);
int   buf_write(const char *path, long from, long to, int append, long *nbytes);
Line *lineat(long i);
const char *ls(long i);                                 /* bytes of line i */
unsigned ll(long i);                                    /* length of line i */
void  ln_set(long i, const char *s, size_t n);
void  ln_insert(long at, const char *s, size_t n);
void  ln_delete(long at, long n);
void  text_insert(Pos p, const char *s, size_t n);      /* s may contain newlines */
void  text_delete(Pos a, Pos b);                        /* charwise [a, b) */
void  undo_begin(void);                                 /* the next change starts a new undo step */
void  undo_cursor(Pos p);
int   undo_do(int redo);                                /* 0 if nothing to do */
void  undo_line_restore(void);                          /* "U" */
void  undo_clear(void);
void  undo_mark_saved(void);
void  mark_modified(void);
char *range_text(const Range *r, size_t *len);
void  range_delete(const Range *r);
void  reg_set(int name, const char *s, size_t n, int type, int is_delete);
Reg  *reg_get(int name);
void  reg_free_all(void);
extern long undo_seq;

/* ---- input.c */
int   getkey(void);                                     /* next key (type-ahead first); redraws when it has to wait */
void  ta_push(const int *keys, int n);                  /* put keys in front of the type-ahead queue */
void  ta_push_str(const char *s, size_t n, int nomap);
int   ta_len(void);
void  ta_save(void **save);
void  ta_restore(void *save);
int   key_to_cp(int k);
int   cp_to_key(int cp);
void  macro_start(int reg);
void  macro_stop(void);
void  dot_begin(void);
void  dot_end(int keep);
void  dot_replay(int count);
void  dot_push(int k);
void  dot_nokeep_set(void);
int   dot_set(int on);
extern int in_dot_replay;
extern int normal_depth;                                /* > 0 inside :normal */
void  map_add(int imode, const char *lhs, const char *rhs);
int   map_del(int imode, const char *lhs);
void  map_list(int imode);
void  abbr_add(const char *lhs, const char *rhs);
const char *abbr_find(const char *w, size_t n);
extern int last_typed_key;

/* ---- screen.c */
void  redraw(void);
void  set_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  scr_init(void);
int   text_rows(void);
int   text_cols(void);
int   vcol_of(long l, int c);                           /* display column of byte offset c in line l (start of the glyph) */
int   vcol_end(long l, int c);                          /* last display column the glyph at c covers */
int   byte_at_vcol(long l, int vcol);                   /* byte offset of the glyph covering vcol (or the line end) */
int   line_vwidth(long l);
void  scroll_to_cursor(void);
void  scroll_lines(long n);                             /* ctrl-e / ctrl-y: move the window n lines */
int   cmdline_read(const char *prompt, const char *init, PtBuf *out);   /* 1 if accepted, 0 if cancelled */
void  force_repaint(void);
void  beep(void);

/* ---- motion.c */
typedef struct { Pos to; int linewise, incl, jump, keepcol, fail, vcol; } Motion;
int   motion(int c, int cnt, int hascnt, int op, Motion *m);   /* 1 if c was a motion key */
int   textobj(int c, int inner, int cnt, Range *r);
int   search_find(const char *pat, int dir, Pos from, Pos *out, int wrap, int accept_at);
int   re_compile(regex_t *re, const char *pat, int ic_override);
int   re_ic(const char *pat);
void  re_clear(void);
int   re_exec_at(regex_t *re, const char *s, size_t len, size_t start, regmatch_t *m, size_t nm);
int   cls(uint32_t cp, int big);                        /* word class: 0 blank, 1 punctuation, 2 word */
uint32_t cp_at(long l, int c);
int   next_c(long l, int c);                            /* byte offset of next code point (may be == len) */
int   prev_c(long l, int c);
int   last_c(long l);                                   /* offset of the last code point (0 for an empty line) */
int   first_nonblank(long l);
int   clamp_col(long l, int c, int allow_end);
void  push_jump(void);
extern char last_find_char[8];
extern int last_find_cmd, last_find_len;

/* ---- normal.c */
void  normal_command(void);
void  normal_run_keys(const char *keys, size_t n);      /* :normal */
void  insert_text_keys(void);
void  enter_insert(int cmdchar, int cnt);
void  set_cursor(Pos p);
void  fix_cursor(void);
void  do_op_range(int op, Range *r, int reg, int cnt);
void  join_lines(long first, long count, int spaces);
void  shift_lines(long first, long last, int amount);
void  reflow_lines(long first, long last);
void  visual_end(void);
int   put_reg(int regname, int after, int cnt, int gmode);
int   put_text(const char *src, size_t len, int type, int after, int cnt, int gmode);
void  ta_flush(void);
int   dot_available(void);
int   indent_of(long l);
void  set_indent(long l, int width);
extern int cmd_is_change;
extern int pending_force_op;

/* ---- ex.c */
int   ex_execute(const char *line);                     /* 1 ok, 0 error */
void  ex_line_prompt(const char *init);
int   ex_edit(const char *path, int force);
int   ex_write_all(int force);
int   ex_quit(int force);
int   ex_substitute_range(long l1, long l2, const char *pat, const char *rep, const char *flags);
void  ex_exit_cmd(void);
extern char last_ex[512];

/* ---- shared helpers */
void  die_restore(void);
void  pager_show(const char *text);
int   blank_line_pub(long l);
int   first_nonblank_raw(long l);
int   cur_start_line(void);
void  enter_insert_after_op(void);
int   cmd_ok_modify(void);
void  fix_cursor_end(void);
size_t pt_u8_prev_len(const char *s, size_t n);
extern int undo_nest;
extern int last_key_was_complete;
long  imin(long a, long b);
long  imax(long a, long b);
int   run_filter(const char *cmd, const char *input, size_t inlen, char **out, size_t *outlen, int *status);
void  fatal(const char *fmt, ...);

#endif
