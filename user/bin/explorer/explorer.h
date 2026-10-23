/* Gensokyo Explorer (explorer): the file manager of the desktop. Shared declarations.
 *
 *   util.c      paths, formatting, file types, shell quoting, tracing
 *   model.c     the folder model: sliced scanning, filtering, sorting, selection helpers
 *   fileops.c   the job engine: copy, move, trash, delete and restore as sliced jobs (no window needed)
 *   trash.c     the Netherworld: unique names, restore sidecars
 *   view.c      list and icon views: drawing, hit tests, selection, rubber band, drag and drop, type-ahead, inline rename
 *   menus.c     menu bar and context menu (drawn inside the window)
 *   dialogs.c   messages, progress, conflicts, error list, properties, help, about
 *   main.c      the window: toolbar, sidebar, address field, status bar, navigation, actions
 *   selftest.c  `explorer --selftest`: model and job engine tests on the console
 */
#ifndef EXPLORER_H
#define EXPLORER_H

#include <kappa.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <dirent.h>
#include <sys/stat.h>

#define EX_VERSION "1.0"
#define EX_PATH 1024
#define EX_NAME 256
#define NETHERWORLD "/Netherworld"
#define TRASHINFO "/Netherworld/.trashinfo"

/* ---------------------------------------------------------------- layout (window content coordinates) */

#define L_MENU_H 24
#define L_TOOL_Y 26
#define L_TOOL_H 34
#define L_MAIN_Y 62
#define L_SIDE_W 150
#define L_STATUS_H 22
#define L_BANNER_H 30                   /* the Netherworld banner above the view */
#define L_HEAD_H 22
#define L_ROW_H 20
#define L_CELL_W 96
#define L_CELL_H 84
#define L_SB_W 14
#define VIEW_ID 0xE0F11E01u             /* the keyboard focus id of the file view */

/* ---------------------------------------------------------------- entries and the folder model */

enum { EK_DIR, EK_FILE, EK_LINK, EK_OTHER };
enum { FT_GENERIC, FT_TEXT, FT_IMAGE, FT_PROGRAM, FT_ARCHIVE, FT_DEVICE, FT_PIPE, FT_SOCKET };
enum { COL_NAME, COL_SIZE, COL_MTIME, COL_TYPE, COL_PERM, COL_OWNER, COL_COUNT };

typedef struct Entry {
    char *name;
    char *key;                          /* folded name: sorting and type-ahead */
    int64_t size, mtime;
    uint32_t mode, uid, gid;
    uint8_t kind;                       /* EK_*, from lstat */
    uint8_t ftype;                      /* FT_* */
    bool is_dir;                        /* a folder, or a link to one */
    bool broken;                        /* a link to nothing */
    bool hidden;
    bool sel;
    bool cut;                           /* waiting in the cut clipboard: drawn faded */
} Entry;

typedef struct Model {
    char path[EX_PATH];
    Entry **all;
    int nall, cap;
    Entry **shown;                      /* filtered and sorted view of `all` */
    int nshown;
    bool scanning;
    DIR *dp;
    int err;                            /* errno of a failed open */
    int sort_col;
    bool sort_desc;
    bool show_hidden;
    int nhidden;
    int64_t dir_mtime;
    int64_t scanned_at;
} Model;

void model_init(Model *m);
void model_clear(Model *m);
void model_abort(Model *m);                               /* stops a scan and forgets its entries */
bool model_open(Model *m, const char *abs_path);          /* starts a scan; false (and m->err) if the folder cannot be read */
bool model_scan_step(Model *m, int budget_ms);            /* true when the scan has finished */
void model_refilter(Model *m);                            /* rebuilds `shown` (hidden files) and sorts */
void model_sort(Model *m);
int  model_find(const Model *m, const char *name);        /* index in shown or -1 */
int  model_prefix(const Model *m, const char *folded, int from, bool include_from);
void model_select_none(Model *m);
void model_select_all(Model *m);
void model_select_range(Model *m, int a, int b, bool on, bool clear_others);
int  model_sel_count(const Model *m, int64_t *bytes);
Entry *entry_make(const char *dir, const char *name);
void entry_free(Entry *e);

/* ---------------------------------------------------------------- util.c */

extern bool g_trace;
void ex_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void path_join(char *out, size_t cap, const char *dir, const char *name);
void path_parent(const char *path, char *out, size_t cap);
const char *path_base(const char *path);
bool path_normalize(const char *in, const char *cwd, char *out, size_t cap);
bool path_inside(const char *path, const char *dir);      /* path is dir or below it (lexical) */
void fmt_size(char *out, size_t cap, int64_t n);
void fmt_time(char *out, size_t cap, int64_t t);          /* 2026-10-23 16:04 */
void fmt_time_long(char *out, size_t cap, int64_t t);
void fmt_mode(char *out, uint32_t mode);                  /* drwxr-xr-x (11 bytes with the NUL) */
const char *uid_name(uint32_t uid);
const char *gid_name(uint32_t gid);
int  natcmp(const char *a, const char *b);
void fold_name(const char *in, char *out, size_t cap);    /* ASCII lower case */
int  classify(const char *name, uint32_t mode, int kind);
const char *type_name(const Entry *e, char *buf, size_t cap);
KIcon icon_for(const Entry *e);
void draw_entry_icon(KCanvas *cv, const Entry *e, int x, int y, int size, uint32_t fade_bg, int fade_alpha);
void sh_quote(char *out, size_t cap, const char *s);
bool name_valid(const char *name, const char **why);      /* usable as a single file name */
void unique_name(const char *dir, const char *base, char *out, size_t cap, const char *style);
bool path_exists(const char *path);                       /* lstat succeeds */
const char *ext_of(const char *name);                     /* "" when none */
void errtext(int err, char *out, size_t cap);
uint64_t ex_now_ms(void);

/* ---------------------------------------------------------------- the job engine (fileops.c, trash.c) */

typedef enum { OP_COPY, OP_MOVE, OP_TRASH, OP_DELETE, OP_RESTORE, OP_SIZE } OpKind;
typedef enum { CF_ASK, CF_REPLACE, CF_SKIP, CF_KEEP } ConflictPolicy;
typedef struct Job Job;

typedef struct JobInfo {
    OpKind kind;
    char title[64];
    char current[EX_NAME];
    int nsrc, items_done, nerr, nskipped;
    int64_t bytes_done, cur_size, cur_done;
    int64_t files, dirs, size_total;    /* OP_SIZE results; also counted by the other jobs */
    bool waiting;                       /* blocked on a conflict question */
    bool cancelling, finished;
    uint64_t elapsed_ms;
    char c_src[EX_PATH], c_dst[EX_PATH];
    bool c_src_dir, c_dst_dir;
    int64_t c_src_size, c_dst_size, c_src_mtime, c_dst_mtime;
} JobInfo;

typedef struct JobErr { char *path; char msg[112]; } JobErr;

Job *job_new(OpKind kind, const char *dest_dir);
void job_add(Job *j, const char *abs_path);
void job_set_policy(Job *j, ConflictPolicy p);
bool job_run(Job *j, int budget_ms);                      /* does a slice of work; true when the job is finished */
void job_info(const Job *j, JobInfo *out);
void job_cancel(Job *j);
void job_resolve(Job *j, ConflictPolicy choice, bool apply_to_all);
const JobErr *job_errors(const Job *j, int *n);
void job_free(Job *j);
const char *job_dest(const Job *j);
int  job_nsrc(const Job *j);
const char *job_src(const Job *j, int i);
int  job_ok_count(const Job *j);                          /* top-level items that went through */
uint32_t job_serial(const Job *j);                         /* unique per submitted job: a window must not trust a stale pointer */
int  job_restored_home(const Job *j);                     /* restored items whose original place was unknown */
bool job_cancelled(const Job *j);

/* the queue the window works on */
void jobs_submit(Job *j);                                  /* takes ownership; runs after the jobs ahead of it */
bool jobs_busy(void);
Job *jobs_front(void);
void jobs_step(int budget_ms);                             /* calls ex_job_done() for finished jobs */
void ex_job_done(Job *j);                                  /* main.c; the job is freed afterwards */
extern void (*g_job_done_hook)(Job *);                     /* the selftest takes finished jobs itself */
int  jobs_count(void);

/* trash.c */
void trash_unique(const char *base, char *out_path, size_t cap);          /* /Netherworld/base, base.1, ... */
bool trash_write_info(const char *trashed_name, const char *orig_path);
bool trash_read_info(const char *trashed_name, char *orig, size_t cap, int64_t *when);
void trash_remove_info(const char *trashed_name);
bool trash_ensure(void);
int  trash_count(void);                                                   /* entries in /Netherworld without the sidecar folder */

/* ---------------------------------------------------------------- menus.c */

#define MENU_MAX 40
typedef struct MItem { const char *label; const char *keys; int act; bool sep, disabled, checked; } MItem;
typedef struct Menu { MItem it[MENU_MAX]; int n; } Menu;
void menu_clear(Menu *m);
void menu_item(Menu *m, const char *label, const char *keys, int act, bool disabled, bool checked);
void menu_sep(Menu *m);
void ctx_open(KUI *ui, const Menu *m, int x, int y);      /* x, y: window coordinates */
bool ctx_is_open(void);
void ctx_close(void);
void ctx_key(const KKey *k);
int  ctx_run(KUI *ui);                                    /* draws; returns the chosen action once */
void mbar_open(KUI *ui, int m);
bool mbar_key(KUI *ui, const KKey *k, const Menu *menus, int nmenus);
int  mbar_run(KUI *ui, KRect bar, bool live, const char *const *titles, const Menu *menus, int nmenus);

/* ---------------------------------------------------------------- actions (main.c) */

enum {
    A_NONE, A_OPEN, A_OPEN_NOTEBOOK, A_OPEN_SKETCHBOOK, A_OPEN_TERMINAL, A_OPEN_NEWWIN, A_NEW_FOLDER, A_NEW_FILE, A_NEW_WINDOW,
    A_CLOSE, A_CUT, A_COPY, A_PASTE, A_PASTE_INTO, A_RENAME, A_TRASH, A_DELETE, A_RESTORE, A_EMPTY, A_SELALL, A_SELNONE, A_SELINV,
    A_PROPS, A_VIEW_LIST, A_VIEW_ICONS, A_HIDDEN, A_REFRESH, A_SORT_NAME, A_SORT_SIZE, A_SORT_MTIME, A_SORT_TYPE, A_SORT_PERM,
    A_SORT_OWNER, A_SORT_DESC, A_BACK, A_FWD, A_UP, A_HOME, A_ROOT, A_NETHERWORLD, A_ADDRESS, A_HELP, A_ABOUT, A_QUIT,
    A_PLACE_NEWWIN, A_TERMINAL_HERE,
};

/* ---------------------------------------------------------------- the window state shared by the files */

typedef struct Place { char label[48]; char path[EX_PATH]; KIcon icon; int kind; } Place;
enum { PK_HOME, PK_ROOT, PK_TRASH, PK_VOLUME };

typedef struct Ex {
    KApp *app;
    KUI *ui;                            /* the main window's UI during draw */
    Model m;
    bool trace;
    int modal;                          /* open dialogs that block the window */
    int view_mode;                      /* 0 list, 1 icons */
    bool in_netherworld;
    /* geometry of this frame (window coordinates) */
    KRect r_view, r_body, r_side, r_status, r_banner;
    int W, H;
    /* view state */
    int scroll;
    int cursor, anchor;
    int cols;                           /* icon view: cells per row */
    int content_h;
    bool view_focus;
    /* messages */
    char msg[200];
    uint64_t msg_until;
    /* places */
    Place places[16];
    int nplaces;
    /* navigation */
    char hist[32][EX_PATH];
    int hist_scroll[32];
    int nhist, ihist;
    uint32_t iter;
    bool quitting;
    int children;
} Ex;

extern Ex X;

void ex_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ex_action(int act);
void ex_navigate(const char *path, const char *select_name);
void ex_refresh(const char *select_name);
void ex_open_entry(const Entry *e);
void ex_open_path_with(const char *path, int how);        /* how: 0 default (pictures: the gallery), 1 notebook, 2 sketchbook, 3 terminal, 4 gallery */
void ex_selected_paths(char ***paths, int *n);            /* malloc'd; free each and the array */
void ex_rename_commit(const char *from_name, const char *to_name);
void ex_spawn_explorer(const char *path);
bool ex_clip_has(void);
int  ex_clip_count(void);
void ex_start_trash(char **paths, int n);
void ex_start_delete(char **paths, int n);
void ex_start_paste(const char *dest_dir);
void ex_start_drop(char **paths, int n, const char *dest_dir, int how);   /* how: 0 automatic, 1 copy, 2 move */
void ex_start_restore(char **paths, int n);
void ex_empty_netherworld(void);
void ex_new_item(bool folder);
void ex_properties(void);

/* ---------------------------------------------------------------- view.c */

void view_reset(void);                                     /* a new folder was opened */
void view_draw(KUI *ui, KRect r, bool blocked);            /* also handles the pointer of the view */
bool view_key(KUI *ui, const KKey *k);                     /* navigation and type-ahead; true when the key was used */
void view_scroll_to(int idx);
void view_set_cursor(int idx, bool extend);
void view_start_rename(int idx, bool is_new);
bool view_renaming(void);
bool view_busy(void);
void view_cancel_rename(void);
void view_cancel_drag(void);
bool view_dragging(void);
void view_ghost(KUI *ui);                                  /* drawn last, over everything */
int  view_entry_at(int mx, int my);                        /* index of the entry under a window position or -1 */
void view_save_selection(void);                            /* before a rescan */
void view_restore_selection(const char *select_name);      /* after it */
void view_model_changed(void);                             /* the entries were replaced: indexes kept by the view are stale */
void view_typeahead_clear(void);
const char *view_typeahead(void);
void view_context(KUI *ui, int mx, int my, int hit);      /* main.c: builds the context menu for what was right-clicked */
extern int g_drop_place;                                   /* sidebar place highlighted by a drag (-1 none), set by view.c */
int  ex_place_at(int mx, int my);                          /* main.c: the sidebar place under a window position, or -1 */
void ex_set_sort(int col);                                 /* main.c: sort by a column (again: reverse) */

int selftest_run(void);                                    /* selftest.c */

/* ---------------------------------------------------------------- dialogs.c */

typedef void (*ChoiceCb)(int choice, void *user);
void dlg_message(const char *title, const char *msg, KIcon icon);
void dlg_choice(const char *title, const char *msg, KIcon icon, const char *const *labels, int n, int cancel, ChoiceCb cb, void *user);
void dlg_errors(const char *title, const char *intro, const JobErr *errs, int n, int nskipped);
void dlg_jobs_update(void);                                /* progress and conflict windows follow the front job */
void dlg_properties(char **paths, int n);
void dlg_help(void);
void dlg_about(void);
void dlg_gc(void);
void dlg_close_all(void);
void dlg_modal_enter(void);
void dlg_modal_leave(void);

#endif
