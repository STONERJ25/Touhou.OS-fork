/* The panel's volume control: a speaker in the tray (click for the slider, wheel to change, middle click to mute). */
#include "shell.h"
#include <audio.h>
#include <stdio.h>
#include <unistd.h>

#define POP_W 214
#define POP_H 76

static KWin *g_popup;
static int g_vol = 60;
static bool g_muted;
static uint64_t g_read_ms;
static int g_present = -1;
static KRect g_icon;
static bool g_drag;

bool volume_present(void) {
    if (g_present < 0) g_present = access("/dev/audio", F_OK) == 0;
    return g_present;
}

/* The volume lives in the kernel and anyone (the volume command, the Jukebox) may change it, so look again now and then. */
static void refresh(bool force) {
    uint64_t now = k_now_ms();
    if (!force && now - g_read_ms < 500) return;
    g_read_ms = now;
    Audio *a = audio_open(AUDIO_CONTROL);
    if (!a) return;
    int l, r, m;
    if (audio_get_master(a, &l, &r, &m) == 0) { g_vol = (l + r) / 2; g_muted = m != 0; }
    audio_close(a);
}

static void apply(int vol, bool mute) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    Audio *a = audio_open(AUDIO_CONTROL);
    if (a) { audio_set_master(a, vol, vol, mute); audio_close(a); }
    g_vol = vol;
    g_muted = mute;
}

/* A small speaker: a box, a cone, and one to three waves for the volume (a cross when muted). */
static void speaker(KCanvas *cv, int x, int y, int level, bool muted, uint32_t c) {
    kc_fill(cv, kr(x, y + 6, 4, 6), c);
    for (int i = 0; i < 6; i++) kc_vline(cv, x + 4 + i, y + 6 - i, 6 + 2 * i, c);
    if (muted) {
        kc_line(cv, x + 13, y + 5, x + 19, y + 13, 0xFF6A5A);
        kc_line(cv, x + 13, y + 13, x + 19, y + 5, 0xFF6A5A);
        kc_line(cv, x + 14, y + 5, x + 20, y + 13, 0xFF6A5A);
        kc_line(cv, x + 14, y + 13, x + 20, y + 5, 0xFF6A5A);
        return;
    }
    for (int k = 0; k < 3; k++)
        if (level > k * 33) kc_vline(cv, x + 13 + k * 3, y + 9 - (2 + k * 2), 4 + k * 4, c);
}

static void popup_draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    kc_panel(cv, kr(0, 0, cv->w, cv->h), KB_RAISED, ui->theme->window_bg);
    refresh(false);
    kc_text_bold(cv, 10, 8, "Volume", ui->theme->accent_dark);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", g_vol);
    kc_text(cv, cv->w - 10 - kc_text_width(pct), 8, pct, ui->theme->text);
    KRect b = kr(12, 30, cv->w - 24, 16);
    if (k_hover(ui, b) && ui->pressed[0]) g_drag = true;
    if (g_drag) {
        int v = (ui->mx - b.x) * 100 / (b.w - 1);
        if (v != g_vol || g_muted) apply(v, false);
        if (ui->released[0] || !(ui->buttons & 1)) g_drag = false;
    }
    kc_panel(cv, kr(b.x, b.y + 4, b.w, 8), KB_SUNKEN, ui->theme->field_bg);
    int fw = g_vol * (b.w - 4) / 100;
    if (fw > 0) kc_fill(cv, kr(b.x + 2, b.y + 6, fw, 4), g_muted ? 0x909090 : KC_GOLD);
    kc_panel(cv, kr(b.x + 2 + fw - 4, b.y, 9, b.h), KB_RAISED, ui->theme->button_bg);
    if (k_button(ui, kr(12, 50, 80, 22), g_muted ? "Unmute" : "Mute")) apply(g_vol, !g_muted);
    kc_text(cv, 100, 53, g_muted ? "muted" : "", ui->theme->text_dim);
    if (k_key_pressed(ui, KEY_ESC)) { kwin_close(g_popup); g_popup = NULL; }
}

static void popup_closed(KWin *w, void *user) { (void)w; (void)user; g_popup = NULL; g_drag = false; }

void volume_close(void) {
    if (g_popup) { kwin_close(g_popup); g_popup = NULL; }
}

static void popup_toggle(void) {
    if (g_popup) { volume_close(); return; }
    refresh(true);
    int x = g_icon.x + g_icon.w / 2 - POP_W / 2;
    if (x + POP_W > G.screen_w) x = G.screen_w - POP_W;
    if (x < 0) x = 0;
    g_popup = kwin_new(G.app, x, G.screen_h - PANEL_H - POP_H - 2, POP_W, POP_H, "Volume", FW_POPUP | FW_NO_DECOR, popup_draw, NULL);
    if (g_popup) kwin_set_close_handler(g_popup, popup_closed, NULL);
}

void volume_tray(KUI *ui, KRect r) {
    refresh(false);
    g_icon = r;
    bool hover = k_hover(ui, r);
    kc_panel(&ui->cv, r, g_popup ? KB_SUNKEN : (hover ? KB_RAISED : KB_FLAT), g_popup ? 0x1A2244 : hover ? 0x2A356E : 0x141B42);
    speaker(&ui->cv, r.x + 3, r.y + (r.h - 18) / 2, g_vol, g_muted, 0xF4EFD8);
    if (hover) {
        char tip[48];
        snprintf(tip, sizeof tip, g_muted ? "Volume %d%% (muted)" : "Volume %d%%", g_vol);
        k_tooltip_text(ui, tip);
        if (ui->wheel) apply(g_vol + ui->wheel * 5, false);
        if (ui->released[2]) apply(g_vol, !g_muted);
    }
    if (k_clicked(ui, r)) popup_toggle();
}
