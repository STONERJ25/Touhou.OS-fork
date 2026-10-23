/* The window: playlist, the Prismriver Concert Hall stage, now playing, seek bar, transport buttons and volume. */
#include "jukebox.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_H   22
#define STATUS_H 22
#define PAD      8

enum { M_FILE = 1, M_PLAY, M_HELP };

typedef struct {
    KRect menu, status, list, open_btn, stage, now, seek, times, transport, volume;
} Layout;

typedef struct { bool dragging; double frac; } Bar;
static Bar g_seek, g_vol;
static int g_tick_set;

void ui_init(void) { k_list_init(&J.list); if (J.ntr) J.list.selected = 0; }

/* ------------------------------------------------------------------ layout */

static Layout layout(KUI *ui) {
    Layout L;
    int W = ui->cv.w, H = ui->cv.h;
    int top = MENU_H + PAD, bottom = H - STATUS_H - PAD;
    int lw = W >= 640 ? 236 : 190;
    L.menu = kr(0, 0, W, MENU_H);
    L.status = kr(0, H - STATUS_H, W, STATUS_H);
    L.list = kr(PAD, top + 20, lw, bottom - top - 20 - 30);
    L.open_btn = kr(PAD, bottom - 24, lw, 24);
    int rx = PAD + lw + PAD, rw = W - rx - PAD;
    int stage_h = (bottom - top) * 53 / 100;
    if (stage_h < 150) stage_h = 150;
    if (stage_h > 270) stage_h = 270;
    L.stage = kr(rx, top, rw, stage_h);
    int y = top + stage_h + 8;
    L.now = kr(rx, y, rw, 38);
    L.seek = kr(rx, y + 42, rw, 14);
    L.times = kr(rx, y + 58, rw, 16);
    L.transport = kr(rx, y + 78, 4 * 40, 30);
    L.volume = kr(rx + 4 * 40 + 14, y + 78, rw - 4 * 40 - 14, 30);
    return L;
}

/* ------------------------------------------------------------------ small drawing helpers */

static void tri_right(KCanvas *cv, int x, int y, int h, uint32_t c) {         /* a play triangle: (x, y) is its top left, h tall, h/2+1 wide */
    for (int i = 0; i <= h / 2; i++) kc_vline(cv, x + i, y + i, h - 2 * i, c);
}

enum { G_PREV, G_PLAY, G_PAUSE, G_STOP, G_NEXT };

static bool glyph_button(KUI *ui, KRect r, int glyph, bool enabled) {
    bool hit = k_button(ui, r, "") && enabled;
    uint32_t c = enabled ? ui->theme->text : kc_mix(ui->theme->window_bg, ui->theme->text, 110);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    KCanvas *cv = &ui->cv;
    switch (glyph) {
    case G_PLAY:  tri_right(cv, cx - 4, cy - 7, 14, c); break;
    case G_PAUSE: kc_fill(cv, kr(cx - 6, cy - 7, 4, 14), c); kc_fill(cv, kr(cx + 2, cy - 7, 4, 14), c); break;
    case G_STOP:  kc_fill(cv, kr(cx - 6, cy - 6, 12, 12), c); break;
    case G_PREV:  kc_fill(cv, kr(cx - 8, cy - 7, 3, 14), c); for (int i = 0; i <= 7; i++) kc_vline(cv, cx - 4 + i, cy - i, 2 * i + 1, c); break;
    case G_NEXT:  kc_fill(cv, kr(cx + 5, cy - 7, 3, 14), c); for (int i = 0; i <= 7; i++) kc_vline(cv, cx + 4 - i, cy - i, 2 * i + 1, c); break;
    }
    return hit;
}

/* A horizontal bar you can click and drag. `live` returns true on every change while dragging; otherwise once, on release. */
static bool bar(KUI *ui, KRect r, Bar *st, double value, bool enabled, bool live, uint32_t fill) {
    KCanvas *cv = &ui->cv;
    bool hit = false;
    if (enabled) {
        if (k_hover(ui, r) && ui->pressed[0]) st->dragging = true;
        if (st->dragging) {
            double f = r.w > 1 ? (double)(ui->mx - r.x) / (double)(r.w - 1) : 0;
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            bool changed = f != st->frac;
            st->frac = f;
            if (ui->released[0] || !(ui->buttons & 1)) { st->dragging = false; hit = true; }
            else if (live && changed) hit = true;
        }
    }
    double shown = st->dragging ? st->frac : value;
    if (shown < 0) shown = 0;
    if (shown > 1) shown = 1;
    kc_panel(cv, kr(r.x, r.y + r.h / 2 - 4, r.w, 8), KB_SUNKEN, ui->theme->field_bg);
    int fw = (int)(shown * (r.w - 4));
    if (fw > 0) kc_fill(cv, kr(r.x + 2, r.y + r.h / 2 - 2, fw, 4), enabled ? fill : kc_mix(fill, ui->theme->window_bg, 160));
    int kx = r.x + 2 + fw - 4;
    kc_panel(cv, kr(kx, r.y + 1, 9, r.h - 2), KB_RAISED, ui->theme->button_bg);
    return hit;
}

/* ------------------------------------------------------------------ the playlist */

#define ROW_H 20

/* The list of tracks: an icon, the title, the length on the right. Returns the index that was double-clicked or entered, else -1. */
static int playlist_widget(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    int activated = -1;
    kc_panel(cv, r, KB_SUNKEN, ui->theme->field_bg);
    KRect in = kr_inset(r, 2);
    int vis = in.h / ROW_H;
    bool need_bar = J.ntr > vis;
    KRect body = need_bar ? kr(in.x, in.y, in.w - 14, in.h) : in;
    if (k_hover(ui, r) && ui->wheel) J.list.scroll -= ui->wheel * 3;
    if (J.list.scroll > J.ntr - vis) J.list.scroll = J.ntr - vis;
    if (J.list.scroll < 0) J.list.scroll = 0;
    int hover = -1;
    if (k_hover(ui, body)) {
        int row = (ui->my - body.y) / ROW_H + J.list.scroll;
        if (row >= 0 && row < J.ntr) hover = row;
    }
    if (ui->pressed[0] && hover >= 0) {
        J.list.selected = hover;
        if (ui->double_click) activated = hover;
    }
    int sel = J.list.selected;
    if (!(ui->mods & (MOD_CTRL | MOD_ALT)) && J.ntr) {                    /* Up / Down / Home / End / Enter work wherever the pointer is */
        if (k_key_pressed(ui, KEY_DOWN) && sel < J.ntr - 1) sel++;
        if (k_key_pressed(ui, KEY_UP) && sel > 0) sel--;
        if (k_key_pressed(ui, KEY_HOME)) sel = 0;
        if (k_key_pressed(ui, KEY_END)) sel = J.ntr - 1;
        if (k_key_pressed(ui, KEY_ENTER) && sel >= 0) activated = sel;
        if (sel < 0) sel = 0;
        J.list.selected = sel;
        if (sel < J.list.scroll) J.list.scroll = sel;
        if (sel >= J.list.scroll + vis) J.list.scroll = sel - vis + 1;
    }
    KRect old = cv->clip;
    kc_set_clip(cv, body);
    for (int i = 0; i <= vis; i++) {
        int row = J.list.scroll + i;
        if (row >= J.ntr) break;
        KRect rr = kr(body.x, body.y + i * ROW_H, body.w, ROW_H);
        const Track *t = &J.tr[row];
        bool selected = row == J.list.selected;
        uint32_t fg = ui->theme->field_text;
        if (selected) { kc_fill(cv, rr, ui->theme->select_bg); fg = ui->theme->select_text; }
        else if (row == hover) kc_fill(cv, rr, kc_mix(ui->theme->field_bg, ui->theme->accent_light, 40));
        KIcon icon = t->err[0] ? KI_WARNING : row == J.cur ? (J.playing ? KI_STAR : KI_PETAL) : t->kind == TK_MUSIC ? KI_MOON : KI_FILE;
        k_icon(cv, icon, rr.x + 3, rr.y + 2, 16);
        char clock[16] = "";
        if (t->length > 0) fmt_clock(t->length, clock, sizeof clock);
        int cw = kc_text_width(clock);
        if (row == J.cur) kc_text_bold(cv, rr.x + 24, rr.y + 2, t->title, fg);
        else kc_text_in(cv, kr(rr.x + 24, rr.y, rr.w - 24 - cw - 8, rr.h), t->title, fg, KA_LEFT);
        kc_text(cv, rr.x + rr.w - cw - 4, rr.y + 2, clock, selected ? fg : ui->theme->text_dim);
    }
    cv->clip = old;
    if (need_bar) k_scrollbar(ui, kr(in.x + in.w - 14, in.y, 14, in.h), &J.list.scroll, J.ntr, vis);
    return activated;
}

/* ------------------------------------------------------------------ the stage */

static void performer(KCanvas *cv, int cx, int floor_y, int who, float level, uint64_t now_ms) {
    static const uint32_t dress[3] = { 0x8C97CF, 0xD2495B, 0x4F93DC };
    static const uint32_t dress_d[3] = { 0x5E689D, 0x8E2B3A, 0x2F66A8 };
    static const uint32_t hat[3] = { 0x2E3260, 0x6A1B2A, 0x203F70 };
    static const char *const name[3] = { "Lunasa", "Merlin", "Lyrica" };
    int bob = (int)(level * 9.0f);
    int sway = (int)(sinf((float)now_ms * 0.004f + (float)who * 2.1f) * (1.0f + level * 3.0f));
    int y = floor_y - bob;
    /* glow */
    if (level > 0.05f) {
        for (int i = 0; i < 4; i++) {
            int rad = 30 + i * 6;
            kc_fill_alpha(cv, kr(cx - rad, y - 52 - i * 2, rad * 2, 70 + i * 4), dress[who], (int)(level * 14.0f));
        }
    }
    kc_ellipse_fill(cv, cx, floor_y + 4, 17, 4, 0x1A0F10);                              /* shadow */
    /* dress */
    for (int i = 0; i < 22; i++) {
        int w = 7 + i * 7 / 22 + (i > 14 ? (i - 14) / 2 : 0);
        kc_hline(cv, cx - w + sway / 2, y - 26 + i, 2 * w + 1, i > 17 ? dress_d[who] : dress[who]);
    }
    kc_fill(cv, kr(cx - 5 + sway / 2, y - 3, 4, 5 + bob / 3), 0x2A1E22);                 /* legs */
    kc_fill(cv, kr(cx + 2 + sway / 2, y - 3, 4, 5 + bob / 3), 0x2A1E22);
    kc_fill(cv, kr(cx - 2 + sway / 2, y - 26, 5, 5), 0xF3D9C0);                         /* neck */
    /* head */
    int hx = cx + sway, hy = y - 34;
    kc_circle_fill(cv, hx, hy, 7, 0xF3D9C0);
    kc_circle_fill(cv, hx, hy - 2, 7, who == 1 ? 0xE8D27A : who == 0 ? 0x2A2630 : 0xE9C46A);   /* hair */
    kc_fill(cv, kr(hx - 6, hy - 1, 13, 8), 0xF3D9C0);
    kc_fill(cv, kr(hx - 3, hy + 1, 2, 2), 0x2A1E22);                                    /* eyes */
    kc_fill(cv, kr(hx + 2, hy + 1, 2, 2), 0x2A1E22);
    if (level > 0.25f) kc_fill(cv, kr(hx - 1, hy + 4, 3, 2), 0xB04A4A);                 /* singing along */
    /* hat */
    kc_fill(cv, kr(hx - 9, hy - 7, 19, 3), hat[who]);
    kc_fill(cv, kr(hx - 5, hy - 14, 11, 8), hat[who]);
    kc_fill(cv, kr(hx - 5, hy - 9, 11, 2), who == 1 ? 0xE8C84A : 0xF2E7D0);
    /* instrument */
    if (who == 0) {                                                                     /* violin and bow */
        kc_ellipse_fill(cv, cx - 11 + sway / 2, y - 22, 5, 7, 0x9A5A2C);
        kc_line(cv, cx - 17 + sway / 2, y - 31 - bob / 2, cx - 4 + sway / 2, y - 12, 0xD9C9A0);
    } else if (who == 1) {                                                              /* trumpet */
        kc_line(cv, cx + 4, y - 24, cx + 22, y - 28, 0xE8C84A);
        kc_line(cv, cx + 4, y - 23, cx + 22, y - 27, 0xE8C84A);
        for (int i = 0; i < 6; i++) kc_vline(cv, cx + 22 + i, y - 31 - i / 2, 7 + i, 0xF4DC7A);
    } else {                                                                            /* keyboard */
        kc_fill(cv, kr(cx - 22 + sway / 2, y - 14, 44, 7), 0x242028);
        for (int k = 0; k < 11; k++) kc_fill(cv, kr(cx - 21 + sway / 2 + k * 4, y - 14, 3, 5), (k + (int)(now_ms / 90)) % 5 == who && level > 0.2f ? 0xFFE9A8 : 0xEFE9DA);
    }
    int nw = kc_text_width(name[who]);
    kc_text(cv, cx - nw / 2, floor_y + 10, name[who], 0xD8C6A0);
}

static void draw_stage(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    uint64_t now = ui->now_ms;
    KRect old = cv->clip;
    kc_set_clip(cv, r);
    int pit_h = r.h > 190 ? 52 : 42, floor_h = 34;
    int wall_y = r.y + 22, floor_y = r.y + r.h - pit_h - floor_h;
    kc_gradient_v(cv, kr(r.x, r.y, r.w, r.h), 0x2B1830, 0x120C1C);                       /* back wall */
    for (int x = r.x + 46; x < r.x + r.w - 46; x += 22)                                  /* wallpaper stripes */
        kc_vline(cv, x, wall_y, floor_y - wall_y, 0x33203A);
    /* spotlights */
    for (int i = 0; i < 3; i++) {
        int cx = r.x + r.w * (2 * i + 1) / 6;
        for (int y = wall_y; y < floor_y + floor_h - 8; y += 2) {
            int half = 8 + (y - wall_y) * 24 / (floor_y - wall_y + 1);
            kc_fill_alpha(cv, kr(cx - half, y, 2 * half, 2), 0xFFF0C8, 10 + (int)(J.level[i] * 26.0f));
        }
    }
    /* floor */
    kc_gradient_v(cv, kr(r.x, floor_y, r.w, floor_h), 0x7A5232, 0x4A2F1C);
    for (int x = r.x; x < r.x + r.w; x += 28) kc_vline(cv, x, floor_y, floor_h, 0x3A2416);
    kc_hline(cv, r.x, floor_y, r.w, 0xB07A48);
    /* performers */
    for (int i = 0; i < 3; i++) performer(cv, r.x + r.w * (2 * i + 1) / 6, floor_y + 20, i, J.level[i], now);
    /* the pit: a level display */
    KRect pit = kr(r.x, r.y + r.h - pit_h, r.w, pit_h);
    kc_fill(cv, pit, 0x0C0A12);
    kc_hline(cv, pit.x, pit.y, pit.w, 0x5A4630);
    int gap = 2, margin = 56;
    int bw = (r.w - 2 * margin - (SPEC_BANDS - 1) * gap) / SPEC_BANDS;
    if (bw < 2) bw = 2;
    int total = SPEC_BANDS * bw + (SPEC_BANDS - 1) * gap;
    int bx = r.x + (r.w - total) / 2, maxh = pit_h - 8;
    for (int b = 0; b < SPEC_BANDS; b++) {
        int h = (int)(J.spec[b] * (float)maxh);
        int ph = (int)(J.spec_peak[b] * (float)maxh);
        int x = bx + b * (bw + gap);
        for (int s = 0; s < h; s++) {
            float f = (float)s / (float)maxh;
            uint32_t col = f < 0.55f ? 0x58C878 : f < 0.8f ? 0xE0C040 : 0xE05A48;
            kc_hline(cv, x, pit.y + pit_h - 4 - s, bw, col);
        }
        if (ph > 1) kc_hline(cv, x, pit.y + pit_h - 5 - ph, bw, 0xF4EFD8);
    }
    /* curtains and the valance */
    for (int side = 0; side < 2; side++) {
        int cw = r.w / 9 > 46 ? 46 : r.w / 9;
        int x0 = side ? r.x + r.w - cw : r.x;
        for (int x = 0; x < cw; x++) {
            int fold = (x % 11) < 3 ? 0x00 : (x % 11) < 7 ? 0x14 : 0x08;
            uint32_t col = kc_shade(0xA01830, 100 - fold * 3 + (side ? x : cw - x) * 10 / cw);
            kc_vline(cv, x0 + x, r.y + 12, floor_y + floor_h - r.y - 12, col);
        }
    }
    kc_gradient_v(cv, kr(r.x, r.y, r.w, 22), 0xC41E3A, 0x7E0F22);
    for (int x = r.x + 4; x < r.x + r.w - 4; x += 12) kc_fill(cv, kr(x, r.y + 20, 6, 4), KC_GOLD);
    kc_hline(cv, r.x, r.y + 22, r.w, 0x3A0A14);
    const char *title = "Prismriver Concert Hall";
    kc_text_bold(cv, r.x + (r.w - kc_text_width(title)) / 2 + 1, r.y + 4, title, 0x4A0A16);
    kc_text_bold(cv, r.x + (r.w - kc_text_width(title)) / 2, r.y + 3, title, 0xFFE9A8);
    kc_outline(cv, r, 0x2A1A10);
    cv->clip = old;
}

/* ------------------------------------------------------------------ the window */

static void about(KApp *app) {
    static const char *const ok[] = { "Close" };
    k_messagebox(kapp_display(app), "About Reimu's Jukebox",
                 "Reimu's Jukebox\n\nThe Prismriver sisters (Lunasa, Merlin and Lyrica) play in the Concert Hall.\n"
                 "The songs that come with the system are original music for TouhouOS;\n"
                 "put .wav, .tms or .mod files in your Music folder to add your own.",
                 KI_INFO, ok, 1);
}

static void file_chosen(const char *path, void *user) {
    (void)user;
    if (!path) return;
    int i = playlist_add(path, false);
    if (i < 0) { snprintf(J.status, sizeof J.status, "%s is not a .wav, .tms or .mod file.", k_basename(path)); return; }
    J.list.selected = i;
    trace("added %s", J.tr[i].title);
    player_load(i, 0, true);
}

void ui_open_file(void) {
    const char *home = getenv("HOME");
    k_file_dialog(J.app, 0, "Open music", home ? home : "/", NULL, file_chosen, NULL);
}

static void keys(KApp *app, KUI *ui) {
    (void)app;
    bool ctrl = (ui->mods & MOD_CTRL) != 0;
    if (k_key_pressed(ui, KEY_SPACE)) player_toggle();
    if (!ctrl) {
        if (k_key_pressed(ui, KEY_S)) player_stop();
        if (k_key_pressed(ui, KEY_N)) player_next(+1);
        if (k_key_pressed(ui, KEY_P)) player_next(-1);
        if (k_key_pressed(ui, KEY_M)) player_set_mute(!J.muted);
        if (k_key_pressed(ui, KEY_R)) { J.repeat = !J.repeat; trace("repeat %s", J.repeat ? "on" : "off"); }
        if (k_key_pressed(ui, KEY_RIGHT)) player_seek(player_position() + 5);
        if (k_key_pressed(ui, KEY_LEFT)) player_seek(player_position() - 5);
        if (k_key_pressed(ui, KEY_F1)) about(app);
    } else {
        if (k_key_pressed(ui, KEY_UP)) player_set_volume(J.volume + 5);
        if (k_key_pressed(ui, KEY_DOWN)) player_set_volume(J.volume - 5);
        if (k_key_pressed(ui, KEY_O)) ui_open_file();
        if (k_key_pressed(ui, KEY_Q) || k_key_pressed(ui, KEY_W)) kapp_quit(app, 0);
    }
}

static void menus(KApp *app, KUI *ui, KRect r) {
    static char l_repeat[32], l_mute[32], l_play[32];
    snprintf(l_repeat, sizeof l_repeat, "%sRepeat the playlist", J.repeat ? "\xE2\x9C\x93 " : "  ");
    snprintf(l_mute, sizeof l_mute, "%s", J.muted ? "Unmute" : "Mute");
    snprintf(l_play, sizeof l_play, "%s", J.playing ? "Pause" : "Play");
    const KMenuItem file[] = { { "Open music...", 0, false, false, "Ctrl+O" }, { NULL, 0, true, false, NULL }, { "Quit", 0, false, false, "Ctrl+Q" } };
    const KMenuItem play[] = {
        { l_play, 0, false, false, "Space" }, { "Stop", 0, false, false, "S" }, { "Next", 0, false, false, "N" }, { "Previous", 0, false, false, "P" },
        { NULL, 0, true, false, NULL }, { l_repeat, 0, false, false, "R" }, { l_mute, 0, false, false, "M" },
    };
    const KMenuItem help[] = { { "About Reimu's Jukebox", 0, false, false, "F1" } };
    static const char *const titles[] = { "File", "Playback", "Help" };
    const KMenuItem *const items[] = { file, play, help };
    const int counts[] = { 3, 7, 1 };
    int sel = k_menubar(ui, r, titles, items, counts, 3);
    if (!sel) return;
    int m = sel >> 8, it = (sel & 255) - 1;
    if (m == M_FILE) { if (it == 0) ui_open_file(); else if (it == 2) kapp_quit(app, 0); }
    else if (m == M_PLAY) {
        if (it == 0) player_toggle();
        else if (it == 1) player_stop();
        else if (it == 2) player_next(+1);
        else if (it == 3) player_next(-1);
        else if (it == 5) { J.repeat = !J.repeat; trace("repeat %s", J.repeat ? "on" : "off"); }
        else if (it == 6) player_set_mute(!J.muted);
    } else if (m == M_HELP) about(app);
}

void ui_draw(KApp *app, KUI *ui, void *user) {
    (void)user;
    KCanvas *cv = &ui->cv;
    player_tick(ui->now_ms);
    Layout L = layout(ui);
    k_clear(ui);

    /* keep the window title and the tick rate in step with what is playing */
    static int last_cur = -2;
    if (J.cur != last_cur) {
        last_cur = J.cur;
        char title[160];
        if (J.cur >= 0) snprintf(title, sizeof title, "Jukebox - %s", J.tr[J.cur].title);
        else snprintf(title, sizeof title, "Jukebox");
        kapp_set_title(app, title);
    }
    int want_tick = J.playing ? 45 : 200;
    if (want_tick != g_tick_set) { g_tick_set = want_tick; kapp_set_tick(app, want_tick); }

    keys(app, ui);

    /* the playlist */
    k_heading(ui, L.list.x, L.list.y - 18, "Playlist");
    int picked = playlist_widget(ui, L.list);
    if (picked >= 0) player_load(picked, 0, true);
    if (k_button_icon(ui, L.open_btn, KI_FOLDER_OPEN, "Open music...")) ui_open_file();

    /* the stage */
    draw_stage(ui, L.stage);

    /* now playing */
    const Track *cur = J.cur >= 0 ? &J.tr[J.cur] : NULL;
    const Track *shown = cur ? cur : (J.list.selected >= 0 && J.list.selected < J.ntr ? &J.tr[J.list.selected] : NULL);
    KRect t1 = kr(L.now.x, L.now.y, L.now.w, 18), t2 = kr(L.now.x, L.now.y + 18, L.now.w, 18);
    if (shown) {
        char head[120];
        snprintf(head, sizeof head, "%s%s", cur ? (J.playing ? "Now playing: " : "Loaded: ") : "Selected: ", shown->title);
        kc_text_in(cv, t1, head, ui->theme->accent_dark, KA_LEFT);
        kc_text_in(cv, t2, shown->sub, ui->theme->text_dim, KA_LEFT);
    } else {
        kc_text_in(cv, t1, "Nothing to play yet.", ui->theme->text_dim, KA_LEFT);
        kc_text_in(cv, t2, "Put .wav, .tms or .mod files in your Music folder.", ui->theme->text_dim, KA_LEFT);
    }

    /* seek bar and times */
    double len = player_length(), pos = player_position();
    bool seekable = cur != NULL && len > 0 && !J.no_device;
    if (bar(ui, L.seek, &g_seek, len > 0 ? pos / len : 0, seekable, false, KC_SHRINE) && seekable) player_seek(g_seek.frac * len);
    char a[16], b[16], times[40];
    fmt_clock(g_seek.dragging && len > 0 ? g_seek.frac * len : pos, a, sizeof a);
    fmt_clock(len, b, sizeof b);
    snprintf(times, sizeof times, "%s / %s", a, b);
    kc_text_in(cv, L.times, times, ui->theme->text, KA_LEFT);
    if (J.cur >= 0 && J.tr[J.cur].kind == TK_MUSIC && J.mus) {
        char info[80];
        if (J.minfo.bpm) snprintf(info, sizeof info, "%d bpm, %d voices", J.minfo.bpm, J.minfo.channels);
        else snprintf(info, sizeof info, "%d channels", J.minfo.channels);
        kc_text_in(cv, L.times, info, ui->theme->text_dim, KA_RIGHT);
    }

    /* transport */
    KRect tb = kr(L.transport.x, L.transport.y, 36, 30);
    bool have = J.ntr > 0 && !J.no_device;
    if (glyph_button(ui, tb, G_PREV, have)) player_next(-1);
    tb.x += 40;
    if (glyph_button(ui, tb, J.playing ? G_PAUSE : G_PLAY, have)) player_toggle();
    tb.x += 40;
    if (glyph_button(ui, tb, G_STOP, have && J.cur >= 0)) player_stop();
    tb.x += 40;
    if (glyph_button(ui, tb, G_NEXT, have)) player_next(+1);

    /* volume */
    KRect vr = L.volume;
    kc_text_in(cv, kr(vr.x, vr.y, 30, vr.h), "Vol", ui->theme->text, KA_LEFT);
    KRect vb = kr(vr.x + 32, vr.y + 6, vr.w - 32 - 100 > 60 ? vr.w - 32 - 100 : 60, 18);
    if (bar(ui, vb, &g_vol, J.volume / 100.0, !J.no_device, true, J.muted ? 0x909090 : KC_GOLD) && !J.no_device) player_set_volume((int)(g_vol.frac * 100.0 + 0.5));
    char vol[16];
    snprintf(vol, sizeof vol, "%d%%", J.volume);
    kc_text_in(cv, kr(vb.x + vb.w + 6, vr.y, 40, vr.h), vol, ui->theme->text, KA_LEFT);
    if (k_button(ui, kr(vb.x + vb.w + 44, vr.y + 3, 54, 24), J.muted ? "Unmute" : "Mute") && !J.no_device) player_set_mute(!J.muted);

    /* the status line */
    kc_fill(cv, L.status, kc_shade(ui->theme->window_bg, 94));
    kc_hline(cv, 0, L.status.y, L.status.w, kc_shade(ui->theme->window_bg, 78));
    kc_text_in(cv, kr(8, L.status.y, L.status.w - 120, L.status.h), J.status, ui->theme->text, KA_LEFT);
    char count[40];
    snprintf(count, sizeof count, "%d track%s%s", J.ntr, J.ntr == 1 ? "" : "s", J.repeat ? ", repeat" : "");
    kc_text_in(cv, kr(L.status.w - 150, L.status.y, 142, L.status.h), count, ui->theme->text_dim, KA_RIGHT);

    menus(app, ui, L.menu);
    if (J.playing) ui->redraw_soon = true;
}
