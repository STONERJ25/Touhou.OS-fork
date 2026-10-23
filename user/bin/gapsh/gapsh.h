/* gapsh: the Gensokyo shell. Shared declarations. */
#ifndef GAPSH_H
#define GAPSH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <limits.h>
#include <termios.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* Words keep their quoting as marker bytes so expansion can tell literal text from active text. */
#define Q_SQ_BEGIN 0x01
#define Q_SQ_END   0x02
#define Q_DQ_BEGIN 0x03
#define Q_DQ_END   0x04
#define Q_ESC      0x05          /* the next byte is literal */

typedef enum {
    T_EOF, T_NEWLINE, T_WORD, T_SEMI, T_DSEMI, T_AMP, T_AND_IF, T_OR_IF, T_PIPE, T_LPAREN, T_RPAREN, T_REDIR,
} TokType;

typedef enum { R_IN, R_OUT, R_APPEND, R_DUPIN, R_DUPOUT, R_HEREDOC, R_OUTERR, R_OUTERR_APPEND } RedirKind;

typedef struct Redir {
    int fd;
    RedirKind kind;
    char *target;                /* word (unexpanded), or heredoc body */
    int heredoc_quoted;          /* delimiter was quoted: no expansion in the body */
    struct Redir *next;
} Redir;

typedef struct Token {
    TokType type;
    char *text;
    int fd;                      /* T_REDIR: explicit fd or -1 */
    RedirKind rkind;
} Token;

typedef enum {
    N_SIMPLE, N_PIPE, N_AND, N_OR, N_LIST, N_BG, N_SUBSHELL, N_GROUP, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_FUNC, N_NOT,
} NodeKind;

typedef struct Assign {
    char *name;
    char *value;
    struct Assign *next;
} Assign;

typedef struct CaseItem {
    char **patterns;
    int npat;
    struct Node *body;
    struct CaseItem *next;
} CaseItem;

typedef struct Node {
    NodeKind kind;
    struct Node *a, *b, *c;
    char **words;
    int nwords;
    Assign *assigns;
    Redir *redirs;
    char *name;
    CaseItem *items;
} Node;

/* ---- lexer / parser (lex.c, parse.c) ---- */
typedef struct Lexer {
    const char *src;
    size_t pos;
    Token tok;                   /* current lookahead */
    int have_tok;
    Redir *pending_heredocs[8];
    char *heredoc_delims[8];
    int npending;
    int incomplete;              /* ran out of input inside a quote, heredoc or compound command */
    int error;
    char errmsg[160];
} Lexer;

void  lex_init(Lexer *lx, const char *src);
Token *lex_peek(Lexer *lx);
void  lex_next(Lexer *lx);
Node *parse_program(Lexer *lx);            /* NULL on error or incomplete input; check lx->incomplete / lx->error */

/* ---- variables (vars.c) ---- */
const char *var_get(const char *name);
void  var_set(const char *name, const char *value);
void  var_export(const char *name);
void  var_unset(const char *name);
int   var_is_exported(const char *name);
void  vars_init(char **envp);
char **vars_environ(void);                 /* malloc'd envp for children */
void  vars_each(void (*fn)(const char *name, const char *value, int exported, void *ctx), void *ctx);
void  local_push(void);
void  local_pop(void);
void  local_declare(const char *name);

/* ---- expansion (expand.c) ---- */
char **expand_words(char **words, int n, int *out_n);   /* full expansion with splitting and globbing */
char  *expand_string(const char *w);                     /* single field: no splitting or globbing (assignments, redirects) */
char  *expand_heredoc(const char *body);
long   arith_eval(const char *expr, int *ok);
int    glob_match(const char *pat, const char *s);        /* '\' escapes */
char  *run_capture(const char *cmd);                      /* command substitution */

/* ---- execution (exec.c) ---- */
extern int g_status;
extern int g_interactive;
extern int g_shell_pgid;
extern int g_exit_requested;
extern int g_break, g_continue, g_return;
int   run_node(Node *n);
int   run_string(const char *src, const char *name);
int   run_file(const char *path, int argc, char **argv);
void  exec_init(void);
void  jobs_report(void);
void  jobs_reap(void);
int   job_wait_fg(int idx);
int   jobs_command(int argc, char **argv, const char *which);   /* jobs / fg / bg */
int   is_builtin(const char *name);
int   run_builtin(int argc, char **argv);
char *find_in_path(const char *name);
void  define_function(const char *name, Node *body);
Node *find_function(const char *name);
void  shell_error(const char *fmt, ...);
extern char **g_pos_params;
extern int g_npos;
extern char *g_arg0;

/* ---- editing (edit.c) ---- */
char *edit_readline(const char *prompt);
void  history_add(const char *line);
void  history_load(void);
void  history_save(void);
void  history_print(void);
int   history_count(void);
const char *history_get(int i);

/* ---- misc ---- */
char *xstrdup(const char *s);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xasprintf(const char *fmt, ...);

typedef struct { char **v; int n, cap; } StrVec;
void sv_push(StrVec *sv, char *s);

#endif
