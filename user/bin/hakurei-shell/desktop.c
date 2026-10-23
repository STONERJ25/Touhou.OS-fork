/* The desktop: wallpaper, icons, the right-click menu, the About box and the shutdown dialog. */
#include "shell.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <gensokyo.h>
#include <audio.h>
#include <sys/utsname.h>

static uint32_t *g_wall;
static int g_wall_w, g_wall_h;

typedef struct { const char *label; KIcon icon; char cmd[200]; bool present; } DeskIcon;
static DeskIcon g_icons[8];
static int g_nicons;

static void init_icons(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/";
    struct { const char *label; KIcon icon; const char *bin; char args[160]; } defs[] = {
        { "Home", KI_HOME, "explorer", "" }, { "Netherworld", KI_TRASH, "explorer", "/Netherworld" },
        { "Terminal", KI_TERMINAL, "gapterm", "" }, { "Explorer", KI_FOLDER, "explorer", "/" },
        { "Notebook", KI_NOTEBOOK, "notebook", "" }, { "Spell Manager", KI_SPELLCARD, "spellmgr", "" },
    };
    snprintf(defs[0].args, sizeof defs[0].args, "%s", home);
    for (unsigned i = 0; i < sizeof defs / sizeof defs[0] && g_nicons < 8; i++) {
        char path[64];
        snprintf(path, sizeof path, "/bin/%s", defs[i].bin);
        if (access(path, X_OK) != 0) continue;
        DeskIcon *d = &g_icons[g_nicons++];
        d->label = defs[i].label;
        d->icon = defs[i].icon;
        snprintf(d->cmd, sizeof d->cmd, "%s %s", defs[i].bin, defs[i].args);
    }
}

void desktop_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    if (!g_nicons) init_icons();
    if (!g_wall || g_wall_w != cv->w || g_wall_h != cv->h) {
        free(g_wall);
        g_wall = malloc((size_t)cv->w * (size_t)cv->h * 4);
        g_wall_w = cv->w;
        g_wall_h = cv->h;
        if (g_wall) wallpaper_render(g_wall, cv->w, cv->h, cv->w);
    }
    if (g_wall) for (int y = 0; y < cv->h; y++) memcpy(cv->px + (size_t)y * cv->stride, g_wall + (size_t)y * g_wall_w, (size_t)cv->w * 4);
    reap_children();

    int hover = -1;
    for (int i = 0; i < g_nicons; i++) {
        KRect r = kr(16, 16 + i * 92, 88, 84);
        bool sel = G.selected_icon == i;
        if (k_hover(ui, r)) hover = i;
        if (sel) kc_fill_alpha(cv, r, 0xB5122B, 150);
        else if (hover == i) kc_fill_alpha(cv, r, 0xFFFFFF, 40);
        k_icon(cv, g_icons[i].icon, r.x + (r.w - 40) / 2, r.y + 6, 40);
        int tw = kc_text_width(g_icons[i].label);
        int tx = r.x + (r.w - tw) / 2;
        kc_text(cv, tx + 1, r.y + 56, g_icons[i].label, 0x000000);
        kc_text(cv, tx, r.y + 55, g_icons[i].label, 0xFFF6E0);
    }
    if (ui->pressed[0]) {
        G.selected_icon = hover;
        if (G.menu) menu_close();
        if (hover >= 0 && ui->double_click) launch(g_icons[hover].cmd);
    }
    if (ui->pressed[1]) {                                       /* right button: pressed[0] left, [1] right, [2] middle */
        G.selected_icon = hover;
        context_open(ui->mx, ui->my);
    }
}

/* ---------------------------------------------------------------- context menu */

typedef struct { const char *label; KIcon icon; int action; } CtxItem;
enum { CA_TERM, CA_FILES, CA_SHOT, CA_ABOUT, CA_LOCK, CA_SHUTDOWN, CA_SEP };
static const CtxItem g_ctx[] = {
    { "Open Terminal", KI_TERMINAL, CA_TERM }, { "Gensokyo Explorer", KI_FOLDER, CA_FILES }, { NULL, 0, CA_SEP },
    { "Take screenshot", KI_IMAGE, CA_SHOT }, { "About TouhouOS", KI_INFO, CA_ABOUT }, { NULL, 0, CA_SEP },
    { "Lock screen", KI_LOCK, CA_LOCK },
    { "Shut down...", KI_POWER, CA_SHUTDOWN },
};

static void context_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    kc_panel(cv, kr(0, 0, cv->w, cv->h), KB_RAISED, ui->theme->window_bg);
    int y = 3;
    for (unsigned i = 0; i < sizeof g_ctx / sizeof g_ctx[0]; i++) {
        if (g_ctx[i].action == CA_SEP) { k_separator(ui, kr(8, y + 3, cv->w - 16, 2)); y += 8; continue; }
        KRect r = kr(3, y, cv->w - 6, 26);
        bool hover = k_hover(ui, r);
        if (hover) kc_fill(cv, r, ui->theme->select_bg);
        k_icon(cv, g_ctx[i].icon, r.x + 6, r.y + 5, 16);
        kc_text(cv, r.x + 30, r.y + 5, g_ctx[i].label, hover ? ui->theme->select_text : ui->theme->text);
        if (hover && ui->released[0]) {
            switch (g_ctx[i].action) {
            case CA_TERM: launch("gapterm"); break;
            case CA_FILES: launch("explorer /"); break;
            case CA_SHOT: fairy_screenshot(kapp_display(G.app), "/tmp/screenshot.ppm"); break;
            case CA_ABOUT: about_open(); break;
            case CA_LOCK: launch("hakurei-lock"); break;
            case CA_SHUTDOWN: shutdown_open(); break;
            }
            kwin_close(G.context);
            G.context = NULL;
            return;
        }
        y += 26;
    }
}

static void context_closed(KWin *w, void *user) { (void)w; (void)user; G.context = NULL; }

void context_open(int x, int y) {
    if (G.context) { kwin_close(G.context); G.context = NULL; }
    int h = 6;
    for (unsigned i = 0; i < sizeof g_ctx / sizeof g_ctx[0]; i++) h += g_ctx[i].action == CA_SEP ? 8 : 26;
    if (x + 200 > G.screen_w) x = G.screen_w - 200;
    if (y + h > G.screen_h - PANEL_H) y = G.screen_h - PANEL_H - h;
    G.context = kwin_new(G.app, x, y, 200, h, "Menu", FW_POPUP | FW_NO_DECOR, context_draw, NULL);
    if (G.context) kwin_set_close_handler(G.context, context_closed, NULL);
}

/* ---------------------------------------------------------------- About */

static void about_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    kc_gradient_v(cv, kr(0, 0, cv->w, 92), 0x0E1330, 0x2A3A78);
    k_icon(cv, KI_YINYANG, 24, 14, 64);
    kc_text_scaled(cv, 108, 22, "TouhouOS", 0xFFF6E0, 2);
    kc_text(cv, 108, 58, "Gensokyo kernel", 0xB9C2F0);
    struct utsname u;
    uname(&u);
    char line[160];
    snprintf(line, sizeof line, "Version %s \"%s\"", u.release, u.codename);
    k_label(ui, 24, 108, line);
    snprintf(line, sizeof line, "%s on %s", u.sysname, u.machine);
    k_label(ui, 24, 128, line);
    struct gsysinfo si;
    if (gap_sysinfo(&si) == 0) {
        snprintf(line, sizeof line, "%s", si.cpu_brand);
        k_label(ui, 24, 148, line);
        snprintf(line, sizeof line, "%lu MiB memory, %lu MiB free", (unsigned long)(si.total_ram >> 20), (unsigned long)(si.free_ram >> 20));
        k_label(ui, 24, 168, line);
    }
    k_separator(ui, kr(16, 194, cv->w - 32, 2));
    kc_text(cv, 24, 204, "An unofficial Touhou Project fan work. Touhou Project", ui->theme->text_dim);
    kc_text(cv, 24, 222, "is the creation of ZUN / Team Shanghai Alice.", ui->theme->text_dim);
    if (k_button(ui, kr(cv->w - 100, cv->h - 40, 84, 26), "OK") || k_key_pressed(ui, KEY_ESC)) { kwin_close(G.about); G.about = NULL; }
}

static void about_closed(KWin *w, void *user) { (void)w; (void)user; G.about = NULL; }

void about_open(void) {
    if (G.about) return;
    G.about = kwin_new(G.app, -1, -1, 460, 290, "About TouhouOS", FW_DIALOG | FW_CENTER, about_draw, NULL);
    if (G.about) kwin_set_close_handler(G.about, about_closed, NULL);
}

/* ---------------------------------------------------------------- shutdown dialog */

static void shutdown_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    k_clear(ui);
    k_icon(cv, KI_POWER, 20, 18, 32);
    kc_text_bold(cv, 66, 20, "Leave the shrine?", ui->theme->accent_dark);
    kc_text(cv, 66, 44, "Reimu will close the shrine for the night.", ui->theme->text);
    int bw = 88, x = 16, y = cv->h - 42;
    if (k_button(ui, kr(x, y, bw, 28), "Shut down")) {
        audio_event_wait(AUDIO_EV_SHUTDOWN);
        sync();
        gap_reboot(REBOOT_POWEROFF);
    }
    if (k_button(ui, kr(x + bw + 8, y, bw, 28), "Restart")) {
        audio_event_wait(AUDIO_EV_SHUTDOWN);
        sync();
        gap_reboot(REBOOT_RESTART);
    }
    if (k_button(ui, kr(x + 2 * (bw + 8), y, bw, 28), "Log out")) { audio_event(AUDIO_EV_SHUTDOWN); G.quit = true; kapp_quit(G.app, 0); }
    if (k_button(ui, kr(x + 3 * (bw + 8), y, bw, 28), "Cancel") || k_key_pressed(ui, KEY_ESC)) { kwin_close(G.shutdown); G.shutdown = NULL; }
}

static void shutdown_closed(KWin *w, void *user) { (void)w; (void)user; G.shutdown = NULL; }

void shutdown_open(void) {
    if (G.shutdown) return;
    G.shutdown = kwin_new(G.app, -1, -1, 424, 130, "Shut down", FW_DIALOG | FW_CENTER | FW_TOP, shutdown_draw, NULL);
    if (G.shutdown) kwin_set_close_handler(G.shutdown, shutdown_closed, NULL);
}
