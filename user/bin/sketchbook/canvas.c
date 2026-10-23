/* The canvas view: zoom, scrolling, drawing the picture with its pixel grid and overlays, and the pointer event queue.
 *
 * Pointer events are collected by an event hook (so no press, release or intermediate position is lost when several
 * arrive between two frames) and replayed in order at the start of the next frame. */
#include "sketchbook.h"
#include <stdlib.h>
#include <string.h>

#define WORKSPACE   0x6F7488
#define SHADOW      0x3D4152
#define BAR         14

typedef struct { uint8_t kind, button, mods; int x, y; } PEv;
#define QMAX 256
static PEv g_q[QMAX];
static int g_nq;

static struct { bool active; int sx, sy, scroll_x, scroll_y; } g_pan;
static bool g_ignore[4];
static KRect g_frame, g_inner;
static bool g_need_h, g_need_v;
static int g_mx, g_my;                         /* last pointer position seen in a frame */
static int *g_xmap;
static int g_xmap_cap;

/* ---------------------------------------------------------------- geometry */

static void geometry(KRect frame) {
    g_frame = frame;
    g_inner = kr_inset(frame, 2);
    int cw = g_doc.w * S.zoom + 2 * CANVAS_PAD, ch = g_doc.h * S.zoom + 2 * CANVAS_PAD;
    bool nh = false, nv = false;
    for (int pass = 0; pass < 2; pass++) {
        nv = ch > g_inner.h - (nh ? BAR : 0);
        nh = cw > g_inner.w - (nv ? BAR : 0);
    }
    g_need_h = nh;
    g_need_v = nv;
    S.view = kr(g_inner.x, g_inner.y, g_inner.w - (nv ? BAR : 0), g_inner.h - (nh ? BAR : 0));
}

static void position(void) {
    int pw = g_doc.w * S.zoom, ph = g_doc.h * S.zoom;
    int cw = pw + 2 * CANVAS_PAD, ch = ph + 2 * CANVAS_PAD;
    S.scroll_x = sk_clamp(S.scroll_x, 0, sk_max(0, cw - S.view.w));
    S.scroll_y = sk_clamp(S.scroll_y, 0, sk_max(0, ch - S.view.h));
    S.ox = cw <= S.view.w ? S.view.x + (S.view.w - pw) / 2 : S.view.x + CANVAS_PAD - S.scroll_x;
    S.oy = ch <= S.view.h ? S.view.y + (S.view.h - ph) / 2 : S.view.y + CANVAS_PAD - S.scroll_y;
}

int canvas_to_ix(int sx) { return sk_fdiv(sx - S.ox, S.zoom); }
int canvas_to_iy(int sy) { return sk_fdiv(sy - S.oy, S.zoom); }

void canvas_init(void) {
    S.zoom = 1;
    S.grid = true;
    g_nq = 0;
}

void canvas_reset_view(void) {
    S.zoom = 1;
    S.scroll_x = S.scroll_y = 0;
    g_pan.active = false;
}

void canvas_set_zoom(int z, bool at_pointer) {
    z = sk_clamp(z, 1, 8);
    if (z == S.zoom) return;
    int ax, ay;
    if (at_pointer && kr_contains(S.view, g_mx, g_my)) { ax = g_mx; ay = g_my; }
    else { ax = S.view.x + S.view.w / 2; ay = S.view.y + S.view.h / 2; }
    long long fx = ((long long)(ax - S.ox) * 256) / S.zoom;        /* the picture point under the anchor, in 1/256 pixels */
    long long fy = ((long long)(ay - S.oy) * 256) / S.zoom;
    S.zoom = z;
    geometry(g_frame);
    int pw = g_doc.w * z, ph = g_doc.h * z;
    int nox = ax - (int)(fx * z / 256), noy = ay - (int)(fy * z / 256);
    S.scroll_x = pw + 2 * CANVAS_PAD > S.view.w ? S.view.x + CANVAS_PAD - nox : 0;
    S.scroll_y = ph + 2 * CANVAS_PAD > S.view.h ? S.view.y + CANVAS_PAD - noy : 0;
    position();
}

void canvas_center(void) {
    S.scroll_x = sk_max(0, (g_doc.w * S.zoom + 2 * CANVAS_PAD - S.view.w) / 2);
    S.scroll_y = sk_max(0, (g_doc.h * S.zoom + 2 * CANVAS_PAD - S.view.h) / 2);
}

void canvas_scroll_to_show(int x, int y, int w, int h) {
    int z = S.zoom;
    int sx0 = x * z, sy0 = y * z, sx1 = (x + w) * z, sy1 = (y + h) * z;           /* in padded-picture pixels relative to the picture */
    int vx0 = S.scroll_x - CANVAS_PAD, vy0 = S.scroll_y - CANVAS_PAD;
    if (sx1 > vx0 + S.view.w) S.scroll_x += sx1 - (vx0 + S.view.w);
    if (sx0 < vx0) S.scroll_x -= vx0 - sx0;
    if (sy1 > vy0 + S.view.h) S.scroll_y += sy1 - (vy0 + S.view.h);
    if (sy0 < vy0) S.scroll_y -= vy0 - sy0;
    position();
}

/* ---------------------------------------------------------------- pointer queue */

void canvas_event_hook(KApp *app, const FairyEvent *ev, void *user) {
    (void)user;
    if (ev->type != FEV_POINTER || ev->window != kapp_window(app)) return;
    const FePointer *p = &ev->u.pointer;
    if (p->kind != FP_MOVE && p->kind != FP_PRESS && p->kind != FP_RELEASE) return;
    if (g_nq == QMAX) {                                          /* full: collapse trailing motion, else drop the oldest */
        if (g_q[g_nq - 1].kind == FP_MOVE && p->kind == FP_MOVE) g_nq--;
        else { memmove(g_q, g_q + 1, (QMAX - 1) * sizeof g_q[0]); g_nq--; }
    }
    g_q[g_nq++] = (PEv){ p->kind, p->button, p->mods, p->x, p->y };
}

static void process_queue(KUI *ui) {
    for (int i = 0; i < g_nq; i++) {
        PEv e = g_q[i];
        switch (e.kind) {
        case FP_PRESS:
            if (e.button == 3) {
                if (kr_contains(S.view, e.x, e.y) && !ui->menu_open && !S.modal) {
                    g_pan.active = true;
                    g_pan.sx = e.x; g_pan.sy = e.y; g_pan.scroll_x = S.scroll_x; g_pan.scroll_y = S.scroll_y;
                } else g_ignore[3] = true;
            } else if (e.button == 1 || e.button == 2) {
                if (tools_dragging() || g_pan.active || S.modal || ui->menu_open || !kr_contains(S.view, e.x, e.y)) { g_ignore[e.button] = true; break; }
                tools_pointer_press(ui, e.button, e.x, e.y, e.mods);
            }
            break;
        case FP_MOVE:
            if (g_pan.active) {
                S.scroll_x = g_pan.scroll_x - (e.x - g_pan.sx);
                S.scroll_y = g_pan.scroll_y - (e.y - g_pan.sy);
                position();
            } else tools_pointer_move(ui, e.x, e.y, e.mods);
            break;
        case FP_RELEASE:
            if (e.button < 4 && g_ignore[e.button]) { g_ignore[e.button] = false; break; }
            if (e.button == 3) g_pan.active = false;
            else tools_pointer_release(ui, e.button, e.x, e.y, e.mods);
            break;
        }
    }
    g_nq = 0;
}

/* While a tool is being dragged and the pointer is outside the view, the view scrolls that way (so a selection or a
 * shape can be dragged past the edge of the window); a timer keeps it going while the pointer holds still. */
static bool g_scroll_tick;

static void autoscroll(KUI *ui) {
    int dx = 0, dy = 0;
    if (tools_dragging() && (ui->buttons & 3)) {
        if (ui->mx < S.view.x) dx = ui->mx - S.view.x - 4;
        else if (ui->mx >= S.view.x + S.view.w) dx = ui->mx - (S.view.x + S.view.w) + 4;
        if (ui->my < S.view.y) dy = ui->my - S.view.y - 4;
        else if (ui->my >= S.view.y + S.view.h) dy = ui->my - (S.view.y + S.view.h) + 4;
        dx = sk_clamp(dx, -32, 32);
        dy = sk_clamp(dy, -32, 32);
    }
    if (dx || dy) {
        int sx = S.scroll_x, sy = S.scroll_y;
        S.scroll_x += dx;
        S.scroll_y += dy;
        position();
        if (S.scroll_x != sx || S.scroll_y != sy) tools_pointer_move(ui, ui->mx, ui->my, ui->mods);   /* the picture moved under the pointer */
        if (!g_scroll_tick) { kapp_set_tick(S.app, 30); g_scroll_tick = true; }
    } else if (g_scroll_tick) {
        g_scroll_tick = false;
        if (!tools_dragging()) kapp_set_tick(S.app, 0);
    }
}

/* ---------------------------------------------------------------- scrollbars */

static void hscrollbar(KUI *ui, KRect r, int *offset, int content, int view) {
    kc_fill(&ui->cv, r, kc_shade(ui->theme->window_bg, 90));
    if (content <= view || view <= 0) return;
    int maxoff = content - view;
    *offset = sk_clamp(*offset, 0, maxoff);
    int tw = r.w * view / content;
    if (tw < 18) tw = 18;
    int tx = r.x + (r.w - tw) * (*offset) / maxoff;
    KRect thumb = kr(tx, r.y, tw, r.h);
    uint32_t id = k_id("sk-hscroll", r);
    if (k_hover(ui, thumb) && ui->pressed[0]) { ui->active = id; ui->drag_dx = ui->mx - tx; }
    if (ui->active == id) {
        if (ui->buttons & 1) {
            int range = r.w - tw;
            *offset = range > 0 ? sk_clamp((ui->mx - ui->drag_dx - r.x) * maxoff / range, 0, maxoff) : 0;
        } else ui->active = 0;
    } else if (k_hover(ui, r) && ui->pressed[0]) {
        *offset = sk_clamp(*offset + (ui->mx < tx ? -view : view), 0, maxoff);
    }
    kc_panel(&ui->cv, kr(sk_clamp(tx, r.x, r.x + r.w - tw), r.y, tw, r.h), KB_RAISED, ui->theme->button_bg);
}

/* ---------------------------------------------------------------- drawing the picture */

void canvas_blit_image(KCanvas *cv, const uint32_t *src, int sw, int sh, int ix, int iy) {
    int z = S.zoom;
    KRect dst = kr(S.ox + ix * z, S.oy + iy * z, sw * z, sh * z);
    KRect r = kr_intersect(kr_intersect(dst, S.view), cv->clip);
    if (kr_empty(r)) return;
    if (z == 1) {
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(cv->px + (size_t)y * cv->stride + r.x, src + (size_t)(y - dst.y) * sw + (r.x - dst.x), (size_t)r.w * 4);
        return;
    }
    if (r.w > g_xmap_cap) {
        int *nm = realloc(g_xmap, (size_t)r.w * sizeof(int));
        if (!nm) return;
        g_xmap = nm;
        g_xmap_cap = r.w;
    }
    for (int i = 0; i < r.w; i++) g_xmap[i] = (r.x + i - dst.x) / z;
    int y = r.y;
    while (y < r.y + r.h) {
        int sy = (y - dst.y) / z;
        int y_end = dst.y + (sy + 1) * z;
        if (y_end > r.y + r.h) y_end = r.y + r.h;
        uint32_t *row = cv->px + (size_t)y * cv->stride + r.x;
        const uint32_t *srow = src + (size_t)sy * sw;
        for (int i = 0; i < r.w; i++) row[i] = srow[g_xmap[i]];
        for (int yy = y + 1; yy < y_end; yy++) memcpy(cv->px + (size_t)yy * cv->stride + r.x, row, (size_t)r.w * 4);
        y = y_end;
    }
}

static inline uint32_t grid_mix(uint32_t p) { return (((p >> 2) & 0x3F3F3F) * 3) + 0x202020; }

static void draw_grid(KCanvas *cv) {
    int z = S.zoom;
    KRect pic = kr_intersect(kr(S.ox, S.oy, g_doc.w * z, g_doc.h * z), kr_intersect(S.view, cv->clip));
    if (kr_empty(pic)) return;
    int ix0 = (pic.x - S.ox) / z + 1, ix1 = (pic.x + pic.w - 1 - S.ox) / z;
    int iy0 = (pic.y - S.oy) / z + 1, iy1 = (pic.y + pic.h - 1 - S.oy) / z;
    for (int ix = ix0; ix <= ix1; ix++) {
        int sx = S.ox + ix * z;
        if (sx < pic.x || sx >= pic.x + pic.w) continue;
        for (int y = pic.y; y < pic.y + pic.h; y++) { uint32_t *p = cv->px + (size_t)y * cv->stride + sx; *p = grid_mix(*p); }
    }
    for (int iy = iy0; iy <= iy1; iy++) {
        int sy = S.oy + iy * z;
        if (sy < pic.y || sy >= pic.y + pic.h) continue;
        uint32_t *row = cv->px + (size_t)sy * cv->stride;
        for (int x = pic.x; x < pic.x + pic.w; x++) row[x] = grid_mix(row[x]);
    }
}

static void render(KUI *ui) {
    KCanvas *cv = &ui->cv;
    KRect old = cv->clip;
    kc_set_clip(cv, S.view);
    kc_fill(cv, S.view, WORKSPACE);
    int z = S.zoom;
    KRect pic = kr(S.ox, S.oy, g_doc.w * z, g_doc.h * z);
    kc_fill(cv, kr(pic.x + 4, pic.y + 4, pic.w, pic.h), SHADOW);
    kc_fill(cv, kr_grow(pic, 1), 0x000000);
    canvas_blit_image(cv, g_doc.px, g_doc.w, g_doc.h, 0, 0);
    if (S.grid && z >= 4) draw_grid(cv);
    tools_draw_overlays(ui, cv);
    cv->clip = old;
}

/* ---------------------------------------------------------------- one frame */

void canvas_frame(KUI *ui, KRect frame) {
    KCanvas *cv = &ui->cv;
    kc_bevel(cv, frame, KB_SUNKEN);
    geometry(frame);
    position();
    process_queue(ui);
    autoscroll(ui);

    bool over = ui->mouse_inside && !ui->menu_open && kr_contains(S.view, ui->mx, ui->my);
    g_mx = ui->mx;
    g_my = ui->my;
    if (ui->wheel && over && !S.modal) {
        if (ui->mods & MOD_CTRL) canvas_set_zoom(S.zoom + (ui->wheel > 0 ? 1 : -1), true);
        else if (ui->mods & MOD_SHIFT) S.scroll_x -= ui->wheel * 48;
        else S.scroll_y -= ui->wheel * 48;
    }
    geometry(frame);
    position();

    /* scrollbars first, so the picture below follows the thumb in the same frame */
    if (g_need_v) {
        KRect vb = kr(S.view.x + S.view.w, g_inner.y, BAR, S.view.h);
        k_scrollbar(ui, vb, &S.scroll_y, g_doc.h * S.zoom + 2 * CANVAS_PAD, S.view.h);
    }
    if (g_need_h) {
        KRect hb = kr(g_inner.x, S.view.y + S.view.h, S.view.w, BAR);
        hscrollbar(ui, hb, &S.scroll_x, g_doc.w * S.zoom + 2 * CANVAS_PAD, S.view.w);
    }
    if (g_need_h && g_need_v) kc_fill(cv, kr(S.view.x + S.view.w, S.view.y + S.view.h, BAR, BAR), ui->theme->window_bg);
    position();

    if (over && !tools_dragging()) ui->cursor_shape = tools_cursor(canvas_to_ix(ui->mx), canvas_to_iy(ui->my));
    else if (tools_dragging() || g_pan.active) ui->cursor_shape = g_pan.active ? FC_MOVE : tools_cursor(S.cur_x, S.cur_y);
    S.cur_x = canvas_to_ix(ui->mx);
    S.cur_y = canvas_to_iy(ui->my);
    S.cur_inside = (over || tools_dragging()) && S.cur_x >= 0 && S.cur_y >= 0 && S.cur_x < g_doc.w && S.cur_y < g_doc.h;
    if (!ui->mouse_inside) S.cur_inside = false;

    render(ui);
}
