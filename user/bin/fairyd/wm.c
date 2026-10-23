/* Window management: geometry, stacking, focus, hit testing, dragging and the keyboard shortcuts. */
#include "fairyd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/* ---------------------------------------------------------------- geometry */

int layer_of(const Win *w) {
    if (w->flags & FW_DESKTOP) return LAYER_DESKTOP;
    if (w->flags & FW_POPUP) return LAYER_POPUP;
    if (w->flags & FW_FULLSCREEN) return LAYER_FULL;
    if (w->flags & FW_TOP) return LAYER_TOP;
    if (w->flags & FW_PANEL) return LAYER_PANEL;
    return LAYER_NORMAL;
}

bool win_has_decor(const Win *w) {
    return !(w->flags & (FW_NO_DECOR | FW_DESKTOP | FW_PANEL | FW_POPUP | FW_FULLSCREEN));
}

static int bd(const Win *w) { return w->state == FS_MAXIMIZED ? 0 : BORDER; }

Rect win_outer(const Win *w) {
    Rect r = { w->x, w->y, w->cw, w->ch };
    if (win_has_decor(w)) {
        int b = bd(w);
        r.w = w->cw + 2 * b;
        r.h = w->ch + b + TITLE_H + b;
    }
    return r;
}

Rect win_content(const Win *w) {
    if (!win_has_decor(w)) return (Rect){ w->x, w->y, w->cw, w->ch };
    int b = bd(w);
    return (Rect){ w->x + b, w->y + b + TITLE_H, w->cw, w->ch };
}

bool win_button(const Win *w, int zone, Rect *out) {
    if (!win_has_decor(w)) return false;
    bool has_max = (w->flags & FW_RESIZABLE) && !(w->flags & FW_DIALOG);
    bool has_min = !(w->flags & FW_DIALOG);
    static const int order[3] = { Z_CLOSE, Z_MAX, Z_MIN };
    bool has[3] = { true, has_max, has_min };
    Rect o = win_outer(w);
    int b = bd(w);
    int x = o.x + o.w - b - 3;
    for (int i = 0; i < 3; i++) {
        if (!has[i]) continue;
        x -= 20;
        if (order[i] == zone) { *out = (Rect){ x, o.y + b + 2, 18, 18 }; return true; }
        x -= 2;
    }
    return false;
}

static bool inside(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

int zone_at(const Win *w, int px, int py) {
    Rect o = win_outer(w);
    if (!inside(o, px, py)) return Z_NONE;
    if (!win_has_decor(w)) return Z_CONTENT;
    if (inside(win_content(w), px, py)) return Z_CONTENT;
    static const int bz[3] = { Z_CLOSE, Z_MAX, Z_MIN };
    for (int i = 0; i < 3; i++) {
        Rect br;
        if (win_button(w, bz[i], &br) && inside(br, px, py)) return bz[i];
    }
    bool resizable = (w->flags & FW_RESIZABLE) && w->state == FS_NORMAL;
    if (resizable) {
        bool left = px < o.x + BORDER, right = px >= o.x + o.w - BORDER, top = py < o.y + BORDER, bottom = py >= o.y + o.h - BORDER;
        bool near_l = px < o.x + 14, near_r = px >= o.x + o.w - 14, near_t = py < o.y + 14, near_b = py >= o.y + o.h - 14;
        if ((top && near_l) || (left && near_t)) return Z_NW;
        if ((top && near_r) || (right && near_t)) return Z_NE;
        if ((bottom && near_l) || (left && near_b)) return Z_SW;
        if ((bottom && near_r) || (right && near_b)) return Z_SE;
        if (top) return Z_N;
        if (bottom) return Z_S;
        if (left) return Z_W;
        if (right) return Z_E;
    }
    Rect title = { o.x + bd(w), o.y + bd(w), o.w - 2 * bd(w), TITLE_H };
    if (inside(title, px, py)) return Z_TITLE;
    return Z_NONE;
}

Win *win_by_id(uint32_t id) {
    for (int i = 0; i < S.nwins; i++) if (S.wins[i]->id == id) return S.wins[i];
    return NULL;
}

Win *win_at(int x, int y, int *zone) {
    for (int i = S.nwins - 1; i >= 0; i--) {
        Win *w = S.wins[i];
        if (!w->visible) continue;
        Rect o = win_outer(w);
        if (!inside(o, x, y)) continue;
        *zone = zone_at(w, x, y);
        return w;
    }
    *zone = Z_NONE;
    return NULL;
}

/* ---------------------------------------------------------------- stacking */

static int index_of(const Win *w) {
    for (int i = 0; i < S.nwins; i++) if (S.wins[i] == w) return i;
    return -1;
}

void win_insert(Win *w) {
    if (S.nwins >= MAX_WINS) return;
    int L = layer_of(w), at = S.nwins;
    for (int i = 0; i < S.nwins; i++) if (layer_of(S.wins[i]) > L) { at = i; break; }
    memmove(&S.wins[at + 1], &S.wins[at], (size_t)(S.nwins - at) * sizeof(Win *));
    S.wins[at] = w;
    S.nwins++;
}

void win_raise(Win *w) {
    int i = index_of(w);
    if (i < 0) return;
    int top = S.nwins - 1;
    while (top > i && layer_of(S.wins[top]) > layer_of(w)) top--;
    if (top == i) return;
    memmove(&S.wins[i], &S.wins[i + 1], (size_t)(top - i) * sizeof(Win *));
    S.wins[top] = w;
    comp_damage_win(w);
    server_mark_list_dirty();
}

void work_area_recompute(void) {
    int x = 0, y = 0, w = S.screen_w, h = S.screen_h;
    for (int i = 0; i < S.nwins; i++) {
        Win *p = S.wins[i];
        if (!(p->flags & FW_PANEL) || p->strut_size <= 0 || !p->visible) continue;
        switch (p->strut_edge) {
        case 0: y += p->strut_size; h -= p->strut_size; break;
        case 1: h -= p->strut_size; break;
        case 2: x += p->strut_size; w -= p->strut_size; break;
        case 3: w -= p->strut_size; break;
        }
    }
    S.work_x = x; S.work_y = y; S.work_w = w; S.work_h = h;
    for (int i = 0; i < S.nwins; i++) {                         /* keep maximized windows inside the work area */
        Win *m = S.wins[i];
        if (m->state == FS_MAXIMIZED) win_set_geometry(m, x, y, w, h - TITLE_H);
    }
}

/* ---------------------------------------------------------------- focus */

static bool focusable(const Win *w) {
    return w->visible && !(w->flags & (FW_NO_FOCUS | FW_POPUP | FW_PANEL));
}

static void send_focus(Win *w, bool gained) {
    FeFocus f = { gained ? 1u : 0u };
    send_msg(w->client, FE_FOCUS, w->id, 0, &f, sizeof f);
}

void win_focus(Win *w) {
    if (w == S.focus) return;
    Win *old = S.focus;
    S.focus = w;
    if (old) { send_focus(old, false); comp_damage_win(old); }
    if (w) { send_focus(w, true); comp_damage_win(w); }
    server_mark_list_dirty();
}

static void focus_next_available(void) {
    for (int i = S.nwins - 1; i >= 0; i--) {
        Win *w = S.wins[i];
        if (focusable(w) && !(w->flags & FW_DESKTOP)) { win_focus(w); return; }
    }
    for (int i = S.nwins - 1; i >= 0; i--)
        if (focusable(S.wins[i])) { win_focus(S.wins[i]); return; }
    win_focus(NULL);
}

void wm_focus_fallback(void) { focus_next_available(); }

/* ---------------------------------------------------------------- state changes */

static void send_configure(Win *w) {
    FeConfigure c = { w->cw, w->ch, w->state, S.focus == w };
    send_msg(w->client, FE_CONFIGURE, w->id, 0, &c, sizeof c);
}

void win_set_geometry(Win *w, int x, int y, int cw, int ch) {
    bool resized = cw != w->cw || ch != w->ch;
    comp_damage_win(w);
    w->x = x; w->y = y; w->cw = cw; w->ch = ch;
    comp_damage_win(w);
    if (resized) send_configure(w);
    server_mark_list_dirty();
}

void win_set_state(Win *w, unsigned state) {
    if (state == w->state && !(state == FS_NORMAL && !w->visible)) return;
    unsigned old = w->state;
    if (state == FS_MAXIMIZED) {
        if (!(w->flags & FW_RESIZABLE) || (w->flags & FW_DIALOG)) return;
        if (old == FS_NORMAL) { w->rx = w->x; w->ry = w->y; w->rcw = w->cw; w->rch = w->ch; }
        comp_damage_win(w);
        w->state = FS_MAXIMIZED;
        w->visible = true;
        w->x = S.work_x; w->y = S.work_y;
        w->cw = S.work_w;
        w->ch = S.work_h - TITLE_H;
        comp_damage_win(w);
        send_configure(w);
        win_raise(w);
        win_focus(w);
    } else if (state == FS_MINIMIZED) {
        if (w->flags & FW_DIALOG) return;
        w->maximized_before_min = old == FS_MAXIMIZED;
        comp_damage_win(w);
        w->visible = false;
        if (S.focus == w) { Win *prev = w; S.focus = NULL; send_focus(prev, false); focus_next_available(); }
    } else {                                                  /* restore */
        if (!w->visible) {
            w->visible = true;
            comp_damage_win(w);
        } else if (old == FS_MAXIMIZED) {
            comp_damage_win(w);
            w->state = FS_NORMAL;
            w->x = w->rx; w->y = w->ry; w->cw = w->rcw; w->ch = w->rch;
            comp_damage_win(w);
            send_configure(w);
        }
        win_raise(w);
        win_focus(w);
    }
    server_mark_list_dirty();
}

void wm_request_close(Win *w) {
    if (w->flags & FW_NO_CLOSE) return;
    send_msg(w->client, FE_CLOSE, w->id, 0, NULL, 0);
}

void wm_close_popups(Win *except) {
    for (int i = 0; i < S.nwins; i++) {
        Win *p = S.wins[i];
        if ((p->flags & FW_POPUP) && p != except) send_msg(p->client, FE_CLOSE, p->id, 0, NULL, 0);
    }
}

void wm_place_new(Win *w, int req_x, int req_y) {
    Rect o = win_outer(w);
    static int cascade;
    int x, y;
    if (req_x >= 0 && req_y >= 0) { x = req_x; y = req_y; }
    else if (w->flags & FW_CENTER) {
        x = S.work_x + (S.work_w - o.w) / 2;
        y = S.work_y + (S.work_h - o.h) / 2;
    } else if ((w->flags & FW_DIALOG) && w->parent && win_by_id(w->parent)) {
        Rect po = win_outer(win_by_id(w->parent));
        x = po.x + (po.w - o.w) / 2;
        y = po.y + (po.h - o.h) / 2;
    } else if (!win_has_decor(w)) { x = 0; y = 0; }
    else {
        x = S.work_x + 40 + (cascade * 30) % 240;
        y = S.work_y + 30 + (cascade * 30) % 180;
        cascade++;
    }
    if (win_has_decor(w)) {
        if (x + o.w > S.work_x + S.work_w) x = S.work_x + S.work_w - o.w;
        if (y + o.h > S.work_y + S.work_h) y = S.work_y + S.work_h - o.h;
        if (x < S.work_x) x = S.work_x;
        if (y < S.work_y) y = S.work_y;
    }
    w->x = x;
    w->y = y;
}

/* ---------------------------------------------------------------- pointer */

static unsigned cursor_for_zone(int zone) {
    switch (zone) {
    case Z_N: case Z_S: return FC_RESIZE_V;
    case Z_E: case Z_W: return FC_RESIZE_H;
    case Z_NW: case Z_SE: return FC_RESIZE_NWSE;
    case Z_NE: case Z_SW: return FC_RESIZE_NESW;
    default: return FC_ARROW;
    }
}

static void send_pointer(Win *w, int kind, int button, int x, int y, int wheel) {
    Rect c = win_content(w);
    FePointer p;
    memset(&p, 0, sizeof p);
    p.x = x - c.x;
    p.y = y - c.y;
    p.kind = (uint8_t)kind;
    p.button = (uint8_t)button;
    p.buttons = S.buttons;
    p.mods = S.mods;
    p.wheel = wheel;
    send_msg(w->client, FE_POINTER, w->id, 0, &p, sizeof p);
}

static void drag_update(int px, int py) {
    Win *w = S.drag_win;
    if (!w) return;
    if (S.drag_mode == 1) {
        int nx = px - S.drag_ox, ny = py - S.drag_oy;
        if (ny < 0) ny = 0;
        if (ny > S.screen_h - 30) ny = S.screen_h - 30;
        Rect o = win_outer(w);
        if (nx + o.w < 60) nx = 60 - o.w;
        if (nx > S.screen_w - 60) nx = S.screen_w - 60;
        if (nx != w->x || ny != w->y) {
            comp_damage_win(w);
            w->x = nx;
            w->y = ny;
            comp_damage_win(w);
        }
        return;
    }
    int dx = px - S.drag_px, dy = py - S.drag_py;
    int nx = S.drag_start.x, ny = S.drag_start.y, nw = S.drag_start.w, nh = S.drag_start.h;
    int z = S.drag_zone;
    if (z == Z_E || z == Z_NE || z == Z_SE) nw += dx;
    if (z == Z_W || z == Z_NW || z == Z_SW) { nx += dx; nw -= dx; }
    if (z == Z_S || z == Z_SE || z == Z_SW) nh += dy;
    if (z == Z_N || z == Z_NE || z == Z_NW) { ny += dy; nh -= dy; }
    int minw = (w->minw > 0 ? w->minw : 120) + 2 * BORDER, minh = (w->minh > 0 ? w->minh : 40) + DECOR_T + BORDER;
    if (nw < minw) { if (z == Z_W || z == Z_NW || z == Z_SW) nx -= minw - nw; nw = minw; }
    if (nh < minh) { if (z == Z_N || z == Z_NE || z == Z_NW) ny -= minh - nh; nh = minh; }
    if (ny < 0) { nh += ny; ny = 0; }
    int cw = nw - 2 * BORDER, ch = nh - DECOR_T - BORDER;
    if (cw != w->cw || ch != w->ch || nx != w->x || ny != w->y) win_set_geometry(w, nx, ny, cw, ch);
}

static void end_drag(void) {
    S.drag_mode = 0;
    S.drag_win = NULL;
}

void wm_pointer_event(int x, int y, uint8_t buttons_now, int changed, bool pressed, int wheel) {
    S.buttons = buttons_now;
    if (S.drag_mode) {
        if (changed == 1 && !pressed) end_drag();
        else drag_update(x, y);
        return;
    }
    int zone = Z_NONE;
    Win *target;
    if (S.grab && (buttons_now || (changed && !pressed))) { target = S.grab; zone = Z_CONTENT; }
    else target = win_at(x, y, &zone);

    if (target != S.hover) {
        if (S.hover && win_by_id(S.hover->id) == S.hover) send_pointer(S.hover, FP_LEAVE, 0, x, y, 0);
        S.hover = target;
        if (target && zone == Z_CONTENT) send_pointer(target, FP_ENTER, 0, x, y, 0);
    }
    unsigned shape = target && zone == Z_CONTENT ? target->cursor : cursor_for_zone(zone);
    if (S.grab && target == S.grab) shape = target->cursor;
    cursor_set_shape(shape);

    if (changed && pressed) {
        wm_close_popups(target);
        if (!target) return;
        bool alt_drag = changed == 1 && (S.mods & MOD_ALT) && win_has_decor(target) && target->state == FS_NORMAL;
        if (zone == Z_CONTENT && !alt_drag) {
            if (focusable(target) && !(target->flags & FW_DESKTOP)) { win_raise(target); win_focus(target); }
            else if ((target->flags & FW_DESKTOP) && !(target->flags & FW_NO_FOCUS)) win_focus(target);
            S.grab = target;
            send_pointer(target, FP_PRESS, changed, x, y, 0);
        } else if (alt_drag || zone == Z_TITLE) {
            if (changed != 1 && !alt_drag) return;
            win_raise(target);
            win_focus(target);
            if (zone == Z_TITLE && S.now_ms - S.last_title_click < 400 && S.last_title_win == target &&
                (target->flags & FW_RESIZABLE) && !(target->flags & FW_DIALOG)) {
                S.last_title_click = 0;
                win_set_state(target, target->state == FS_MAXIMIZED ? FS_NORMAL : FS_MAXIMIZED);
                return;
            }
            S.last_title_click = S.now_ms;
            S.last_title_win = target;
            if (target->state != FS_NORMAL) return;
            S.drag_mode = 1;
            S.drag_win = target;
            S.drag_ox = x - target->x;
            S.drag_oy = y - target->y;
        } else if (zone >= Z_N && zone <= Z_SW && changed == 1) {
            win_raise(target);
            win_focus(target);
            S.drag_mode = 2;
            S.drag_win = target;
            S.drag_zone = zone;
            S.drag_start = win_outer(target);
            S.drag_px = x;
            S.drag_py = y;
        } else if (changed == 1 && (zone == Z_CLOSE || zone == Z_MAX || zone == Z_MIN)) {
            win_raise(target);
            win_focus(target);
            if (zone == Z_CLOSE) wm_request_close(target);
            else if (zone == Z_MAX) win_set_state(target, target->state == FS_MAXIMIZED ? FS_NORMAL : FS_MAXIMIZED);
            else win_set_state(target, FS_MINIMIZED);
        } else if (zone == Z_NONE || zone == Z_TITLE) {
            win_raise(target);
            win_focus(target);
        }
        return;
    }
    if (changed && !pressed) {
        if (S.grab) send_pointer(S.grab, FP_RELEASE, changed, x, y, 0);
        if (!buttons_now) S.grab = NULL;
        return;
    }
    if (wheel) {
        Win *wt = target && zone == Z_CONTENT ? target : NULL;
        if (wt) send_pointer(wt, FP_WHEEL, 0, x, y, wheel);
        return;
    }
    if (target && zone == Z_CONTENT) {
        if (S.now_ms - target->last_pointer_ms >= 8 || S.grab) {
            target->last_pointer_ms = S.now_ms;
            send_pointer(target, FP_MOVE, 0, x, y, 0);
        }
    }
}

/* ---------------------------------------------------------------- keyboard */

void send_key_to_focus(uint8_t code, uint8_t mods, uint8_t down, uint32_t ch) {
    if (!S.focus) return;
    FeKey k = { code, mods, down, 0, ch };
    send_msg(S.focus->client, FE_KEY, S.focus->id, 0, &k, sizeof k);
}

static bool switcher_eligible(const Win *w) {
    return !(w->flags & (FW_SKIP_TASKS | FW_PANEL | FW_DESKTOP | FW_POPUP));
}

static Rect switcher_rect(void) {
    return (Rect){ (S.screen_w - 360) / 2 - 4, (S.screen_h - (40 + S.switch_n * 26)) / 2 - 4, 376, 54 + S.switch_n * 26 };
}

void wm_switcher_cycle(bool backwards) {
    if (!S.switching) {
        S.switch_n = 0;
        for (int i = S.nwins - 1; i >= 0; i--) if (switcher_eligible(S.wins[i])) S.switch_list[S.switch_n++] = S.wins[i];
        if (S.switch_n < 2) return;
        S.switching = true;
        S.switch_index = 0;
    }
    S.switch_index = (S.switch_index + (backwards ? S.switch_n - 1 : 1)) % S.switch_n;
    comp_damage(switcher_rect());
}

void wm_switcher_commit(void) {
    if (!S.switching) return;
    S.switching = false;
    Win *w = S.switch_index < S.switch_n ? S.switch_list[S.switch_index] : NULL;
    comp_damage(switcher_rect());
    if (w && win_by_id(w->id) == w) {
        if (!w->visible) win_set_state(w, FS_NORMAL);
        else { win_raise(w); win_focus(w); }
    }
}

static void spawn(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(path, path, (char *)NULL);
        _exit(127);
    }
}

/* Returns true when the key was a shortcut and must not be passed on to the focused window. */
bool wm_key_shortcut(uint8_t code, uint8_t down) {
    bool alt = S.mods & MOD_ALT, ctrl = S.mods & MOD_CTRL;
    if (S.switching && (code == KEY_LALT || code == KEY_RALT) && !down) { wm_switcher_commit(); return false; }
    if (alt && code == KEY_TAB && down) { if (down == 1 || S.switching) wm_switcher_cycle(S.mods & MOD_SHIFT); return true; }
    if (down == 1) {
        if (alt && code == KEY_F4 && S.focus) { wm_request_close(S.focus); return true; }
        if (ctrl && alt && code == KEY_T) { if (!S.clients) spawn("/bin/gapterm"); else server_send_hotkey(FH_TERMINAL); return true; }
        if (ctrl && alt && code == KEY_DELETE) { server_send_hotkey(FH_TASKS); return true; }
        if (code == KEY_LSUPER || code == KEY_RSUPER) { server_send_hotkey(FH_LAUNCHER); return true; }
        if (ctrl && code == KEY_F12) { comp_screenshot("/tmp/screenshot.ppm"); return true; }
        if (alt && code == KEY_F2) { server_send_hotkey(FH_RUN); return true; }
    }
    return S.switching;                                 /* the switcher swallows keys while it is up */
}
