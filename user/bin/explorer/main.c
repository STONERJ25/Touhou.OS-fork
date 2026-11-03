/* Gensokyo Explorer: the file manager of the Gensokyo desktop. This file is the application proper: the window layout (menu bar,
 * toolbar, places, address field, banner, status bar), navigation and history, the clipboard, the commands, and starting jobs and
 * other programs.
 *
 *   explorer [path]         open a folder (a file: its folder, with the file selected); no path: your home folder
 *   explorer --selftest     model and job engine tests on the console (no window server needed)
 *
 * EXPLORER_TRACE=1 in the environment makes it print "ex: ..." lines on stdout (used by the tests).
 *
 * Window layout (content coordinates, default size 800 x 540): menu bar y 0..23, toolbar y 26..59, places (x 0..149) and the file
 * view (x 152..797) from y 62, status bar in the last 22 rows. The first window of a session sits at (44, 56) on the screen. */
#include "explorer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

Ex X = { .cursor = -1, .anchor = -1 };
void (*g_job_done_hook)(Job *) = NULL;

#define TB_Y (L_TOOL_Y + 3)
#define SIDE_HEAD_H 22
#define SIDE_ROW_H 26

static char g_addr[EX_PATH];
static char g_free_text[96];
static uint64_t g_free_ms, g_tick_ms, g_scan_t0;
static struct { char select[EX_NAME]; bool restore_sel; int scroll; char rename[EX_NAME]; } PEND = { .scroll = -1 };
static int g_last_nsel = -1;
static char g_title[160];
static bool g_init;
static char g_initial_error[300];
static Menu g_menus[5];
static int g_ctx_place = -1;

static uint32_t field_id(const char *buf) { return (uint32_t)(uintptr_t)buf * 2654435761u | 1; }     /* the id k_textfield derives */
static void trace_order(void);

/* ---------------------------------------------------------------- messages */

void ex_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(X.msg, sizeof X.msg, fmt, ap);
    va_end(ap);
    X.msg_until = k_now_ms() + 6000;
    ex_trace("msg %s", X.msg);
    if (X.app) kapp_redraw(X.app);
}

/* ---------------------------------------------------------------- preferences */

static void prefs_file(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.config/explorer.conf", home && *home ? home : "/tmp");
}

static void prefs_load(void) {
    char p[EX_PATH];
    prefs_file(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        char *nl = strchr(eq, '\n');
        if (nl) *nl = 0;
        if (!strcmp(line, "view")) X.view_mode = !strcmp(eq, "icons") ? 1 : 0;
        else if (!strcmp(line, "sort")) { int c = atoi(eq); if (c >= 0 && c < COL_COUNT) X.m.sort_col = c; }
        else if (!strcmp(line, "desc")) X.m.sort_desc = atoi(eq) != 0;
        else if (!strcmp(line, "hidden")) X.m.show_hidden = atoi(eq) != 0;
    }
    fclose(f);
}

static void prefs_save(void) {
    char p[EX_PATH], dir[EX_PATH];
    prefs_file(p, sizeof p);
    path_parent(p, dir, sizeof dir);
    mkdir(dir, 0755);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "view=%s\nsort=%d\ndesc=%d\nhidden=%d\n", X.view_mode ? "icons" : "list", X.m.sort_col, X.m.sort_desc ? 1 : 0,
            X.m.show_hidden ? 1 : 0);
    fclose(f);
}

/* ---------------------------------------------------------------- places */

static void place_add(const char *label, const char *path, KIcon icon, int kind) {
    if (X.nplaces >= 16) return;
    if (kind == PK_VOLUME) for (int i = 0; i < X.nplaces; i++) if (!strcmp(X.places[i].path, path)) return;
    Place *p = &X.places[X.nplaces++];
    snprintf(p->label, sizeof p->label, "%s", label);
    snprintf(p->path, sizeof p->path, "%s", path);
    p->icon = icon;
    p->kind = kind;
}

static void places_build(void) {
    X.nplaces = 0;
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/";
    place_add("Home", home, KI_HOME, PK_HOME);
    place_add("Root (/)", "/", KI_DRIVE, PK_ROOT);
    place_add("Netherworld", NETHERWORLD, KI_TRASH, PK_TRASH);
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char src[96], target[160], type[32];
        if (sscanf(line, "%95s %159s %31s", src, target, type) != 3) continue;
        if (!strcmp(target, "/") || !strcmp(type, "devfs") || !strcmp(type, "procfs") || !strcmp(type, "sysfs")) continue;
        if (path_inside(target, "/proc") || path_inside(target, "/dev") || path_inside(target, "/sys")) continue;
        place_add(path_base(target), target, KI_DRIVE, PK_VOLUME);
    }
    fclose(f);
}

static KRect place_rect(int i) {
    int y = L_MAIN_Y + 4 + SIDE_HEAD_H;
    if (i < 3) y += i * SIDE_ROW_H;
    else y += 3 * SIDE_ROW_H + SIDE_HEAD_H + 6 + (i - 3) * SIDE_ROW_H;
    return kr(3, y, L_SIDE_W - 6, SIDE_ROW_H - 2);
}

int ex_place_at(int mx, int my) {
    if (mx < 0 || mx >= L_SIDE_W) return -1;
    for (int i = 0; i < X.nplaces; i++) if (kr_contains(place_rect(i), mx, my)) return i;
    return -1;
}

/* ---------------------------------------------------------------- clipboard (a file in /tmp, so another window can paste) */

static struct { bool cut; char **p; int n; int64_t sig_m, sig_s; } CL;

static void clip_file(char *out, size_t cap) { snprintf(out, cap, "/tmp/.explorer-clip-%u", (unsigned)getuid()); }

static void clip_free(void) {
    for (int i = 0; i < CL.n; i++) free(CL.p[i]);
    free(CL.p);
    CL.p = NULL;
    CL.n = 0;
}

static void mark_cut(void) {
    for (int i = 0; i < X.m.nall; i++) X.m.all[i]->cut = false;
    if (!CL.cut) return;
    for (int i = 0; i < CL.n; i++) {
        char parent[EX_PATH];
        path_parent(CL.p[i], parent, sizeof parent);
        if (strcmp(parent, X.m.path)) continue;
        int k = model_find(&X.m, path_base(CL.p[i]));
        if (k >= 0) X.m.shown[k]->cut = true;
    }
}

static void clip_sign(void) {
    char f[EX_PATH];
    struct stat st;
    clip_file(f, sizeof f);
    if (stat(f, &st) == 0) { CL.sig_m = st.st_mtime; CL.sig_s = st.st_size; } else { CL.sig_m = 0; CL.sig_s = -1; }
}

static void clip_set(bool cut, char **paths, int n) {
    clip_free();
    CL.p = calloc((size_t)(n ? n : 1), sizeof *CL.p);
    if (!CL.p) return;
    for (int i = 0; i < n; i++) CL.p[CL.n++] = strdup(paths[i]);
    CL.cut = cut;
    char f[EX_PATH];
    clip_file(f, sizeof f);
    FILE *fp = fopen(f, "w");
    if (fp) {
        fprintf(fp, "%s\n", cut ? "cut" : "copy");
        for (int i = 0; i < n; i++) fprintf(fp, "%s\n", paths[i]);
        fclose(fp);
    }
    clip_sign();
    mark_cut();
    ex_trace("clipboard %s %d", cut ? "cut" : "copy", n);
}

static void clip_clear(void) {
    char f[EX_PATH];
    clip_free();
    clip_file(f, sizeof f);
    unlink(f);
    clip_sign();
}

/* Reads the clipboard file again when another window (or we) changed it. */
static void clip_poll(bool force) {
    char f[EX_PATH];
    struct stat st;
    clip_file(f, sizeof f);
    if (stat(f, &st) != 0) {
        if (CL.n && CL.sig_s >= 0) { clip_free(); mark_cut(); }
        CL.sig_m = 0;
        CL.sig_s = -1;
        return;
    }
    if (!force && st.st_mtime == CL.sig_m && st.st_size == CL.sig_s) return;
    FILE *fp = fopen(f, "r");
    if (!fp) return;
    clip_free();
    char line[EX_PATH + 8];
    bool first = true;
    while (fgets(line, sizeof line, fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (first) { CL.cut = !strcmp(line, "cut"); first = false; continue; }
        if (line[0] != '/') continue;
        char **np = realloc(CL.p, (size_t)(CL.n + 1) * sizeof *np);
        if (!np) break;
        CL.p = np;
        CL.p[CL.n++] = strdup(line);
    }
    fclose(fp);
    CL.sig_m = st.st_mtime;
    CL.sig_s = st.st_size;
    mark_cut();
}

bool ex_clip_has(void) { return CL.n > 0; }
int ex_clip_count(void) { return CL.n; }

/* ---------------------------------------------------------------- selection helpers */

void ex_selected_paths(char ***paths, int *n) {
    int cnt = model_sel_count(&X.m, NULL);
    char **p = calloc((size_t)(cnt ? cnt : 1), sizeof *p);
    int k = 0;
    if (p) {
        for (int i = 0; i < X.m.nshown && k < cnt; i++) {
            if (!X.m.shown[i]->sel) continue;
            char full[EX_PATH];
            path_join(full, sizeof full, X.m.path, X.m.shown[i]->name);
            p[k++] = strdup(full);
        }
    }
    *paths = p;
    *n = k;
}

static void free_paths(char **p, int n) {
    for (int i = 0; i < n; i++) free(p[i]);
    free(p);
}

static int first_selected(void) {
    for (int i = 0; i < X.m.nshown; i++) if (X.m.shown[i]->sel) return i;
    return -1;
}

/* ---------------------------------------------------------------- window title, free space */

static void update_title(void) {
    char t[160];
    if (!strcmp(X.m.path, NETHERWORLD)) snprintf(t, sizeof t, "Explorer - Netherworld");
    else if (strlen(X.m.path) > 38) snprintf(t, sizeof t, "Explorer - ...%s", X.m.path + strlen(X.m.path) - 35);
    else snprintf(t, sizeof t, "Explorer - %s", X.m.path);
    if (strcmp(t, g_title)) {
        snprintf(g_title, sizeof g_title, "%s", t);
        kapp_set_title(X.app, t);
    }
}

static void update_free(void) {
    struct statfs sf;
    g_free_text[0] = 0;
    if (statfs(X.m.path, &sf) == 0 && sf.f_blocks) {
        char a[32], b[32];
        fmt_size(a, sizeof a, (int64_t)(sf.f_bavail * sf.f_bsize));
        fmt_size(b, sizeof b, (int64_t)(sf.f_blocks * sf.f_bsize));
        snprintf(g_free_text, sizeof g_free_text, "%s free of %s", a, b);
    }
    g_free_ms = k_now_ms();
}

/* ---------------------------------------------------------------- opening folders */

static void on_scan_done(void) {
    X.cursor = X.anchor = -1;
    mark_cut();
    if (PEND.restore_sel) view_restore_selection(PEND.select[0] ? PEND.select : NULL);
    else if (PEND.select[0]) view_restore_selection(PEND.select);
    if (PEND.scroll >= 0 && !PEND.select[0]) X.scroll = PEND.scroll;
    PEND.scroll = -1;
    PEND.select[0] = 0;
    PEND.restore_sel = false;
    if (PEND.rename[0]) {
        int i = model_find(&X.m, PEND.rename);
        if (i >= 0) view_start_rename(i, true);
        PEND.rename[0] = 0;
    }
    update_free();
    ex_trace("dir %s items=%d hidden=%d ms=%lld", X.m.path, X.m.nshown, X.m.nhidden, (long long)(k_now_ms() - g_scan_t0));
    trace_order();
}

static void report(const char *title, const char *what, const char *path, int err) {
    char msg[EX_PATH + 240];
    if (err) snprintf(msg, sizeof msg, "%s\n%s\n\n%s", what, path, strerror(err));
    else snprintf(msg, sizeof msg, "%s\n%s", what, path);
    dlg_message(title, msg, KI_ERROR);
}

static void abort_refresh(void);

static bool open_folder(const char *abs, const char *select_name, int scroll, bool loud) {
    struct stat st;
    if (stat(abs, &st) != 0) {
        if (loud) report("No such place", "Nothing is there (any more):", abs, errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (loud) report("Not a folder", "This is not a folder:", abs, 0);
        return false;
    }
    if (!model_open(&X.m, abs)) {
        if (loud) report("Cannot open the folder", "This folder cannot be read:", abs, X.m.err);
        return false;
    }
    abort_refresh();
    view_reset();
    g_scan_t0 = k_now_ms();
    PEND.restore_sel = false;
    PEND.scroll = scroll;
    PEND.rename[0] = 0;
    snprintf(PEND.select, sizeof PEND.select, "%s", select_name ? select_name : "");
    X.in_netherworld = !strcmp(abs, NETHERWORLD);
    update_title();
    ex_trace("nav %s", abs);
    return true;
}

static void hist_push(const char *path) {
    if (X.nhist && !strcmp(X.hist[X.ihist], path)) return;
    X.nhist = X.nhist ? X.ihist + 1 : 0;
    if (X.nhist == 32) {
        memmove(X.hist[0], X.hist[1], 31 * sizeof X.hist[0]);
        memmove(&X.hist_scroll[0], &X.hist_scroll[1], 31 * sizeof X.hist_scroll[0]);
        X.nhist = 31;
    }
    snprintf(X.hist[X.nhist], sizeof X.hist[0], "%s", path);
    X.hist_scroll[X.nhist] = 0;
    X.ihist = X.nhist;
    X.nhist++;
}

void ex_navigate(const char *path, const char *select_name) {
    char abs[EX_PATH], sel[EX_NAME];
    sel[0] = 0;
    if (select_name) snprintf(sel, sizeof sel, "%s", select_name);
    if (!path_normalize(path, X.m.path[0] ? X.m.path : "/", abs, sizeof abs)) {
        dlg_message("Cannot go there", "That address is too long.", KI_ERROR);
        return;
    }
    struct stat st;
    if (stat(abs, &st) == 0 && !S_ISDIR(st.st_mode)) {                 /* a file: show it in its folder */
        char parent[EX_PATH];
        snprintf(sel, sizeof sel, "%s", path_base(abs));
        path_parent(abs, parent, sizeof parent);
        snprintf(abs, sizeof abs, "%s", parent);
    }
    if (X.nhist) X.hist_scroll[X.ihist] = X.scroll;
    if (open_folder(abs, sel, 0, true)) hist_push(abs);
}

static void hist_go(int delta) {
    int ni = X.ihist + delta;
    if (X.nhist == 0 || ni < 0 || ni >= X.nhist) return;
    X.hist_scroll[X.ihist] = X.scroll;
    char path[EX_PATH];
    snprintf(path, sizeof path, "%s", X.hist[ni]);
    if (open_folder(path, NULL, X.hist_scroll[ni], true)) X.ihist = ni;
}

static void go_up(void) {
    if (!strcmp(X.m.path, "/")) { ex_msg("This is the top: there is nothing above the root."); return; }
    char parent[EX_PATH], child[EX_NAME];
    path_parent(X.m.path, parent, sizeof parent);
    snprintf(child, sizeof child, "%s", path_base(X.m.path));
    ex_navigate(parent, child);
}

/* Reading the folder again happens in a second model that replaces the first when it is complete: nothing flickers, the scroll
 * position stays, and a click that lands meanwhile is not lost. */
static Model g_rm;
static bool g_refreshing;

static void abort_refresh(void) {
    if (g_refreshing) model_abort(&g_rm);
    g_refreshing = false;
}

static void finish_refresh(void) {
    view_save_selection();
    Model old = X.m;
    g_rm.sort_col = old.sort_col;                                       /* settings changed while it was being read */
    g_rm.sort_desc = old.sort_desc;
    g_rm.show_hidden = old.show_hidden;
    X.m = g_rm;
    g_rm = old;
    model_clear(&g_rm);
    model_refilter(&X.m);
    view_model_changed();
    on_scan_done();
}

void ex_refresh(const char *select_name) {
    char path[EX_PATH];
    snprintf(path, sizeof path, "%s", X.m.path);
    int scroll = X.scroll;
    if (!X.m.scanning) {                                                /* the usual case: read it again in the background */
        abort_refresh();
        g_rm.sort_col = X.m.sort_col;
        g_rm.sort_desc = X.m.sort_desc;
        g_rm.show_hidden = X.m.show_hidden;
        if (model_open(&g_rm, path)) {
            g_refreshing = true;
            g_scan_t0 = k_now_ms();
            PEND.restore_sel = true;
            PEND.scroll = -1;
            snprintf(PEND.select, sizeof PEND.select, "%s", select_name ? select_name : "");
            return;
        }
    }
    view_save_selection();
    view_cancel_drag();
    view_cancel_rename();
    if (!model_open(&X.m, path)) {                                      /* the folder is gone: show the nearest one that is not */
        char up[EX_PATH];
        snprintf(up, sizeof up, "%s", path);
        for (;;) {
            if (!strcmp(up, "/")) break;
            char parent[EX_PATH];
            path_parent(up, parent, sizeof parent);
            snprintf(up, sizeof up, "%s", parent);
            if (open_folder(up, NULL, 0, false)) { hist_push(up); ex_msg("That folder is gone. This is the nearest one left."); return; }
        }
        open_folder("/", NULL, 0, false);
        return;
    }
    X.cursor = X.anchor = -1;
    PEND.restore_sel = true;
    PEND.scroll = scroll;
    snprintf(PEND.select, sizeof PEND.select, "%s", select_name ? select_name : "");
}

static void auto_refresh(void) {
    if (X.m.scanning || g_refreshing || X.modal || view_busy() || ctx_is_open() || (X.ui && X.ui->menu_open) || jobs_busy()) return;
    struct stat st;
    if (stat(X.m.path, &st) != 0) { ex_refresh(NULL); return; }
    time_t now = time(NULL);
    bool changed = st.st_mtime != X.m.dir_mtime;
    bool recent = X.m.dir_mtime >= X.m.scanned_at - 1 && now > X.m.scanned_at + 1;          /* touched around the scan: look once more */
    if (changed || recent) {
        ex_trace("auto refresh");
        ex_refresh(NULL);
    }
}

/* ---------------------------------------------------------------- starting other programs */

static void spawn(const char *prog, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { ex_msg("Could not start %s: %s", path_base(prog), strerror(errno)); return; }
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        for (int fd = 3; fd < 64; fd++) close(fd);                      /* the window server connection stays ours alone */
        execv(prog, argv);
        _exit(127);
    }
    X.children++;
}

static void reap_children(void) {
    while (X.children > 0 && waitpid(-1, NULL, WNOHANG) > 0) X.children--;
}

void ex_spawn_explorer(const char *path) {
    char *argv[3] = { "explorer", (char *)path, NULL };
    spawn("/bin/explorer", argv);
}

static void spawn_terminal(const char *title, const char *cmd) {
    char *argv[6] = { "gapterm", "-t", (char *)title, "-e", (char *)cmd, NULL };
    spawn("/bin/gapterm", argv);
}

/* A script whose #! line names a shell that does not exist here (there is no /bin/sh) is handed to gapsh instead. */
static bool script_needs_gapsh(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[200];
    bool need = false;
    if (fgets(line, sizeof line, f) && line[0] == '#' && line[1] == '!') {
        char *interp = line + 2;
        while (*interp == ' ') interp++;
        char *end = interp;
        while (*end && *end != ' ' && *end != '\n' && *end != '\r') end++;
        *end = 0;
        const char *base = path_base(interp);
        if (access(interp, X_OK) != 0 && (!strcmp(base, "sh") || !strcmp(base, "bash") || !strcmp(base, "gapsh") || !strcmp(base, "ash") || !strcmp(base, "dash"))) need = true;
    }
    fclose(f);
    return need;
}

static void run_in_terminal(const char *path) {
    char dir[EX_PATH], qdir[EX_PATH * 2], qprog[EX_PATH * 2 + 16], qmsg[EX_NAME * 2 + 80], msg[EX_NAME + 80], cmd[EX_PATH * 5], title[EX_NAME + 16];
    path_parent(path, dir, sizeof dir);
    sh_quote(qdir, sizeof qdir, dir);
    if (script_needs_gapsh(path)) {
        char q[EX_PATH * 2];
        sh_quote(q, sizeof q, path);
        snprintf(qprog, sizeof qprog, "gapsh %s", q);
    } else sh_quote(qprog, sizeof qprog, path);
    snprintf(msg, sizeof msg, "[%s finished - press Enter to close]", path_base(path));
    sh_quote(qmsg, sizeof qmsg, msg);
    snprintf(cmd, sizeof cmd, "cd %s && %s; echo; echo %s; read _k", qdir, qprog, qmsg);
    snprintf(title, sizeof title, "Terminal - %s", path_base(path));
    ex_trace("open %s with terminal", path);
    spawn_terminal(title, cmd);
}

static bool sniff_text(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char buf[512];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0) return false;
    int odd = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == 0) return false;
        if (buf[i] < 9 || (buf[i] > 13 && buf[i] < 32)) odd++;
    }
    return odd * 20 <= n;
}

static void open_as_text_answer(int choice, void *user) {
    char *path = user;
    if (choice == 0) ex_open_path_with(path, 1);
    free(path);
}

void ex_open_path_with(const char *path, int how) {
    struct stat st;
    if (stat(path, &st) != 0) { report("Cannot open", "This does not exist (any more):", path, errno); return; }
    if (S_ISDIR(st.st_mode)) { ex_navigate(path, NULL); return; }
    int kind = how;
    if (kind == 0) {
        int ft = classify(path_base(path), st.st_mode, S_ISREG(st.st_mode) ? EK_FILE : EK_OTHER);
        if (ft == FT_IMAGE) kind = 4;                                    /* a viewer; "Open with Sketchbook" edits it */
        else if (ft == FT_PROGRAM) kind = 3;
        else if (ft == FT_TEXT) kind = 1;
        else if (S_ISREG(st.st_mode) && sniff_text(path)) kind = 1;
        else {
            char *copy = strdup(path);
            static const char *const labels[] = { "Open as text", "Cancel" };
            char msg[EX_PATH + 200];
            snprintf(msg, sizeof msg, "None of the tools in Gensokyo knows what to do with \"%s\". It does not look like text.", path_base(path));
            if (copy) dlg_choice("Unknown kind of file", msg, KI_QUESTION, labels, 2, 1, open_as_text_answer, copy);
            return;
        }
    }
    if (kind == 3) {
        if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
            report("Cannot run it", "This file is not a program (it has no execute permission):", path, 0);
            return;
        }
        run_in_terminal(path);
        return;
    }
    const char *prog = kind == 4 ? "gallery" : kind == 2 ? "sketchbook" : "notebook";
    char *argv[3] = { (char *)prog, (char *)path, NULL };
    char bin[32];
    snprintf(bin, sizeof bin, "/bin/%s", prog);
    ex_trace("open %s with %s", path, argv[0]);
    spawn(bin, argv);
}

void ex_open_entry(const Entry *e) {
    char full[EX_PATH];
    path_join(full, sizeof full, X.m.path, e->name);
    if (e->is_dir) ex_navigate(full, NULL);
    else ex_open_path_with(full, 0);
}

static void open_selection(void) {
    int n = model_sel_count(&X.m, NULL);
    if (n == 0 && X.cursor >= 0 && X.cursor < X.m.nshown) { ex_open_entry(X.m.shown[X.cursor]); return; }
    if (n == 1) { ex_open_entry(X.m.shown[first_selected()]); return; }
    int opened = 0;
    bool viewer = false;                                                 /* one Gallery walks the whole folder: no window per picture */
    for (int i = 0; i < X.m.nshown && opened < 8; i++)
        if (X.m.shown[i]->sel && !X.m.shown[i]->is_dir) {
            if (X.m.shown[i]->ftype == FT_IMAGE) { if (viewer) continue; viewer = true; }
            ex_open_entry(X.m.shown[i]);
            opened++;
        }
    if (!opened && n > 0) ex_open_entry(X.m.shown[first_selected()]);
}

/* ---------------------------------------------------------------- jobs */

static Job *make_job(OpKind k, const char *dest, char **paths, int n) {
    Job *j = job_new(k, dest);
    if (!j) return NULL;
    for (int i = 0; i < n; i++) job_add(j, paths[i]);
    return j;
}

void ex_start_trash(char **paths, int n) {
    Job *j = make_job(OP_TRASH, NULL, paths, n);
    if (j) jobs_submit(j);
}

typedef struct { char **paths; int n; bool empty; } PendingDelete;

static void delete_answer(int choice, void *user) {
    PendingDelete *p = user;
    if (choice == 0) {
        Job *j = make_job(OP_DELETE, NULL, p->paths, p->n);
        if (j) jobs_submit(j);
    }
    free_paths(p->paths, p->n);
    free(p);
}

void ex_start_delete(char **paths, int n) {
    if (n <= 0) return;
    PendingDelete *p = calloc(1, sizeof *p);
    if (!p) return;
    p->paths = calloc((size_t)n, sizeof *p->paths);
    if (!p->paths) { free(p); return; }
    for (int i = 0; i < n; i++) p->paths[i] = strdup(paths[i]);
    p->n = n;
    char msg[EX_PATH + 240];
    if (n == 1) snprintf(msg, sizeof msg, "Delete \"%s\" for good? Yuyuko will eat it, and the Netherworld cannot give it back.", path_base(paths[0]));
    else snprintf(msg, sizeof msg, "Delete these %d things for good? Yuyuko will eat them, and the Netherworld cannot give them back.", n);
    static const char *const labels[] = { "Delete for good", "Cancel" };
    dlg_choice("Delete for good?", msg, KI_WARNING, labels, 2, 1, delete_answer, p);
}

void ex_empty_netherworld(void) {
    DIR *dp = opendir(NETHERWORLD);
    if (!dp) { report("Cannot look into the Netherworld", "It cannot be read:", NETHERWORLD, errno); return; }
    char **paths = NULL;
    int n = 0, cap = 0;
    struct dirent *d;
    while ((d = readdir(dp))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **np = realloc(paths, (size_t)cap * sizeof *np);
            if (!np) break;
            paths = np;
        }
        char full[EX_PATH];
        path_join(full, sizeof full, NETHERWORLD, d->d_name);
        paths[n++] = strdup(full);
    }
    closedir(dp);
    int visible = trash_count();
    if (visible == 0) { ex_msg("The Netherworld is already empty."); free_paths(paths, n); return; }
    PendingDelete *p = calloc(1, sizeof *p);
    if (!p) { free_paths(paths, n); return; }
    p->paths = paths;
    p->n = n;
    p->empty = true;
    char msg[300];
    snprintf(msg, sizeof msg, "Empty the Netherworld? Yuyuko will eat all %d item%s for good. Nothing can be restored afterwards.", visible,
             visible == 1 ? "" : "s");
    static const char *const labels[] = { "Empty it", "Cancel" };
    dlg_choice("Empty the Netherworld?", msg, KI_WARNING, labels, 2, 1, delete_answer, p);
}

void ex_start_restore(char **paths, int n) {
    Job *j = make_job(OP_RESTORE, NULL, paths, n);
    if (j) jobs_submit(j);
}

void ex_start_paste(const char *dest_dir) {
    clip_poll(true);
    if (!CL.n) { ex_msg("There is nothing to paste: copy or cut something first."); return; }
    Job *j = make_job(CL.cut ? OP_MOVE : OP_COPY, dest_dir, CL.p, CL.n);
    if (!j) return;
    jobs_submit(j);
    if (CL.cut) { clip_clear(); mark_cut(); }
}

void ex_start_drop(char **paths, int n, const char *dest_dir, int how) {
    if (n <= 0) return;
    if (!strcmp(dest_dir, NETHERWORLD)) {
        for (int i = 0; i < n; i++)
            if (path_inside(paths[i], NETHERWORLD)) { ex_msg("Those are in the Netherworld already."); return; }
        ex_start_trash(paths, n);
        return;
    }
    bool move;
    if (how == 1) move = false;
    else if (how == 2) move = true;
    else {
        struct stat a, b;
        char parent[EX_PATH];
        path_parent(paths[0], parent, sizeof parent);
        move = stat(parent, &a) == 0 && stat(dest_dir, &b) == 0 && a.st_dev == b.st_dev;       /* same volume: move, else copy */
    }
    Job *j = make_job(move ? OP_MOVE : OP_COPY, dest_dir, paths, n);
    if (j) jobs_submit(j);
}

static void plural(char *out, size_t cap, int n) { snprintf(out, cap, "%d item%s", n, n == 1 ? "" : "s"); }

void ex_job_done(Job *j) {
    if (g_job_done_hook) { g_job_done_hook(j); return; }
    if (!X.app) return;
    JobInfo ji;
    job_info(j, &ji);
    int ok = job_ok_count(j), total = job_nsrc(j), ne;
    const JobErr *errs = job_errors(j, &ne);
    char what[40], msg[300], tail[120];
    plural(what, sizeof what, ok);
    tail[0] = 0;
    if (ji.nskipped) snprintf(tail + strlen(tail), sizeof tail - strlen(tail), " (%d skipped)", ji.nskipped);
    if (job_restored_home(j)) snprintf(tail + strlen(tail), sizeof tail - strlen(tail), " %d had no note and went to your home folder.", job_restored_home(j));
    const char *verb = "Done with";
    switch (ji.kind) {
    case OP_COPY: verb = "Copied"; break;
    case OP_MOVE: verb = "Moved"; break;
    case OP_TRASH: verb = "Sent to the Netherworld:"; break;
    case OP_DELETE: verb = "Deleted for good:"; break;
    case OP_RESTORE: verb = "Restored"; break;
    default: break;
    }
    if (job_cancelled(j)) snprintf(msg, sizeof msg, "Stopped. %s %s before that.%s", verb, what, tail);
    else if (ji.nerr) snprintf(msg, sizeof msg, "%s %d of %d item%s; %d problem%s.%s", verb, ok, total, total == 1 ? "" : "s", ji.nerr, ji.nerr == 1 ? "" : "s", tail);
    else snprintf(msg, sizeof msg, "%s %s.%s", verb, what, tail);
    ex_msg("%s", msg);
    if (ji.nerr && !job_cancelled(j)) {
        char intro[240];
        snprintf(intro, sizeof intro, "%d thing%s did not go through. Everything else did. Here is what went wrong:", ji.nerr, ji.nerr == 1 ? "" : "s");
        dlg_errors("Something went wrong", intro, errs, ne, ji.nskipped);
    }
    ex_refresh(NULL);
}

/* ---------------------------------------------------------------- renaming and creating */

void ex_rename_commit(const char *from, const char *to_in) {
    char to[EX_NAME];
    snprintf(to, sizeof to, "%s", to_in);
    size_t n = strlen(to);
    while (n && to[n - 1] == ' ') to[--n] = 0;
    char *t = to;
    while (*t == ' ') t++;
    if (!strcmp(from, t)) return;
    const char *why;
    if (!name_valid(t, &why)) { dlg_message("Cannot rename", why, KI_WARNING); return; }
    char a[EX_PATH], b[EX_PATH];
    path_join(a, sizeof a, X.m.path, from);
    path_join(b, sizeof b, X.m.path, t);
    if (path_exists(b) && strcasecmp(from, t)) {
        char msg[EX_NAME + 100];
        snprintf(msg, sizeof msg, "There is already something called \"%s\" in this folder.", t);
        dlg_message("Cannot rename", msg, KI_WARNING);
        return;
    }
    if (rename(a, b) != 0) { report("Cannot rename", "The rename failed:", a, errno); return; }
    ex_trace("renamed %s -> %s", from, t);
    ex_refresh(t);
}

void ex_new_item(bool folder) {
    char name[EX_NAME], path[EX_PATH];
    unique_name(X.m.path, folder ? "New Folder" : "New File.txt", name, sizeof name, folder ? "new" : "num");
    path_join(path, sizeof path, X.m.path, name);
    if (folder) {
        if (mkdir(path, 0755) != 0) { report("Cannot create the folder", "It could not be made here:", path, errno); return; }
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) { report("Cannot create the file", "It could not be made here:", path, errno); return; }
        close(fd);
    }
    ex_trace("created %s", path);
    ex_refresh(name);
    snprintf(PEND.rename, sizeof PEND.rename, "%s", name);
}

void ex_properties(void) {
    char **paths;
    int n;
    ex_selected_paths(&paths, &n);
    if (n == 0) {
        free(paths);
        paths = calloc(1, sizeof *paths);
        paths[0] = strdup(X.m.path);
        n = 1;
    }
    dlg_properties(paths, n);
    free_paths(paths, n);
}

/* ---------------------------------------------------------------- sorting */

void ex_set_sort(int col) {
    if (X.m.sort_col == col) X.m.sort_desc = !X.m.sort_desc;
    else { X.m.sort_col = col; X.m.sort_desc = false; }
    Entry *cur = X.cursor >= 0 && X.cursor < X.m.nshown ? X.m.shown[X.cursor] : NULL;
    model_sort(&X.m);
    X.cursor = X.anchor = -1;
    if (cur) for (int i = 0; i < X.m.nshown; i++) if (X.m.shown[i] == cur) X.cursor = X.anchor = i;
    if (X.cursor >= 0) view_scroll_to(X.cursor);
    ex_trace("sort %d desc=%d", X.m.sort_col, X.m.sort_desc ? 1 : 0);
    trace_order();
    prefs_save();
}

static void sort_menu_pick(int col) {
    if (X.m.sort_col != col) ex_set_sort(col);
}

/* ---------------------------------------------------------------- actions */

static void request_quit(void);

static void select_invert(void) {
    for (int i = 0; i < X.m.nshown; i++) X.m.shown[i]->sel = !X.m.shown[i]->sel;
}

void ex_action(int act) {
    KUI *ui = X.ui;
    char **paths;
    int n;
    switch (act) {
    case A_OPEN: open_selection(); break;
    case A_OPEN_NOTEBOOK: case A_OPEN_SKETCHBOOK: case A_OPEN_TERMINAL: {
        int i = first_selected();
        if (i < 0) break;
        char full[EX_PATH];
        path_join(full, sizeof full, X.m.path, X.m.shown[i]->name);
        ex_open_path_with(full, act == A_OPEN_NOTEBOOK ? 1 : act == A_OPEN_SKETCHBOOK ? 2 : 3);
        break;
    }
    case A_OPEN_NEWWIN: {
        int i = first_selected();
        if (i < 0) break;
        char full[EX_PATH];
        path_join(full, sizeof full, X.m.path, X.m.shown[i]->name);
        ex_spawn_explorer(X.m.shown[i]->is_dir ? full : X.m.path);
        break;
    }
    case A_NEW_FOLDER: case A_NEW_FILE:
        if (X.in_netherworld) ex_msg("Nothing new can be made in the Netherworld: it only keeps what was deleted.");
        else ex_new_item(act == A_NEW_FOLDER);
        break;
    case A_NEW_WINDOW: ex_spawn_explorer(X.m.path); break;
    case A_TERMINAL_HERE: {
        char q[EX_PATH * 2], cmd[EX_PATH * 3];
        sh_quote(q, sizeof q, X.m.path);
        snprintf(cmd, sizeof cmd, "cd %s && exec gapsh -l", q);
        spawn_terminal("Terminal", cmd);
        break;
    }
    case A_CLOSE: case A_QUIT: request_quit(); break;
    case A_CUT: case A_COPY:
        ex_selected_paths(&paths, &n);
        if (n) { clip_set(act == A_CUT, paths, n); ex_msg("%s %d item%s. Paste puts %s in another folder.", act == A_CUT ? "Cut" : "Copied", n, n == 1 ? "" : "s", n == 1 ? "it" : "them"); }
        free_paths(paths, n);
        break;
    case A_PASTE:
        if (X.in_netherworld) ex_msg("Nothing can be pasted into the Netherworld. Delete things to send them there.");
        else ex_start_paste(X.m.path);
        break;
    case A_PASTE_INTO: {
        int i = first_selected();
        if (i >= 0 && X.m.shown[i]->is_dir) {
            char full[EX_PATH];
            path_join(full, sizeof full, X.m.path, X.m.shown[i]->name);
            ex_start_paste(full);
        } else ex_start_paste(X.m.path);
        break;
    }
    case A_RENAME: {
        int i = first_selected();
        if (i < 0) i = X.cursor;
        if (i >= 0) view_start_rename(i, false);
        break;
    }
    case A_TRASH:
        ex_selected_paths(&paths, &n);
        if (n) {
            if (X.in_netherworld) ex_start_delete(paths, n);
            else ex_start_trash(paths, n);
        }
        free_paths(paths, n);
        break;
    case A_DELETE:
        ex_selected_paths(&paths, &n);
        if (n) ex_start_delete(paths, n);
        free_paths(paths, n);
        break;
    case A_RESTORE:
        if (!X.in_netherworld) break;
        ex_selected_paths(&paths, &n);
        if (n) ex_start_restore(paths, n);
        else ex_msg("Select what to restore first.");
        free_paths(paths, n);
        break;
    case A_EMPTY: ex_empty_netherworld(); break;
    case A_SELALL: model_select_all(&X.m); break;
    case A_SELNONE: model_select_none(&X.m); break;
    case A_SELINV: select_invert(); break;
    case A_PROPS: ex_properties(); break;
    case A_VIEW_LIST: case A_VIEW_ICONS: {
        int mode = act == A_VIEW_ICONS;
        if (mode != X.view_mode) {
            X.view_mode = mode;
            X.scroll = 0;
            if (X.cursor >= 0) view_scroll_to(X.cursor);
            ex_trace("view %s", mode ? "icons" : "list");
            prefs_save();
        }
        break;
    }
    case A_HIDDEN: {
        Entry *cur = X.cursor >= 0 && X.cursor < X.m.nshown ? X.m.shown[X.cursor] : NULL;
        X.m.show_hidden = !X.m.show_hidden;
        model_refilter(&X.m);
        X.cursor = X.anchor = -1;
        if (cur) for (int i = 0; i < X.m.nshown; i++) if (X.m.shown[i] == cur) X.cursor = X.anchor = i;
        ex_trace("hidden %d items=%d", X.m.show_hidden ? 1 : 0, X.m.nshown);
        trace_order();
        prefs_save();
        break;
    }
    case A_REFRESH: ex_refresh(NULL); break;
    case A_SORT_NAME: sort_menu_pick(COL_NAME); break;
    case A_SORT_SIZE: sort_menu_pick(COL_SIZE); break;
    case A_SORT_MTIME: sort_menu_pick(COL_MTIME); break;
    case A_SORT_TYPE: sort_menu_pick(COL_TYPE); break;
    case A_SORT_PERM: sort_menu_pick(COL_PERM); break;
    case A_SORT_OWNER: sort_menu_pick(COL_OWNER); break;
    case A_SORT_DESC: {
        Entry *cur = X.cursor >= 0 && X.cursor < X.m.nshown ? X.m.shown[X.cursor] : NULL;
        X.m.sort_desc = !X.m.sort_desc;
        model_sort(&X.m);
        X.cursor = X.anchor = -1;
        if (cur) for (int i = 0; i < X.m.nshown; i++) if (X.m.shown[i] == cur) X.cursor = X.anchor = i;
        prefs_save();
        break;
    }
    case A_BACK: hist_go(-1); break;
    case A_FWD: hist_go(1); break;
    case A_UP: go_up(); break;
    case A_HOME: ex_navigate(X.places[0].path, NULL); break;
    case A_ROOT: ex_navigate("/", NULL); break;
    case A_NETHERWORLD: ex_navigate(NETHERWORLD, NULL); break;
    case A_ADDRESS:
        if (ui) { k_focus_textfield(ui, g_addr); ui->sel_anchor = 0; }
        break;
    case A_HELP: dlg_help(); break;
    case A_ABOUT: dlg_about(); break;
    case A_PLACE_NEWWIN:
        if (g_ctx_place >= 0 && g_ctx_place < X.nplaces) ex_spawn_explorer(X.places[g_ctx_place].path);
        break;
    default: break;
    }
}

static void on_quit_answer(int choice, void *user) {
    (void)user;
    if (choice != 0) return;
    Job *j = jobs_front();
    if (j) { job_cancel(j); jobs_step(100); }
    X.quitting = true;
    kapp_quit(X.app, 0);
}

static void request_quit(void) {
    if (X.modal > 0) return;
    if (jobs_busy()) {
        static const char *const labels[] = { "Stop and close", "Keep working" };
        dlg_choice("Still busy", "File operations are still running. Stop them and close the window?", KI_QUESTION, labels, 2, 1, on_quit_answer, NULL);
        return;
    }
    X.quitting = true;
    ex_trace("quit");
    kapp_quit(X.app, 0);
}

static bool on_close(KApp *app, void *user) {
    (void)app; (void)user;
    if (X.modal > 0) return false;
    if (jobs_busy()) { request_quit(); return false; }
    ex_trace("quit");
    return true;
}

/* ---------------------------------------------------------------- context menus */

void view_context(KUI *ui, int mx, int my, int hit) {
    Menu m;
    menu_clear(&m);
    bool nw = X.in_netherworld;
    bool clip = ex_clip_has();
    if (hit >= 0) {
        int n = model_sel_count(&X.m, NULL);
        const Entry *e = X.m.shown[hit];
        bool one = n == 1;
        if (nw) {
            menu_item(&m, "Restore", NULL, A_RESTORE, false, false);
            menu_item(&m, "Delete for Good", "Shift+Del", A_DELETE, false, false);
            menu_sep(&m);
            if (one && e->is_dir) menu_item(&m, "Open", "Enter", A_OPEN, false, false);
            else if (one) menu_item(&m, "Open", "Enter", A_OPEN, false, false);
            menu_sep(&m);
            menu_item(&m, "Properties", "Alt+Enter", A_PROPS, false, false);
        } else {
            menu_item(&m, "Open", "Enter", A_OPEN, false, false);
            if (one && e->is_dir) menu_item(&m, "Open in New Window", NULL, A_OPEN_NEWWIN, false, false);
            if (one && !e->is_dir) {
                menu_item(&m, "Open with Notebook", NULL, A_OPEN_NOTEBOOK, false, false);
                menu_item(&m, "Open with Sketchbook", NULL, A_OPEN_SKETCHBOOK, false, false);
                if (e->ftype == FT_PROGRAM || (e->mode & 0111)) menu_item(&m, "Run in Terminal", NULL, A_OPEN_TERMINAL, false, false);
            }
            menu_sep(&m);
            menu_item(&m, "Cut", "Ctrl+X", A_CUT, false, false);
            menu_item(&m, "Copy", "Ctrl+C", A_COPY, false, false);
            if (one && e->is_dir) menu_item(&m, "Paste Into Folder", NULL, A_PASTE_INTO, !clip, false);
            menu_sep(&m);
            menu_item(&m, "Rename", "F2", A_RENAME, !one, false);
            menu_item(&m, "Send to the Netherworld", "Del", A_TRASH, false, false);
            menu_item(&m, "Delete for Good", "Shift+Del", A_DELETE, false, false);
            menu_sep(&m);
            menu_item(&m, "Properties", "Alt+Enter", A_PROPS, false, false);
        }
    } else {
        if (nw) {
            menu_item(&m, "Restore Selected", NULL, A_RESTORE, model_sel_count(&X.m, NULL) == 0, false);
            menu_item(&m, "Empty the Netherworld", NULL, A_EMPTY, false, false);
            menu_sep(&m);
        } else {
            menu_item(&m, "New Folder", "Ctrl+Shift+N", A_NEW_FOLDER, false, false);
            menu_item(&m, "New File", "Ctrl+Shift+F", A_NEW_FILE, false, false);
            menu_sep(&m);
            menu_item(&m, "Paste", "Ctrl+V", A_PASTE, !clip, false);
            menu_sep(&m);
        }
        menu_item(&m, "Select All", "Ctrl+A", A_SELALL, false, false);
        menu_item(&m, "Refresh", "F5", A_REFRESH, false, false);
        menu_sep(&m);
        menu_item(&m, "Show Hidden Files", "Ctrl+H", A_HIDDEN, false, X.m.show_hidden);
        menu_item(&m, X.view_mode ? "View as List" : "View as Icons", NULL, X.view_mode ? A_VIEW_LIST : A_VIEW_ICONS, false, false);
        menu_sep(&m);
        menu_item(&m, "Open Terminal Here", NULL, A_TERMINAL_HERE, false, false);
        menu_item(&m, "Properties", "Alt+Enter", A_PROPS, false, false);
    }
    ctx_open(ui, &m, mx, my);
}

/* ---------------------------------------------------------------- the menu bar */

static void build_menus(void) {
    int n = model_sel_count(&X.m, NULL);
    bool none = n == 0, one = n == 1, nw = X.in_netherworld, clip = ex_clip_has();
    bool dir1 = one && X.m.shown[first_selected()]->is_dir;
    Menu *m;
    m = &g_menus[0];
    menu_clear(m);
    menu_item(m, "New Folder", "Ctrl+Shift+N", A_NEW_FOLDER, nw, false);
    menu_item(m, "New File", "Ctrl+Shift+F", A_NEW_FILE, nw, false);
    menu_item(m, "New Window", "Ctrl+N", A_NEW_WINDOW, false, false);
    menu_sep(m);
    menu_item(m, "Open", "Enter", A_OPEN, none, false);
    menu_item(m, "Open with Notebook", NULL, A_OPEN_NOTEBOOK, !one || dir1, false);
    menu_item(m, "Open with Sketchbook", NULL, A_OPEN_SKETCHBOOK, !one || dir1, false);
    menu_item(m, "Run in Terminal", NULL, A_OPEN_TERMINAL, !one || dir1, false);
    menu_item(m, "Open Terminal Here", NULL, A_TERMINAL_HERE, false, false);
    menu_sep(m);
    menu_item(m, "Properties", "Alt+Enter", A_PROPS, false, false);
    menu_sep(m);
    menu_item(m, "Close", "Ctrl+W", A_CLOSE, false, false);
    m = &g_menus[1];
    menu_clear(m);
    menu_item(m, "Cut", "Ctrl+X", A_CUT, none || nw, false);
    menu_item(m, "Copy", "Ctrl+C", A_COPY, none, false);
    menu_item(m, "Paste", "Ctrl+V", A_PASTE, !clip || nw, false);
    menu_sep(m);
    menu_item(m, "Rename", "F2", A_RENAME, !one || nw, false);
    menu_item(m, "Send to the Netherworld", "Del", A_TRASH, none || nw, false);
    menu_item(m, "Delete for Good", "Shift+Del", A_DELETE, none, false);
    menu_sep(m);
    menu_item(m, "Restore", NULL, A_RESTORE, none || !nw, false);
    menu_item(m, "Empty the Netherworld", NULL, A_EMPTY, !nw, false);
    menu_sep(m);
    menu_item(m, "Select All", "Ctrl+A", A_SELALL, false, false);
    menu_item(m, "Select None", NULL, A_SELNONE, none, false);
    menu_item(m, "Invert Selection", NULL, A_SELINV, false, false);
    m = &g_menus[2];
    menu_clear(m);
    menu_item(m, "List", "Ctrl+1", A_VIEW_LIST, false, X.view_mode == 0);
    menu_item(m, "Icons", "Ctrl+2", A_VIEW_ICONS, false, X.view_mode == 1);
    menu_sep(m);
    menu_item(m, "Show Hidden Files", "Ctrl+H", A_HIDDEN, false, X.m.show_hidden);
    menu_sep(m);
    menu_item(m, "Sort by Name", NULL, A_SORT_NAME, false, X.m.sort_col == COL_NAME);
    menu_item(m, "Sort by Size", NULL, A_SORT_SIZE, false, X.m.sort_col == COL_SIZE);
    menu_item(m, "Sort by Date Modified", NULL, A_SORT_MTIME, false, X.m.sort_col == COL_MTIME);
    menu_item(m, "Sort by Type", NULL, A_SORT_TYPE, false, X.m.sort_col == COL_TYPE);
    menu_item(m, "Sort by Permissions", NULL, A_SORT_PERM, false, X.m.sort_col == COL_PERM);
    menu_item(m, "Sort by Owner", NULL, A_SORT_OWNER, false, X.m.sort_col == COL_OWNER);
    menu_item(m, "Descending", NULL, A_SORT_DESC, false, X.m.sort_desc);
    menu_sep(m);
    menu_item(m, "Refresh", "F5", A_REFRESH, false, false);
    m = &g_menus[3];
    menu_clear(m);
    menu_item(m, "Back", "Alt+Left", A_BACK, X.ihist <= 0, false);
    menu_item(m, "Forward", "Alt+Right", A_FWD, X.ihist + 1 >= X.nhist, false);
    menu_item(m, "Up", "Alt+Up", A_UP, !strcmp(X.m.path, "/"), false);
    menu_sep(m);
    menu_item(m, "Home", "Alt+Home", A_HOME, false, false);
    menu_item(m, "Root", NULL, A_ROOT, false, false);
    menu_item(m, "Netherworld", NULL, A_NETHERWORLD, false, false);
    menu_sep(m);
    menu_item(m, "Address Bar", "Ctrl+L", A_ADDRESS, false, false);
    m = &g_menus[4];
    menu_clear(m);
    menu_item(m, "Keyboard Shortcuts", "F1", A_HELP, false, false);
    menu_item(m, "About Gensokyo Explorer", NULL, A_ABOUT, false, false);
}

static const char *const g_titles[5] = { "File", "Edit", "View", "Go", "Help" };

/* ---------------------------------------------------------------- keyboard */

static bool app_shortcut(KUI *ui, KKey *k) {
    bool ctrl = (k->mods & MOD_CTRL) != 0, shift = (k->mods & MOD_SHIFT) != 0, alt = (k->mods & MOD_ALT) != 0;
    bool field = ui->focus != VIEW_ID && ui->focus != 0;                 /* a text field has the keyboard */
    bool in_view = !field;
    if (k->code == KEY_ESC && !ctrl && !alt) {
        if (view_renaming()) { view_cancel_rename(); ui->focus = VIEW_ID; return true; }
        if (view_dragging()) { view_cancel_drag(); return true; }
        if (ui->focus == field_id(g_addr)) { snprintf(g_addr, sizeof g_addr, "%s", X.m.path); ui->focus = VIEW_ID; return true; }
        if (view_typeahead()[0]) { view_typeahead_clear(); return true; }
        if (model_sel_count(&X.m, NULL)) { model_select_none(&X.m); return true; }
        return false;
    }
    if (ctrl && !alt) {
        switch (k->code) {
        case KEY_N: ex_action(shift ? A_NEW_FOLDER : A_NEW_WINDOW); return true;
        case KEY_F: if (shift) { ex_action(A_NEW_FILE); return true; } break;
        case KEY_L: ex_action(A_ADDRESS); return true;
        case KEY_H: ex_action(A_HIDDEN); return true;
        case KEY_R: ex_action(A_REFRESH); return true;
        case KEY_1: ex_action(A_VIEW_LIST); return true;
        case KEY_2: ex_action(A_VIEW_ICONS); return true;
        case KEY_Q: case KEY_W: ex_action(A_QUIT); return true;
        case KEY_A: if (in_view) { ex_action(A_SELALL); return true; } break;
        case KEY_C: if (in_view) { ex_action(A_COPY); return true; } break;
        case KEY_X: if (in_view) { ex_action(A_CUT); return true; } break;
        case KEY_V: if (in_view) { ex_action(A_PASTE); return true; } break;
        default: break;
        }
    }
    if (alt && !ctrl && !shift) {
        switch (k->code) {
        case KEY_LEFT: ex_action(A_BACK); return true;
        case KEY_RIGHT: ex_action(A_FWD); return true;
        case KEY_UP: ex_action(A_UP); return true;
        case KEY_HOME: ex_action(A_HOME); return true;
        case KEY_ENTER: case KEY_KPENTER: ex_action(A_PROPS); return true;
        case KEY_D: ex_action(A_ADDRESS); return true;
        case KEY_F: mbar_open(ui, ui->menu_open == 1 ? 0 : 1); return true;
        case KEY_E: mbar_open(ui, ui->menu_open == 2 ? 0 : 2); return true;
        case KEY_V: mbar_open(ui, ui->menu_open == 3 ? 0 : 3); return true;
        case KEY_G: mbar_open(ui, ui->menu_open == 4 ? 0 : 4); return true;
        case KEY_H: mbar_open(ui, ui->menu_open == 5 ? 0 : 5); return true;
        default: break;
        }
    }
    if (!ctrl && !alt) {
        switch (k->code) {
        case KEY_F1: ex_action(A_HELP); return true;
        case KEY_F2: if (in_view) { ex_action(A_RENAME); return true; } break;
        case KEY_F5: ex_action(A_REFRESH); return true;
        case KEY_F6: ex_action(A_ADDRESS); return true;
        case KEY_F10: mbar_open(ui, ui->menu_open ? 0 : 1); return true;
        case KEY_ENTER: case KEY_KPENTER: if (in_view) { ex_action(A_OPEN); return true; } break;
        case KEY_BACKSPACE: if (in_view) { ex_action(A_UP); return true; } break;
        case KEY_DELETE: if (in_view) { ex_action(shift ? A_DELETE : A_TRASH); return true; } break;
        default: break;
        }
    }
    return false;
}

static void handle_keys(KUI *ui, bool modal) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        if (modal) { k->down = 0; k->code = 0; continue; }
        if (ctx_is_open()) { ctx_key(k); k->down = 0; k->code = 0; continue; }
        if (ui->menu_open && mbar_key(ui, k, g_menus, 5)) { k->down = 0; k->code = 0; continue; }
        if (app_shortcut(ui, k)) { k->down = 0; k->code = 0; continue; }
        if ((ui->focus == VIEW_ID || ui->focus == 0) && !ui->menu_open && view_key(ui, k)) { k->down = 0; k->code = 0; }
    }
}

/* ---------------------------------------------------------------- drawing: toolbar, places, banner, status */

static bool tool_button(KUI *ui, KRect r, KIcon icon, bool enabled, const char *tip) {
    bool click = false;
    if (enabled) click = k_icon_button(ui, r, icon, false);
    else {
        k_icon(&ui->cv, icon, r.x + (r.w - 20) / 2, r.y + (r.h - 20) / 2, 20);
        kc_fill_alpha(&ui->cv, r, ui->theme->window_bg, 160);
    }
    if (k_hover(ui, r)) k_tooltip_text(ui, tip);
    return click && enabled;
}

static bool glyph_button(KUI *ui, KRect r, bool icons, bool selected, const char *tip) {
    KCanvas *cv = &ui->cv;
    bool click = k_button_flat(ui, r, "", selected);
    uint32_t ink = ui->theme->text;
    int gx = r.x + (r.w - 16) / 2, gy = r.y + (r.h - 16) / 2;
    if (!icons) {
        for (int i = 0; i < 3; i++) {
            kc_fill(cv, kr(gx, gy + 1 + i * 5, 3, 3), ink);
            kc_fill(cv, kr(gx + 5, gy + 1 + i * 5, 11, 3), ink);
        }
    } else {
        for (int i = 0; i < 4; i++) kc_fill(cv, kr(gx + (i & 1) * 9, gy + (i >> 1) * 9, 7, 7), ink);
    }
    if (k_hover(ui, r)) k_tooltip_text(ui, tip);
    return click;
}

static void draw_toolbar(KUI *ui, int W) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    kc_fill(cv, kr(0, L_TOOL_Y, W, L_TOOL_H), th->window_bg);
    kc_hline(cv, 0, L_TOOL_Y + L_TOOL_H - 1, W, kc_shade(th->window_bg, 78));
    if (tool_button(ui, kr(6, TB_Y, 28, 28), KI_BACK, X.ihist > 0, "Back (Alt+Left)")) ex_action(A_BACK);
    if (tool_button(ui, kr(38, TB_Y, 28, 28), KI_FORWARD, X.ihist + 1 < X.nhist, "Forward (Alt+Right)")) ex_action(A_FWD);
    if (tool_button(ui, kr(70, TB_Y, 28, 28), KI_UP, strcmp(X.m.path, "/") != 0, "Up one folder (Backspace)")) ex_action(A_UP);
    if (tool_button(ui, kr(102, TB_Y, 28, 28), KI_REFRESH, true, "Read the folder again (F5)")) ex_action(A_REFRESH);
    bool focused = ui->focus == field_id(g_addr);
    if (!focused) snprintf(g_addr, sizeof g_addr, "%s", X.m.path);
    KRect ar = kr(138, TB_Y + 2, W - 138 - 114, 24);
    if (k_textfield(ui, ar, g_addr, sizeof g_addr)) {
        char target[EX_PATH];
        snprintf(target, sizeof target, "%s", g_addr);
        ui->focus = VIEW_ID;
        ex_trace("address %s", target);
        ex_navigate(target, NULL);
    }
    if (glyph_button(ui, kr(W - 108, TB_Y, 30, 28), false, X.view_mode == 0, "Show as a list (Ctrl+1)")) ex_action(A_VIEW_LIST);
    if (glyph_button(ui, kr(W - 78, TB_Y, 30, 28), true, X.view_mode == 1, "Show as icons (Ctrl+2)")) ex_action(A_VIEW_ICONS);
    KRect nf = kr(W - 38, TB_Y, 32, 28);
    if (tool_button(ui, nf, KI_FOLDER, !X.in_netherworld, "New folder (Ctrl+Shift+N)")) ex_action(A_NEW_FOLDER);
    if (!X.in_netherworld) {
        kc_fill(cv, kr(nf.x + nf.w - 15, nf.y + nf.h - 14, 10, 10), 0x3E8F4E);
        kc_hline(cv, nf.x + nf.w - 14, nf.y + nf.h - 9, 8, 0xFFFFFF);
        kc_vline(cv, nf.x + nf.w - 10, nf.y + nf.h - 13, 8, 0xFFFFFF);
    }
}

static void draw_places(KUI *ui) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect side = X.r_side;
    kc_fill(cv, side, kc_shade(th->window_bg, 96));
    kc_vline(cv, side.x + side.w - 1, side.y, side.h, kc_shade(th->window_bg, 78));
    kc_text_bold(cv, 8, L_MAIN_Y + 6, "Places", th->accent_dark);
    bool inside = !ui->input_blocked && ui->mouse_inside;
    bool dragging = view_dragging();
    static int press;                                                   /* the place a press began on: a click needs the press and the release on it */
    int pressed_on = press;
    if (ui->released[0]) press = -1;
    int navigate = -1;
    for (int i = 0; i < X.nplaces; i++) {
        Place *p = &X.places[i];
        KRect r = place_rect(i);
        if (i == 3) kc_text_bold(cv, 8, r.y - SIDE_HEAD_H, "Volumes", th->accent_dark);
        bool cur = !strcmp(p->path, X.m.path);
        bool hv = inside && kr_contains(r, ui->mx, ui->my) && !dragging;
        if (g_drop_place == i) { kc_fill(cv, r, kc_mix(th->window_bg, th->accent_light, 130)); kc_outline(cv, r, th->accent_dark); }
        else if (cur) kc_panel(cv, r, KB_SUNKEN, kc_shade(th->window_bg, 90));
        else if (hv) kc_bevel(cv, r, KB_RAISED);
        k_icon(cv, p->icon, r.x + 4, r.y + 3, 18);
        kc_text_in(cv, kr(r.x + 28, r.y, r.w - 30, r.h), p->label, th->text, KA_LEFT);
        if (hv) {
            if (ui->pressed[0]) press = i;
            if (ui->released[0] && (pressed_on == i || ui->pressed[0])) navigate = i;
            if (ui->pressed[1]) {
                g_ctx_place = i;
                Menu m;
                menu_clear(&m);
                menu_item(&m, "Open in New Window", NULL, A_PLACE_NEWWIN, false, false);
                ctx_open(ui, &m, ui->mx, ui->my);
            }
            k_tooltip_text(ui, p->path);
        }
    }
    if (navigate >= 0) {
        ex_trace("place %s", X.places[navigate].path);
        ex_navigate(X.places[navigate].path, NULL);
    }
}

static void draw_banner(KUI *ui) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect b = X.r_banner;
    kc_panel(cv, b, KB_FLAT, kc_mix(th->window_bg, th->accent_light, 40));
    k_icon(cv, KI_TRASH, b.x + 6, b.y + 5, 20);
    char t[160];
    int n = X.m.nall;                                                   /* what was read, without the folder of notes */
    for (int i = 0; i < X.m.nall; i++) if (!strcmp(X.m.all[i]->name, ".trashinfo")) { n--; break; }
    snprintf(t, sizeof t, "The Netherworld holds %d item%s. Restore puts them back.", n, n == 1 ? "" : "s");
    kc_text_in(cv, kr(b.x + 32, b.y, b.w - 32 - 212, b.h), t, th->text, KA_LEFT);
    KRect re = kr(b.x + b.w - 208, b.y + 3, 96, 24), em = kr(b.x + b.w - 106, b.y + 3, 100, 24);
    if (k_button(ui, re, "Restore")) ex_action(A_RESTORE);
    if (k_button(ui, em, "Empty it")) ex_action(A_EMPTY);
}

static void draw_status(KUI *ui, int W, int H) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect r = X.r_status;
    kc_fill(cv, r, th->window_bg);
    kc_hline(cv, 0, r.y, W, kc_shade(th->window_bg, 72));
    kc_hline(cv, 0, r.y + 1, W, 0xFFFFFF);
    (void)H;
    int64_t bytes = 0;
    int ns = model_sel_count(&X.m, &bytes);
    char left[200];
    if (X.m.scanning) snprintf(left, sizeof left, "Reading the folder... %d items so far", X.m.nshown);
    else {
        int pos = snprintf(left, sizeof left, "%d item%s", X.m.nshown, X.m.nshown == 1 ? "" : "s");
        if (!X.m.show_hidden && X.m.nhidden) pos += snprintf(left + pos, sizeof left - (size_t)pos, " (+%d hidden)", X.m.nhidden);
        if (ns) {
            char sz[40];
            fmt_size(sz, sizeof sz, bytes);
            pos += snprintf(left + pos, sizeof left - (size_t)pos, "   %d selected", ns);
            if (bytes) snprintf(left + pos, sizeof left - (size_t)pos, " (%s)", sz);
        }
    }
    int ty = r.y + 4;
    kc_text(cv, 8, ty, left, th->text);
    int left_end = 8 + kc_text_width(left) + 20;
    int right_w = g_free_text[0] ? kc_text_width(g_free_text) + 16 : 0;
    if (right_w) {
        kc_text(cv, W - right_w + 4, ty, g_free_text, th->text_dim);
        kc_vline(cv, W - right_w - 4, r.y + 4, r.h - 6, kc_shade(th->window_bg, 78));
    }
    const char *ta = view_typeahead();
    char buf[200];
    const char *m = X.msg[0] ? X.msg : NULL;
    if (ta[0]) { snprintf(buf, sizeof buf, "Find: %s", ta); m = buf; }
    if (m) {
        int mw = W - right_w - left_end - 12;
        if (mw > 16) kc_text_in(cv, kr(left_end, ty - 2, mw, 20), m, th->accent_dark, KA_LEFT);
    }
}

/* ---------------------------------------------------------------- the window */

static void trace_selection(void) {
    if (!g_trace) return;
    static char last_first[EX_NAME];
    int ns = model_sel_count(&X.m, NULL);
    int f = first_selected();
    const char *first = f >= 0 ? X.m.shown[f]->name : "";
    if (ns != g_last_nsel || strcmp(first, last_first)) {
        g_last_nsel = ns;
        snprintf(last_first, sizeof last_first, "%s", first);
        ex_trace("sel %d%s%s", ns, f >= 0 ? " first=" : "", first);
    }
}

/* The names in the order they are shown (the first dozen): tests read the sort order from this. */
static void trace_order(void) {
    if (!g_trace) return;
    char buf[300];
    int pos = 0;
    buf[0] = 0;
    for (int i = 0; i < X.m.nshown && i < 14 && pos < (int)sizeof buf - 40; i++)
        pos += snprintf(buf + pos, sizeof buf - (size_t)pos, "%s%s", i ? "|" : "", X.m.shown[i]->name);
    ex_trace("order %s", buf);
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    X.ui = ui;
    X.iter++;
    KCanvas *cv = &ui->cv;
    int W = cv->w, H = cv->h;
    X.W = W;
    X.H = H;
    if (!g_init) {
        ui->focus = VIEW_ID;
        ui->menu_item_hover = -1;
        g_init = true;
        if (g_initial_error[0]) dlg_message("Cannot open that place", g_initial_error, KI_ERROR);
    }

    /* work in slices: reading the folder and the file jobs */
    reap_children();
    bool more = false;
    if (X.m.scanning) {
        if (model_scan_step(&X.m, 20)) on_scan_done();
        else more = true;
    }
    if (g_refreshing) {
        if (model_scan_step(&g_rm, 15)) { g_refreshing = false; finish_refresh(); }
        else more = true;
    }
    if (jobs_busy()) {
        jobs_step(25);
        more = more || jobs_busy();
    }
    dlg_jobs_update();
    dlg_gc();
    uint64_t now = ui->now_ms;
    if (now - g_tick_ms >= 500) {
        g_tick_ms = now;
        clip_poll(false);
        if (now - g_free_ms > 5000) update_free();
        static uint64_t places_ms;
        if (now - places_ms > 4000) { places_ms = now; int old = X.nplaces; places_build(); (void)old; }
        auto_refresh();
    }
    if (X.msg[0] && now > X.msg_until) X.msg[0] = 0;
    else if (X.msg[0]) ui->redraw_soon = true;

    X.in_netherworld = !strcmp(X.m.path, NETHERWORLD);
    bool modal = X.modal > 0;
    if (modal) ui->input_blocked = true;
    handle_keys(ui, modal);

    k_clear(ui);
    if (ctx_is_open()) ui->input_blocked = true;
    bool blocked = ui->input_blocked || modal;
    X.r_side = kr(0, L_MAIN_Y, L_SIDE_W, H - L_MAIN_Y - L_STATUS_H);
    int vy = L_MAIN_Y + (X.in_netherworld ? L_BANNER_H + 2 : 0);
    X.r_banner = X.in_netherworld ? kr(L_SIDE_W + 2, L_MAIN_Y, W - L_SIDE_W - 4, L_BANNER_H) : kr(0, 0, 0, 0);
    X.r_status = kr(0, H - L_STATUS_H, W, L_STATUS_H);
    KRect rview = kr(L_SIDE_W + 2, vy, W - L_SIDE_W - 4, H - L_STATUS_H - vy - 2);

    draw_toolbar(ui, W);
    draw_places(ui);
    if (X.in_netherworld) draw_banner(ui);
    X.view_focus = ui->focus == VIEW_ID;
    view_draw(ui, rview, blocked);
    draw_status(ui, W, H);
    if (ui->focus == 0 && !ui->menu_open && !modal) ui->focus = VIEW_ID;

    /* menu bar last: its drop-down paints over everything */
    build_menus();
    int pick = mbar_run(ui, kr(0, 0, W, L_MENU_H), !modal && !ctx_is_open(), g_titles, g_menus, 5);
    if (pick) ex_action(pick);
    if (ctx_is_open()) {
        int cact = ctx_run(ui);
        if (cact) ex_action(cact);
    }
    view_ghost(ui);
    update_title();
    trace_selection();
    if (more) kwin_redraw(kapp_main(X.app));
}

/* ---------------------------------------------------------------- main */

static void usage(void) {
    puts("usage: explorer [path]       open a folder (a file: its folder, with the file selected)\n"
         "       explorer --selftest   run the model and job engine tests on the console");
}

int main(int argc, char **argv) {
    g_trace = getenv("EXPLORER_TRACE") != NULL;
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest_run();
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) { puts("explorer " EX_VERSION); return 0; }
    model_init(&X.m);
    prefs_load();
    char cwd[EX_PATH];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    const char *home = getenv("HOME");
    char start[EX_PATH], sel[EX_NAME];
    sel[0] = 0;
    snprintf(start, sizeof start, "%s", home && *home ? home : cwd);
    if (argc > 1 && argv[1][0]) {
        char want[EX_PATH];
        if (path_normalize(argv[1], cwd, want, sizeof want)) {
            struct stat st;
            if (stat(want, &st) != 0) snprintf(g_initial_error, sizeof g_initial_error, "Nothing is there:\n%s\n\n%s", want, strerror(errno));
            else if (!S_ISDIR(st.st_mode)) {
                snprintf(sel, sizeof sel, "%s", path_base(want));
                path_parent(want, start, sizeof start);
            } else snprintf(start, sizeof start, "%s", want);
        } else snprintf(g_initial_error, sizeof g_initial_error, "That address is too long:\n%s", argv[1]);
    }
    X.app = kapp_new("Explorer", 800, 540, FW_RESIZABLE);
    if (!X.app) return 1;
    kapp_set_draw(X.app, draw, NULL);
    kapp_set_close_handler(X.app, on_close, NULL);
    kapp_set_minsize(X.app, 560, 340);
    kapp_set_tick(X.app, 500);
    places_build();
    clip_poll(true);
    ex_trace("start");
    if (open_folder(start, sel, 0, false)) hist_push(start);
    else if (open_folder("/", NULL, 0, false)) hist_push("/");
    int rc = kapp_run(X.app);
    dlg_close_all();
    kapp_destroy(X.app);
    return rc;
}
