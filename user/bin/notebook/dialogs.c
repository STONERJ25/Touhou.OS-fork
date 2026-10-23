/* Dialogs of the editor: a message / choice box (the stock k_confirm_dialog only has two buttons and cannot tell Esc from "No",
 * which is not good enough for "save changes?"), the About box and the shortcut list. All of them are non-blocking windows of
 * the editor's KApp; while one is open the editor ignores input (A.modal). */
#include "notebook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nb_modal_enter(void) { A.modal++; }
void nb_modal_leave(void) { if (A.modal > 0) A.modal--; }

/* ---------------------------------------------------------------- message / choice */

typedef struct {
    ChoiceCb cb;
    void *user;
    char msg[640];
    KIcon icon;
    char labels[4][40];
    int n, cancel;
    KWin *win;
    bool done;
} Choice;

#define CH_W 470
#define CH_TEXT_X 66

static int wrap_lines(const char *msg, int maxcols, int *starts, int *lens, int cap) {
    int n = 0;
    const char *p = msg;
    while (*p && n < cap) {
        int cols = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && cols < maxcols) { if (*q == ' ') last_space = (int)(q - p); q++; cols++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        if (starts) starts[n] = (int)(p - msg);
        if (lens) lens[n] = (int)len;
        n++;
        p += len;
        if (*p == ' ' || *p == '\n') p++;
    }
    return n;
}

static void choice_finish(Choice *c, int which) {
    if (c->done) return;
    c->done = true;
    ChoiceCb cb = c->cb;
    void *user = c->user;
    kwin_close(c->win);
    nb_modal_leave();
    if (cb) cb(which, user);
}

static void choice_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Choice *c = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, c->icon, 18, 18, 32);
    int maxcols = (cv->w - CH_TEXT_X - 16) / 8;
    int st[24], ln[24];
    int nl = wrap_lines(c->msg, maxcols, st, ln, 24);
    for (int i = 0; i < nl; i++) kc_text_n(cv, CH_TEXT_X, 18 + i * 18, c->msg + st[i], (size_t)ln[i], ui->theme->text);
    int total = 0, bw[4];
    for (int i = 0; i < c->n; i++) { bw[i] = kc_text_width(c->labels[i]) + 28; if (bw[i] < 84) bw[i] = 84; total += bw[i] + (i ? 8 : 0); }
    int x = cv->w - 16 - total, by = cv->h - 40;
    for (int i = 0; i < c->n; i++) {
        if (k_button(ui, kr(x, by, bw[i], 26), c->labels[i])) { choice_finish(c, i); return; }
        if (i == 0) kc_outline(cv, kr_inset(kr(x, by, bw[i], 26), 3), ui->theme->accent);      /* the default button */
        x += bw[i] + 8;
    }
    if (k_key_pressed(ui, KEY_ESC)) { choice_finish(c, c->cancel); return; }
    if (k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_KPENTER)) choice_finish(c, 0);
}

static void choice_closed(KWin *w, void *user) {
    (void)w;
    Choice *c = user;
    choice_finish(c, c->cancel);
}

void dlg_choice(KApp *app, const char *title, const char *msg, KIcon icon, const char *const *labels, int n, int cancel, ChoiceCb cb, void *user) {
    Choice *c = calloc(1, sizeof *c);
    if (!c) { if (cb) cb(cancel, user); return; }
    c->cb = cb;
    c->user = user;
    c->icon = icon;
    c->n = n > 4 ? 4 : n;
    c->cancel = cancel;
    snprintf(c->msg, sizeof c->msg, "%s", msg);
    for (int i = 0; i < c->n; i++) snprintf(c->labels[i], sizeof c->labels[i], "%s", labels[i]);
    int lines = wrap_lines(c->msg, (CH_W - CH_TEXT_X - 16) / 8, NULL, NULL, 24);
    int h = 90 + (lines < 2 ? 2 : lines) * 18;
    nb_modal_enter();
    c->win = kwin_new(app, -1, -1, CH_W, h, title, FW_DIALOG | FW_CENTER | FW_TOP, choice_draw, c);
    if (!c->win) {
        nb_modal_leave();
        if (cb) cb(cancel, user);
        free(c);
        return;
    }
    kwin_set_close_handler(c->win, choice_closed, c);
    nb_trace("dialog %s", title);
}

void dlg_message(KApp *app, const char *title, const char *msg, KIcon icon) {
    static const char *const ok[] = { "OK" };
    dlg_choice(app, title, msg, icon, ok, 1, 0, NULL, NULL);
}

/* ---------------------------------------------------------------- About */

static KWin *g_about;

static void about_close(void) {
    if (!g_about) return;
    kwin_close(g_about);
    g_about = NULL;
    nb_modal_leave();
}

static void about_closed(KWin *w, void *user) {
    (void)w; (void)user;
    if (g_about) { g_about = NULL; nb_modal_leave(); }
}

static void about_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_gradient_v(cv, kr(0, 0, cv->w, 96), 0x2A1650, 0x6C3FA3);
    k_icon(cv, KI_NOTEBOOK, 22, 16, 64);
    kc_text_scaled(cv, 104, 24, "Patchouli's Notebook", 0xFFF6E0, 2);
    kc_text(cv, 104, 62, "Version " NB_VERSION " - text editor of the Voile Library", 0xD9CCF2);
    static const char *const lines[] = {
        "Patchouli reads every page twice and forgets nothing, so",
        "your pages are safe here: every edit can be undone, and a",
        "page with unsaved changes carries a star (*) in its title.",
        "",
        "Plain UTF-8 text with LF, CRLF or CR line endings. Colours",
        "for C, shell scripts and Markdown. Big files are fine.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) kc_text(cv, 24, 112 + (int)i * 18, lines[i], ui->theme->text);
    kc_text(cv, 24, 232, "Part of TouhouOS. An unofficial fan work: Touhou Project", ui->theme->text_dim);
    kc_text(cv, 24, 250, "belongs to ZUN / Team Shanghai Alice.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) about_close();
}

void dlg_about(KApp *app) {
    if (g_about) return;
    nb_modal_enter();
    g_about = kwin_new(app, -1, -1, 520, 310, "About Patchouli's Notebook", FW_DIALOG | FW_CENTER | FW_TOP, about_draw, NULL);
    if (!g_about) { nb_modal_leave(); return; }
    kwin_set_close_handler(g_about, about_closed, NULL);
    nb_trace("dialog about");
}

/* ---------------------------------------------------------------- shortcuts */

typedef struct { const char *keys, *what; } Row;
static const Row k_left[] = {
    { "FILE", NULL },
    { "Ctrl+N", "New page" }, { "Ctrl+Shift+N", "New window" }, { "Ctrl+O", "Open..." }, { "Ctrl+S", "Save" },
    { "Ctrl+Shift+S", "Save As..." }, { "Ctrl+W", "Close the page" }, { "Ctrl+Q", "Quit" },
    { "EDIT", NULL },
    { "Ctrl+Z", "Undo" }, { "Ctrl+Y", "Redo (Ctrl+Shift+Z)" }, { "Ctrl+X / C / V", "Cut, copy, paste" },
    { "Ctrl+A", "Select all" }, { "Ctrl+F", "Find" }, { "F3 / Shift+F3", "Next / previous match" },
    { "Ctrl+H", "Find and replace" }, { "Ctrl+G", "Go to line" },
};
static const Row k_right[] = {
    { "TEXT", NULL },
    { "Tab / Shift+Tab", "Indent / unindent" }, { "Ctrl+] / Ctrl+[", "Indent / unindent" },
    { "Ctrl+Shift+D", "Duplicate line" }, { "Ctrl+Shift+K", "Delete line" }, { "Alt+Up / Down", "Move line" },
    { "Ctrl+Left / Right", "Move by word" }, { "Ctrl+Bksp / Del", "Delete a word" }, { "Home", "Start of text / line" },
    { "MOUSE", NULL },
    { "Double click", "Select a word" }, { "Triple click", "Select a line" }, { "Click a number", "Select a line" },
    { "Wheel", "Scroll, Shift: sideways" },
    { "WINDOW", NULL },
    { "Alt+F E V H", "Open a menu" }, { "F1", "This list" }, { "Esc", "Close the find bar" },
};

static KWin *g_help;

static void help_close(void) {
    if (!g_help) return;
    kwin_close(g_help);
    g_help = NULL;
    nb_modal_leave();
}

static void help_closed(KWin *w, void *user) {
    (void)w; (void)user;
    if (g_help) { g_help = NULL; nb_modal_leave(); }
}

static void help_column(KUI *ui, int x, int y, const Row *rows, int n, int keyw, int linew) {
    KCanvas *cv = &ui->cv;
    for (int i = 0; i < n; i++) {
        if (!rows[i].what) {
            if (i) y += 6;
            kc_text_bold(cv, x, y, rows[i].keys, ui->theme->accent_dark);
            kc_hline(cv, x, y + 17, linew, kc_shade(ui->theme->window_bg, 80));
            y += 22;
            continue;
        }
        kc_text(cv, x, y, rows[i].keys, ui->theme->accent_dark);
        kc_text(cv, x + keyw, y, rows[i].what, ui->theme->text);
        y += 18;
    }
}

static void help_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    help_column(ui, 14, 12, k_left, (int)(sizeof k_left / sizeof k_left[0]), 124, 296);
    help_column(ui, 340, 12, k_right, (int)(sizeof k_right / sizeof k_right[0]), 148, 346);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "Close") || k_key_pressed(ui, KEY_ESC) || k_key_pressed(ui, KEY_ENTER)) help_close();
}

void dlg_help(KApp *app) {
    if (g_help) return;
    nb_modal_enter();
    g_help = kwin_new(app, -1, -1, 704, 420, "Notebook keyboard shortcuts", FW_DIALOG | FW_CENTER | FW_TOP, help_draw, NULL);
    if (!g_help) { nb_modal_leave(); return; }
    kwin_set_close_handler(g_help, help_closed, NULL);
    nb_trace("dialog shortcuts");
}
