/* One frame of the PC-98 application's window (app.h): the machine is run for the time that is owed, its picture is drawn at a whole-number
 * scale in the middle of the window, under a menu bar and over a status line; the F12 menu, the commands and the keys are here too. */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- the picture */

static int picture_factor(int area_w, int area_h) {
    int s = A.emu->cfg.scale;
    if (s > 0) return s > 8 ? 8 : s;                                /* a fixed scale */
    int f = area_w / EMU_W < area_h / EMU_H ? area_w / EMU_W : area_h / EMU_H;      /* fit: the largest whole number that fits */
    return f < 1 ? 1 : f > 8 ? 8 : f;
}

/* The machine's last picture, enlarged by `f` (nearest neighbour: PC-98 pixels are meant to be square blocks) and centred in `area`; the rest
 * of the area is black. The window is a 1x canvas (kapp_set_hidpi(false)): on a hi-dpi screen the window server enlarges the whole window. */
static void draw_picture(KUI *ui, KRect area) {
    KCanvas *cv = &ui->cv;
    kc_fill(cv, area, 0x000000);
    const Emu *e = A.emu;
    if (!e->have_picture || cv->scale > 1) return;
    int f = picture_factor(area.w, area.h);
    int pw = EMU_W * f, ph = EMU_H * f;
    int x0 = area.x + (area.w - pw) / 2, y0 = area.y + (area.h - ph) / 2;
    int xa = x0 < area.x ? area.x : x0, xb = x0 + pw > area.x + area.w ? area.x + area.w : x0 + pw;       /* the visible columns */
    int ya = y0 < area.y ? area.y : y0, yb = y0 + ph > area.y + area.h ? area.y + area.h : y0 + ph;
    if (xb <= xa || yb <= ya) return;
    static uint32_t row[EMU_W * 8];
    int last_sy = -1;
    for (int y = ya; y < yb; y++) {
        int sy = (y - y0) / f;
        uint32_t *dst = cv->px + (size_t)y * (size_t)cv->stride + (size_t)xa;
        if (f == 1) {
            memcpy(dst, e->pixels + (size_t)sy * EMU_W + (size_t)(xa - x0), (size_t)(xb - xa) * 4);
            continue;
        }
        if (sy != last_sy) {                                        /* a source row is enlarged once and copied for the f rows it covers */
            const uint32_t *src = e->pixels + (size_t)sy * EMU_W;
            for (int sx = 0; sx < EMU_W; sx++) for (int k = 0; k < f; k++) row[sx * f + k] = src[sx];
            last_sy = sy;
        }
        memcpy(dst, row + (xa - x0), (size_t)(xb - xa) * 4);
    }
}

/* ---------------------------------------------------------------- the status line */

static void build_status(void) {
    Emu *e = A.emu;
    char left[160], right[100];
    static uint64_t prev_skipped, prev_cycles;
    static int skip_pct;
    uint64_t cyc = e->booted ? e->m->cpu.cycles : 0;
    if (e->booted && cyc > prev_cycles + 4000000 && e->idle->skipped_clocks >= prev_skipped) {
        skip_pct = (int)((e->idle->skipped_clocks - prev_skipped) * 100 / (cyc - prev_cycles));
        prev_skipped = e->idle->skipped_clocks;
        prev_cycles = cyc;
    } else if (cyc < prev_cycles) { prev_skipped = 0; prev_cycles = cyc; skip_pct = 0; }
    if (!e->booted) snprintf(left, sizeof left, "No disk in the drives");
    else if (e->stopped) snprintf(left, sizeof left, "Stopped");
    else if (e->paused) snprintf(left, sizeof left, "Paused");
    else if (e->held) snprintf(left, sizeof left, "Held");
    else {
        const Pacer *p = &e->pacer;
        snprintf(left, sizeof left, "%u.%u fps  %u%% speed%s", p->fps_x10 / 10, p->fps_x10 % 10, p->speed_x10 / 10,
                 e->cfg.idle && skip_pct > 0 ? "  (idle skip " : "");
        if (e->cfg.idle && skip_pct > 0) {
            size_t n = strlen(left);
            snprintf(left + n, sizeof left - n, "%d%%)", skip_pct);
        }
    }
    if (A.changed_blocks) {
        size_t n = strlen(left);
        snprintf(left + n, sizeof left - n, "  | disk changed in memory: %u blocks", A.changed_blocks);
    }
    snprintf(right, sizeof right, "%s%s", e->font_kind == EMU_FONT_BUILTIN ? "built-in font: no kanji" : e->font_kind == EMU_FONT_ANK ? "font ROM: ANK only" : "font ROM",
             A.audio || !e->cfg.sound ? "" : "  | no sound device");
    snprintf(A.status, sizeof A.status, "%s\x01%s", left, right);           /* \x01 separates the left and the right part */
}

static void draw_status(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_fill(cv, r, ui->theme->window_bg);
    kc_hline(cv, r.x, r.y, r.w, 0xFFFFFF);
    kc_hline(cv, r.x, r.y + 1, r.w, kc_shade(ui->theme->window_bg, 80));
    char left[300], right[120];
    const char *sep = strchr(A.status, '\x01');
    size_t ln = sep ? (size_t)(sep - A.status) : strlen(A.status);
    snprintf(left, sizeof left, "%.*s", (int)ln, A.status);
    snprintf(right, sizeof right, "%s", sep ? sep + 1 : "");
    int rw = (int)strlen(right) * 8 + 12;
    kc_text_in(cv, kr(r.x + 6, r.y + 2, r.w - rw - 8, r.h - 2), left, ui->theme->text, KA_LEFT);
    kc_text_in(cv, kr(r.x + r.w - rw, r.y + 2, rw - 4, r.h - 2), right, ui->theme->text_dim, KA_RIGHT);
}

/* ---------------------------------------------------------------- the title */

void ui_update_title(void) {
    Emu *e = A.emu;
    const char *name = "no disk";
    static const int order[DISK_SLOTS] = { SLOT_FD1, SLOT_FD2, SLOT_HD1, SLOT_HD2 };
    for (int i = 0; i < DISK_SLOTS; i++)
        if (e->disk[order[i]].loaded) { name = e->disk[order[i]].path[0] ? disk_basename(e->disk[order[i]].path) : "image"; break; }
    char t[220];
    snprintf(t, sizeof t, "PC-98 - %s%s%s", name, A.changed_blocks ? "*" : "", e->paused ? " (paused)" : e->stopped ? " (stopped)" : "");
    if (strcmp(t, A.title)) {
        snprintf(A.title, sizeof A.title, "%s", t);
        kapp_set_title(A.kapp, t);
    }
}

/* ---------------------------------------------------------------- commands */

static void do_reset(int dev) {
    char err[300];
    if (emu_boot(A.emu, dev, err, sizeof err)) dlg_message("The machine did not start", KI_WARNING, "%s", err);
    else { sound_flush(); A.stop_shown = 0; }
    ui_update_title();
}

static void resize_for_scale(int s) {
    if (s <= 0) return;
    int want_w = EMU_W * s, want_h = MENU_H + EMU_H * s + STATUS_H;
    int ox, oy, ow, oh;
    fairy_window_geometry(kapp_window(A.kapp), &ox, &oy, &ow, &oh);
    kapp_move(A.kapp, ox, oy, ow + (want_w - kapp_width(A.kapp)), oh + (want_h - kapp_height(A.kapp)));
}

static int slot_with_changes(void) {
    for (int s = 0; s < DISK_SLOTS; s++) if (A.emu->disk[s].loaded && disk_changed(&A.emu->disk[s], NULL)) return s;
    return -1;
}

void ui_command(int cmd) {
    Emu *e = A.emu;
    uint64_t t = now_ns();
    char err[300];
    unsigned touch = 0;                                     /* the settings this command changes: TOUCH_* (they are written to the settings file) */
    if (cmd >= CMD_RECENT0 && cmd < CMD_RECENT0 + RECENT_MAX) { dlg_open_recent(cmd - CMD_RECENT0); return; }
    switch (cmd) {
    case CMD_PAUSE: emu_pause(e, !e->paused, t); sound_flush(); ui_update_title(); break;
    case CMD_RESET: do_reset(-1); break;
    case CMD_RESET_HD: do_reset(disk_slot_daua(e->disk[SLOT_HD1].loaded ? SLOT_HD1 : SLOT_HD2)); break;
    case CMD_INSERT_FD1: dlg_insert(SLOT_FD1); break;
    case CMD_INSERT_FD2: dlg_insert(SLOT_FD2); break;
    case CMD_INSERT_HD: dlg_insert(SLOT_HD1); break;
    case CMD_EJECT_FD1: emu_eject(e, SLOT_FD1); ui_update_title(); break;
    case CMD_EJECT_FD2: emu_eject(e, SLOT_FD2); ui_update_title(); break;
    case CMD_SAVE: { int s = slot_with_changes(); if (s >= 0) dlg_save(s); break; }
    case CMD_REVERT:
        for (int s = 0; s < DISK_SLOTS; s++)
            if (e->disk[s].loaded && disk_changed(&e->disk[s], NULL) && emu_revert(e, s, err, sizeof err)) dlg_message("Cannot read the image again", KI_ERROR, "%s", err);
        A.changed_blocks = emu_changed_total(e);
        do_reset(-1);
        break;
    case CMD_QUIT: if (dlg_close_handler(A.kapp, NULL)) kapp_quit(A.kapp, 0); break;
    case CMD_SPEED_50: emu_set_speed(e, 50, t); sound_flush(); touch = TOUCH_SPEED; break;
    case CMD_SPEED_100: emu_set_speed(e, 100, t); touch = TOUCH_SPEED; break;
    case CMD_SPEED_200: emu_set_speed(e, 200, t); sound_flush(); touch = TOUCH_SPEED; break;
    case CMD_SPEED_MAX: emu_set_speed(e, 0, t); sound_flush(); touch = TOUCH_SPEED; break;
    case CMD_SOUND:
        if (!A.audio) dlg_message("No sound", KI_INFO, "This machine has no sound device the emulator can use (see docs/AUDIO.md).");
        else { emu_set_sound(e, !e->sound); sound_flush(); touch = TOUCH_SOUND; }
        break;
    case CMD_VOLUME_UP: emu_set_volume(e, e->cfg.volume + 10 > 100 ? 100 : e->cfg.volume + 10); touch = TOUCH_VOLUME; break;
    case CMD_VOLUME_DOWN: emu_set_volume(e, e->cfg.volume - 10 < 0 ? 0 : e->cfg.volume - 10); touch = TOUCH_VOLUME; break;
    case CMD_SCALE_1: e->cfg.scale = 1; resize_for_scale(1); touch = TOUCH_SCALE; break;
    case CMD_SCALE_2: e->cfg.scale = 2; resize_for_scale(2); touch = TOUCH_SCALE; break;
    case CMD_SCALE_3: e->cfg.scale = 3; resize_for_scale(3); touch = TOUCH_SCALE; break;
    case CMD_SCALE_FIT: e->cfg.scale = 0; touch = TOUCH_SCALE; break;
    case CMD_IDLE: emu_set_idle(e, !e->cfg.idle); touch = TOUCH_IDLE; break;
    case CMD_ABOUT: dlg_about(); break;
    case CMD_OVERLAY: A.overlay = !A.overlay; A.overlay_sel = 0; say("pc98app: menu %s", A.overlay ? "open" : "closed"); break;
    default: break;
    }
    if (touch) {
        A.touched |= touch;
        settings_save();
    }
}

/* ---------------------------------------------------------------- the menu bar */

static const char *check(bool on) { return on ? "[x] " : "[ ] "; }

typedef struct { char label[72]; int cmd; bool disabled; bool separator; const char *shortcut; } Entry;

static int menu_file(Entry *en) {
    Emu *e = A.emu;
    int n = 0;
    en[n++] = (Entry){ "Insert floppy 1...", CMD_INSERT_FD1 };
    en[n++] = (Entry){ "Insert floppy 2...", CMD_INSERT_FD2 };
    en[n++] = (Entry){ "Open hard disk image...", CMD_INSERT_HD };
    en[n++] = (Entry){ "Eject floppy 1", CMD_EJECT_FD1, !e->disk[SLOT_FD1].loaded };
    en[n++] = (Entry){ "Eject floppy 2", CMD_EJECT_FD2, !e->disk[SLOT_FD2].loaded };
    en[n++] = (Entry){ "", 0, false, true };
    en[n++] = (Entry){ "Save changes as...", CMD_SAVE, !A.changed_blocks };
    en[n++] = (Entry){ "Discard changes and restart", CMD_REVERT, !A.changed_blocks };
    if (A.recent.n) {
        en[n++] = (Entry){ "", 0, false, true };
        for (int i = 0; i < A.recent.n && i < 5; i++) {
            const char *base = disk_basename(A.recent.path[i]);
            snprintf(en[n].label, sizeof en[n].label, "%d  %.60s", i + 1, base);
            en[n].cmd = CMD_RECENT0 + i;
            n++;
        }
    }
    en[n++] = (Entry){ "", 0, false, true };
    en[n++] = (Entry){ "Quit", CMD_QUIT };
    return n;
}

static int menu_machine(Entry *en) {
    Emu *e = A.emu;
    int n = 0, sp = e->cfg.speed;
    en[n++] = (Entry){ "", CMD_PAUSE };
    snprintf(en[n - 1].label, sizeof en[n - 1].label, "%s", e->paused ? "Resume" : "Pause");
    en[n++] = (Entry){ "Reset", CMD_RESET };
    en[n++] = (Entry){ "Reset from the hard disk", CMD_RESET_HD, !e->disk[SLOT_HD1].loaded && !e->disk[SLOT_HD2].loaded };
    en[n++] = (Entry){ "", 0, false, true };
    snprintf(en[n].label, sizeof en[n].label, "%sHalf speed (50%%)", check(sp == 50)); en[n++].cmd = CMD_SPEED_50;
    snprintf(en[n].label, sizeof en[n].label, "%sReal time (100%%)", check(sp == 100)); en[n++].cmd = CMD_SPEED_100;
    snprintf(en[n].label, sizeof en[n].label, "%sDouble speed (200%%)", check(sp == 200)); en[n++].cmd = CMD_SPEED_200;
    snprintf(en[n].label, sizeof en[n].label, "%sAs fast as possible", check(sp == 0)); en[n++].cmd = CMD_SPEED_MAX;
    en[n++] = (Entry){ "", 0, false, true };
    snprintf(en[n].label, sizeof en[n].label, "%sSound%s", check(e->sound), A.audio ? "" : " (no sound device)"); en[n++].cmd = CMD_SOUND;
    snprintf(en[n].label, sizeof en[n].label, "Volume up (%d%%)", e->cfg.volume); en[n++].cmd = CMD_VOLUME_UP;
    en[n++] = (Entry){ "Volume down", CMD_VOLUME_DOWN };
    en[n++] = (Entry){ "", 0, false, true };
    snprintf(en[n].label, sizeof en[n].label, "%sSkip the game's idle loops", check(e->cfg.idle)); en[n++].cmd = CMD_IDLE;
    return n;
}

static int menu_view(Entry *en) {
    int s = A.emu->cfg.scale, n = 0;
    snprintf(en[n].label, sizeof en[n].label, "%s1x (640 x 400)", check(s == 1)); en[n++].cmd = CMD_SCALE_1;
    snprintf(en[n].label, sizeof en[n].label, "%s2x (1280 x 800)", check(s == 2)); en[n++].cmd = CMD_SCALE_2;
    snprintf(en[n].label, sizeof en[n].label, "%s3x (1920 x 1200)", check(s == 3)); en[n++].cmd = CMD_SCALE_3;
    snprintf(en[n].label, sizeof en[n].label, "%sFit to the window", check(s == 0)); en[n++].cmd = CMD_SCALE_FIT;
    return n;
}

static int menu_help(Entry *en) {
    int n = 0;
    en[n++] = (Entry){ "Menu for the keyboard", CMD_OVERLAY, false, false, "F12" };
    en[n++] = (Entry){ "About PC-98", CMD_ABOUT };
    return n;
}

static void draw_menubar(KUI *ui, KRect r) {
    static const char *const titles[] = { "File", "Machine", "View", "Help" };
    Entry en[4][20];
    KMenuItem items[4][20];
    const KMenuItem *menus[4];
    int counts[4];
    counts[0] = menu_file(en[0]);
    counts[1] = menu_machine(en[1]);
    counts[2] = menu_view(en[2]);
    counts[3] = menu_help(en[3]);
    for (int m = 0; m < 4; m++) {
        for (int i = 0; i < counts[m]; i++)
            items[m][i] = (KMenuItem){ .label = en[m][i].label, .separator = en[m][i].separator, .disabled = en[m][i].disabled, .shortcut = en[m][i].shortcut };
        menus[m] = items[m];
    }
    int r2 = k_menubar(ui, r, titles, menus, counts, 4);
    if (r2) {
        int m = (r2 >> 8) - 1, it = (r2 & 255) - 1;
        if (m >= 0 && m < 4 && it >= 0 && it < counts[m]) ui_command(en[m][it].cmd);
    }
}

/* ---------------------------------------------------------------- the F12 menu (keyboard) */

typedef struct { char label[72]; int cmd; char key; bool disabled; } OverlayItem;

static int overlay_items(OverlayItem *it) {
    Emu *e = A.emu;
    int n = 0;
    snprintf(it[n].label, sizeof it[n].label, "%s", e->paused ? "Resume" : "Pause"); it[n].cmd = CMD_PAUSE; it[n++].key = 'P';
    snprintf(it[n].label, sizeof it[n].label, "Reset"); it[n].cmd = CMD_RESET; it[n++].key = 'R';
    snprintf(it[n].label, sizeof it[n].label, "Reset from the hard disk"); it[n].cmd = CMD_RESET_HD; it[n].key = 'H';
    it[n++].disabled = !e->disk[SLOT_HD1].loaded && !e->disk[SLOT_HD2].loaded;
    snprintf(it[n].label, sizeof it[n].label, "Insert floppy 1..."); it[n].cmd = CMD_INSERT_FD1; it[n++].key = '1';
    snprintf(it[n].label, sizeof it[n].label, "Insert floppy 2..."); it[n].cmd = CMD_INSERT_FD2; it[n++].key = '2';
    snprintf(it[n].label, sizeof it[n].label, "Open hard disk image..."); it[n].cmd = CMD_INSERT_HD; it[n++].key = '3';
    snprintf(it[n].label, sizeof it[n].label, "Save changes as..."); it[n].cmd = CMD_SAVE; it[n].key = 'S'; it[n++].disabled = !A.changed_blocks;
    int sp = e->cfg.speed;
    snprintf(it[n].label, sizeof it[n].label, "Speed: %s (next)", sp == 0 ? "as fast as possible" : sp == 50 ? "half" : sp == 200 ? "double" : "real time");
    it[n].cmd = sp == 100 ? CMD_SPEED_200 : sp == 200 ? CMD_SPEED_MAX : sp == 0 ? CMD_SPEED_50 : CMD_SPEED_100; it[n++].key = 'F';
    snprintf(it[n].label, sizeof it[n].label, "Sound: %s", e->sound ? "on" : A.audio ? "off" : "no device"); it[n].cmd = CMD_SOUND; it[n++].key = 'M';
    snprintf(it[n].label, sizeof it[n].label, "Volume: %d%%   (+ and -)", e->cfg.volume); it[n].cmd = CMD_VOLUME_UP; it[n++].key = '+';
    int sc = e->cfg.scale;
    snprintf(it[n].label, sizeof it[n].label, "Size: %s (next)", sc == 0 ? "fit" : sc == 1 ? "1x" : sc == 2 ? "2x" : "3x");
    it[n].cmd = sc == 1 ? CMD_SCALE_2 : sc == 2 ? CMD_SCALE_3 : sc == 3 ? CMD_SCALE_FIT : CMD_SCALE_1; it[n++].key = 'V';
    snprintf(it[n].label, sizeof it[n].label, "About"); it[n].cmd = CMD_ABOUT; it[n++].key = 'A';
    snprintf(it[n].label, sizeof it[n].label, "Quit"); it[n].cmd = CMD_QUIT; it[n++].key = 'Q';
    return n;
}

static void overlay_close(void) {
    if (A.overlay) say("pc98app: menu closed");
    A.overlay = 0;
}

static void overlay_activate(OverlayItem *it) {
    if (it->disabled) return;
    int cmd = it->cmd;
    if (cmd != CMD_VOLUME_UP) overlay_close();               /* the volume row stays open: it is adjusted a step at a time */
    ui_command(cmd);
}

static void overlay_key(const FeKey *k) {
    OverlayItem it[20];
    int n = overlay_items(it);
    if (k->code == KEY_ESC) { overlay_close(); return; }
    if (k->code == KEY_UP) { A.overlay_sel = (A.overlay_sel + n - 1) % n; return; }
    if (k->code == KEY_DOWN) { A.overlay_sel = (A.overlay_sel + 1) % n; return; }
    if (k->code == KEY_HOME) { A.overlay_sel = 0; return; }
    if (k->code == KEY_END) { A.overlay_sel = n - 1; return; }
    if (k->code == KEY_LEFT || k->code == KEY_MINUS || k->code == KEY_KPMINUS) { ui_command(CMD_VOLUME_DOWN); return; }
    if (k->code == KEY_RIGHT || k->code == KEY_EQUAL || k->code == KEY_KPPLUS) { ui_command(CMD_VOLUME_UP); return; }
    if (k->code == KEY_ENTER || k->code == KEY_KPENTER) { overlay_activate(&it[A.overlay_sel]); return; }
    if (k->ch && k->ch < 128) {
        char c = (char)k->ch;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        for (int i = 0; i < n; i++) if (it[i].key == c && !it[i].disabled) { A.overlay_sel = i; overlay_activate(&it[i]); return; }
    }
}

static void draw_overlay(KUI *ui, KRect area) {
    KCanvas *cv = &ui->cv;
    OverlayItem it[20];
    int n = overlay_items(it);
    kc_fill_alpha(cv, area, 0x000000, 150);
    int w = 360, h = n * 22 + 44;
    KRect box = kr(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
    if (box.y < area.y) box.y = area.y;
    kc_panel(cv, box, KB_RAISED, ui->theme->window_bg);
    kc_text_in(cv, kr(box.x + 10, box.y + 4, w - 20, 20), "PC-98  (F12 or Esc: back to the game)", ui->theme->accent_dark, KA_LEFT);
    for (int i = 0; i < n; i++) {
        KRect row = kr(box.x + 4, box.y + 28 + i * 22, w - 8, 22);
        bool hot = k_hover(ui, row) && !it[i].disabled;
        if (hot && ui->released[0]) { A.overlay_sel = i; overlay_activate(&it[i]); return; }
        if (hot && ui->buttons == 0) A.overlay_sel = i;
        if (i == A.overlay_sel) kc_fill(cv, row, ui->theme->select_bg);
        uint32_t fg = it[i].disabled ? ui->theme->text_dim : i == A.overlay_sel ? ui->theme->select_text : ui->theme->text;
        char key[8];
        snprintf(key, sizeof key, "%c", it[i].key);
        kc_text(cv, row.x + 8, row.y + 3, key, i == A.overlay_sel ? fg : ui->theme->accent);
        kc_text(cv, row.x + 32, row.y + 3, it[i].label, fg);
    }
}

/* ---------------------------------------------------------------- events: keys for the guest, the focus */

bool ui_event(KApp *app, const FairyEvent *ev) {
    KWin *mainwin = kapp_main(app);
    FairyWindow *mw = mainwin ? kwin_fairy(mainwin) : NULL;
    if (!mw || ev->window != mw) return false;
    if (ev->type == FEV_FOCUS) {
        A.focused = ev->u.focus.gained != 0;
        if (!A.focused) emu_release_keys(A.emu);            /* a key let go in another window must not stay down in the guest */
        return false;
    }
    if (ev->type != FEV_KEY) return false;
    const FeKey *k = &ev->u.key;
    if (k->code == KEY_F12) {                               /* the machine has no F12: the key belongs to the emulator */
        if (k->down == 1) ui_command(CMD_OVERLAY);
        return true;
    }
    if (A.overlay) {
        if (k->down) overlay_key(k);
        return true;
    }
    KUI *ui = kapp_ui(app);
    if (ui->menu_open || A.quit_asked) return true;         /* a drop-down of the menu bar has the keys */
    if (A.trace && k->down != 2) say("pc98app: key %02X %s", k->code, k->down ? "down" : "up");
    emu_host_key(A.emu, k->code, k->down);                  /* (the table, the held-key bookkeeping and the dropped auto-repeat are keymap.c's) */
    return true;
}

/* ---------------------------------------------------------------- the frame */

void ui_draw(KApp *app, KUI *ui, void *user) {
    (void)app;
    (void)user;
    Emu *e = A.emu;
    KCanvas *cv = &ui->cv;
    uint64_t t = now_ns();
    int hold = ui->menu_open || A.overlay || A.quit_asked || (A.dialogs > 0 && !A.focused);
    static int was_held;
    emu_hold(e, hold, t);
    if (hold && !was_held) sound_flush();
    was_held = hold;
    unsigned what = emu_step(e, t);
    if (what & EMU_PICTURE) {
        if (!A.picture_serial) say("pc98app: first picture (machine frame %llu)", (unsigned long long)e->m->frames);
        A.picture_serial++;
    }
    sound_pump();
    if (e->stopped && !A.stop_shown && e->booted) {
        A.stop_shown = 1;
        dlg_message("The program stopped", KI_WARNING, "%s", e->stop_msg);
    }
    if (e->booted && emu_disks_written(e) && t - A.last_changed_check_ns > 250000000ull) {      /* the guest wrote a sector: count what changed, a few times a second at most */
        uint32_t before = A.changed_blocks;
        A.changed_blocks = emu_changed_total(e);
        A.last_changed_check_ns = t;
        if (A.changed_blocks != before) say("pc98app: disk changed: %u blocks", A.changed_blocks);
        ui_update_title();
    }
    int status_changed = 0;
    if (t - A.last_status_ns > 500000000ull) {
        char before[sizeof A.status];
        memcpy(before, A.status, sizeof before);
        build_status();
        A.last_status_ns = t;
        status_changed = memcmp(before, A.status, sizeof before) != 0;
        ui_update_title();
    }
    bool size_changed = cv->w != A.last_w || cv->h != A.last_h;
    A.last_w = cv->w;
    A.last_h = cv->h;
    bool menu_activity = ui->menu_open || A.menu_was_open || (ui->my < MENU_H && ui->mouse_inside) || ui->pressed[0] || ui->released[0] || ui->nkeys > 0;
    A.menu_was_open = ui->menu_open;

    KRect area = kr(0, MENU_H, cv->w, cv->h - MENU_H - STATUS_H);
    k_clear(ui);
    if (e->booted) draw_picture(ui, area);
    else {
        kc_fill(cv, area, 0x000000);
        kc_text_in(cv, kr(area.x, area.y + area.h / 2 - 40, area.w, 20), "No disk image.", 0xF4EFD8, KA_CENTER);
        kc_text_in(cv, kr(area.x, area.y + area.h / 2 - 16, area.w, 20), "Use File > Insert floppy 1... or Open hard disk image...", 0xB9C2F0, KA_CENTER);
        kc_text_in(cv, kr(area.x, area.y + area.h / 2 + 8, area.w, 20), "or start with:  pc98 IMAGE", 0xB9C2F0, KA_CENTER);
    }
    if (A.overlay) draw_overlay(ui, area);
    if (!A.status[0]) build_status();
    draw_status(ui, kr(0, cv->h - STATUS_H, cv->w, STATUS_H));
    draw_menubar(ui, kr(0, 0, cv->w, MENU_H - 1));
    if (!(menu_activity || status_changed || size_changed || A.overlay || e->stopped || e->paused)) k_damage(ui, area);     /* only the picture changed */

    /* Run again when the machine is next owed time (about every 8 ms of machine time); slowly while nothing runs. */
    int wait = 100;
    if (e->booted && !e->stopped && !e->paused && !hold) {
        uint32_t w = pacer_wait_ms(&e->pacer, now_ns(), 8000000ull);
        wait = w < 2 ? 2 : w > 16 ? 16 : (int)w;
        if (e->cfg.speed == 0) wait = 1;
    }
    kapp_set_tick(A.kapp, wait);
}
