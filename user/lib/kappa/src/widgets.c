/* Immediate-mode widgets: buttons, labels, check boxes, text fields, lists, tabs, menus and friends. */
#include <kappa.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

const KTheme k_theme_shrine = {
    .window_bg = KC_PAPER, .text = KC_INK, .text_dim = 0x6B6757, .text_on_accent = 0xFFFFFF,
    .accent = KC_SHRINE, .accent_dark = KC_SHRINE_D, .accent_light = 0xE8607A,
    .button_bg = 0xE4DECB, .field_bg = 0xFBF9F2, .field_text = KC_INK, .select_bg = 0xB5122B, .select_text = 0xFFFFFF,
    .border_dark = 0x4A4638, .border_light = 0xFFFFFF,
    .title_active_a = 0xD61F3F, .title_active_b = 0x7C0A1E, .title_inactive_a = 0x6A7088, .title_inactive_b = 0x464B63,
    .title_text = 0xFFF6E0, .title_text_inactive = 0xC9CCD8,
};

#define TH (ui->theme)

uint32_t k_id(const char *label, KRect r) {
    uint32_t h = 2166136261u;
    for (const char *p = label; p && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h ^= (uint32_t)(r.x * 73856093) ^ (uint32_t)(r.y * 19349663) ^ (uint32_t)(r.w * 83492791);
    return h ? h : 1;
}

void k_clear(KUI *ui) { kc_fill(&ui->cv, kr(0, 0, ui->cv.w, ui->cv.h), TH->window_bg); }

bool k_hover(KUI *ui, KRect r) {
    return !ui->input_blocked && ui->mouse_inside && kr_contains(kr_intersect(r, ui->cv.clip), ui->mx, ui->my);
}

bool k_clicked(KUI *ui, KRect r) { return k_hover(ui, r) && ui->released[0]; }

bool k_key_pressed(KUI *ui, uint8_t code) {
    for (int i = 0; i < ui->nkeys; i++)
        if (ui->keys[i].code == code && ui->keys[i].down) { ui->keys[i].down = 0; ui->keys[i].code = 0; return true; }
    return false;
}

/* Common press/release logic. Returns true when the left button was released over the widget after being pressed on it. */
static bool behave(KUI *ui, KRect r, uint32_t id, bool *hover, bool *down) {
    bool h = k_hover(ui, r);
    bool clicked = false;
    if (h) ui->hot = id;
    if (h && ui->pressed[0]) ui->active = id;
    if (ui->active == id && ui->released[0]) { clicked = h; ui->active = 0; }
    if (hover) *hover = h;
    if (down) *down = ui->active == id && (ui->buttons & 1) && h;
    return clicked;
}

/* ---------------------------------------------------------------- buttons and labels */

static void draw_button_face(KUI *ui, KRect r, bool hover, bool down, bool focused) {
    uint32_t bg = down ? kc_shade(TH->button_bg, 92) : hover ? kc_shade(TH->button_bg, 104) : TH->button_bg;
    kc_panel(&ui->cv, r, down ? KB_SUNKEN : KB_RAISED, bg);
    if (focused) kc_outline(&ui->cv, kr_inset(r, 3), TH->accent);
}

bool k_button(KUI *ui, KRect r, const char *label) {
    bool hover, down;
    uint32_t id = k_id(label, r);
    bool clicked = behave(ui, r, id, &hover, &down);
    draw_button_face(ui, r, hover, down, false);
    KRect t = down ? kr(r.x + 1, r.y + 1, r.w, r.h) : r;
    kc_text_in(&ui->cv, kr_inset(t, 3), label, TH->text, KA_CENTER);
    return clicked;
}

bool k_button_icon(KUI *ui, KRect r, KIcon icon, const char *label) {
    bool hover, down;
    uint32_t id = k_id(label, r);
    bool clicked = behave(ui, r, id, &hover, &down);
    draw_button_face(ui, r, hover, down, false);
    int off = down ? 1 : 0;
    int isz = r.h >= 28 ? 20 : 16;
    int tw = kc_text_width(label);
    int total = isz + (tw ? 6 + tw : 0);
    int x = r.x + (r.w - total) / 2 + off;
    k_icon(&ui->cv, icon, x, r.y + (r.h - isz) / 2 + off, isz);
    if (tw) kc_text(&ui->cv, x + isz + 6, r.y + (r.h - 16) / 2 + off, label, TH->text);
    return clicked;
}

bool k_button_flat(KUI *ui, KRect r, const char *label, bool selected) {
    bool hover, down;
    bool clicked = behave(ui, r, k_id(label, r), &hover, &down);
    if (selected) kc_panel(&ui->cv, r, KB_SUNKEN, kc_shade(TH->window_bg, 94));
    else if (hover) kc_panel(&ui->cv, r, down ? KB_SUNKEN : KB_RAISED, TH->window_bg);
    kc_text_in(&ui->cv, r, label, TH->text, KA_CENTER);
    return clicked;
}

bool k_icon_button(KUI *ui, KRect r, KIcon icon, bool selected) {
    bool hover, down;
    bool clicked = behave(ui, r, k_id(k_icon_name(icon), r), &hover, &down);
    if (selected) kc_panel(&ui->cv, r, KB_SUNKEN, kc_shade(TH->window_bg, 94));
    else if (hover) kc_panel(&ui->cv, r, down ? KB_SUNKEN : KB_RAISED, TH->window_bg);
    int isz = r.h >= 28 ? 20 : 16;
    k_icon(&ui->cv, icon, r.x + (r.w - isz) / 2 + (down ? 1 : 0), r.y + (r.h - isz) / 2 + (down ? 1 : 0), isz);
    return clicked;
}

void k_label(KUI *ui, int x, int y, const char *text) { kc_text(&ui->cv, x, y, text, TH->text); }
void k_label_col(KUI *ui, int x, int y, const char *text, uint32_t color) { kc_text(&ui->cv, x, y, text, color); }
void k_label_in(KUI *ui, KRect r, const char *text, KAlign align) { kc_text_in(&ui->cv, r, text, TH->text, align); }
void k_heading(KUI *ui, int x, int y, const char *text) { kc_text_bold(&ui->cv, x, y, text, TH->accent_dark); }

bool k_checkbox(KUI *ui, int x, int y, const char *label, bool *value) {
    KRect box = kr(x, y + 1, 14, 14), hit = kr(x, y, 14 + 8 + kc_text_width(label), 16);
    bool hover, down;
    bool clicked = behave(ui, hit, k_id(label, hit), &hover, &down);
    if (clicked) *value = !*value;
    kc_panel(&ui->cv, box, KB_SUNKEN, down ? kc_shade(TH->field_bg, 92) : TH->field_bg);
    if (*value) k_icon(&ui->cv, KI_CHECK, box.x - 1, box.y - 1, 16);
    kc_text(&ui->cv, x + 22, y, label, TH->text);
    return clicked;
}

bool k_radio(KUI *ui, int x, int y, const char *label, int *group, int value) {
    KRect hit = kr(x, y, 14 + 8 + kc_text_width(label), 16);
    bool hover, down;
    bool clicked = behave(ui, hit, k_id(label, hit), &hover, &down);
    if (clicked) *group = value;
    kc_circle_fill(&ui->cv, x + 7, y + 8, 7, TH->border_dark);
    kc_circle_fill(&ui->cv, x + 7, y + 8, 6, down ? kc_shade(TH->field_bg, 92) : TH->field_bg);
    if (*group == value) kc_circle_fill(&ui->cv, x + 7, y + 8, 3, TH->accent);
    kc_text(&ui->cv, x + 22, y, label, TH->text);
    return clicked;
}

void k_separator(KUI *ui, KRect r) {
    if (r.w >= r.h) { kc_hline(&ui->cv, r.x, r.y, r.w, kc_shade(TH->window_bg, 72)); kc_hline(&ui->cv, r.x, r.y + 1, r.w, 0xFFFFFF); }
    else { kc_vline(&ui->cv, r.x, r.y, r.h, kc_shade(TH->window_bg, 72)); kc_vline(&ui->cv, r.x + 1, r.y, r.h, 0xFFFFFF); }
}

void k_group(KUI *ui, KRect r, const char *title) {
    KRect box = kr(r.x, r.y + 8, r.w, r.h - 8);
    kc_bevel(&ui->cv, box, KB_ETCHED);
    int tw = kc_text_width(title) + 8;
    kc_fill(&ui->cv, kr(r.x + 10, r.y, tw, 16), TH->window_bg);
    kc_text(&ui->cv, r.x + 14, r.y, title, TH->text);
}

void k_progress(KUI *ui, KRect r, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    kc_panel(&ui->cv, r, KB_SUNKEN, TH->field_bg);
    KRect in = kr_inset(r, 2);
    in.w = in.w * percent / 100;
    kc_gradient_v(&ui->cv, in, TH->accent_light, TH->accent_dark);
}

bool k_slider(KUI *ui, KRect r, int *value, int min, int max) {
    uint32_t id = k_id("slider", r);
    bool hover, down;
    behave(ui, r, id, &hover, &down);
    bool changed = false;
    int track = r.w - 14;
    if (hover && ui->pressed[0]) ui->active = id;
    if (ui->active == id && (ui->buttons & 1)) {
        int v = min + (ui->mx - r.x - 7) * (max - min) / (track > 0 ? track : 1);
        if (v < min) v = min;
        if (v > max) v = max;
        if (v != *value) { *value = v; changed = true; }
    } else if (ui->active == id) ui->active = 0;
    int cy = r.y + r.h / 2;
    kc_panel(&ui->cv, kr(r.x, cy - 2, r.w, 4), KB_SUNKEN, TH->field_bg);
    int tx = r.x + (max > min ? (*value - min) * track / (max - min) : 0);
    kc_panel(&ui->cv, kr(tx, r.y, 14, r.h), KB_RAISED, TH->button_bg);
    return changed;
}

int k_tabs(KUI *ui, KRect r, const char *const *labels, int n, int *selected) {
    int x = r.x;
    kc_hline(&ui->cv, r.x, r.y + r.h - 1, r.w, 0x4A4638);
    for (int i = 0; i < n; i++) {
        int w = kc_text_width(labels[i]) + 24;
        KRect tab = kr(x, r.y + (i == *selected ? 0 : 3), w, r.h - (i == *selected ? 0 : 3));
        bool hover, down;
        if (behave(ui, tab, k_id(labels[i], tab), &hover, &down)) *selected = i;
        kc_fill(&ui->cv, tab, i == *selected ? TH->window_bg : kc_shade(TH->window_bg, 94));
        kc_hline(&ui->cv, tab.x, tab.y, tab.w, 0xFFFFFF);
        kc_vline(&ui->cv, tab.x, tab.y, tab.h, 0xFFFFFF);
        kc_vline(&ui->cv, tab.x + tab.w - 1, tab.y, tab.h, 0x4A4638);
        if (i == *selected) kc_hline(&ui->cv, tab.x + 1, r.y + r.h - 1, tab.w - 2, TH->window_bg);
        kc_text_in(&ui->cv, tab, labels[i], i == *selected ? TH->text : TH->text_dim, KA_CENTER);
        x += w;
    }
    return *selected;
}

void k_scrollbar(KUI *ui, KRect r, int *offset, int content, int view) {
    kc_fill(&ui->cv, r, kc_shade(TH->window_bg, 90));
    if (content <= view || view <= 0) return;
    int maxoff = content - view;
    if (*offset < 0) *offset = 0;
    if (*offset > maxoff) *offset = maxoff;
    int th = r.h * view / content;
    if (th < 18) th = 18;
    int ty = r.y + (r.h - th) * (*offset) / maxoff;
    KRect thumb = kr(r.x, ty, r.w, th);
    uint32_t id = k_id("scroll", r);
    bool hover, down;
    behave(ui, thumb, id, &hover, &down);
    if (ui->active == id && (ui->buttons & 1)) {
        if (ui->pressed[0]) ui->drag_dy = ui->my - ty;
        int ny = ui->my - ui->drag_dy - r.y;
        int range = r.h - th;
        *offset = range > 0 ? ny * maxoff / range : 0;
        if (*offset < 0) *offset = 0;
        if (*offset > maxoff) *offset = maxoff;
    } else if (k_hover(ui, r) && ui->pressed[0]) {          /* click on the track: page up/down */
        *offset += ui->my < ty ? -view : view;
        if (*offset < 0) *offset = 0;
        if (*offset > maxoff) *offset = maxoff;
    }
    kc_panel(&ui->cv, thumb, KB_RAISED, TH->button_bg);
}

void k_tooltip_text(KUI *ui, const char *text) {
    ui->tooltip = text;
    ui->tooltip_x = ui->mx + 14;
    ui->tooltip_y = ui->my + 18;
}

/* ---------------------------------------------------------------- text fields */

static size_t u8_prev(const char *s, size_t i) { if (!i) return 0; i--; while (i && (s[i] & 0xC0) == 0x80) i--; return i; }
static size_t u8_next(const char *s, size_t i) { if (!s[i]) return i; i++; while (s[i] && (s[i] & 0xC0) == 0x80) i++; return i; }

static void field_delete_range(char *buf, size_t lo, size_t hi) { memmove(buf + lo, buf + hi, strlen(buf + hi) + 1); }

static bool textfield_impl(KUI *ui, KRect r, char *buf, size_t cap, bool pw) {
    uint32_t id = (uint32_t)(uintptr_t)buf * 2654435761u | 1;
    bool hover = k_hover(ui, r), enter = false;
    kc_panel(&ui->cv, r, KB_SUNKEN, TH->field_bg);
    KRect inner = kr_inset(r, 3);
    int cols = inner.w / 8;
    size_t len = strlen(buf);
    if (hover) ui->cursor_shape = FC_TEXT;

    /* The first visible column while this field is focused follows the caret; clicks are measured from it. */
    size_t first_prev = 0;
    if (ui->focus == id) {
        char tmp[2048];
        size_t cn = (size_t)ui->caret < sizeof tmp - 1 ? (size_t)ui->caret : sizeof tmp - 1;
        memcpy(tmp, buf, cn); tmp[cn] = 0;
        size_t cp = kc_utf8_len(tmp);
        first_prev = cp >= (size_t)cols ? cp - (size_t)cols + 1 : 0;
    }
    if (ui->pressed[0]) {
        if (hover) {
            ui->focus = id;
            ui->active = id;
            int click_col = (ui->mx - inner.x) / 8;
            if (click_col < 0) click_col = 0;
            size_t target = kc_utf8_offset(buf, first_prev + (size_t)click_col);
            ui->caret = ui->sel_anchor = (int)(target > len ? len : target);
            ui->caret_ms = ui->now_ms;
        } else if (ui->focus == id) ui->focus = 0;
    }
    if (ui->focus == id) {
        if ((size_t)ui->caret > len) ui->caret = (int)len;
        if ((size_t)ui->sel_anchor > len) ui->sel_anchor = (int)len;
        if (ui->active == id && (ui->buttons & 1) && !ui->pressed[0]) {          /* drag selection */
            int col = (ui->mx - inner.x) / 8;
            if (col < 0) col = 0;
            size_t off = kc_utf8_offset(buf, first_prev + (size_t)col);
            ui->caret = (int)(off > len ? len : off);
        }
        if (ui->released[0] && ui->active == id) ui->active = 0;
        for (int i = 0; i < ui->nkeys; i++) {
            KKey *k = &ui->keys[i];
            if (!k->down || !k->code) continue;
            bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
            size_t c = (size_t)ui->caret, a = (size_t)ui->sel_anchor;
            size_t lo = c < a ? c : a, hi = c < a ? a : c;
            bool handled = true;
            if (ctrl && !(k->mods & MOD_ALT)) {
                if (k->code == KEY_A) { ui->sel_anchor = 0; ui->caret = (int)len; }
                else if ((k->code == KEY_C || k->code == KEY_X) && hi > lo) {
                    kapp_clipboard_copy(ui->app, buf + lo, hi - lo);
                    if (k->code == KEY_X) { field_delete_range(buf, lo, hi); ui->caret = ui->sel_anchor = (int)lo; }
                } else if (k->code == KEY_V) {
                    size_t n;
                    const char *clip = kapp_clipboard(ui->app, &n);
                    if (clip && n) {
                        if (hi > lo) { field_delete_range(buf, lo, hi); c = lo; len = strlen(buf); }
                        else c = (size_t)ui->caret;
                        size_t room = cap - 1 - strlen(buf);
                        size_t put = 0;
                        while (put < n && put < room && clip[put] != '\n' && clip[put] != '\r') put++;
                        memmove(buf + c + put, buf + c, strlen(buf + c) + 1);
                        memcpy(buf + c, clip, put);
                        ui->caret = ui->sel_anchor = (int)(c + put);
                    }
                } else handled = false;
            } else if (k->code == KEY_LEFT) {
                size_t to = hi > lo && !shift ? lo : u8_prev(buf, c);
                ui->caret = (int)to; if (!shift) ui->sel_anchor = (int)to;
            } else if (k->code == KEY_RIGHT) {
                size_t to = hi > lo && !shift ? hi : u8_next(buf, c);
                ui->caret = (int)to; if (!shift) ui->sel_anchor = (int)to;
            } else if (k->code == KEY_HOME) { ui->caret = 0; if (!shift) ui->sel_anchor = 0; }
            else if (k->code == KEY_END) { ui->caret = (int)len; if (!shift) ui->sel_anchor = (int)len; }
            else if (k->code == KEY_BACKSPACE) {
                if (hi > lo) { field_delete_range(buf, lo, hi); ui->caret = ui->sel_anchor = (int)lo; }
                else if (c) { size_t p = u8_prev(buf, c); field_delete_range(buf, p, c); ui->caret = ui->sel_anchor = (int)p; }
            } else if (k->code == KEY_DELETE) {
                if (hi > lo) { field_delete_range(buf, lo, hi); ui->caret = ui->sel_anchor = (int)lo; }
                else if (c < len) field_delete_range(buf, c, u8_next(buf, c));
            } else if (k->code == KEY_ENTER || k->code == KEY_KPENTER) enter = true;
            else if (k->code == KEY_ESC) ui->focus = 0;
            else if (k->ch >= 32 && k->ch != 127 && !(k->mods & (MOD_CTRL | MOD_ALT))) {
                if (hi > lo) { field_delete_range(buf, lo, hi); c = lo; }
                char enc[4];
                int n = kc_utf8_encode(k->ch, enc);
                size_t curlen = strlen(buf);
                if (curlen + (size_t)n < cap) {
                    memmove(buf + c + n, buf + c, curlen - c + 1);
                    memcpy(buf + c, enc, (size_t)n);
                    ui->caret = ui->sel_anchor = (int)(c + (size_t)n);
                }
            } else handled = false;
            if (handled) { k->down = 0; k->code = 0; ui->caret_ms = ui->now_ms; }
            len = strlen(buf);
        }
        ui->redraw_soon = true;
    }

    /* draw: horizontal scroll keeps the caret in view */
    size_t caret_cp = 0;
    if (ui->focus == id) {
        char tmp[2048];
        size_t cn = (size_t)ui->caret < sizeof tmp - 1 ? (size_t)ui->caret : sizeof tmp - 1;
        memcpy(tmp, buf, cn); tmp[cn] = 0;
        caret_cp = kc_utf8_len(tmp);
    }
    size_t first = caret_cp >= (size_t)cols ? caret_cp - (size_t)cols + 1 : 0;
    KRect old = ui->cv.clip;
    kc_set_clip(&ui->cv, inner);
    size_t off = kc_utf8_offset(buf, first);
    int x = inner.x, ty = inner.y + (inner.h - 16) / 2;
    if (ui->focus == id && ui->caret != ui->sel_anchor) {
        size_t lo = (size_t)(ui->caret < ui->sel_anchor ? ui->caret : ui->sel_anchor), hi = (size_t)(ui->caret < ui->sel_anchor ? ui->sel_anchor : ui->caret);
        char tmp[2048];
        size_t n1 = lo < sizeof tmp - 1 ? lo : sizeof tmp - 1;
        memcpy(tmp, buf, n1); tmp[n1] = 0;
        int c0 = (int)kc_utf8_len(tmp) - (int)first;
        n1 = hi < sizeof tmp - 1 ? hi : sizeof tmp - 1;
        memcpy(tmp, buf, n1); tmp[n1] = 0;
        int c1 = (int)kc_utf8_len(tmp) - (int)first;
        kc_fill(&ui->cv, kr(inner.x + c0 * 8, ty, (c1 - c0) * 8, 16), TH->select_bg);
    }
    if (pw) {
        size_t n = kc_utf8_len(buf + off);
        for (size_t i = 0; i < n; i++) kc_glyph(&ui->cv, x + (int)i * 8, ty, 0x2022, TH->field_text, 1);
    } else {
        kc_text(&ui->cv, x, ty, buf + off, TH->field_text);
    }
    if (ui->focus == id && ((ui->now_ms - ui->caret_ms) / 500) % 2 == 0) {
        int cx = inner.x + (int)(caret_cp - first) * 8;
        kc_vline(&ui->cv, cx, ty, 16, TH->field_text);
    }
    ui->cv.clip = old;
    if (ui->focus == id) kc_outline(&ui->cv, kr_inset(r, 2), TH->accent);
    return enter;
}

bool k_textfield(KUI *ui, KRect r, char *buf, size_t cap) { return textfield_impl(ui, r, buf, cap, false); }
bool k_textfield_pw(KUI *ui, KRect r, char *buf, size_t cap) { return textfield_impl(ui, r, buf, cap, true); }

/* ---------------------------------------------------------------- lists */

void k_list_init(KListState *st) { st->scroll = 0; st->selected = -1; st->activated = -1; st->hover = -1; }

bool k_list(KUI *ui, KRect r, const char *const *items, const KIcon *icons, int n, KListState *st) {
    const int rowh = 20;
    uint32_t id = k_id("list", r);
    int vis = r.h / rowh;
    bool changed = false;
    st->activated = -1;
    kc_panel(&ui->cv, r, KB_SUNKEN, TH->field_bg);
    KRect inner = kr_inset(r, 2);
    bool need_bar = n > vis;
    KRect barr = kr(inner.x + inner.w - 14, inner.y, 14, inner.h);
    KRect body = need_bar ? kr(inner.x, inner.y, inner.w - 14, inner.h) : inner;
    bool hover = k_hover(ui, r);
    if (hover) ui->hot = id;
    if (hover && ui->wheel) st->scroll -= ui->wheel * 3;
    if (st->scroll > n - vis) st->scroll = n - vis;
    if (st->scroll < 0) st->scroll = 0;
    st->hover = -1;
    if (k_hover(ui, body)) {
        int row = (ui->my - body.y) / rowh + st->scroll;
        if (row >= 0 && row < n) st->hover = row;
    }
    if (ui->pressed[0] && st->hover >= 0 && k_hover(ui, body)) {
        ui->focus = id;
        if (st->selected != st->hover) { st->selected = st->hover; changed = true; }
        if (ui->double_click) st->activated = st->hover;
    } else if (ui->pressed[0] && hover && !k_hover(ui, body)) ui->focus = id;
    else if (ui->pressed[0] && !hover && ui->focus == id) ui->focus = 0;
    if (ui->focus == id && n > 0) {
        for (int i = 0; i < ui->nkeys; i++) {
            KKey *k = &ui->keys[i];
            if (!k->down || !k->code) continue;
            int sel = st->selected;
            bool used = true;
            if (k->code == KEY_DOWN) sel = sel < n - 1 ? sel + 1 : sel;
            else if (k->code == KEY_UP) sel = sel > 0 ? sel - 1 : 0;
            else if (k->code == KEY_PAGEDOWN) sel = sel + vis < n ? sel + vis : n - 1;
            else if (k->code == KEY_PAGEUP) sel = sel - vis > 0 ? sel - vis : 0;
            else if (k->code == KEY_HOME) sel = 0;
            else if (k->code == KEY_END) sel = n - 1;
            else if ((k->code == KEY_ENTER || k->code == KEY_KPENTER) && sel >= 0) st->activated = sel;
            else used = false;
            if (used) {
                k->down = 0; k->code = 0;
                if (sel != st->selected) { st->selected = sel; changed = true; }
                if (st->selected < st->scroll) st->scroll = st->selected;
                if (st->selected >= st->scroll + vis) st->scroll = st->selected - vis + 1;
            }
        }
    }
    KRect old = ui->cv.clip;
    kc_set_clip(&ui->cv, body);
    for (int i = 0; i < vis + 1; i++) {
        int row = st->scroll + i;
        if (row >= n) break;
        KRect rr = kr(body.x, body.y + i * rowh, body.w, rowh);
        bool sel = row == st->selected;
        uint32_t fg = TH->field_text;
        if (sel) { kc_fill(&ui->cv, rr, ui->focus == id ? TH->select_bg : kc_shade(TH->select_bg, 130)); fg = TH->select_text; }
        else if (row == st->hover) kc_fill(&ui->cv, rr, kc_mix(TH->field_bg, TH->accent_light, 40));
        int tx = rr.x + 4;
        if (icons) { k_icon(&ui->cv, icons[row], rr.x + 3, rr.y + 2, 16); tx += 22; }
        kc_text_in(&ui->cv, kr(tx, rr.y, rr.w - (tx - rr.x), rr.h), items[row], fg, KA_LEFT);
    }
    ui->cv.clip = old;
    if (need_bar) k_scrollbar(ui, barr, &st->scroll, n, vis);
    return changed;
}

/* ---------------------------------------------------------------- menus */

int k_menubar(KUI *ui, KRect r, const char *const *titles, const KMenuItem *const *menus, const int *counts, int nmenus) {
    int result = 0;
    kc_fill(&ui->cv, r, TH->window_bg);
    kc_hline(&ui->cv, r.x, r.y + r.h - 1, r.w, kc_shade(TH->window_bg, 72));
    kc_hline(&ui->cv, r.x, r.y + r.h, r.w, 0xFFFFFF);
    int x = r.x + 2;
    int open = ui->menu_open;
    int open_x = 0;
    bool block_save = ui->input_blocked;
    ui->input_blocked = false;                                   /* the bar itself stays live while its menu is open */
    for (int m = 0; m < nmenus; m++) {
        int w = kc_text_width(titles[m]) + 16;
        KRect tr = kr(x, r.y + 1, w, r.h - 2);
        bool hover = k_hover(ui, tr);
        if (hover && ui->pressed[0]) { ui->menu_open = open == m + 1 ? 0 : m + 1; open = ui->menu_open; }
        else if (hover && open && open != m + 1) { ui->menu_open = m + 1; open = m + 1; }
        if (open == m + 1) { kc_panel(&ui->cv, tr, KB_SUNKEN, kc_shade(TH->window_bg, 94)); open_x = x; }
        else if (hover) kc_bevel(&ui->cv, tr, KB_RAISED);
        kc_text_in(&ui->cv, tr, titles[m], TH->text, KA_CENTER);
        x += w;
    }
    if (open) {
        const KMenuItem *items = menus[open - 1];
        int n = counts[open - 1], w = 0;
        for (int i = 0; i < n; i++) {
            int iw = kc_text_width(items[i].label) + 36 + (items[i].shortcut ? kc_text_width(items[i].shortcut) + 24 : 0);
            if (iw > w) w = iw;
        }
        int h = 4;
        for (int i = 0; i < n; i++) h += items[i].separator ? 7 : 22;
        KRect box = kr(open_x, r.y + r.h + 1, w + 4, h);
        kc_fill(&ui->cv, kr(box.x + 3, box.y + 3, box.w, box.h), kc_shade(TH->window_bg, 55));
        kc_panel(&ui->cv, box, KB_RAISED, TH->window_bg);
        int y = box.y + 2;
        for (int i = 0; i < n; i++) {
            if (items[i].separator) { k_separator(ui, kr(box.x + 6, y + 2, box.w - 12, 2)); y += 7; continue; }
            KRect ir = kr(box.x + 2, y, box.w - 4, 22);
            bool hover = !items[i].disabled && k_hover(ui, ir);
            if (hover) kc_fill(&ui->cv, ir, TH->select_bg);
            uint32_t fg = items[i].disabled ? TH->text_dim : hover ? TH->select_text : TH->text;
            if (items[i].icon != KI_COUNT && items[i].icon) k_icon(&ui->cv, items[i].icon, ir.x + 4, ir.y + 3, 16);
            kc_text(&ui->cv, ir.x + 26, ir.y + 3, items[i].label, fg);
            if (items[i].shortcut) kc_text(&ui->cv, ir.x + ir.w - 8 - kc_text_width(items[i].shortcut), ir.y + 3, items[i].shortcut, items[i].disabled ? TH->text_dim : fg);
            if (hover && ui->released[0]) { result = (open << 8) | (i + 1); ui->menu_open = 0; }
            y += 22;
        }
        if (ui->pressed[0] && !kr_contains(box, ui->mx, ui->my) && !kr_contains(r, ui->mx, ui->my)) ui->menu_open = 0;
        if (k_key_pressed(ui, KEY_ESC)) ui->menu_open = 0;
    }
    ui->input_blocked = block_save;
    return result;
}

/* ---------------------------------------------------------------- multi-line text area */

void kta_init(KTextArea *t, size_t cap) {
    memset(t, 0, sizeof *t);
    t->cap = cap ? cap : 4096;
    t->text = calloc(1, t->cap);
}

void kta_free(KTextArea *t) { free(t->text); free(t->ls); memset(t, 0, sizeof *t); }

static void kta_changed(KTextArea *t) { t->ls_valid = false; t->modified = true; }

static void kta_reserve(KTextArea *t, size_t need) {
    if (need + 1 <= t->cap) return;
    size_t nc = t->cap * 2;
    while (nc < need + 1) nc *= 2;
    t->text = realloc(t->text, nc);
    t->cap = nc;
}

void kta_set(KTextArea *t, const char *s, size_t len) {
    kta_reserve(t, len);
    memcpy(t->text, s, len);
    t->text[len] = 0;
    t->len = len;
    t->caret = t->anchor = 0;
    t->scroll_x = t->scroll_y = 0;
    kta_changed(t);
    t->modified = false;
}

void kta_insert(KTextArea *t, const char *s, size_t n) {
    size_t lo = t->caret < t->anchor ? t->caret : t->anchor, hi = t->caret < t->anchor ? t->anchor : t->caret;
    if (hi > lo) { memmove(t->text + lo, t->text + hi, t->len - hi + 1); t->len -= hi - lo; t->caret = t->anchor = lo; }
    kta_reserve(t, t->len + n);
    memmove(t->text + t->caret + n, t->text + t->caret, t->len - t->caret + 1);
    memcpy(t->text + t->caret, s, n);
    t->len += n;
    t->caret += n;
    t->anchor = t->caret;
    kta_changed(t);
}

static void kta_lines(KTextArea *t) {
    if (t->ls_valid) return;
    if (!t->ls) { t->ls_cap = 256; t->ls = malloc((size_t)t->ls_cap * sizeof(size_t)); }
    int n = 0;
    t->ls[n++] = 0;
    for (size_t i = 0; i < t->len; i++) {
        if (t->text[i] == '\n') {
            if (n >= t->ls_cap) { t->ls_cap *= 2; t->ls = realloc(t->ls, (size_t)t->ls_cap * sizeof(size_t)); }
            t->ls[n++] = i + 1;
        }
    }
    t->nls = n;
    t->ls_valid = true;
}

int kta_line_count(const KTextArea *t) { kta_lines((KTextArea *)t); return t->nls; }

static int kta_line_of(KTextArea *t, size_t off) {
    kta_lines(t);
    int lo = 0, hi = t->nls - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (t->ls[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static size_t kta_line_end(KTextArea *t, int line) {
    return line + 1 < t->nls ? t->ls[line + 1] - 1 : t->len;
}

/* visual column of byte offset `off` within its line (tabs advance to the next multiple of 4) */
static int kta_col(KTextArea *t, int line, size_t off) {
    int col = 0;
    size_t i = t->ls[line];
    while (i < off && i < t->len) {
        if (t->text[i] == '\t') col = (col / 4 + 1) * 4;
        else col++;
        i = u8_next(t->text, i);
    }
    return col;
}

static size_t kta_off_of_col(KTextArea *t, int line, int want) {
    int col = 0;
    size_t i = t->ls[line], end = kta_line_end(t, line);
    while (i < end) {
        int next = t->text[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
        if (next > want && want - col < next - want) break;     /* closer to the left edge of this cell */
        if (col >= want) break;
        col = next;
        i = u8_next(t->text, i);
    }
    return i;
}

void kta_goto_line(KTextArea *t, int line) {
    kta_lines(t);
    if (line < 0) line = 0;
    if (line >= t->nls) line = t->nls - 1;
    t->caret = t->anchor = t->ls[line];
    t->scroll_y = line > 3 ? line - 3 : 0;
}

bool k_textarea(KUI *ui, KRect r, KTextArea *t) {
    uint32_t id = (uint32_t)(uintptr_t)t * 2654435761u | 1;
    bool changed = false;
    const int lh = 16;
    kta_lines(t);
    kc_panel(&ui->cv, r, KB_SUNKEN, TH->field_bg);
    KRect inner = kr_inset(r, 2);
    int vis = inner.h / lh;
    bool need_bar = t->nls > vis;
    KRect body = need_bar ? kr(inner.x, inner.y, inner.w - 14, inner.h) : inner;
    int cols = (body.w - 6) / 8;
    bool hover = k_hover(ui, body);
    if (hover) ui->cursor_shape = FC_TEXT;
    if (hover && ui->wheel) t->scroll_y -= ui->wheel * 3;

    int line = kta_line_of(t, t->caret);
    size_t before_caret = t->caret, before_anchor = t->anchor;
    (void)before_caret; (void)before_anchor;

    /* mouse */
    if (hover && ui->pressed[0]) {
        ui->focus = id; ui->active = id;
        int row = (ui->my - body.y) / lh + t->scroll_y;
        if (row >= t->nls) row = t->nls - 1;
        if (row < 0) row = 0;
        int col = (ui->mx - body.x - 3) / 8 + t->scroll_x;
        t->caret = kta_off_of_col(t, row, col < 0 ? 0 : col);
        if (!(ui->mods & MOD_SHIFT)) t->anchor = t->caret;
        ui->caret_ms = ui->now_ms;
    } else if (ui->pressed[0] && !hover && ui->focus == id && !k_hover(ui, kr(r.x + r.w - 14, r.y, 14, r.h))) ui->focus = 0;
    if (ui->active == id && (ui->buttons & 1) && !ui->pressed[0]) {
        int row = (ui->my - body.y) / lh + t->scroll_y;
        if (row >= t->nls) row = t->nls - 1;
        if (row < 0) row = 0;
        int col = (ui->mx - body.x - 3) / 8 + t->scroll_x;
        t->caret = kta_off_of_col(t, row, col < 0 ? 0 : col);
        if (ui->my < body.y) t->scroll_y -= 1;
        if (ui->my > body.y + body.h) t->scroll_y += 1;
    }
    if (ui->released[0] && ui->active == id) ui->active = 0;

    /* keyboard */
    if (ui->focus == id) {
        static int want_col = -1;
        for (int i = 0; i < ui->nkeys; i++) {
            KKey *k = &ui->keys[i];
            if (!k->down || !k->code) continue;
            bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL, alt = k->mods & MOD_ALT;
            size_t lo = t->caret < t->anchor ? t->caret : t->anchor, hi = t->caret < t->anchor ? t->anchor : t->caret;
            bool handled = true, moved = false, vertical = false;
            int ln = kta_line_of(t, t->caret);
            if (ctrl && !alt) {
                if (k->code == KEY_A) { t->anchor = 0; t->caret = t->len; }
                else if (k->code == KEY_C && hi > lo) kapp_clipboard_copy(ui->app, t->text + lo, hi - lo);
                else if (k->code == KEY_X && hi > lo && !t->readonly) { kapp_clipboard_copy(ui->app, t->text + lo, hi - lo); kta_insert(t, "", 0); changed = true; }
                else if (k->code == KEY_V && !t->readonly) {
                    size_t n;
                    const char *clip = kapp_clipboard(ui->app, &n);
                    if (clip && n) { kta_insert(t, clip, n); changed = true; }
                } else if (k->code == KEY_HOME) { t->caret = 0; moved = true; }
                else if (k->code == KEY_END) { t->caret = t->len; moved = true; }
                else handled = false;
            } else if (k->code == KEY_LEFT) { t->caret = hi > lo && !shift ? lo : u8_prev(t->text, t->caret); moved = true; }
            else if (k->code == KEY_RIGHT) { t->caret = hi > lo && !shift ? hi : u8_next(t->text, t->caret); moved = true; }
            else if (k->code == KEY_UP || k->code == KEY_DOWN || k->code == KEY_PAGEUP || k->code == KEY_PAGEDOWN) {
                int step = (k->code == KEY_PAGEUP || k->code == KEY_PAGEDOWN) ? (vis > 1 ? vis - 1 : 1) : 1;
                int nl = (k->code == KEY_UP || k->code == KEY_PAGEUP) ? ln - step : ln + step;
                if (nl < 0) nl = 0;
                if (nl >= t->nls) nl = t->nls - 1;
                if (want_col < 0) want_col = kta_col(t, ln, t->caret);
                t->caret = kta_off_of_col(t, nl, want_col);
                moved = true; vertical = true;
            } else if (k->code == KEY_HOME) { t->caret = t->ls[ln]; moved = true; }
            else if (k->code == KEY_END) { t->caret = kta_line_end(t, ln); moved = true; }
            else if (t->readonly) handled = false;
            else if (k->code == KEY_BACKSPACE) {
                if (hi > lo) kta_insert(t, "", 0);
                else if (t->caret) { size_t p = u8_prev(t->text, t->caret); t->anchor = p; kta_insert(t, "", 0); }
                changed = true;
            } else if (k->code == KEY_DELETE) {
                if (hi > lo) kta_insert(t, "", 0);
                else if (t->caret < t->len) { t->anchor = u8_next(t->text, t->caret); kta_insert(t, "", 0); }
                changed = true;
            } else if (k->code == KEY_ENTER || k->code == KEY_KPENTER) {
                char ins[130];
                size_t n = 0;
                ins[n++] = '\n';
                size_t ls = t->ls[ln];                                   /* auto-indent */
                while (ls + n - 1 < t->caret && (t->text[ls + n - 1] == ' ' || t->text[ls + n - 1] == '\t') && n < 120) { ins[n] = t->text[ls + n - 1]; n++; }
                kta_insert(t, ins, n);
                changed = true;
            } else if (k->code == KEY_TAB) { kta_insert(t, "    ", 4); changed = true; }
            else if (k->ch >= 32 && k->ch != 127 && !alt) {
                char enc[4];
                int n = kc_utf8_encode(k->ch, enc);
                kta_insert(t, enc, (size_t)n);
                changed = true;
            } else handled = false;
            if (moved && !shift) t->anchor = t->caret;
            if (handled) {
                if (!vertical) want_col = -1;
                k->down = 0; k->code = 0;
                ui->caret_ms = ui->now_ms;
            }
        }
        ui->redraw_soon = true;
        kta_lines(t);
    }

    /* keep the caret visible */
    line = kta_line_of(t, t->caret);
    if (ui->focus == id) {
        if (line < t->scroll_y) t->scroll_y = line;
        if (line >= t->scroll_y + vis) t->scroll_y = line - vis + 1;
        int ccol = kta_col(t, line, t->caret);
        if (ccol < t->scroll_x) t->scroll_x = ccol;
        if (ccol >= t->scroll_x + cols) t->scroll_x = ccol - cols + 1;
    }
    if (t->scroll_y > t->nls - vis) t->scroll_y = t->nls - vis;
    if (t->scroll_y < 0) t->scroll_y = 0;

    /* draw */
    KRect old = ui->cv.clip;
    kc_set_clip(&ui->cv, body);
    size_t slo = t->caret < t->anchor ? t->caret : t->anchor, shi = t->caret < t->anchor ? t->anchor : t->caret;
    for (int i = 0; i < vis + 1; i++) {
        int ln = t->scroll_y + i;
        if (ln >= t->nls) break;
        int y = body.y + i * lh;
        size_t a = t->ls[ln], b = kta_line_end(t, ln);
        int col = 0;
        for (size_t p = a; p <= b; p = u8_next(t->text, p)) {
            int x = body.x + 3 + (col - t->scroll_x) * 8;
            bool sel = slo < shi && p >= slo && p < shi;
            if (p == b) {
                if (sel || (slo < shi && b >= slo && b < shi && ln + 1 < t->nls)) kc_fill(&ui->cv, kr(x, y, 8, lh), TH->select_bg);   /* the newline */
                break;
            }
            uint32_t cp;
            kc_utf8_next(t->text + p, &cp);
            int w = 1;
            if (cp == '\t') { int nc = (col / 4 + 1) * 4; w = nc - col; }
            if (x + w * 8 > body.x && x < body.x + body.w) {
                if (sel) kc_fill(&ui->cv, kr(x, y, w * 8, lh), TH->select_bg);
                if (cp != '\t') kc_glyph(&ui->cv, x, y, cp, sel ? TH->select_text : TH->field_text, 1);
            }
            col += w;
            if (x > body.x + body.w) break;
        }
        if (ui->focus == id && ln == line && ((ui->now_ms - ui->caret_ms) / 500) % 2 == 0) {
            int cx = body.x + 3 + (kta_col(t, ln, t->caret) - t->scroll_x) * 8;
            kc_vline(&ui->cv, cx, y, lh, TH->field_text);
        }
    }
    ui->cv.clip = old;
    if (need_bar) {
        KRect barr = kr(inner.x + inner.w - 14, inner.y, 14, inner.h);
        k_scrollbar(ui, barr, &t->scroll_y, t->nls, vis);
    }
    if (ui->focus == id) kc_outline(&ui->cv, kr_inset(r, 1), TH->accent);
    return changed;
}
