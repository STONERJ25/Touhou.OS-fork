/*
 * kappa-panel: the Kappa Control Panel (settings).
 *
 *   kappa-panel                      the desktop application
 *   kappa-panel --pane NAME          start on a pane (system display input devices users power appearance)
 *   kappa-panel --trace              log what happens on stdout (used by the tests)
 *   kappa-panel --dry-run            Shut down and Restart only print what they would do
 *   kappa-panel --devices            print the device list as text and exit
 *   kappa-panel --conf get KEY       print a value of ~/.config/hakurei.conf and exit
 *   kappa-panel --conf set KEY VAL   change a value of ~/.config/hakurei.conf and exit
 */
#include "panel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

Panel P;

static const struct { const char *label, *name; KIcon icon; } k_cats[PANE_COUNT] = {
    { "System", "system", KI_YINYANG }, { "Display", "display", KI_MONITOR }, { "Keyboard & Mouse", "input", KI_GAME },
    { "Devices", "devices", KI_DRIVE }, { "Users", "users", KI_USER }, { "Power", "power", KI_POWER },
    { "Appearance", "appearance", KI_SKETCH },
};

static void select_pane(KUI *ui, int pane) {
    if (pane < 0 || pane >= PANE_COUNT || pane == P.pane) return;
    P.pane = pane;
    ui->focus = 0;
    trace("kappa-panel: pane %s", k_cats[pane].name);
}

static void sidebar(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_panel(cv, r, KB_SUNKEN, kc_shade(ui->theme->window_bg, 96));
    const int rowh = 42;
    for (int i = 0; i < PANE_COUNT; i++) {
        KRect row = kr(r.x + 3, r.y + 3 + i * rowh, r.w - 6, rowh - 2);
        bool sel = i == P.pane;
        bool hover = k_hover(ui, row);
        if (sel) kc_fill(cv, row, ui->theme->select_bg);
        else if (hover) kc_fill(cv, row, kc_mix(kc_shade(ui->theme->window_bg, 96), ui->theme->accent_light, 60));
        k_icon(cv, k_cats[i].icon, row.x + 8, row.y + (row.h - 24) / 2, 24);
        kc_text_in(cv, kr(row.x + 42, row.y, row.w - 46, row.h), k_cats[i].label, sel ? ui->theme->select_text : ui->theme->text, KA_LEFT);
        if (hover && ui->released[0]) select_pane(ui, i);
    }
}

static void handle_keys(KUI *ui) {
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool alt = k->mods & MOD_ALT, ctrl = k->mods & MOD_CTRL;
        if ((alt || (ctrl && P.pane != PANE_INPUT)) && k->code >= KEY_1 && k->code <= KEY_1 + PANE_COUNT - 1) {
            select_pane(ui, k->code - KEY_1);
            k->down = 0; k->code = 0;
        } else if (ctrl && !alt && (k->code == KEY_Q || k->code == KEY_W) && P.pane != PANE_INPUT) {
            kapp_quit(P.app, 0);
            k->down = 0; k->code = 0;
        }
    }
    if (P.pane == PANE_INPUT || ui->focus != 0) return;                       /* the key test must see every key */
    if (k_key_pressed(ui, KEY_DOWN)) select_pane(ui, (P.pane + 1) % PANE_COUNT);
    if (k_key_pressed(ui, KEY_UP)) select_pane(ui, (P.pane + PANE_COUNT - 1) % PANE_COUNT);
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    P.now_ms = ui->now_ms;
    k_clear(ui);
    handle_keys(ui);
    int sw = 184;
    sidebar(ui, kr(8, 8, sw, cv->h - 16));
    KRect pane = kr(8 + sw + 8, 8, cv->w - sw - 24, cv->h - 16);
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, pane));
    switch (P.pane) {
    case PANE_SYSTEM: pane_system(ui, pane); break;
    case PANE_DISPLAY: pane_display(ui, pane); break;
    case PANE_INPUT: pane_input(ui, pane); break;
    case PANE_DEVICES: pane_devices(ui, pane); break;
    case PANE_USERS: pane_users(ui, pane); break;
    case PANE_POWER: pane_power(ui, pane); break;
    case PANE_APPEARANCE: pane_appearance(ui, pane); break;
    }
    cv->clip = old;
}

static bool on_close(KApp *app, void *user) { (void)app; (void)user; return true; }

static int cli_conf(int argc, char **argv, int i) {
    char err[160], v[96];
    if (i + 2 < argc + 0 && !strcmp(argv[i + 1], "get")) {
        if (conf_get(argv[i + 2], v, sizeof v)) { puts(v); return 0; }
        return 1;
    }
    if (i + 3 < argc + 0 && !strcmp(argv[i + 1], "set")) {
        if (conf_set(argv[i + 2], argv[i + 3], err, sizeof err)) return 0;
        fprintf(stderr, "kappa-panel: %s\n", err);
        return 1;
    }
    fprintf(stderr, "usage: kappa-panel --conf get KEY | set KEY VALUE\n");
    return 2;
}

int main(int argc, char **argv) {
    int start = PANE_SYSTEM;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) P.trace = true;
        else if (!strcmp(argv[i], "--dry-run")) P.dry_run = true;
        else if (!strcmp(argv[i], "--conf")) return cli_conf(argc, argv, i);
        else if (!strcmp(argv[i], "--devices")) { devices_refresh(); doc_dump(devices_doc()); return 0; }
        else if (!strcmp(argv[i], "--pane") && i + 1 < argc) {
            for (int c = 0; c < PANE_COUNT; c++) if (!strcmp(argv[i + 1], k_cats[c].name)) start = c;
            i++;
        }
    }
    KApp *app = kapp_new("Control Panel", 880, 580, FW_RESIZABLE);
    if (!app) return 1;
    P.app = app;
    P.pane = start;
    kapp_set_draw(app, draw, NULL);
    kapp_set_close_handler(app, on_close, NULL);
    kapp_set_minsize(app, 640, 440);
    kapp_set_tick(app, 1000);
    trace("kappa-panel: ready");
    int rc = kapp_run(app);
    kapp_destroy(app);
    return rc;
}
