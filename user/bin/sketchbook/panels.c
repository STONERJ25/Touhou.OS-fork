/* The panels around the canvas: toolbox with tool options, palette with the colour mixer, and the status bar. */
#include "sketchbook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 16 colours in the PC-98 manner: four-bit levels, a dark row and a bright row */
static const uint32_t g_pal[16] = {
    0x000000, 0x000077, 0x770000, 0x770077, 0x007700, 0x007777, 0x777700, 0x777777,
    0xBBBBBB, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF,
};

static const Tool g_toolbox_order[T_COUNT] = {
    T_SELECT, T_TEXT, T_PENCIL, T_BRUSH, T_ERASER, T_SPRAY, T_FILL, T_PICKER, T_LINE, T_RECT, T_ELLIPSE,
};

void panels_init(void) {
    S.fg = 0x000000;
    S.bg = 0xFFFFFF;
    S.mix_target = 0;
}

void color_set(int which, uint32_t c) {
    c &= 0xFFFFFF;
    if (which == 0) S.fg = c; else S.bg = c;
    S.mix_target = which;
}

void color_swap(void) {
    uint32_t t = S.fg;
    S.fg = S.bg;
    S.bg = t;
}

static bool slider(KUI *ui, KRect r, int *v, int lo, int hi, uint32_t c0, uint32_t c1, const char *name);

static const char *hex_tip(uint32_t c) {
    static char buf[16];
    snprintf(buf, sizeof buf, "#%06X", (unsigned)(c & 0xFFFFFF));
    return buf;
}

/* ---------------------------------------------------------------- toolbox */

static void brush_preview(KUI *ui, KRect box) {
    KCanvas *cv = &ui->cv;
    kc_panel(cv, box, KB_SUNKEN, 0xFFFFFF);
    KRect old = cv->clip;
    kc_set_clip(cv, kr_inset(box, 2));
    ScreenSink ss;
    ssink_init(&ss, cv, box.x + box.w / 2, box.y + box.h / 2, 1);
    ss.base.color = S.tool == T_ERASER ? S.bg : S.fg;
    if (S.tool == T_SPRAY) {
        int r = S.size[T_SPRAY];
        if (r > 15) r = 15;
        rs_ellipse(&ss.base, -r, -r, r, r, 1, 0x888888, 0, S_OUTLINE);
        ss.base.color = S.fg;
        rs_spray(&ss.base, 0, 0, r, r * 2);
    } else if (S.tool == T_LINE) {
        ssink_init(&ss, cv, box.x + 8, box.y + box.h / 2, 1);
        ss.base.color = S.fg;
        rs_line(&ss.base, 0, 6, box.w - 18, -6, S.size[T_LINE], false);
    } else {
        rs_stamp(&ss.base, 0, 0, S.size[S.tool], S.tool == T_ERASER);
    }
    cv->clip = old;
}

static void tool_options(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    int x = r.x + 4, w = r.w - 8, y = r.y + 4 + 6 * 34 + 8;
    kc_hline(cv, r.x + 4, y - 5, r.w - 8, kc_shade(ui->theme->window_bg, 78));
    Tool t = S.tool;
    bool sized = t == T_BRUSH || t == T_ERASER || t == T_SPRAY || t == T_LINE || t == T_RECT || t == T_ELLIPSE;
    char buf[32];
    if (sized) {
        snprintf(buf, sizeof buf, t == T_SPRAY ? "Radius %d" : t == T_RECT || t == T_ELLIPSE || t == T_LINE ? "Width %d" : "Size %d", S.size[t]);
        k_label(ui, x, y, buf);
        slider(ui, kr(x + 3, y + 20, w - 6, 14), &S.size[t], 1, 32, 0, 0, "size");
        y += 42;
    }
    if (t == T_BRUSH || t == T_ERASER || t == T_SPRAY || t == T_LINE) {
        brush_preview(ui, kr(x, y, w, 40));
    } else if (t == T_RECT || t == T_ELLIPSE) {
        static const char *const names[3] = { "Outline", "Solid", "Both" };
        static const char *const tips[3] = { "Outline only", "Solid, in the foreground colour", "Outline in the foreground colour, fill in the background colour" };
        for (int i = 0; i < 3; i++) {
            KRect b = kr(x, y + i * 20, w, 19);
            if (k_button_flat(ui, b, names[i], S.shape_style == i)) S.shape_style = i;
            if (k_hover(ui, b)) k_tooltip_text(ui, tips[i]);
        }
    } else if (t == T_TEXT) {
        k_label(ui, x, y, "Scale");
        for (int i = 0; i < 4; i++) {
            KRect b = kr(x + (i % 2) * 34, y + 20 + (i / 2) * 22, 32, 20);
            snprintf(buf, sizeof buf, "%dx", i + 1);
            if (k_button_flat(ui, b, buf, S.text_scale == i + 1)) S.text_scale = i + 1;
        }
        k_checkbox(ui, x, y + 70, "Opaque", &S.text_opaque);
        if (k_hover(ui, kr(x, y + 68, w, 20))) k_tooltip_text(ui, "Paint the background colour behind the letters");
    } else if (t == T_PENCIL) {
        k_label_col(ui, x, y, "1 pixel", ui->theme->text_dim);
    } else if (t == T_FILL) {
        k_label_col(ui, x, y, "Click an", ui->theme->text_dim);
        k_label_col(ui, x, y + 16, "area to", ui->theme->text_dim);
        k_label_col(ui, x, y + 32, "fill it", ui->theme->text_dim);
    } else if (t == T_PICKER) {
        k_label_col(ui, x, y, "Click to", ui->theme->text_dim);
        k_label_col(ui, x, y + 16, "pick up", ui->theme->text_dim);
        k_label_col(ui, x, y + 32, "a colour", ui->theme->text_dim);
    } else if (t == T_SELECT) {
        k_label_col(ui, x, y, "Drag a", ui->theme->text_dim);
        k_label_col(ui, x, y + 16, "box, then", ui->theme->text_dim);
        k_label_col(ui, x, y + 32, "move it", ui->theme->text_dim);
    }
}

void panels_toolbox(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_bevel(cv, r, KB_ETCHED);
    for (int i = 0; i < T_COUNT; i++) {
        Tool t = g_toolbox_order[i];
        KRect b = kr(r.x + 4 + (i % 2) * 36, r.y + 4 + (i / 2) * 34, 32, 32);
        bool hover = k_hover(ui, b), sel = S.tool == t;
        if (sel) kc_panel(cv, b, KB_SUNKEN, kc_shade(ui->theme->window_bg, 92));
        else kc_panel(cv, b, KB_RAISED, hover ? kc_shade(ui->theme->button_bg, 106) : ui->theme->button_bg);
        icon_tool(cv, t, b.x + 6 + (sel ? 1 : 0), b.y + 6 + (sel ? 1 : 0));
        if (hover) {
            static char tip[48];
            snprintf(tip, sizeof tip, "%s (%c)", tool_name(t), tool_key(t));
            k_tooltip_text(ui, tip);
        }
        if (k_clicked(ui, b)) tool_set(t);
    }
    tool_options(ui, r);
}

/* ---------------------------------------------------------------- sliders */

/* A horizontal slider from lo to hi. Unlike a click that is only noticed while the button is still down, a press sets
 * the value at once, so even a very quick click is not lost. Draws a plain track, or a colour gradient when c0 != c1. */
static bool slider(KUI *ui, KRect r, int *v, int lo, int hi, uint32_t c0, uint32_t c1, const char *name) {
    KCanvas *cv = &ui->cv;
    uint32_t id = k_id(name, r);
    KRect in = kr_inset(r, 2);
    bool gradient = c0 != c1, changed = false;
    if (gradient) {
        kc_panel(cv, r, KB_SUNKEN, 0);
        kc_gradient_h(cv, in, c0, c1);
    } else kc_panel(cv, kr(r.x, r.y + r.h / 2 - 2, r.w, 4), KB_SUNKEN, ui->theme->field_bg);
    int span = in.w > 1 ? in.w - 1 : 1;
    if (k_hover(ui, kr_grow(r, 2)) && ui->pressed[0]) ui->active = id;
    if (ui->active == id) {
        if (ui->pressed[0] || (ui->buttons & 1)) {
            int nv = lo + sk_clamp(ui->mx - in.x, 0, span) * (hi - lo) / span;
            if (nv != *v) { *v = nv; changed = true; }
        }
        if (!(ui->buttons & 1)) ui->active = 0;
    }
    int tx = in.x + (*v - lo) * span / (hi > lo ? hi - lo : 1);
    kc_panel(cv, gradient ? kr(tx - 3, r.y - 2, 7, r.h + 4) : kr(tx - 3, r.y, 7, r.h), KB_RAISED, ui->theme->button_bg);
    return changed;
}

/* ---------------------------------------------------------------- palette and mixer */

static bool color_slider(KUI *ui, KRect r, int *v, uint32_t c0, uint32_t c1, const char *name) {
    return slider(ui, r, v, 0, 255, c0, c1, name);
}

static bool parse_hex(const char *s, uint32_t *out) {
    if (*s == '#') s++;
    if (strlen(s) != 6) return false;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | (uint32_t)d;
    }
    *out = v;
    return true;
}

void panels_palette(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_fill(cv, r, ui->theme->window_bg);
    k_separator(ui, kr(r.x, r.y, r.w, 2));

    /* foreground / background */
    KRect bgr = kr(r.x + 24, r.y + 26, 34, 28), fgr = kr(r.x + 8, r.y + 10, 34, 28);
    kc_panel(cv, bgr, KB_SUNKEN, S.bg);
    kc_panel(cv, fgr, KB_SUNKEN, S.fg);
    kc_outline(cv, kr_grow(S.mix_target == 0 ? fgr : bgr, 2), ui->theme->accent);
    if (k_clicked(ui, fgr)) S.mix_target = 0;
    else if (k_clicked(ui, bgr)) S.mix_target = 1;
    if (k_hover(ui, fgr)) k_tooltip_text(ui, "Foreground colour (left button)");
    else if (k_hover(ui, bgr)) k_tooltip_text(ui, "Background colour (right button)");
    KRect sw = kr(r.x + 8, r.y + 42, 16, 16);
    if (k_icon_button(ui, sw, KI_COUNT, false)) color_swap();
    icon_swap(cv, sw.x + 1, sw.y + 1);
    if (k_hover(ui, sw)) k_tooltip_text(ui, "Swap the colours (X)");

    /* the sixteen colours */
    int px0 = r.x + 70, py0 = r.y + 10;
    for (int i = 0; i < 16; i++) {
        KRect c = kr(px0 + (i % 8) * 21, py0 + (i / 8) * 21, 20, 20);
        kc_panel(cv, c, KB_SUNKEN, g_pal[i]);
        if (k_hover(ui, c)) {
            kc_outline(cv, kr_inset(c, 2), ((g_pal[i] >> 16) & 255) + ((g_pal[i] >> 8) & 255) + (g_pal[i] & 255) > 380 ? 0x000000 : 0xFFFFFF);
            k_tooltip_text(ui, hex_tip(g_pal[i]));
            if (ui->pressed[0]) color_set(0, g_pal[i]);
            if (ui->pressed[1]) color_set(1, g_pal[i]);
        }
    }

    /* the mixer edits whichever of the two colours is marked */
    int mx = px0 + 8 * 21 + 16;
    uint32_t cur = S.mix_target ? S.bg : S.fg;
    int rgb[3] = { (int)(cur >> 16) & 255, (int)(cur >> 8) & 255, (int)cur & 255 };
    static const char *const names[3] = { "R", "G", "B" };
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        int y = r.y + 8 + i * 19;
        k_label(ui, mx, y - 1, names[i]);
        int a[3] = { rgb[0], rgb[1], rgb[2] }, b[3] = { rgb[0], rgb[1], rgb[2] };
        a[i] = 0;
        b[i] = 255;
        if (color_slider(ui, kr(mx + 16, y, 124, 16), &rgb[i], KRGB(a[0], a[1], a[2]), KRGB(b[0], b[1], b[2]), names[i])) changed = true;
        char num[8];
        snprintf(num, sizeof num, "%3d", rgb[i]);
        k_label(ui, mx + 16 + 124 + 8, y - 1, num);
    }
    if (changed) color_set(S.mix_target, KRGB(rgb[0], rgb[1], rgb[2]));

    static char hex[8];
    static uint32_t hex_for = 0xFFFFFFFFu;
    cur = S.mix_target ? S.bg : S.fg;
    if (cur != hex_for) { snprintf(hex, sizeof hex, "%06X", (unsigned)cur); hex_for = cur; }
    int hx = mx + 16 + 124 + 8 + 32;
    k_label(ui, hx, r.y + 9, "#");
    bool entered = k_textfield(ui, kr(hx + 10, r.y + 6, 70, 22), hex, sizeof hex);
    uint32_t v;
    if (parse_hex(hex, &v) && v != cur) { color_set(S.mix_target, v); hex_for = v; }
    if (entered) { ui->focus = 0; hex_for = 0xFFFFFFFFu; }          /* Enter: back to the canvas, text shows the clean value */
    k_label_col(ui, hx, r.y + 34, "Editing the", ui->theme->text_dim);
    k_label_col(ui, hx, r.y + 50, S.mix_target ? "background" : "foreground", ui->theme->accent_dark);
}

/* ---------------------------------------------------------------- status bar */

void panels_status(KUI *ui, KRect r) {
    KCanvas *cv = &ui->cv;
    kc_fill(cv, r, ui->theme->window_bg);
    kc_hline(cv, r.x, r.y, r.w, kc_shade(ui->theme->window_bg, 72));
    kc_hline(cv, r.x, r.y + 1, r.w, 0xFFFFFF);
    int y = r.y + 3;
    int x = r.x + r.w - 6;

    /* zoom: [-] 100% [+] */
    x -= 80;
    KRect minus = kr(x, y - 1, 18, 18), plus = kr(x + 62, y - 1, 18, 18);
    if (k_button(ui, minus, "-")) canvas_set_zoom(S.zoom - 1, false);
    if (k_button(ui, plus, "+")) canvas_set_zoom(S.zoom + 1, false);
    char buf[48];
    snprintf(buf, sizeof buf, "%d%%", S.zoom * 100);
    kc_text_in(cv, kr(x + 20, y, 40, 16), buf, ui->theme->text, KA_CENTER);
    if (k_hover(ui, minus)) k_tooltip_text(ui, "Zoom out (-)");
    if (k_hover(ui, plus)) k_tooltip_text(ui, "Zoom in (+); the keys 1 to 8 pick a zoom directly");

    x -= 10;
    kc_vline(cv, x, r.y + 4, r.h - 7, kc_shade(ui->theme->window_bg, 78));
    x -= 6 + 88;
    snprintf(buf, sizeof buf, "%d x %d", g_doc.w, g_doc.h);
    kc_text_in(cv, kr(x, y, 88, 16), buf, ui->theme->text, KA_RIGHT);

    x -= 10;
    kc_vline(cv, x, r.y + 4, r.h - 7, kc_shade(ui->theme->window_bg, 78));
    x -= 6 + 76;
    if (S.cur_inside) {
        uint32_t c = g_doc.px[(size_t)S.cur_y * g_doc.w + S.cur_x] & 0xFFFFFF;
        kc_panel(cv, kr(x, y + 1, 14, 14), KB_SUNKEN, c);
        snprintf(buf, sizeof buf, "#%06X", (unsigned)c);
        kc_text(cv, x + 20, y, buf, ui->theme->text);
    }

    x -= 10;
    kc_vline(cv, x, r.y + 4, r.h - 7, kc_shade(ui->theme->window_bg, 78));
    x -= 6 + 80;
    if (S.cur_inside) {
        snprintf(buf, sizeof buf, "%d, %d", S.cur_x, S.cur_y);
        kc_text_in(cv, kr(x, y, 80, 16), buf, ui->theme->text, KA_LEFT);
    }

    x -= 10;
    kc_vline(cv, x, r.y + 4, r.h - 7, kc_shade(ui->theme->window_bg, 78));
    if (x - (r.x + 6) > 16) kc_text_in(cv, kr(r.x + 6, y, x - r.x - 12, 16), S.hint, ui->theme->text, KA_LEFT);
}
