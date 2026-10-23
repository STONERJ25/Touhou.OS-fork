/* Spell Manager (task manager): shared declarations. */
#ifndef SPELL_H
#define SPELL_H

#include <kappa.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ---------------------------------------------------------------- util.c */
ssize_t read_file(const char *path, char *buf, size_t cap);          /* whole (small) file, NUL terminated; -1 on error */
void    fmt_kb(unsigned long kb, char *out, size_t cap);              /* 412 kB, 1.2 MB, 3.4 GB */
void    fmt_bytes(unsigned long long bytes, char *out, size_t cap);
void    fmt_duration(unsigned long long ms, char *out, size_t cap);   /* 3 h 12 min, 45 s */
const char *user_name(unsigned uid);                                  /* cached getpwuid */
const char *state_word(char st);                                      /* "running", "sleeping", "zombie" */
void    trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern bool g_trace;                                                  /* --trace */

/* ---------------------------------------------------------------- procs.c */
typedef struct Proc {
    int pid, ppid, pgid;
    unsigned uid, gid;
    char state;
    unsigned long vsz_kb, rss_kb;
    unsigned long ticks;                 /* CPU time used so far, in milliseconds */
    unsigned long start_ms;              /* uptime at which it started */
    char name[32];
    char user[24];
    int cpu10;                           /* CPU use since the previous scan in tenths of a percent */
} Proc;

typedef struct ProcList {
    Proc *v;
    int n, cap;
    uint64_t at_ms;
} ProcList;

enum { COL_PID, COL_NAME, COL_USER, COL_STATE, COL_CPU, COL_MEM, COL_COUNT };

typedef struct ProcView {
    int *idx;                            /* indexes into the list, sorted and filtered */
    int n, cap;
} ProcView;

bool proc_read(int pid, Proc *p);
void proclist_scan(ProcList *cur, const ProcList *prev);
void proclist_free(ProcList *l);
void proclist_copy(ProcList *dst, const ProcList *src);
void procview_build(ProcView *view, const ProcList *list, int sort_col, bool descending, const char *filter);
const Proc *proclist_find(const ProcList *l, int pid);
bool proc_cmdline(int pid, char *out, size_t cap);
bool proc_cwd(int pid, char *out, size_t cap);

/* ---------------------------------------------------------------- tabs */
typedef struct App {
    KApp *app;
    int tab;
    ProcList procs, prev;
    ProcView view;
    uint64_t last_scan_ms;
    int sort_col;
    bool sort_desc;
    char filter[48];
    int selected_pid;                    /* 0: nothing selected */
    Proc selected_copy;                  /* last known data of the selected process */
    bool selected_gone;
    int scroll;                          /* first visible row of the table */
    bool force_refresh;
    char note[96];                       /* one line of feedback under the table */
    uint64_t note_until;
} App;
extern App A;

void spells_tab(KUI *ui, KRect body);
void spells_keys(KUI *ui);
void spells_refresh(void);               /* scans /proc */
void perf_sample(uint64_t now_ms);       /* called once a second whatever tab is showing */
void perf_tab(KUI *ui, KRect body);
void hardware_tab(KUI *ui, KRect body);
void hardware_refresh(void);
void users_tab(KUI *ui, KRect body);
void users_refresh(void);
int  perf_cpu_percent(void);             /* latest sample */
int  perf_mem_percent(void);
unsigned long long perf_uptime_ms(void);

/* ---------------------------------------------------------------- doc.c: a scrolling page of headings, key/value pairs and small tables */
enum { DOC_HEAD, DOC_KV, DOC_ROW, DOC_HEADROW, DOC_NOTE, DOC_GAP };
#define DOC_COLS 7                       /* at most 6 columns plus one sentinel offset */
typedef struct DocRow {
    int kind;
    char text[DOC_COLS][96];
    int x[DOC_COLS];                     /* pixel offsets of the columns */
} DocRow;
typedef struct Doc {
    DocRow *rows;
    int n, cap;
    int scroll;
} Doc;
void doc_clear(Doc *d);
void doc_free(Doc *d);
void doc_head(Doc *d, const char *text);
void doc_kv(Doc *d, const char *key, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void doc_note(Doc *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void doc_gap(Doc *d);
DocRow *doc_row(Doc *d, int kind, const int *xs, int ncols);         /* then fill row->text[i] */
void doc_draw(KUI *ui, KRect r, Doc *d);
void doc_dump(const Doc *d);             /* the page as plain text (spellmgr --hardware / --users) */
Doc *hardware_doc(void);
Doc *users_doc(void);

#endif
