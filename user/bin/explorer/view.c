/* The file view: list ("details") and icon mode. Only the rows or cells on screen are ever drawn or hit-tested, so thousands of
 * entries cost nothing extra. Selection (click, Ctrl, Shift, rubber band, keyboard), drag and drop, type-ahead find and inline
 * rename live here. The pointer and keys arrive through KUI; the app (main.c) decides what a command means. */
#include "explorer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_drop_place = -1;

static struct {
    bool pending;                       /* a press on an entry is down; it may turn into a drag */
    bool defer;                         /* a click on one of several selected entries collapses the selection on release */
    bool ctrl_off;                      /* a Ctrl+click on a selected entry removes it on release (unless it became a drag) */
    bool dragging;
    int idx, px, py;
    int drop_idx;
    bool drop_valid;
    char drop_path[EX_PATH];
    int last_click_idx;
    bool band;
    int bx0, by0;                       /* band anchor in content coordinates */
    bool band_ctrl;
    unsigned char *base;                /* selection before a Ctrl-band */
    int base_n;
    bool ren;                           /* inline rename */
    bool ren_new, ren_init;
    int ren_idx;
    char ren_buf[EX_NAME], ren_orig[EX_NAME];
    char ta[48];                        /* type-ahead */
    uint64_t ta_ms;
} V = { .idx = -1, .drop_idx = -1, .last_click_idx = -1 };

static struct {
    KRect r, inner, hdr, body, sbar;
    bool bar;
    int icols;                          /* icon view: cells per row */
    int nx;                             /* visible extra columns of the list */
    struct { int col, x, w; } cl[5];
    int name_w;
} G;

static const struct { int col; int w; const char *title; } k_cols[5] = {
    { COL_SIZE, 84, "Size" }, { COL_MTIME, 148, "Modified" }, { COL_TYPE, 124, "Type" }, { COL_PERM, 108, "Permissions" },
    { COL_OWNER, 72, "Owner" },
};

/* ---------------------------------------------------------------- layout */

static void layout(KRect r) {
    G.r = r;
    G.inner = kr_inset(r, 2);
    bool list = X.view_mode == 0;
    int n = X.m.nshown;
    int head = list ? L_HEAD_H : 0;
    G.hdr = kr(G.inner.x, G.inner.y, G.inner.w, head);
    KRect body = kr(G.inner.x, G.inner.y + head, G.inner.w, G.inner.h - head);
    if (body.h < 1) body.h = 1;
    G.bar = false;
    if (list) {
        X.content_h = n * L_ROW_H;
        G.bar = X.content_h > body.h;
        if (G.bar) body.w -= L_SB_W;
    } else {
        int cols = (body.w - 8) / L_CELL_W;
        if (cols < 1) cols = 1;
        int content = ((n + cols - 1) / cols) * L_CELL_H;
        if (content > body.h) {
            G.bar = true;
            body.w -= L_SB_W;
            cols = (body.w - 8) / L_CELL_W;
            if (cols < 1) cols = 1;
            content = ((n + cols - 1) / cols) * L_CELL_H;
        }
        G.icols = cols;
        X.cols = cols;
        X.content_h = content;
    }
    G.body = body;
    G.sbar = kr(G.inner.x + G.inner.w - L_SB_W, body.y, L_SB_W, body.h);
    int maxs = X.content_h - body.h;
    if (maxs < 0) maxs = 0;
    if (X.scroll > maxs) X.scroll = maxs;
    if (X.scroll < 0) X.scroll = 0;
    if (list) {
        int avail = body.w - 160, used = 0;
        G.nx = 0;
        for (int i = 0; i < 5; i++) {
            if (used + k_cols[i].w > avail) break;
            G.cl[i].col = k_cols[i].col;
            G.cl[i].w = k_cols[i].w;
            used += k_cols[i].w;
            G.nx++;
        }
        G.name_w = body.w - used;
        int x = body.x + G.name_w;
        for (int i = 0; i < G.nx; i++) { G.cl[i].x = x; x += G.cl[i].w; }
    }
}

static KRect cell_rect(int i) {
    int row = i / G.icols, col = i % G.icols;
    return kr(G.body.x + 4 + col * L_CELL_W, G.body.y + row * L_CELL_H - X.scroll, L_CELL_W, L_CELL_H);
}

static KRect row_rect(int i) { return kr(G.body.x, G.body.y + i * L_ROW_H - X.scroll, G.body.w, L_ROW_H); }

/* Two lines of at most 11 columns for an icon label; breaks after a separator when one is near the end of the first line. */
static void wrap_label(const char *name, char *l1, char *l2, size_t cap) {
    size_t n = kc_utf8_len(name);
    if (n <= 11) { strlcpy(l1, name, cap); l2[0] = 0; return; }
    size_t cut = 11;
    for (size_t k = 11; k >= 5; k--) {
        size_t off = kc_utf8_offset(name, k - 1);
        char c = name[off];
        if (c == ' ' || c == '-' || c == '_' || c == '.') { cut = k; break; }
    }
    size_t off = kc_utf8_offset(name, cut);
    snprintf(l1, cap, "%.*s", (int)off, name);
    const char *rest = name + off;
    while (*rest == ' ') rest++;
    if (kc_utf8_len(rest) <= 11) strlcpy(l2, rest, cap);
    else {
        size_t o2 = kc_utf8_offset(rest, 8);
        snprintf(l2, cap, "%.*s...", (int)o2, rest);
    }
}

static KRect label_box(int i, KRect c) {
    char l1[EX_NAME], l2[EX_NAME];
    wrap_label(X.m.shown[i]->name, l1, l2, sizeof l1);
    int w1 = kc_text_width(l1), w2 = kc_text_width(l2);
    int w = (w1 > w2 ? w1 : w2) + 6;
    int lines = l2[0] ? 2 : 1;
    return kr(c.x + (c.w - w) / 2, c.y + 44, w, lines * 16 + 2);
}

static KRect icon_box(KRect c) { return kr(c.x + (c.w - 40) / 2, c.y + 2, 40, 40); }

static int hit_at(int mx, int my) {
    if (!kr_contains(G.body, mx, my)) return -1;
    int n = X.m.nshown;
    if (X.view_mode == 0) {
        int row = (my - G.body.y + X.scroll) / L_ROW_H;
        return row >= 0 && row < n ? row : -1;
    }
    int x = mx - G.body.x - 4;
    if (x < 0) return -1;
    int col = x / L_CELL_W, row = (my - G.body.y + X.scroll) / L_CELL_H;
    if (col >= G.icols || row < 0) return -1;
    int i = row * G.icols + col;
    if (i >= n) return -1;
    KRect c = cell_rect(i);
    if (kr_contains(icon_box(c), mx, my) || kr_contains(label_box(i, c), mx, my)) return i;
    return -1;
}

int view_entry_at(int mx, int my) { return hit_at(mx, my); }

void view_scroll_to(int idx) {
    if (idx < 0 || idx >= X.m.nshown) return;
    int top, h;
    if (X.view_mode == 0) { top = idx * L_ROW_H; h = L_ROW_H; }
    else { top = (idx / (X.cols > 0 ? X.cols : 1)) * L_CELL_H; h = L_CELL_H; }
    int vh = X.r_body.h > 0 ? X.r_body.h : 200;
    if (top < X.scroll) X.scroll = top;
    else if (top + h > X.scroll + vh) X.scroll = top + h - vh;
    if (X.scroll < 0) X.scroll = 0;
}

/* ---------------------------------------------------------------- state changes */

void view_reset(void) {
    X.scroll = 0;
    X.cursor = X.anchor = -1;
    V.pending = V.dragging = V.band = V.ren = false;
    V.last_click_idx = -1;
    V.ta[0] = 0;
    g_drop_place = -1;
}

bool view_renaming(void) { return V.ren; }
bool view_dragging(void) { return V.dragging; }
bool view_busy(void) { return V.pending || V.band || V.dragging || V.ren; }          /* a mouse gesture or an edit is in progress */
void view_cancel_drag(void) { V.dragging = V.pending = false; g_drop_place = -1; }
void view_cancel_rename(void) { V.ren = false; }
void view_typeahead_clear(void) { V.ta[0] = 0; }

const char *view_typeahead(void) {
    if (V.ta[0] && ex_now_ms() - V.ta_ms > 1000) V.ta[0] = 0;
    return V.ta;
}

void view_set_cursor(int idx, bool extend) {
    if (idx < 0 || idx >= X.m.nshown) return;
    if (extend) {
        if (X.anchor < 0) X.anchor = X.cursor >= 0 ? X.cursor : idx;
        model_select_range(&X.m, X.anchor, idx, true, true);
    } else {
        model_select_range(&X.m, idx, idx, true, true);
        X.anchor = idx;
    }
    X.cursor = idx;
    view_scroll_to(idx);
}

void view_start_rename(int idx, bool is_new) {
    if (idx < 0 || idx >= X.m.nshown) return;
    Entry *e = X.m.shown[idx];
    V.ren = true;
    V.ren_new = is_new;
    V.ren_init = false;
    V.ren_idx = idx;
    strlcpy(V.ren_buf, e->name, sizeof V.ren_buf);
    strlcpy(V.ren_orig, e->name, sizeof V.ren_orig);
    view_set_cursor(idx, false);
    ex_trace("rename start %s", e->name);
}

/* The entries were replaced (a refresh): drop indexes kept for gestures, find an inline rename again by name. */
void view_model_changed(void) {
    V.idx = V.drop_idx = V.last_click_idx = -1;
    V.pending = V.dragging = V.band = false;
    free(V.base);
    V.base = NULL;
    if (V.ren) {
        int i = model_find(&X.m, V.ren_orig);
        if (i < 0) V.ren = false;
        else V.ren_idx = i;
    }
}

/* Remember the selection (by name) before a rescan and put it back afterwards. */
static char **s_saved;
static int s_nsaved;
static char s_saved_cursor[EX_NAME];

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void view_save_selection(void) {
    for (int i = 0; i < s_nsaved; i++) free(s_saved[i]);
    free(s_saved);
    s_saved = NULL;
    s_nsaved = 0;
    s_saved_cursor[0] = 0;
    if (X.cursor >= 0 && X.cursor < X.m.nshown) strlcpy(s_saved_cursor, X.m.shown[X.cursor]->name, sizeof s_saved_cursor);
    int n = 0;
    for (int i = 0; i < X.m.nshown; i++) if (X.m.shown[i]->sel) n++;
    if (!n) return;
    s_saved = malloc((size_t)n * sizeof *s_saved);
    if (!s_saved) return;
    for (int i = 0; i < X.m.nshown; i++) if (X.m.shown[i]->sel) s_saved[s_nsaved++] = strdup(X.m.shown[i]->name);
    qsort(s_saved, (size_t)s_nsaved, sizeof *s_saved, cmp_str);
}

void view_restore_selection(const char *select_name) {
    model_select_none(&X.m);
    X.cursor = X.anchor = -1;
    if (select_name && *select_name) {
        int i = model_find(&X.m, select_name);
        if (i >= 0) { view_set_cursor(i, false); }
        return;
    }
    for (int i = 0; i < X.m.nshown; i++) {
        Entry *e = X.m.shown[i];
        if (s_nsaved && bsearch(&e->name, s_saved, (size_t)s_nsaved, sizeof *s_saved, cmp_str)) e->sel = true;
    }
    if (s_saved_cursor[0]) X.cursor = model_find(&X.m, s_saved_cursor);
    X.anchor = X.cursor;
}

/* ---------------------------------------------------------------- keyboard */

static int page_step(void) {
    int rows = X.r_body.h / (X.view_mode == 0 ? L_ROW_H : L_CELL_H);
    if (rows < 1) rows = 1;
    return X.view_mode == 0 ? rows - 1 : (rows - 1) * (X.cols > 0 ? X.cols : 1);
}

bool view_key(KUI *ui, const KKey *k) {
    (void)ui;
    int n = X.m.nshown;
    bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL, alt = k->mods & MOD_ALT;
    if (alt) return false;
    int cur = X.cursor;
    int cols = X.view_mode == 0 ? 1 : (X.cols > 0 ? X.cols : 1);
    int to = -2;
    switch (k->code) {
    case KEY_DOWN: to = cur < 0 ? 0 : cur + cols; break;
    case KEY_UP: to = cur < 0 ? n - 1 : cur - cols; break;
    case KEY_RIGHT: if (X.view_mode == 1) to = cur < 0 ? 0 : cur + 1; break;
    case KEY_LEFT: if (X.view_mode == 1) to = cur < 0 ? n - 1 : cur - 1; break;
    case KEY_HOME: to = 0; break;
    case KEY_END: to = n - 1; break;
    case KEY_PAGEDOWN: to = cur < 0 ? 0 : cur + page_step(); break;
    case KEY_PAGEUP: to = cur < 0 ? 0 : cur - page_step(); break;
    default: break;
    }
    if (to != -2) {
        if (n == 0) return true;
        if (to < 0) to = k->code == KEY_PAGEUP ? 0 : (cur >= 0 ? cur : 0);          /* at the top: stay */
        if (to >= n) {
            if (k->code == KEY_DOWN && cur >= 0 && cur / cols < (n - 1) / cols) to = n - 1;   /* a shorter last row */
            else if (k->code == KEY_PAGEDOWN) to = n - 1;
            else to = cur >= 0 ? cur : n - 1;
        }
        V.ta[0] = 0;
        if (ctrl && !shift) { X.cursor = to; view_scroll_to(to); }
        else view_set_cursor(to, shift);
        ex_trace("cursor %d %s", to, X.m.shown[to]->name);
        return true;
    }
    if (k->code == KEY_SPACE && !V.ta[0] && n > 0) {
        if (cur < 0) cur = 0;
        Entry *e = X.m.shown[cur];
        if (ctrl) e->sel = !e->sel; else view_set_cursor(cur, false);
        X.cursor = X.anchor = cur;
        return true;
    }
    if (ctrl || k->ch < 32 || k->ch == 127 || k->ch > 0x10FFFF) return false;
    /* type-ahead find */
    uint64_t now = ex_now_ms();
    if (V.ta[0] && now - V.ta_ms > 1000) V.ta[0] = 0;
    char enc[8];
    int el = kc_utf8_encode(k->ch, enc);
    enc[el] = 0;
    char low[8];
    fold_name(enc, low, sizeof low);
    size_t len = strlen(V.ta);
    if (len + (size_t)el >= sizeof V.ta) return true;
    memcpy(V.ta + len, low, (size_t)el + 1);
    V.ta_ms = now;
    bool single = strlen(V.ta) == (size_t)el;
    int from = X.cursor < 0 ? 0 : X.cursor;
    int found = model_prefix(&X.m, V.ta, from, !single || X.cursor < 0);
    if (found < 0 && single) found = model_prefix(&X.m, V.ta, 0, true);
    if (found >= 0) {
        view_set_cursor(found, false);
        ex_trace("find %s -> %s", V.ta, X.m.shown[found]->name);
    } else ex_trace("find %s -> none", V.ta);
    return true;
}

/* ---------------------------------------------------------------- the pointer */

static void collapse_to(int idx) {
    model_select_range(&X.m, idx, idx, true, true);
    X.anchor = X.cursor = idx;
}

static void update_drop(KUI *ui) {
    V.drop_valid = false;
    V.drop_path[0] = 0;
    V.drop_idx = -1;
    g_drop_place = -1;
    int hit = hit_at(ui->mx, ui->my);
    if (hit >= 0) {
        Entry *e = X.m.shown[hit];
        if (e->is_dir && !e->sel) {
            V.drop_idx = hit;
            path_join(V.drop_path, sizeof V.drop_path, X.m.path, e->name);
            V.drop_valid = true;
            return;
        }
        return;
    }
    int pl = ex_place_at(ui->mx, ui->my);
    if (pl >= 0) {
        g_drop_place = pl;
        strlcpy(V.drop_path, X.places[pl].path, sizeof V.drop_path);
        V.drop_valid = true;
    }
}

static void do_drop(KUI *ui) {
    if (!V.drop_valid) return;
    char **paths;
    int n;
    ex_selected_paths(&paths, &n);
    if (n > 0) {
        int how = (ui->mods & MOD_CTRL) ? 1 : (ui->mods & MOD_SHIFT) ? 2 : 0;
        ex_trace("drop %d items on %s", n, V.drop_path);
        ex_start_drop(paths, n, V.drop_path, how);
    }
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

static void band_update(KUI *ui) {
    int mx = ui->mx < G.body.x ? G.body.x : ui->mx >= G.body.x + G.body.w ? G.body.x + G.body.w - 1 : ui->mx;
    int my = ui->my;
    if (my < G.body.y) { X.scroll -= (G.body.y - my) / 2 + 4; kapp_redraw(ui->app); }
    else if (my >= G.body.y + G.body.h) { X.scroll += (my - G.body.y - G.body.h) / 2 + 4; kapp_redraw(ui->app); }
    int maxs = X.content_h - G.body.h;
    if (maxs < 0) maxs = 0;
    if (X.scroll > maxs) X.scroll = maxs;
    if (X.scroll < 0) X.scroll = 0;
    int cy = (my < G.body.y ? G.body.y : my >= G.body.y + G.body.h ? G.body.y + G.body.h - 1 : my) - G.body.y + X.scroll;
    int cx = mx - G.body.x;
    int x0 = V.bx0 < cx ? V.bx0 : cx, x1 = V.bx0 < cx ? cx : V.bx0;
    int y0 = V.by0 < cy ? V.by0 : cy, y1 = V.by0 < cy ? cy : V.by0;
    int n = X.m.nshown;
    for (int i = 0; i < n; i++) {
        bool in = false;
        if (X.view_mode == 0) {
            int ry0 = i * L_ROW_H, ry1 = ry0 + L_ROW_H - 1;
            in = ry1 >= y0 && ry0 <= y1 && x0 < 24 + kc_text_width(X.m.shown[i]->name) + 8 && x1 >= 0;
        } else {
            int row = i / G.icols, col = i % G.icols;
            int cxs = 4 + col * L_CELL_W, cys = row * L_CELL_H;
            if (cys + L_CELL_H < y0 || cys > y1) { in = false; }
            else {
                KRect c = kr(G.body.x + cxs, G.body.y + cys, L_CELL_W, L_CELL_H);
                KRect ib = icon_box(c), lb = label_box(i, c);
                KRect band = kr(G.body.x + x0, G.body.y + y0, x1 - x0 + 1, y1 - y0 + 1);
                in = !kr_empty(kr_intersect(ib, band)) || !kr_empty(kr_intersect(lb, band));
            }
        }
        bool base = V.band_ctrl && V.base && i < V.base_n && V.base[i];
        X.m.shown[i]->sel = in || base;
    }
}

static KRect rename_rect(void) {
    if (X.view_mode == 0) {
        KRect rr = row_rect(V.ren_idx);
        return kr(rr.x + 22, rr.y - 2, G.name_w - 26 > 80 ? G.name_w - 26 : 80, 24);
    }
    KRect c = cell_rect(V.ren_idx);
    return kr(c.x + 1, c.y + 42, c.w - 2, 24);
}

static void on_press(KUI *ui, int hit) {
    bool ctrl = ui->mods & MOD_CTRL, shift = ui->mods & MOD_SHIFT;
    ui->focus = VIEW_ID;
    X.view_focus = true;
    V.ta[0] = 0;
    if (hit >= 0) {
        bool dbl = ui->double_click && hit == V.last_click_idx;
        V.last_click_idx = hit;
        if (dbl) {
            V.pending = false;
            ex_open_entry(X.m.shown[hit]);
            return;
        }
        if (ctrl) {                                                     /* adds to the selection; an already selected one is toggled off on release unless dragged */
            Entry *e = X.m.shown[hit];
            V.ctrl_off = e->sel;
            e->sel = true;
            X.anchor = X.cursor = hit;
            V.pending = true;
            V.defer = false;
            V.idx = hit;
            V.px = ui->mx;
            V.py = ui->my;
            return;
        }
        if (shift) {
            if (X.anchor < 0) X.anchor = hit;
            model_select_range(&X.m, X.anchor, hit, true, true);
            X.cursor = hit;
            V.pending = false;
            return;
        }
        Entry *e = X.m.shown[hit];
        V.defer = false;
        if (!e->sel) collapse_to(hit);
        else {
            V.defer = model_sel_count(&X.m, NULL) > 1;
            X.anchor = X.cursor = hit;
        }
        V.pending = true;
        V.idx = hit;
        V.px = ui->mx;
        V.py = ui->my;
        return;
    }
    V.last_click_idx = -1;
    V.band = true;
    V.band_ctrl = ctrl || shift;
    V.bx0 = ui->mx - G.body.x;
    V.by0 = ui->my - G.body.y + X.scroll;
    free(V.base);
    V.base = NULL;
    if (V.band_ctrl) {
        V.base_n = X.m.nshown;
        V.base = malloc((size_t)(V.base_n ? V.base_n : 1));
        if (V.base) for (int i = 0; i < V.base_n; i++) V.base[i] = X.m.shown[i]->sel;
    } else {
        model_select_none(&X.m);
        X.cursor = X.anchor = -1;
    }
}

/* ---------------------------------------------------------------- drawing */

static uint32_t name_color(const KTheme *th, const Entry *e, bool sel) {
    if (sel) return th->select_text;
    if (e->cut || e->hidden) return th->text_dim;
    if (e->kind == EK_LINK) return e->broken ? 0xB5122B : 0x2A5CAA;
    return th->field_text;
}

static void draw_sort_arrow(KCanvas *cv, int x, int y, bool desc, uint32_t c) {
    for (int i = 0; i < 4; i++) {
        int w = desc ? 7 - i * 2 : 1 + i * 2;
        kc_hline(cv, x + 3 - w / 2, y + i, w, c);
    }
}

static void draw_header(KUI *ui, bool inside) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    struct { int col; int x, w; const char *title; } cells[6];
    int nc = 0;
    cells[nc].col = COL_NAME; cells[nc].x = G.body.x; cells[nc].w = G.name_w; cells[nc].title = "Name"; nc++;
    for (int i = 0; i < G.nx; i++) {
        cells[nc].col = G.cl[i].col; cells[nc].x = G.cl[i].x; cells[nc].w = G.cl[i].w;
        for (int k = 0; k < 5; k++) if (k_cols[k].col == G.cl[i].col) cells[nc].title = k_cols[k].title;
        nc++;
    }
    cells[nc - 1].w += G.hdr.x + G.hdr.w - (cells[nc - 1].x + cells[nc - 1].w);       /* the last cell also covers the scrollbar column */
    static int s_press = -1;                                             /* a click needs its press and its release on the same heading */
    int pressed_on = s_press;
    if (ui->released[0]) s_press = -1;
    int sort_col = -1;
    const char *sort_title = "";
    for (int i = 0; i < nc; i++) {
        KRect c = kr(cells[i].x, G.hdr.y, cells[i].w, L_HEAD_H);
        bool hv = inside && kr_contains(c, ui->mx, ui->my) && !V.dragging && !V.band;
        bool down = hv && (ui->buttons & 1) && s_press == i;
        kc_panel(cv, c, down ? KB_SUNKEN : KB_RAISED, hv ? kc_shade(th->button_bg, 104) : th->button_bg);
        KRect tr = kr(c.x + 6 + (down ? 1 : 0), c.y + (down ? 1 : 0), c.w - 12, c.h);
        kc_text_in(cv, tr, cells[i].title, th->text, KA_LEFT);
        if (X.m.sort_col == cells[i].col && c.w > 40) draw_sort_arrow(cv, c.x + c.w - 14, c.y + 8, X.m.sort_desc, th->text);
        if (hv && ui->pressed[0]) s_press = i;
        if (hv && ui->released[0] && (pressed_on == i || ui->pressed[0])) { sort_col = cells[i].col; sort_title = cells[i].title; }
    }
    if (sort_col >= 0) {
        ex_trace("header click %s", sort_title);
        ex_set_sort(sort_col);
    }
}

static void draw_list(KUI *ui, bool inside, bool focus) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect old = cv->clip;
    kc_set_clip(cv, G.body);
    int n = X.m.nshown;
    int first = X.scroll / L_ROW_H;
    int hover = inside && !V.dragging ? hit_at(ui->mx, ui->my) : -1;
    for (int i = first; i < n; i++) {
        KRect rr = row_rect(i);
        if (rr.y >= G.body.y + G.body.h) break;
        Entry *e = X.m.shown[i];
        bool sel = e->sel;
        uint32_t bg = (i & 1) ? kc_shade(th->field_bg, 97) : th->field_bg;
        if (sel) bg = focus ? th->select_bg : kc_mix(th->select_bg, th->field_bg, 120);
        else if (i == hover) bg = kc_mix(th->field_bg, th->accent_light, 40);
        if (V.dragging && i == V.drop_idx) bg = kc_mix(th->field_bg, th->accent_light, 130);
        kc_fill(cv, rr, bg);
        draw_entry_icon(cv, e, rr.x + 3, rr.y + 2, 16, bg, e->cut ? 150 : e->hidden ? 100 : 0);
        uint32_t fg = name_color(th, e, sel);
        bool renaming_here = V.ren && V.ren_idx == i;
        if (!renaming_here) kc_text_in(cv, kr(rr.x + 24, rr.y, G.name_w - 28, rr.h), e->name, fg, KA_LEFT);
        uint32_t cfg = sel ? th->select_text : e->cut ? th->text_dim : th->text;
        for (int c = 0; c < G.nx; c++) {
            char buf[64];
            KRect cr = kr(G.cl[c].x + 6, rr.y, G.cl[c].w - 12, rr.h);
            KAlign al = KA_LEFT;
            buf[0] = 0;
            switch (G.cl[c].col) {
            case COL_SIZE:
                if (!e->is_dir && e->kind != EK_OTHER) fmt_size(buf, sizeof buf, e->size);
                al = KA_RIGHT;
                break;
            case COL_MTIME: fmt_time(buf, sizeof buf, e->mtime); break;
            case COL_TYPE: { char tmp[24]; snprintf(buf, sizeof buf, "%s", type_name(e, tmp, sizeof tmp)); break; }
            case COL_PERM: fmt_mode(buf, e->mode); break;
            case COL_OWNER: snprintf(buf, sizeof buf, "%s", uid_name(e->uid)); break;
            default: break;
            }
            if (buf[0]) kc_text_in(cv, cr, buf, cfg, al);
        }
        if (V.dragging && i == V.drop_idx) kc_outline(cv, rr, th->accent_dark);
        else if (i == X.cursor && focus && !renaming_here) kc_outline(cv, kr_inset(rr, 1), sel ? 0xFFE9A8 : th->accent);
    }
    cv->clip = old;
}

static void draw_icons(KUI *ui, bool inside, bool focus) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    KRect old = cv->clip;
    kc_set_clip(cv, G.body);
    int n = X.m.nshown;
    int hover = inside && !V.dragging ? hit_at(ui->mx, ui->my) : -1;
    int first = (X.scroll / L_CELL_H) * G.icols;
    for (int i = first; i < n; i++) {
        KRect c = cell_rect(i);
        if (c.y >= G.body.y + G.body.h) break;
        Entry *e = X.m.shown[i];
        bool sel = e->sel;
        KRect ib = icon_box(c), lb = label_box(i, c);
        uint32_t bg = th->field_bg;
        if (sel) kc_fill_alpha(cv, ib, th->select_bg, focus ? 90 : 50);
        else if (i == hover) kc_fill_alpha(cv, ib, th->accent_light, 60);
        if (V.dragging && i == V.drop_idx) { kc_fill_alpha(cv, kr_union(ib, lb), th->accent_light, 120); kc_outline(cv, kr_union(ib, lb), th->accent_dark); }
        draw_entry_icon(cv, e, c.x + (c.w - 32) / 2, c.y + 6, 32, sel ? kc_mix(bg, th->select_bg, 90) : bg, e->cut ? 150 : e->hidden ? 100 : 0);
        bool renaming_here = V.ren && V.ren_idx == i;
        if (!renaming_here) {
            char l1[EX_NAME], l2[EX_NAME];
            wrap_label(e->name, l1, l2, sizeof l1);
            if (sel) kc_fill(cv, lb, focus ? th->select_bg : kc_mix(th->select_bg, th->field_bg, 120));
            uint32_t fg = name_color(th, e, sel);
            kc_text(cv, c.x + (c.w - kc_text_width(l1)) / 2, c.y + 45, l1, fg);
            if (l2[0]) kc_text(cv, c.x + (c.w - kc_text_width(l2)) / 2, c.y + 61, l2, fg);
            if (i == X.cursor && focus) kc_outline(cv, lb, sel ? 0xFFE9A8 : th->accent);
        }
    }
    cv->clip = old;
}

static void draw_band(KUI *ui) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    int mx = ui->mx < G.body.x ? G.body.x : ui->mx >= G.body.x + G.body.w ? G.body.x + G.body.w - 1 : ui->mx;
    int my = ui->my < G.body.y ? G.body.y : ui->my >= G.body.y + G.body.h ? G.body.y + G.body.h - 1 : ui->my;
    int ax = G.body.x + V.bx0, ay = G.body.y + V.by0 - X.scroll;
    int x0 = ax < mx ? ax : mx, x1 = ax < mx ? mx : ax, y0 = ay < my ? ay : my, y1 = ay < my ? my : ay;
    KRect r = kr(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    KRect old = cv->clip;
    kc_set_clip(cv, G.body);
    kc_fill_alpha(cv, r, th->select_bg, 50);
    kc_outline(cv, r, th->select_bg);
    cv->clip = old;
}

void view_draw(KUI *ui, KRect r, bool blocked) {
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    layout(r);
    X.r_view = r;
    X.r_body = G.body;
    bool inside = !blocked && ui->mouse_inside;
    bool focus = ui->has_focus && ui->focus == VIEW_ID;
    kc_panel(cv, r, KB_SUNKEN, th->field_bg);

    if (inside && ui->wheel && kr_contains(r, ui->mx, ui->my)) {
        X.scroll -= ui->wheel * 60;
        int maxs = X.content_h - G.body.h;
        if (maxs < 0) maxs = 0;
        if (X.scroll > maxs) X.scroll = maxs;
        if (X.scroll < 0) X.scroll = 0;
    }
    if (G.bar) {
        bool save = ui->input_blocked;
        if (blocked) ui->input_blocked = true;
        k_scrollbar(ui, G.sbar, &X.scroll, X.content_h, G.body.h);
        ui->input_blocked = save;
    }

    /* pointer */
    bool over_body = inside && kr_contains(G.body, ui->mx, ui->my);
    bool committed = false;
    if (V.ren && V.ren_idx >= 0 && V.ren_idx < X.m.nshown && inside && (ui->pressed[0] || ui->pressed[1]) &&
        !kr_contains(rename_rect(), ui->mx, ui->my)) {
        V.ren = false;                                                  /* a click elsewhere keeps what was typed */
        ui->focus = VIEW_ID;
        if (strcmp(V.ren_orig, V.ren_buf)) {
            ex_rename_commit(V.ren_orig, V.ren_buf);
            committed = true;                                           /* the folder is being read again: that click was only for the rename */
        }
    }
    if (committed) over_body = false;
    if (over_body && ui->pressed[0] && !V.ren) on_press(ui, hit_at(ui->mx, ui->my));
    if (over_body && ui->pressed[1] && !V.ren) {
        int hit = hit_at(ui->mx, ui->my);
        ui->focus = VIEW_ID;
        if (hit >= 0 && !X.m.shown[hit]->sel) collapse_to(hit);
        else if (hit >= 0) X.cursor = hit;
        else { model_select_none(&X.m); X.cursor = X.anchor = -1; }
        view_context(ui, ui->mx, ui->my, hit);
    }
    if (!blocked) {
        if (V.pending && (ui->buttons & 1) && !V.dragging) {
            if (abs(ui->mx - V.px) > 4 || abs(ui->my - V.py) > 4) {
                V.dragging = true;
                V.pending = false;
                V.defer = false;
                ex_trace("drag start");
            }
        }
        if (V.dragging) { update_drop(ui); ui->cursor_shape = FC_MOVE; }
        if (V.band && (ui->buttons & 1)) band_update(ui);
        if (ui->released[0]) {
            if (V.dragging) {
                do_drop(ui);
                V.dragging = false;
                g_drop_place = -1;
            } else if (V.pending && V.defer) collapse_to(V.idx);
            else if (V.pending && V.ctrl_off && V.idx >= 0 && V.idx < X.m.nshown) X.m.shown[V.idx]->sel = false;
            V.ctrl_off = false;
            if (V.band) { V.band = false; free(V.base); V.base = NULL; ex_trace("band done"); }
            V.pending = false;
        }
        if (!(ui->buttons & 1) && !ui->released[0] && !ui->pressed[0]) {          /* a gesture whose release was never seen */
            if (V.band) { V.band = false; free(V.base); V.base = NULL; }
            V.pending = V.dragging = V.ctrl_off = false;
            g_drop_place = -1;
        }
    }

    /* contents */
    if (X.view_mode == 0) draw_header(ui, inside);
    if (X.m.nshown == 0) {
        const char *msg = X.m.scanning ? "Reading the folder..." : X.in_netherworld ? "The Netherworld is empty. Yuyuko is hungry." : "Nothing here but the wind (this folder is empty).";
        KRect old = cv->clip;
        kc_set_clip(cv, G.body);
        kc_text_in(cv, kr(G.body.x, G.body.y + 20, G.body.w, 24), msg, th->text_dim, KA_CENTER);
        cv->clip = old;
    } else if (X.view_mode == 0) draw_list(ui, inside, focus);
    else draw_icons(ui, inside, focus);
    if (V.band && (ui->buttons & 1)) draw_band(ui);

    /* inline rename */
    if (V.ren && V.ren_idx >= 0 && V.ren_idx < X.m.nshown) {
        KRect fr = rename_rect();
        KRect old = cv->clip;
        kc_set_clip(cv, G.body);
        if (!V.ren_init) {
            k_focus_textfield(ui, V.ren_buf);
            const Entry *e = X.m.shown[V.ren_idx];
            const char *dot = strrchr(V.ren_buf, '.');
            int stem = (!e->is_dir && dot && dot != V.ren_buf) ? (int)(dot - V.ren_buf) : (int)strlen(V.ren_buf);
            ui->sel_anchor = 0;
            ui->caret = stem;
            V.ren_init = true;
        }
        bool enter = k_textfield(ui, fr, V.ren_buf, sizeof V.ren_buf);
        cv->clip = old;
        if (enter) {
            V.ren = false;
            ui->focus = VIEW_ID;
            ex_rename_commit(V.ren_orig, V.ren_buf);
        }
    }
}

void view_ghost(KUI *ui) {
    if (!V.dragging) return;
    KCanvas *cv = &ui->cv;
    const KTheme *th = ui->theme;
    int n = model_sel_count(&X.m, NULL);
    const Entry *first = NULL;
    for (int i = 0; i < X.m.nshown && !first; i++) if (X.m.shown[i]->sel) first = X.m.shown[i];
    int x = ui->mx + 14, y = ui->my + 10;
    KRect old = cv->clip;
    kc_set_clip(cv, kr(0, 0, cv->w, cv->h));
    if (first) draw_entry_icon(cv, first, x, y, 24, th->window_bg, 0);
    if (n > 1) {
        char c[16];
        snprintf(c, sizeof c, "%d", n);
        int w = kc_text_width(c) + 8;
        kc_fill(cv, kr(x + 24 - w / 2, y - 6, w, 16), th->accent);
        kc_text(cv, x + 24 - w / 2 + 4, y - 6, c, 0xFFFFFF);
    }
    const char *how = NULL;
    char buf[EX_PATH + 40];
    if (V.drop_valid) {
        if (!strcmp(V.drop_path, NETHERWORLD)) how = "Send to the Netherworld";
        else {
            bool copy = (ui->mods & MOD_CTRL) != 0;
            snprintf(buf, sizeof buf, "%s into %s", copy ? "Copy" : (ui->mods & MOD_SHIFT) ? "Move" : "Drop", path_base(V.drop_path));
            how = buf;
        }
    }
    if (how) {
        int w = kc_text_width(how) + 10;
        int tx = x + 28, ty = y + 6;
        if (tx + w > cv->w) tx = cv->w - w;
        kc_panel(cv, kr(tx, ty, w, 20), KB_FRAME, 0xFFF8D0);
        kc_text(cv, tx + 5, ty + 2, how, th->text);
    }
    cv->clip = old;
}
