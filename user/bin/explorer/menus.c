/* Menus drawn inside the window: the keyboard-driven menu bar and the context menu. The window server cannot tell a window where
 * it is on the screen after the user moved it, so a popup window could not be placed at the pointer; the menu is painted over the
 * window's own content instead (the Notebook does the same). Both use the same Menu description. */
#include "explorer.h"
#include <stdio.h>
#include <string.h>

void menu_clear(Menu *m) { m->n = 0; }

void menu_item(Menu *m, const char *label, const char *keys, int act, bool disabled, bool checked) {
    if (m->n >= MENU_MAX) return;
    MItem *it = &m->it[m->n++];
    it->label = label;
    it->keys = keys;
    it->act = act;
    it->sep = false;
    it->disabled = disabled;
    it->checked = checked;
}

void menu_sep(Menu *m) {
    if (m->n >= MENU_MAX || m->n == 0 || m->it[m->n - 1].sep) return;
    MItem *it = &m->it[m->n++];
    memset(it, 0, sizeof *it);
    it->label = "";
    it->sep = true;
}

static int menu_w(const Menu *m) {
    int w = 0;
    for (int i = 0; i < m->n; i++) {
        if (m->it[i].sep) continue;
        int iw = kc_text_width(m->it[i].label) + 40 + (m->it[i].keys ? kc_text_width(m->it[i].keys) + 24 : 0);
        if (iw > w) w = iw;
    }
    return w + 4;
}

static int menu_h(const Menu *m) {
    int h = 4;
    for (int i = 0; i < m->n; i++) h += m->it[i].sep ? 7 : 22;
    return h;
}

static void menu_layout(const Menu *m, KRect box, KRect *rects) {
    int y = box.y + 2;
    for (int i = 0; i < m->n; i++) {
        rects[i] = kr(box.x + 2, y, box.w - 4, m->it[i].sep ? 5 : 22);
        y += m->it[i].sep ? 7 : 22;
    }
}

static void menu_paint(KUI *ui, const Menu *m, KRect box, const KRect *rects, int hover) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect old = cv->clip;
    kc_set_clip(cv, kr(0, 0, cv->w, cv->h));
    kc_fill(cv, kr(box.x + 3, box.y + 3, box.w, box.h), kc_shade(th->window_bg, 55));
    kc_panel(cv, box, KB_RAISED, th->window_bg);
    for (int i = 0; i < m->n; i++) {
        const MItem *it = &m->it[i];
        if (it->sep) { k_separator(ui, kr(rects[i].x + 4, rects[i].y + 1, rects[i].w - 8, 2)); continue; }
        bool hot = !it->disabled && hover == i;
        if (hot) kc_fill(cv, rects[i], th->select_bg);
        uint32_t fg = it->disabled ? th->text_dim : hot ? th->select_text : th->text;
        if (it->checked) k_icon(cv, KI_CHECK, rects[i].x + 4, rects[i].y + 3, 16);
        kc_text(cv, rects[i].x + 26, rects[i].y + 3, it->label, fg);
        if (it->keys) kc_text(cv, rects[i].x + rects[i].w - 8 - kc_text_width(it->keys), rects[i].y + 3, it->keys, fg);
    }
    cv->clip = old;
}

static int step_hover(const Menu *m, int from, int dir) {
    int i = from;
    for (int tries = 0; tries < m->n; tries++) {
        i = i < 0 || i >= m->n ? (dir > 0 ? 0 : m->n - 1) : (i + dir + m->n) % m->n;
        if (!m->it[i].sep && !m->it[i].disabled) return i;
    }
    return from;
}

/* ---------------------------------------------------------------- context menu */

static struct { bool open; Menu m; KRect box; int hover, key_act, px, py; uint32_t iter; } g_cx;

bool ctx_is_open(void) { return g_cx.open; }
void ctx_close(void) { g_cx.open = false; }

void ctx_open(KUI *ui, const Menu *m, int x, int y) {
    g_cx.m = *m;
    int w = menu_w(m), h = menu_h(m);
    int W = ui->cv.w, H = ui->cv.h;
    if (x + w > W) x = W - w;
    if (y + h > H) y = H - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_cx.box = kr(x, y, w, h);
    g_cx.hover = -1;
    g_cx.key_act = 0;
    g_cx.px = ui->mx;
    g_cx.py = ui->my;
    g_cx.iter = X.iter;
    g_cx.open = true;
    ex_trace("context menu %d items", m->n);
}

void ctx_key(const KKey *k) {
    if (!g_cx.open) return;
    if (k->code == KEY_ESC) g_cx.open = false;
    else if (k->code == KEY_DOWN) g_cx.hover = step_hover(&g_cx.m, g_cx.hover, 1);
    else if (k->code == KEY_UP) g_cx.hover = step_hover(&g_cx.m, g_cx.hover, -1);
    else if (k->code == KEY_HOME) { g_cx.hover = -1; g_cx.hover = step_hover(&g_cx.m, -1, 1); }
    else if (k->code == KEY_END) { g_cx.hover = -1; g_cx.hover = step_hover(&g_cx.m, -1, -1); }
    else if ((k->code == KEY_ENTER || k->code == KEY_KPENTER) && g_cx.hover >= 0) g_cx.key_act = g_cx.m.it[g_cx.hover].act;
}

int ctx_run(KUI *ui) {
    if (!g_cx.open) return 0;
    KRect rects[MENU_MAX];
    menu_layout(&g_cx.m, g_cx.box, rects);
    int over = -1;
    for (int i = 0; i < g_cx.m.n; i++)
        if (!g_cx.m.it[i].sep && !g_cx.m.it[i].disabled && ui->mouse_inside && kr_contains(rects[i], ui->mx, ui->my)) over = i;
    if (over >= 0 && (ui->mx != g_cx.px || ui->my != g_cx.py)) g_cx.hover = over;
    int act = g_cx.key_act;
    g_cx.key_act = 0;
    bool close = act != 0;
    bool fresh = g_cx.iter == X.iter;                                   /* the click that opened it is still in this frame's input */
    if (!fresh && ui->released[0] && over >= 0) { act = g_cx.m.it[over].act; close = true; }
    else if (!fresh && (ui->pressed[0] || ui->pressed[1] || ui->pressed[2]) && !kr_contains(g_cx.box, ui->mx, ui->my)) close = true;
    if (!g_cx.open) close = true;
    if (close) {
        g_cx.open = false;
        kapp_redraw(ui->app);
        return act;
    }
    menu_paint(ui, &g_cx.m, g_cx.box, rects, g_cx.hover);
    return 0;
}

/* ---------------------------------------------------------------- menu bar */

/* Like the stock k_menubar but with the keyboard: Alt+letter and F10 open a menu (the caller does that), then arrows, Home/End,
 * Enter and Esc work. ui->menu_open is the open menu (1-based) and ui->menu_item_hover the highlighted item (-1: none). */

static int g_mb_act, g_mb_lastx = -1, g_mb_lasty = -1;

void mbar_open(KUI *ui, int m) {
    ui->menu_open = m;
    ui->menu_item_hover = -1;
}

bool mbar_key(KUI *ui, const KKey *k, const Menu *menus, int nmenus) {
    int open = ui->menu_open;
    if (!open || (k->mods & (MOD_CTRL | MOD_ALT))) return false;
    const Menu *m = &menus[open - 1];
    switch (k->code) {
    case KEY_ESC: mbar_open(ui, 0); break;
    case KEY_LEFT: mbar_open(ui, open == 1 ? nmenus : open - 1); break;
    case KEY_RIGHT: mbar_open(ui, open == nmenus ? 1 : open + 1); break;
    case KEY_DOWN: ui->menu_item_hover = step_hover(m, ui->menu_item_hover, 1); break;
    case KEY_UP: ui->menu_item_hover = step_hover(m, ui->menu_item_hover, -1); break;
    case KEY_HOME: ui->menu_item_hover = step_hover(m, -1, 1); break;
    case KEY_END: ui->menu_item_hover = step_hover(m, -1, -1); break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
        if (ui->menu_item_hover >= 0 && ui->menu_item_hover < m->n) {
            g_mb_act = m->it[ui->menu_item_hover].act;
            mbar_open(ui, 0);
        }
        break;
    default: break;
    }
    return true;
}

int mbar_run(KUI *ui, KRect bar, bool live, const char *const *titles, const Menu *menus, int nmenus) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    int act = g_mb_act;
    g_mb_act = 0;
    KRect tr[8];
    int x = bar.x + 2;
    for (int i = 0; i < nmenus && i < 8; i++) {
        int w = kc_text_width(titles[i]) + 16;
        tr[i] = kr(x, bar.y + 1, w, bar.h - 2);
        x += w;
    }
    bool mouse = live && ui->mouse_inside;
    bool moved = ui->mx != g_mb_lastx || ui->my != g_mb_lasty;
    g_mb_lastx = ui->mx;
    g_mb_lasty = ui->my;
    int open = ui->menu_open;
    bool over_bar = false;
    for (int i = 0; i < nmenus; i++) {
        bool hv = mouse && kr_contains(tr[i], ui->mx, ui->my);
        if (hv) over_bar = true;
        if (hv && ui->pressed[0]) { open = open == i + 1 ? 0 : i + 1; ui->menu_item_hover = -1; }
        else if (hv && open && open != i + 1 && moved) { open = i + 1; ui->menu_item_hover = -1; }
    }
    ui->menu_open = open;
    KRect box = kr(0, 0, 0, 0);
    KRect irs[MENU_MAX];
    if (open) {
        const Menu *m = &menus[open - 1];
        box = kr(tr[open - 1].x, bar.y + bar.h + 1, menu_w(m), menu_h(m));
        if (box.x + box.w > cv->w) box.x = cv->w - box.w;
        if (box.x < 0) box.x = 0;
        menu_layout(m, box, irs);
        int over = -1;
        for (int i = 0; i < m->n; i++)
            if (mouse && !m->it[i].sep && !m->it[i].disabled && kr_contains(irs[i], ui->mx, ui->my)) over = i;
        if (over >= 0 && moved) ui->menu_item_hover = over;
        if (live && ui->released[0] && over >= 0) { act = m->it[over].act; ui->menu_open = 0; }
        else if (live && ui->pressed[0] && !kr_contains(box, ui->mx, ui->my) && !over_bar) { ui->menu_open = 0; ui->menu_item_hover = -1; }
        if (act) ui->menu_open = 0;
    }
    kc_fill(cv, bar, th->window_bg);
    kc_hline(cv, bar.x, bar.y + bar.h - 1, bar.w, kc_shade(th->window_bg, 72));
    kc_hline(cv, bar.x, bar.y + bar.h, bar.w, 0xFFFFFF);
    bool underline = (ui->mods & MOD_ALT) != 0 || ui->menu_open;
    for (int i = 0; i < nmenus; i++) {
        bool hv = mouse && kr_contains(tr[i], ui->mx, ui->my);
        if (ui->menu_open == i + 1) kc_panel(cv, tr[i], KB_SUNKEN, kc_shade(th->window_bg, 94));
        else if (hv) kc_bevel(cv, tr[i], KB_RAISED);
        kc_text_in(cv, tr[i], titles[i], th->text, KA_CENTER);
        if (underline) kc_hline(cv, tr[i].x + (tr[i].w - kc_text_width(titles[i])) / 2, tr[i].y + 17, 8, th->text);
    }
    if (ui->menu_open && ui->menu_open == open) menu_paint(ui, &menus[open - 1], box, irs, ui->menu_item_hover);
    return act;
}
