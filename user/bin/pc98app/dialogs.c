/* The dialogs of the PC-98 application. They are windows of this program's own KApp (kwin_new), not the blocking k_messagebox: the machine
 * is held while one is open but the window stays alive, the other windows can still be used, and nothing nests an event loop inside
 * another. The file dialogs are the toolkit's own (k_file_dialog). */
#include "app.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

enum { DK_MESSAGE, DK_ABOUT, DK_QUIT };

typedef struct {
    KWin *win;
    int used, kind;
    char title[64];
    char text[900];
    KIcon icon;
} Dlg;

static Dlg g_dlg[6];                    /* a pool: a closed dialog's slot is free at once (its window is never drawn again) */

static Dlg *dlg_alloc(int kind, const char *title) {
    for (unsigned i = 0; i < sizeof g_dlg / sizeof g_dlg[0]; i++) {
        if (g_dlg[i].used) continue;
        memset(&g_dlg[i], 0, sizeof g_dlg[i]);
        g_dlg[i].used = 1;
        g_dlg[i].kind = kind;
        snprintf(g_dlg[i].title, sizeof g_dlg[i].title, "%s", title);
        return &g_dlg[i];
    }
    return NULL;
}

static void dlg_done(Dlg *d) {
    if (!d->used) return;
    d->used = 0;
    if (d->win) kwin_close(d->win);
    if (A.dialogs > 0) A.dialogs--;
}

static void dlg_closed(KWin *w, void *user) {         /* the window server asked for it to close (the X button) */
    (void)w;
    Dlg *d = user;
    if (d->kind == DK_QUIT) A.quit_asked = 0;
    dlg_done(d);
}

/* ---------------------------------------------------------------- wrapped text (the bitmap font: 8 pixels a character) */

static int wrap_lines(const char *text, int cols, int draw, KCanvas *cv, int x, int y, uint32_t color) {
    int lines = 0;
    const char *p = text;
    while (*p) {
        int n = 0, last_space = -1;
        const char *q = p;
        while (*q && *q != '\n' && n < cols) { if (*q == ' ') last_space = (int)(q - p); q++; n++; }
        size_t len = (size_t)(q - p);
        if (*q && *q != '\n' && last_space > 0) len = (size_t)last_space;
        if (draw) kc_text_n(cv, x, y + lines * 18, p, len, color);
        lines++;
        p += len;
        if (*p == ' ' || *p == '\n') p++;
    }
    return lines ? lines : 1;
}

/* ---------------------------------------------------------------- message box */

static void msg_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, d->icon, 16, 16, 32);
    wrap_lines(d->text, (cv->w - 76) / 8, 1, cv, 62, 16, ui->theme->text);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_ESC)) dlg_done(d);
}

void dlg_message(const char *title, KIcon icon, const char *fmt, ...) {
    Dlg *d = dlg_alloc(DK_MESSAGE, title);
    char text[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    say("pc98app: message: %s", text);
    if (!d) return;
    snprintf(d->text, sizeof d->text, "%s", text);
    d->icon = icon;
    int lines = wrap_lines(d->text, (480 - 76) / 8, 0, NULL, 0, 0, 0);
    int h = 90 + lines * 18;
    d->win = kwin_new(A.kapp, -1, -1, 480, h < 130 ? 130 : h, d->title, FW_DIALOG | FW_CENTER | FW_TOP, msg_draw, d);
    if (!d->win) { d->used = 0; return; }
    kwin_set_close_handler(d->win, dlg_closed, d);
    A.dialogs++;
}

/* ---------------------------------------------------------------- About */

/* The picture of the About box: a gap between two worlds (Yukari's), the old machine inside it, and a few eyes looking out. */
static void draw_gap(KCanvas *cv, int cx, int cy) {
    kc_ellipse_fill(cv, cx, cy, 74, 40, 0x2A1448);
    kc_ellipse_fill(cv, cx, cy, 64, 33, 0x4B2A78);
    kc_ellipse_fill(cv, cx, cy, 54, 26, 0x1A0C30);
    static const int eyes[][2] = { { -58, -6 }, { -36, -30 }, { 8, -34 }, { 44, -22 }, { 60, 8 }, { 30, 32 }, { -22, 34 }, { -56, 18 } };
    for (unsigned i = 0; i < sizeof eyes / sizeof eyes[0]; i++) {
        kc_ellipse_fill(cv, cx + eyes[i][0], cy + eyes[i][1], 7, 4, 0xF4EFD8);
        kc_circle_fill(cv, cx + eyes[i][0], cy + eyes[i][1], 2, 0xC8102E);
    }
    k_icon(cv, KI_MONITOR, cx - 24, cy - 24, 48);
}

static void about_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_gradient_v(cv, kr(0, 0, cv->w, 110), 0x0E1330, 0x3A2A78);
    draw_gap(cv, 86, 56);
    kc_text_scaled(cv, 176, 22, "PC-98", 0xFFF6E0, 2);
    kc_text(cv, 176, 58, "Yukari's Gap to PC-98", 0xB9C2F0);
    kc_text(cv, 176, 78, "the Hakurei emulator", 0xB9C2F0);
    int y = 124;
    static const char *const lines[] = {
        "An NEC PC-9801 in a window: a 386 at 16 MHz, the BIOS in C,",
        "the 640x400 display with its text layer, and the FM board.",
        "It runs your own PC-98 software from your own disk images.",
        "No BIOS, font or game data comes with it: the font ROM is",
        "read from the file named in ~/.config/pc98/config.",
        "Writes by a program never reach the image file: use",
        "File > Save changes as... to keep them in a new image.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) kc_text(cv, 16, y + (int)i * 18, lines[i], ui->theme->text);
    k_separator(ui, kr(16, cv->h - 78, cv->w - 32, 2));
    kc_text(cv, 16, cv->h - 68, "Touhou Project is the creation of ZUN / Team Shanghai Alice.", ui->theme->text_dim);
    kc_text(cv, 16, cv->h - 50, "An unofficial fan work.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ENTER) || k_key_pressed(ui, KEY_ESC)) dlg_done(d);
}

void dlg_about(void) {
    for (unsigned i = 0; i < sizeof g_dlg / sizeof g_dlg[0]; i++) if (g_dlg[i].used && g_dlg[i].kind == DK_ABOUT) return;    /* one is enough */
    Dlg *d = dlg_alloc(DK_ABOUT, "About PC-98");
    if (!d) return;
    say("pc98app: about box");
    d->win = kwin_new(A.kapp, -1, -1, 560, 350, d->title, FW_DIALOG | FW_CENTER | FW_TOP, about_draw, d);
    if (!d->win) { d->used = 0; return; }
    kwin_set_close_handler(d->win, dlg_closed, d);
    A.dialogs++;
}

/* ---------------------------------------------------------------- the images: insert, save, recent */

static void report_error(const char *what, const char *path, const char *err) {
    dlg_message(what, KI_ERROR, "%s\n\n%s", path, err);
}

static void do_insert(int prefer, const char *path) {
    char err[300];
    int slot = -1;
    if (emu_insert_auto(A.emu, path, prefer, &slot, err, sizeof err)) {
        report_error("Cannot use this image", path, err);
        return;
    }
    snprintf(A.dir, sizeof A.dir, "%s", path);
    char *slash = strrchr(A.dir, '/');
    if (slash) *slash = 0;
    recent_note(path);
    /* A hard disk, or a machine that has nothing running, starts over from its disks; a floppy put into a running machine is just there. */
    if (!A.emu->booted || A.emu->stopped || disk_slot_is_hd(slot)) {
        char berr[300];
        if (emu_boot(A.emu, -1, berr, sizeof berr)) dlg_message("The machine did not start", KI_WARNING, "%s", berr);
        else { sound_flush(); A.stop_shown = 0; }
    }
    ui_update_title();
}

static void insert_cb(const char *path, void *user) {
    A.dialogs--;
    if (path) do_insert((int)(intptr_t)user, path);
}

void dlg_insert(int slot) {
    char title[64];
    snprintf(title, sizeof title, "Insert an image into %s", disk_slot_name(slot));
    const char *start = A.dir[0] ? A.dir : getenv("HOME");
    A.dialogs++;                        /* (the machine is held while the main window has no focus and a dialog is open: ui_draw) */
    k_file_dialog(A.kapp, 0, title, start, "", insert_cb, (void *)(intptr_t)slot);
}

void dlg_open_recent(int index) {
    if (index >= 0 && index < A.recent.n) {
        char path[CONFIG_PATH_MAX];
        snprintf(path, sizeof path, "%s", A.recent.path[index]);
        do_insert(-1, path);
    }
}

/* The name a saved copy gets by default: the image's own name with "-saved" before the extension, a number added until it is new. */
void dlg_default_save_name(int slot, char *out, size_t cap) {
    const DiskSlot *s = &A.emu->disk[slot];
    char stem[DISK_PATH_MAX];
    snprintf(stem, sizeof stem, "%s", s->path[0] ? disk_basename(s->path) : "image");
    const char *dot = strrchr(stem, '.');
    char ext[32] = "";
    if (dot && dot != stem) { snprintf(ext, sizeof ext, "%s", dot); stem[dot - stem] = 0; }
    char dir[DISK_PATH_MAX] = "";
    if (s->path[0]) { snprintf(dir, sizeof dir, "%s", s->path); char *slash = strrchr(dir, '/'); if (slash) *slash = 0; else dir[0] = 0; }
    for (int n = 0; n < 100; n++) {
        char name[DISK_PATH_MAX + 32];
        if (n) snprintf(name, sizeof name, "%s-saved-%d%s", stem, n + 1, ext); else snprintf(name, sizeof name, "%s-saved%s", stem, ext);
        char full[DISK_PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", dir, name);
        struct stat st;
        if (stat(dir[0] ? full : name, &st) != 0 || n == 99) { snprintf(out, cap, "%s", name); return; }
    }
}

static void save_cb(const char *path, void *user) {
    int slot = (int)(intptr_t)user;
    A.dialogs--;
    if (!path) { A.quit_after_save = 0; return; }
    char err[300];
    if (emu_save(A.emu, slot, path, err, sizeof err)) {
        A.quit_after_save = 0;
        report_error("The image was not saved", path, err);
        return;
    }
    A.changed_blocks = emu_changed_total(A.emu);
    recent_note(path);
    say("pc98app: %s now comes from %s", disk_slot_name(slot), path);
    ui_update_title();
    if (A.quit_after_save) {
        A.quit_after_save = 0;
        if (A.changed_blocks) dlg_ask_quit();           /* another drive has changes too */
        else { A.confirmed_quit = 1; kapp_quit(A.kapp, 0); }
    }
}

void dlg_save(int slot) {
    if (slot < 0 || slot >= DISK_SLOTS || !A.emu->disk[slot].loaded) return;
    char name[DISK_PATH_MAX + 32], dir[DISK_PATH_MAX];
    dlg_default_save_name(slot, name, sizeof name);
    snprintf(dir, sizeof dir, "%s", A.emu->disk[slot].path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, "%s", A.dir[0] ? A.dir : "/");
    A.dialogs++;
    k_file_dialog(A.kapp, 1, "Save changes as a new image", dir, name, save_cb, (void *)(intptr_t)slot);
}

/* ---------------------------------------------------------------- "save the changes?" */

static int first_changed_slot(void) {
    for (int s = 0; s < DISK_SLOTS; s++) if (A.emu->disk[s].loaded && disk_changed(&A.emu->disk[s], NULL)) return s;
    return -1;
}

static void quit_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    Dlg *d = user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, KI_QUESTION, 16, 16, 32);
    wrap_lines(d->text, (cv->w - 76) / 8, 1, cv, 62, 16, ui->theme->text);
    int bw = 150, y = cv->h - 40;
    int slot = first_changed_slot();
    if (k_button(ui, kr(cv->w - 3 * bw - 36, y, bw, 26), "Save as...") || k_key_pressed(ui, KEY_ENTER)) {
        dlg_done(d);
        A.quit_asked = 0;
        A.quit_after_save = 1;
        if (slot >= 0) dlg_save(slot);
        else { A.quit_after_save = 0; A.confirmed_quit = 1; kapp_quit(A.kapp, 0); }
    } else if (k_button(ui, kr(cv->w - 2 * bw - 24, y, bw, 26), "Discard and quit")) {
        dlg_done(d);
        A.quit_asked = 0;
        A.confirmed_quit = 1;
        kapp_quit(A.kapp, 0);
    } else if (k_button(ui, kr(cv->w - bw - 12, y, bw, 26), "Keep running") || k_key_pressed(ui, KEY_ESC)) {
        dlg_done(d);
        A.quit_asked = 0;
    }
}

void dlg_ask_quit(void) {
    if (A.quit_asked) return;
    Dlg *d = dlg_alloc(DK_QUIT, "Quit PC-98?");
    if (!d) return;
    uint32_t n = A.emu ? emu_changed_total(A.emu) : 0;
    snprintf(d->text, sizeof d->text, "The program has written to its disks (%u blocks of 512 bytes changed). The image files are not touched: "
             "the changes live in memory and are lost when the emulator quits. Save them as a new image first?", n);
    d->win = kwin_new(A.kapp, -1, -1, 560, 190, d->title, FW_DIALOG | FW_CENTER | FW_TOP, quit_draw, d);
    if (!d->win) { d->used = 0; return; }
    kwin_set_close_handler(d->win, dlg_closed, d);
    A.dialogs++;
    A.quit_asked = 1;
    say("pc98app: asking before quit: %u blocks changed", n);
}

/* The window's close button, Alt+F4 and the Quit command all come here: with unsaved changes the question is asked, else the window closes. */
bool dlg_close_handler(KApp *app, void *user) {
    (void)app;
    (void)user;
    if (A.confirmed_quit) return true;
    if (A.emu && emu_changed_total(A.emu) > 0) {
        dlg_ask_quit();
        return false;
    }
    return true;
}
