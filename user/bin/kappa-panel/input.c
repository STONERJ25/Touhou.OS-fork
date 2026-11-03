/*
 * The Keyboard and Mouse pane. The window server only tells a program about the keys and the pointer while it has the
 * focus, so the pane works as a test page: a picture of the keyboard that lights up the keys you press, and a test area
 * that shows the mouse buttons, the wheel and the pointer position.
 */
#include "panel.h"
#include <stdio.h>
#include <string.h>

typedef struct KeyDef { uint8_t code; const char *label; short x, y, w, h; } KeyDef;   /* quarter key units */

#define K1(code, label, x, y) { code, label, x, y, 4, 4 }
static const KeyDef g_main[] = {
    K1(KEY_ESC, "Esc", 0, 0),
    K1(KEY_F1, "F1", 8, 0), K1(KEY_F2, "F2", 12, 0), K1(KEY_F3, "F3", 16, 0), K1(KEY_F4, "F4", 20, 0),
    K1(KEY_F5, "F5", 26, 0), K1(KEY_F6, "F6", 30, 0), K1(KEY_F7, "F7", 34, 0), K1(KEY_F8, "F8", 38, 0),
    K1(KEY_F9, "F9", 44, 0), K1(KEY_F10, "F10", 48, 0), K1(KEY_F11, "F11", 52, 0), K1(KEY_F12, "F12", 56, 0),
    K1(KEY_GRAVE, "`", 0, 6), K1(KEY_1, "1", 4, 6), K1(KEY_2, "2", 8, 6), K1(KEY_3, "3", 12, 6), K1(KEY_4, "4", 16, 6),
    K1(KEY_5, "5", 20, 6), K1(KEY_6, "6", 24, 6), K1(KEY_7, "7", 28, 6), K1(KEY_8, "8", 32, 6), K1(KEY_9, "9", 36, 6),
    K1(KEY_0, "0", 40, 6), K1(KEY_MINUS, "-", 44, 6), K1(KEY_EQUAL, "=", 48, 6), { KEY_BACKSPACE, "Bksp", 52, 6, 8, 4 },
    { KEY_TAB, "Tab", 0, 10, 6, 4 }, K1(KEY_Q, "Q", 6, 10), K1(KEY_W, "W", 10, 10), K1(KEY_E, "E", 14, 10), K1(KEY_R, "R", 18, 10),
    K1(KEY_T, "T", 22, 10), K1(KEY_Y, "Y", 26, 10), K1(KEY_U, "U", 30, 10), K1(KEY_I, "I", 34, 10), K1(KEY_O, "O", 38, 10),
    K1(KEY_P, "P", 42, 10), K1(KEY_LBRACKET, "[", 46, 10), K1(KEY_RBRACKET, "]", 50, 10), { KEY_BACKSLASH, "\\", 54, 10, 6, 4 },
    { KEY_CAPSLOCK, "Caps", 0, 14, 7, 4 }, K1(KEY_A, "A", 7, 14), K1(KEY_S, "S", 11, 14), K1(KEY_D, "D", 15, 14), K1(KEY_F, "F", 19, 14),
    K1(KEY_G, "G", 23, 14), K1(KEY_H, "H", 27, 14), K1(KEY_J, "J", 31, 14), K1(KEY_K, "K", 35, 14), K1(KEY_L, "L", 39, 14),
    K1(KEY_SEMICOLON, ";", 43, 14), K1(KEY_APOSTROPHE, "'", 47, 14), { KEY_ENTER, "Enter", 51, 14, 9, 4 },
    { KEY_LSHIFT, "Shift", 0, 18, 9, 4 }, K1(KEY_Z, "Z", 9, 18), K1(KEY_X, "X", 13, 18), K1(KEY_C, "C", 17, 18), K1(KEY_V, "V", 21, 18),
    K1(KEY_B, "B", 25, 18), K1(KEY_N, "N", 29, 18), K1(KEY_M, "M", 33, 18), K1(KEY_COMMA, ",", 37, 18), K1(KEY_DOT, ".", 41, 18),
    K1(KEY_SLASH, "/", 45, 18), { KEY_RSHIFT, "Shift", 49, 18, 11, 4 },
    { KEY_LCTRL, "Ctl", 0, 22, 5, 4 }, { KEY_LSUPER, "Sup", 5, 22, 5, 4 }, { KEY_LALT, "Alt", 10, 22, 5, 4 },
    { KEY_SPACE, "Space", 15, 22, 25, 4 }, { KEY_RALT, "Alt", 40, 22, 5, 4 }, { KEY_RSUPER, "Sup", 45, 22, 5, 4 },
    { KEY_MENU, "Mnu", 50, 22, 5, 4 }, { KEY_RCTRL, "Ctl", 55, 22, 5, 4 },
};
static const KeyDef g_nav[] = {
    K1(KEY_INSERT, "Ins", 64, 6), K1(KEY_HOME, "Hom", 68, 6), K1(KEY_PAGEUP, "PgU", 72, 6),
    K1(KEY_DELETE, "Del", 64, 10), K1(KEY_END, "End", 68, 10), K1(KEY_PAGEDOWN, "PgD", 72, 10),
    K1(KEY_UP, "\xE2\x86\x91", 68, 18), K1(KEY_LEFT, "\xE2\x86\x90", 64, 22), K1(KEY_DOWN, "\xE2\x86\x93", 68, 22), K1(KEY_RIGHT, "\xE2\x86\x92", 72, 22),
};
static const KeyDef g_pad[] = {
    K1(KEY_NUMLOCK, "Num", 80, 6), K1(KEY_KPSLASH, "/", 84, 6), K1(KEY_KPSTAR, "*", 88, 6), K1(KEY_KPMINUS, "-", 92, 6),
    K1(KEY_KP7, "7", 80, 10), K1(KEY_KP8, "8", 84, 10), K1(KEY_KP9, "9", 88, 10), { KEY_KPPLUS, "+", 92, 10, 4, 8 },
    K1(KEY_KP4, "4", 80, 14), K1(KEY_KP5, "5", 84, 14), K1(KEY_KP6, "6", 88, 14),
    K1(KEY_KP1, "1", 80, 18), K1(KEY_KP2, "2", 84, 18), K1(KEY_KP3, "3", 88, 18), { KEY_KPENTER, "Ent", 92, 18, 4, 8 },
    { KEY_KP0, "0", 80, 22, 8, 4 }, K1(KEY_KPDOT, ".", 88, 22),
};

static uint64_t g_lit_until[256];
static struct { bool any; uint8_t code, mods; uint32_t ch; } g_last;
static struct { int left, middle, right, up, down, doubles; uint64_t flash_up, flash_down; bool inside; int x, y; } g_mouse;

static const char *key_label(uint8_t code, char *buf, size_t cap) {
    const KeyDef *tabs[3] = { g_main, g_nav, g_pad };
    const size_t counts[3] = { sizeof g_main / sizeof g_main[0], sizeof g_nav / sizeof g_nav[0], sizeof g_pad / sizeof g_pad[0] };
    for (int t = 0; t < 3; t++)
        for (size_t i = 0; i < counts[t]; i++)
            if (tabs[t][i].code == code) {
                const char *l = tabs[t][i].label;
                if (!strcmp(l, "Sup")) return "Super";
                if (!strcmp(l, "Ctl")) return "Ctrl";
                if (!strcmp(l, "Bksp")) return "Backspace";
                if (!strcmp(l, "Mnu")) return "Menu";
                if (!strcmp(l, "Hom")) return "Home";
                if (!strcmp(l, "PgU")) return "Page Up";
                if (!strcmp(l, "PgD")) return "Page Down";
                if (!strcmp(l, "Ins")) return "Insert";
                if (!strcmp(l, "Num")) return "Num Lock";
                if (!strcmp(l, "Ent")) return "Keypad Enter";
                return l;
            }
    snprintf(buf, cap, "key %02X", code);
    return buf;
}

static void mods_text(uint8_t m, char *out, size_t cap) {
    out[0] = 0;
    if (m & MOD_CTRL) strncat(out, "Ctrl ", cap - strlen(out) - 1);
    if (m & MOD_ALT) strncat(out, "Alt ", cap - strlen(out) - 1);
    if (m & MOD_SHIFT) strncat(out, "Shift ", cap - strlen(out) - 1);
    if (m & MOD_SUPER) strncat(out, "Super ", cap - strlen(out) - 1);
    if (m & MOD_CAPS) strncat(out, "CapsLock ", cap - strlen(out) - 1);
    if (!out[0]) snprintf(out, cap, "none");
    for (size_t n = strlen(out); n && out[n - 1] == ' '; ) out[--n] = 0;
}

static void draw_keys(KUI *ui, const KeyDef *keys, size_t n, int ox, int oy, int u) {
    KCanvas *cv = &ui->cv;
    uint64_t now = ui->now_ms;
    for (size_t i = 0; i < n; i++) {
        const KeyDef *k = &keys[i];
        KRect r = kr(ox + k->x * u / 4, oy + k->y * u / 4, k->w * u / 4 - 2, k->h * u / 4 - 2);
        bool held = ui->key_down[k->code];
        bool lit = held || now < g_lit_until[k->code];
        uint32_t face = 0xF2EEE0, fg = KC_INK;
        if (lit) {
            face = held ? ui->theme->accent : kc_mix(0xF2EEE0, ui->theme->accent_light, 190);
            fg = held ? 0xFFFFFF : KC_INK;
        }
        if (k->code == KEY_CAPSLOCK && (ui->mods & MOD_CAPS) && !lit) face = 0xF6D58A;
        kc_panel(cv, r, held ? KB_SUNKEN : KB_RAISED, face);
        KRect t = r;
        if (held) { t.x++; t.y++; }
        kc_text_in(cv, kr_inset(t, 1), k->label, fg, KA_CENTER);
    }
}

static void draw_mouse_sprite(KUI *ui, int x, int y) {
    KCanvas *cv = &ui->cv;
    uint64_t now = ui->now_ms;
    int w = 54, h = 80;
    uint32_t body = 0xE4DECB, edge = 0x4A4638, lit = ui->theme->accent;
    kc_ellipse_fill(cv, x + w / 2, y + 30, w / 2, 30, edge);
    kc_fill(cv, kr(x, y + 30, w, h - 50), edge);
    kc_ellipse_fill(cv, x + w / 2, y + h - 22, w / 2, 22, edge);
    kc_ellipse_fill(cv, x + w / 2, y + 30, w / 2 - 2, 28, body);
    kc_fill(cv, kr(x + 2, y + 30, w - 4, h - 50), body);
    kc_ellipse_fill(cv, x + w / 2, y + h - 22, w / 2 - 2, 20, body);
    KRect lb = kr(x + 4, y + 6, w / 2 - 6, 34), rb = kr(x + w / 2 + 2, y + 6, w / 2 - 6, 34);
    if (ui->buttons & 1) kc_fill(cv, lb, lit);
    if (ui->buttons & 2) kc_fill(cv, rb, lit);
    kc_vline(cv, x + w / 2, y + 2, 42, edge);
    kc_hline(cv, x + 4, y + 42, w - 8, edge);
    KRect wheel = kr(x + w / 2 - 4, y + 12, 8, 20);
    bool mid = (ui->buttons & 4) != 0;
    kc_fill(cv, wheel, mid ? lit : 0x9A9482);
    kc_outline(cv, wheel, edge);
    if (now < g_mouse.flash_up) kc_text_bold(cv, x + w / 2 - 4, y - 14, "\xE2\x86\x91", lit);
    if (now < g_mouse.flash_down) kc_text_bold(cv, x + w / 2 - 4, y + h + 2, "\xE2\x86\x93", lit);
}

void pane_input(KUI *ui, KRect pane) {
    KCanvas *cv = &ui->cv;
    uint64_t now = ui->now_ms;
    draw_heading(ui, pane, KI_GAME, "Keyboard and Mouse", "Press keys and click: the pictures show what the window receives");
    KRect b = pane_body(pane);

    /* remember what was pressed (the page consumes no keys, so everything you type shows up) */
    for (int i = 0; i < ui->nkeys; i++) {
        const KKey *k = &ui->keys[i];
        if (!k->down) continue;
        g_lit_until[k->code] = now + 350;
        if (k->down == 1) trace("kappa-panel: key %02x", k->code);
        g_last.any = true;
        g_last.code = k->code;
        g_last.mods = k->mods;
        g_last.ch = k->ch;
    }
    bool fading = false;
    for (int c = 0; c < 256; c++) if (now < g_lit_until[c]) fading = true;

    /* the keyboard, as large as fits */
    int avail = b.w - 24;
    bool pad = avail / 24 >= 26, nav = avail / 19 >= 20;
    int units = pad ? 24 : nav ? 19 : 15;
    int u = avail / units;
    if (u > 32) u = 32;
    if (u < 14) u = 14;
    int kb_w = units * u;
    int ox = b.x + (b.w - kb_w) / 2, oy = b.y + 6;
    kc_panel(cv, kr(ox - 8, oy - 6, kb_w + 16, u * 25 / 4 + 12 + 0), KB_ETCHED, kc_shade(ui->theme->window_bg, 93));
    draw_keys(ui, g_main, sizeof g_main / sizeof g_main[0], ox, oy, u);
    if (nav) draw_keys(ui, g_nav, sizeof g_nav / sizeof g_nav[0], ox, oy, u);
    if (pad) draw_keys(ui, g_pad, sizeof g_pad / sizeof g_pad[0], ox, oy, u);
    int y = oy + u * 26 / 4 + 10;

    /* what the last key was */
    char line[200], mods[64], lbl[32];
    if (g_last.any) {
        mods_text(g_last.mods, mods, sizeof mods);
        char chs[16] = "";
        if (g_last.ch >= 32 && g_last.ch < 127) snprintf(chs, sizeof chs, "  character '%c'", (char)g_last.ch);
        snprintf(line, sizeof line, "Last key: %s  (code 0x%02X)%s  modifiers: %s", key_label(g_last.code, lbl, sizeof lbl), g_last.code, chs, mods);
    } else snprintf(line, sizeof line, "Press any key. Layout: US (the only one the window server knows).");
    kc_text_in(cv, kr(b.x + 8, y, b.w - 16, 16), line, ui->theme->text, KA_LEFT);
    y += 20;

    /* the mouse test area */
    KRect area = kr(b.x + 8, y + 4, b.w - 16, b.y + b.h - y - 12);
    if (area.h < 60) { if (fading) ui->redraw_soon = true; return; }
    kc_panel(cv, area, KB_SUNKEN, 0x15204A);
    kc_text_in(cv, kr(area.x + 8, area.y + 4, area.w - 16, 16), "Mouse test area: click, scroll and move inside this box", 0x8B97C8, KA_LEFT);
    bool inside = k_hover(ui, area);
    if (inside) {
        g_mouse.x = ui->mx - area.x;
        g_mouse.y = ui->my - area.y;
        for (int i = 0; i < 3; i++) {
            if (!ui->pressed[i]) continue;
            int *counter = i == 0 ? &g_mouse.left : i == 1 ? &g_mouse.right : &g_mouse.middle;
            (*counter)++;
            trace("kappa-panel: mouse button %s down", i == 0 ? "left" : i == 1 ? "right" : "middle");
        }
        if (ui->double_click) { g_mouse.doubles++; trace("kappa-panel: double click"); }
        if (ui->wheel > 0) { g_mouse.up += ui->wheel; g_mouse.flash_up = now + 400; trace("kappa-panel: wheel up %d", ui->wheel); }
        if (ui->wheel < 0) { g_mouse.down += -ui->wheel; g_mouse.flash_down = now + 400; trace("kappa-panel: wheel down %d", -ui->wheel); }
        ui->cursor_shape = FC_CROSS;
        KRect old = cv->clip;
        kc_set_clip(cv, kr_intersect(old, kr_inset(area, 2)));
        kc_hline(cv, area.x + 2, ui->my, area.w - 4, 0x2D3C80);
        kc_vline(cv, ui->mx, area.y + 2, area.h - 4, 0x2D3C80);
        cv->clip = old;
    }
    g_mouse.inside = inside;
    draw_mouse_sprite(ui, area.x + 28, area.y + 40);

    int tx = area.x + 130, ty = area.y + 30;
    char t[96];
    snprintf(t, sizeof t, "Left button    %d clicks", g_mouse.left);
    kc_text(cv, tx, ty, t, ui->buttons & 1 ? 0xFFB0A0 : 0xD7DBF4);
    snprintf(t, sizeof t, "Middle button  %d clicks", g_mouse.middle);
    kc_text(cv, tx, ty + 20, t, ui->buttons & 4 ? 0xFFB0A0 : 0xD7DBF4);
    snprintf(t, sizeof t, "Right button   %d clicks", g_mouse.right);
    kc_text(cv, tx, ty + 40, t, ui->buttons & 2 ? 0xFFB0A0 : 0xD7DBF4);
    snprintf(t, sizeof t, "Wheel          %d up, %d down", g_mouse.up, g_mouse.down);
    kc_text(cv, tx, ty + 60, t, 0xD7DBF4);
    snprintf(t, sizeof t, "Double clicks  %d", g_mouse.doubles);
    kc_text(cv, tx, ty + 80, t, 0xD7DBF4);
    if (inside) snprintf(t, sizeof t, "Pointer        %d, %d", g_mouse.x, g_mouse.y);
    else snprintf(t, sizeof t, "Pointer        outside the box");
    kc_text(cv, tx, ty + 100, t, 0x8B97C8);
    if (k_button(ui, kr(area.x + area.w - 100, area.y + area.h - 34, 88, 26), "Reset")) memset(&g_mouse, 0, sizeof g_mouse);

    if (fading || now < g_mouse.flash_up || now < g_mouse.flash_down) ui->redraw_soon = true;
}
