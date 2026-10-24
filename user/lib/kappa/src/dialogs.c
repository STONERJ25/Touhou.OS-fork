/* Non-blocking dialogs: text prompt, confirmation and file open/save. Each opens a window in the caller's KApp and calls
 * back when the user decides; the window closes itself. */
#include <kappa.h>
#include <audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- input dialog */

typedef struct {
    KTextCb cb;
    void *user;
    char text[256];
    char prompt[160];
    KWin *win;
    bool focused;
} InputDlg;

static void input_finish(InputDlg *d, bool ok) {
    KTextCb cb = d->cb;
    void *user = d->user;
    char text[256];
    memcpy(text, d->text, sizeof text);
    KWin *w = d->win;
    d->cb = NULL;
    kwin_close(w);
    if (cb) cb(ok ? text : NULL, user);
}

static void input_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    InputDlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_text_in(cv, kr(16, 14, cv->w - 32, 18), d->prompt, ui->theme->text, KA_LEFT);
    KRect field = kr(16, 38, cv->w - 32, 26);
    if (!d->focused) { k_focus_textfield(ui, d->text); d->focused = true; }
    bool enter = k_textfield(ui, field, d->text, sizeof d->text);
    int bw = 84, y = cv->h - 40;
    if (k_button(ui, kr(cv->w - 2 * bw - 24, y, bw, 26), "OK") || enter) input_finish(d, true);
    else if (k_button(ui, kr(cv->w - bw - 16, y, bw, 26), "Cancel") || k_key_pressed(ui, KEY_ESC)) input_finish(d, false);
}

static void input_closed(KWin *w, void *user) {
    (void)w;
    InputDlg *d = user;
    if (d->cb) { KTextCb cb = d->cb; d->cb = NULL; cb(NULL, d->user); }
}

void k_input_dialog(KApp *app, const char *title, const char *prompt, const char *initial, KTextCb cb, void *user) {
    InputDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->cb = cb;
    d->user = user;
    snprintf(d->prompt, sizeof d->prompt, "%s", prompt);
    snprintf(d->text, sizeof d->text, "%s", initial ? initial : "");
    d->win = kwin_new(app, -1, -1, 400, 110, title, FW_DIALOG | FW_CENTER | FW_TOP, input_draw, d);
    audio_event(AUDIO_EV_DIALOG);
    if (!d->win) { free(d); return; }
    kwin_set_close_handler(d->win, input_closed, d);
}

/* ---------------------------------------------------------------- confirm dialog */

typedef struct {
    KConfirmCb cb;
    void *user;
    char message[256];
    char yes[24], no[24];
    KIcon icon;
    KWin *win;
} ConfirmDlg;

static void confirm_finish(ConfirmDlg *d, bool yes) {
    KConfirmCb cb = d->cb;
    d->cb = NULL;
    kwin_close(d->win);
    if (cb) cb(yes, d->user);
}

static void confirm_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    ConfirmDlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, d->icon, 16, 16, 32);
    int maxcols = (cv->w - 76) / 8, y = 16;
    const char *p = d->message;
    while (*p && y < cv->h - 50) {
        int cols = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && cols < maxcols) { if (*q == ' ') last_space = (int)(q - p); q++; cols++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        kc_text_n(cv, 62, y, p, len, ui->theme->text);
        p += len;
        if (*p == ' ' || *p == '\n') p++;
        y += 18;
    }
    int bw = 92, by = cv->h - 40;
    if (k_button(ui, kr(cv->w - 2 * bw - 24, by, bw, 26), d->yes) || k_key_pressed(ui, KEY_ENTER)) confirm_finish(d, true);
    else if (k_button(ui, kr(cv->w - bw - 16, by, bw, 26), d->no) || k_key_pressed(ui, KEY_ESC)) confirm_finish(d, false);
}

static void confirm_closed(KWin *w, void *user) {
    (void)w;
    ConfirmDlg *d = user;
    if (d->cb) { KConfirmCb cb = d->cb; d->cb = NULL; cb(false, d->user); }
}

void k_confirm_dialog(KApp *app, const char *title, const char *message, KIcon icon, const char *yes, const char *no, KConfirmCb cb, void *user) {
    ConfirmDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->cb = cb;
    d->user = user;
    d->icon = icon;
    snprintf(d->message, sizeof d->message, "%s", message);
    snprintf(d->yes, sizeof d->yes, "%s", yes ? yes : "OK");
    snprintf(d->no, sizeof d->no, "%s", no ? no : "Cancel");
    int lines = 1 + (int)(strlen(message) / 40);
    for (const char *c = message; *c; c++) if (*c == '\n') lines++;
    int h = 100 + lines * 18;
    d->win = kwin_new(app, -1, -1, 440, h < 140 ? 140 : h, title, FW_DIALOG | FW_CENTER | FW_TOP, confirm_draw, d);
    audio_event(icon == KI_ERROR ? AUDIO_EV_ERROR : AUDIO_EV_DIALOG);          /* a quiet bell, or the low buzz for an error */
    if (!d->win) { free(d); return; }
    kwin_set_close_handler(d->win, confirm_closed, d);
}

/* ---------------------------------------------------------------- file dialog */

typedef struct {
    int mode;
    char dir[512];
    char name[256];
    char path_field[512];
    char **names;
    KIcon *icons;
    bool *is_dir;
    int n;
    KListState ls;
    KFileCb cb;
    void *user;
    KWin *win;
    char err[96];
    bool focused;
    bool show_hidden;
} FileDlg;

static int cmp_entries(const void *a, const void *b) {
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    bool dx = x[strlen(x) - 1] == '/', dy = y[strlen(y) - 1] == '/';
    if (dx != dy) return dx ? -1 : 1;
    return strcasecmp(x, y);
}

static void fd_free_entries(FileDlg *d) {
    for (int i = 0; i < d->n; i++) free(d->names[i]);
    free(d->names); free(d->icons); free(d->is_dir);
    d->names = NULL; d->icons = NULL; d->is_dir = NULL; d->n = 0;
}

static void fd_reload(FileDlg *d) {
    fd_free_entries(d);
    d->err[0] = 0;
    DIR *dir = opendir(d->dir);
    if (!dir) { snprintf(d->err, sizeof d->err, "cannot open this folder"); return; }
    struct dirent *e;
    int cap = 64;
    d->names = malloc((size_t)cap * sizeof *d->names);
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (e->d_name[0] == '.' && !d->show_hidden) continue;
        char full[1100];
        snprintf(full, sizeof full, "%s/%s", d->dir, e->d_name);
        struct stat st;
        bool isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (d->mode == 2 && !isdir) continue;
        if (d->n == cap) { cap *= 2; d->names = realloc(d->names, (size_t)cap * sizeof *d->names); }
        size_t len = strlen(e->d_name);
        char *s = malloc(len + 2);
        memcpy(s, e->d_name, len);
        s[len] = isdir ? '/' : 0;
        s[len + (isdir ? 1 : 0)] = 0;
        if (!isdir) s[len] = 0;
        d->names[d->n++] = s;
    }
    closedir(dir);
    qsort(d->names, (size_t)d->n, sizeof *d->names, cmp_entries);
    d->icons = malloc((size_t)(d->n ? d->n : 1) * sizeof *d->icons);
    d->is_dir = malloc((size_t)(d->n ? d->n : 1) * sizeof *d->is_dir);
    for (int i = 0; i < d->n; i++) {
        d->is_dir[i] = d->names[i][strlen(d->names[i]) - 1] == '/';
        d->icons[i] = d->is_dir[i] ? KI_FOLDER : KI_FILE;
    }
    k_list_init(&d->ls);
    snprintf(d->path_field, sizeof d->path_field, "%s", d->dir);
}

static void fd_goto(FileDlg *d, const char *path) {
    char resolved[512];
    if (!realpath(path, resolved)) { snprintf(d->err, sizeof d->err, "no such folder"); return; }
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) { snprintf(d->err, sizeof d->err, "not a folder"); return; }
    snprintf(d->dir, sizeof d->dir, "%s", resolved);
    fd_reload(d);
}

static void fd_finish(FileDlg *d, const char *path) {
    KFileCb cb = d->cb;
    char out[1100];
    if (path) snprintf(out, sizeof out, "%s", path);
    d->cb = NULL;
    kwin_close(d->win);
    if (cb) cb(path ? out : NULL, d->user);
}

static void fd_choose(FileDlg *d) {
    char full[1100];
    if (d->mode == 2) {
        if (d->ls.selected >= 0 && d->is_dir[d->ls.selected]) {
            char nm[256];
            snprintf(nm, sizeof nm, "%s", d->names[d->ls.selected]);
            nm[strlen(nm) - 1] = 0;
            snprintf(full, sizeof full, "%s/%s", d->dir, nm);
        } else snprintf(full, sizeof full, "%s", d->dir);
        fd_finish(d, full);
        return;
    }
    if (!d->name[0]) { snprintf(d->err, sizeof d->err, "type a file name"); return; }
    if (d->name[0] == '/') snprintf(full, sizeof full, "%s", d->name);
    else snprintf(full, sizeof full, "%s/%s", d->dir, d->name);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) { fd_goto(d, full); d->name[0] = 0; return; }
    if (d->mode == 0 && stat(full, &st) != 0) { snprintf(d->err, sizeof d->err, "that file does not exist"); return; }
    fd_finish(d, full);
}

static void fd_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    FileDlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    int w = cv->w, h = cv->h;
    if (k_icon_button(ui, kr(10, 8, 28, 26), KI_UP, false)) { char up[1100]; snprintf(up, sizeof up, "%s/..", d->dir); fd_goto(d, up); }
    if (k_icon_button(ui, kr(42, 8, 28, 26), KI_HOME, false)) { const char *home = getenv("HOME"); fd_goto(d, home ? home : "/"); }
    if (k_icon_button(ui, kr(74, 8, 28, 26), KI_REFRESH, false)) fd_reload(d);
    if (k_textfield(ui, kr(110, 8, w - 120, 26), d->path_field, sizeof d->path_field)) fd_goto(d, d->path_field);
    if (k_list(ui, kr(10, 42, w - 20, h - 126), (const char *const *)d->names, d->icons, d->n, &d->ls)) {
        if (d->ls.selected >= 0 && !d->is_dir[d->ls.selected] && d->mode != 2) snprintf(d->name, sizeof d->name, "%s", d->names[d->ls.selected]);
    }
    if (d->ls.activated >= 0) {
        int i = d->ls.activated;
        if (d->is_dir[i]) {
            char nm[256], full[1100];
            snprintf(nm, sizeof nm, "%s", d->names[i]);
            nm[strlen(nm) - 1] = 0;
            snprintf(full, sizeof full, "%s/%s", d->dir, nm);
            fd_goto(d, full);
        } else if (d->mode != 2) { snprintf(d->name, sizeof d->name, "%s", d->names[i]); fd_choose(d); }
    }
    if (d->mode != 2) {
        k_label(ui, 12, h - 76, d->mode == 1 ? "Save as:" : "File name:");
        if (!d->focused) { k_focus_textfield(ui, d->name); d->focused = true; }
        if (k_textfield(ui, kr(100, h - 80, w - 112, 24), d->name, sizeof d->name)) fd_choose(d);
    } else k_label(ui, 12, h - 76, "Choose a folder, then OK.");
    k_checkbox(ui, 12, h - 44, "Show hidden files", &d->show_hidden);
    if (ui->released[0] && k_hover(ui, kr(12, h - 44, 160, 16))) fd_reload(d);
    if (d->err[0]) kc_text(cv, 190, h - 44, d->err, ui->theme->accent);
    int bw = 84;
    if (k_button(ui, kr(w - 2 * bw - 22, h - 38, bw, 26), d->mode == 1 ? "Save" : "OK")) fd_choose(d);
    if (k_button(ui, kr(w - bw - 12, h - 38, bw, 26), "Cancel") || k_key_pressed(ui, KEY_ESC)) fd_finish(d, NULL);
}

static void fd_closed(KWin *w, void *user) {
    (void)w;
    FileDlg *d = user;
    if (d->cb) { KFileCb cb = d->cb; d->cb = NULL; cb(NULL, d->user); }
}

void k_file_dialog(KApp *app, int mode, const char *title, const char *start_dir, const char *default_name, KFileCb cb, void *user) {
    FileDlg *d = calloc(1, sizeof *d);
    if (!d) return;
    d->mode = mode;
    d->cb = cb;
    d->user = user;
    snprintf(d->name, sizeof d->name, "%s", default_name ? default_name : "");
    const char *start = start_dir && *start_dir ? start_dir : getenv("HOME");
    if (!start) start = "/";
    snprintf(d->dir, sizeof d->dir, "%s", start);
    struct stat st;
    if (stat(d->dir, &st) != 0 || !S_ISDIR(st.st_mode)) snprintf(d->dir, sizeof d->dir, "/");
    fd_reload(d);
    d->win = kwin_new(app, -1, -1, 560, 400, title, FW_DIALOG | FW_CENTER | FW_TOP, fd_draw, d);
    if (!d->win) { free(d); return; }
    kwin_set_close_handler(d->win, fd_closed, d);
}
