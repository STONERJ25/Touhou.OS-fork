/* What each tool does with the pointer; selections, the internal clipboard and the text tool.
 *
 * Painting always goes through g_isink (so the history sees it); previews go through a ScreenSink with the very same
 * rasteriser. Freehand tools write into the picture while the button is down, shapes are only drawn when it is released. */
#include "sketchbook.h"
#include <font8x16.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name, *hint; int key; } g_def[T_COUNT] = {
    [T_PENCIL]  = { "Pencil",        "Pencil: drag to draw (right button: background)", 'P' },
    [T_BRUSH]   = { "Brush",         "Brush: drag to paint (right button: background)", 'B' },
    [T_ERASER]  = { "Eraser",        "Eraser: drag to rub out with the background", 'E' },
    [T_LINE]    = { "Line",          "Line: drag it out; Shift snaps the angle", 'L' },
    [T_RECT]    = { "Rectangle",     "Rectangle: drag corner to corner; Shift: square", 'R' },
    [T_ELLIPSE] = { "Ellipse",       "Ellipse: drag across its box; Shift: circle", 'O' },
    [T_FILL]    = { "Fill",          "Fill: click to flood an area with the colour", 'F' },
    [T_PICKER]  = { "Colour picker", "Picker: click to take a colour (right: background)", 'I' },
    [T_TEXT]    = { "Text",          "Text: click, type; Enter places it, Esc cancels", 'T' },
    [T_SELECT]  = { "Select",        "Select: drag a box; drag inside it to move it", 'S' },
    [T_SPRAY]   = { "Spray can",     "Spray can: hold the button and move around", 'A' },
};

const char *tool_name(Tool t) { return g_def[t].name; }
const char *tool_hint(Tool t) { return g_def[t].hint; }
int tool_key(Tool t) { return g_def[t].key; }

/* ---------------------------------------------------------------- state */

typedef struct {
    bool active;
    int button;                                  /* 1 left, 2 right */
    uint32_t prim, sec;
    int x0, y0, lx, ly, cx, cy;
    int mods;
} Drag;
static Drag D;

typedef struct {
    bool active;                                 /* a selection rectangle exists */
    int x, y, w, h;
    bool floating;                               /* its pixels are lifted into fpx */
    bool from_paste;
    uint32_t *fpx;
    int ox, oy;                                  /* where it was lifted from */
    bool dragging_new, moving;
    int ax, ay, gdx, gdy;
} Sel;
static Sel Z;

static struct { int w, h; uint32_t *px; } g_clip;

static struct {
    bool active;
    int x, y;
    char buf[1024];
    int len, caret;
    uint32_t color, bgcol;
    int scale;
    bool opaque;
} T;

void tools_init(void) {
    for (int i = 0; i < T_COUNT; i++) S.size[i] = 1;
    S.size[T_BRUSH] = 4;
    S.size[T_ERASER] = 8;
    S.size[T_SPRAY] = 12;
    S.shape_style = S_OUTLINE;
    S.text_scale = 1;
    S.tool = T_PENCIL;
}

static void commit_float(void);
static void text_commit(void);

void tool_set(Tool t) {
    if (t == S.tool) return;
    if (S.tool == T_TEXT) text_commit();
    if (S.tool == T_SELECT && t != T_SELECT) commit_float();
    if (D.active) tools_cancel_drag();
    S.tool = t;
}

bool tools_dragging(void) { return D.active; }

/* ---------------------------------------------------------------- freehand tools */

static int stroke_size(void) { return S.tool == T_PENCIL ? 1 : S.size[S.tool]; }
static uint32_t stroke_color(void) { return S.tool == T_ERASER ? S.bg : D.prim; }

static void stroke_begin(int ix, int iy) {
    hist_begin(g_def[S.tool].name);
    g_isink.color = stroke_color();
    rs_stamp(&g_isink, ix, iy, stroke_size(), S.tool == T_ERASER);
}

static void stroke_to(int x, int y) {
    g_isink.color = stroke_color();
    rs_line(&g_isink, D.lx, D.ly, x, y, stroke_size(), S.tool == T_ERASER);
    D.lx = x;
    D.ly = y;
}

/* ---------------------------------------------------------------- shapes */

static void constrain(int x0, int y0, int *x1, int *y1, bool line) {
    int dx = *x1 - x0, dy = *y1 - y0, adx = abs(dx), ady = abs(dy);
    if (line) {
        if (adx > 2 * ady) *y1 = y0;
        else if (ady > 2 * adx) *x1 = x0;
        else { int m = sk_max(adx, ady); *x1 = x0 + (dx < 0 ? -m : m); *y1 = y0 + (dy < 0 ? -m : m); }
    } else {
        int m = sk_max(adx, ady);
        *x1 = x0 + (dx < 0 ? -m : m);
        *y1 = y0 + (dy < 0 ? -m : m);
    }
}

static void shape_coords(int mods, int *x0, int *y0, int *x1, int *y1) {
    *x0 = D.x0; *y0 = D.y0; *x1 = D.cx; *y1 = D.cy;
    if (mods & MOD_SHIFT) constrain(*x0, *y0, x1, y1, S.tool == T_LINE);
}

static void shape_draw(Sink *sn, int mods) {
    int x0, y0, x1, y1;
    shape_coords(mods, &x0, &y0, &x1, &y1);
    switch (S.tool) {
    case T_LINE:
        sn->color = D.prim;
        rs_line(sn, x0, y0, x1, y1, S.size[T_LINE], false);
        break;
    case T_RECT: rs_rect(sn, x0, y0, x1, y1, S.size[T_RECT], D.prim, D.sec, S.shape_style); break;
    case T_ELLIPSE: rs_ellipse(sn, x0, y0, x1, y1, S.size[T_ELLIPSE], D.prim, D.sec, S.shape_style); break;
    default: break;
    }
}

static bool is_shape_tool(void) { return S.tool == T_LINE || S.tool == T_RECT || S.tool == T_ELLIPSE; }

/* ---------------------------------------------------------------- colour picker */

static void pick(int ix, int iy) {
    if (ix < 0 || iy < 0 || ix >= g_doc.w || iy >= g_doc.h) return;
    color_set(D.button == 1 ? 0 : 1, g_doc.px[(size_t)iy * g_doc.w + ix]);
}

/* ---------------------------------------------------------------- selection */

static uint32_t *dup_pixels(const uint32_t *src, int w, int h) {
    uint32_t *p = malloc((size_t)w * (size_t)h * 4);
    if (p) memcpy(p, src, (size_t)w * (size_t)h * 4);
    return p;
}

static void lift(bool copy) {
    if (Z.floating || !Z.active) return;
    uint32_t *buf = img_make_crop(Z.x, Z.y, Z.w, Z.h);
    if (!buf) return;
    Z.fpx = buf;
    Z.ox = Z.x;
    Z.oy = Z.y;
    hist_begin(copy ? "Copy selection" : "Move selection");
    if (!copy) {
        g_isink.color = S.bg;
        rs_fill_rect(&g_isink, Z.x, Z.y, Z.x + Z.w - 1, Z.y + Z.h - 1);
    }
    Z.floating = true;
    Z.from_paste = false;
}

static void commit_float(void) {
    if (!Z.floating) return;
    if (!hist_pending()) hist_begin(Z.from_paste ? "Paste" : "Move selection");
    for (int r = 0; r < Z.h; r++) {
        int y = Z.y + r;
        if (y < 0 || y >= g_doc.h) continue;
        int xs = sk_max(0, Z.x), xe = sk_min(g_doc.w, Z.x + Z.w);
        if (xs >= xe) continue;
        hist_touch(xs, y, xe - 1, y);
        memcpy(g_doc.px + (size_t)y * g_doc.w + xs, Z.fpx + (size_t)r * Z.w + (xs - Z.x), (size_t)(xe - xs) * 4);
    }
    hist_commit();
    free(Z.fpx);
    Z.fpx = NULL;
    Z.floating = false;
    Z.from_paste = false;
    /* the rectangle stays, trimmed to the picture */
    int x0 = sk_max(Z.x, 0), y0 = sk_max(Z.y, 0), x1 = sk_min(Z.x + Z.w, g_doc.w), y1 = sk_min(Z.y + Z.h, g_doc.h);
    if (x1 > x0 && y1 > y0) { Z.x = x0; Z.y = y0; Z.w = x1 - x0; Z.h = y1 - y0; }
    else Z.active = false;
}

static void abort_float(void) {
    if (!Z.floating) return;
    hist_abort();
    free(Z.fpx);
    Z.fpx = NULL;
    Z.floating = false;
    if (Z.from_paste) Z.active = false;
    else { Z.x = Z.ox; Z.y = Z.oy; }
}

static bool in_selection(int ix, int iy) {
    return Z.active && !Z.dragging_new && ix >= Z.x && iy >= Z.y && ix < Z.x + Z.w && iy < Z.y + Z.h;
}

static void select_press(int ix, int iy, int mods) {
    if (in_selection(ix, iy)) {
        Z.moving = true;
        Z.gdx = ix - Z.x;
        Z.gdy = iy - Z.y;
        if (!Z.floating) lift((mods & MOD_CTRL) != 0);
        return;
    }
    commit_float();
    Z.active = false;
    Z.moving = false;
    Z.dragging_new = true;
    Z.ax = sk_clamp(ix, 0, g_doc.w - 1);
    Z.ay = sk_clamp(iy, 0, g_doc.h - 1);
    Z.x = Z.ax; Z.y = Z.ay; Z.w = Z.h = 1;
}

static void select_move(int ix, int iy) {
    if (Z.dragging_new) {
        int cx = sk_clamp(ix, 0, g_doc.w - 1), cy = sk_clamp(iy, 0, g_doc.h - 1);
        Z.x = sk_min(Z.ax, cx);
        Z.y = sk_min(Z.ay, cy);
        Z.w = abs(cx - Z.ax) + 1;
        Z.h = abs(cy - Z.ay) + 1;
        Z.active = true;
    } else if (Z.moving) {
        Z.x = ix - Z.gdx;
        Z.y = iy - Z.gdy;
    }
}

static void select_release(void) {
    if (Z.dragging_new) {
        Z.dragging_new = false;
        if (D.cx == D.x0 && D.cy == D.y0) Z.active = false;           /* a plain click deselects */
    }
    Z.moving = false;
}

/* ---------------------------------------------------------------- text */

static void text_commit(void) {
    if (!T.active) return;
    T.active = false;
    if (T.len == 0) return;
    hist_begin("Text");
    rs_text(&g_isink, T.x, T.y, T.buf, T.scale, T.color, T.opaque, T.bgcol);
    hist_commit();
}

static int text_offset_for(int line, int col) {                 /* byte offset of (line, column) */
    int off = 0, l = 0, c = 0;
    while (off < T.len) {
        if (l == line && c == col) return off;
        if (T.buf[off] == '\n') { if (l == line) return off; l++; c = 0; off++; continue; }
        uint32_t cp;
        int b = kc_utf8_next(T.buf + off, &cp);
        off += b > 0 ? b : 1;
        c++;
    }
    return T.len;
}

static void text_caret_pos(int *line, int *col) {
    int l = 0, c = 0, off = 0;
    while (off < T.caret) {
        if (T.buf[off] == '\n') { l++; c = 0; off++; continue; }
        uint32_t cp;
        int b = kc_utf8_next(T.buf + off, &cp);
        off += b > 0 ? b : 1;
        c++;
    }
    *line = l;
    *col = c;
}

static void text_click(int ix, int iy) {
    if (T.active) {
        int cols, lines;
        text_metrics(T.buf, &cols, &lines);
        if (cols < 1) cols = 1;
        if (ix >= T.x && ix < T.x + cols * FONT_W * T.scale && iy >= T.y && iy < T.y + lines * FONT_H * T.scale) {
            T.caret = text_offset_for((iy - T.y) / (FONT_H * T.scale), (ix - T.x) / (FONT_W * T.scale));
            return;
        }
        text_commit();
    }
    if (ix < 0 || iy < 0 || ix >= g_doc.w || iy >= g_doc.h) return;
    T.active = true;
    T.x = ix;
    T.y = iy;
    T.len = T.caret = 0;
    T.buf[0] = 0;
    T.color = D.prim;
    T.bgcol = D.sec;
    T.scale = S.text_scale;
    T.opaque = S.text_opaque;
}

bool tools_text_active(void) { return T.active; }

static void text_insert(const char *s, int n) {
    if (T.len + n >= (int)sizeof T.buf) return;
    memmove(T.buf + T.caret + n, T.buf + T.caret, (size_t)(T.len - T.caret + 1));
    memcpy(T.buf + T.caret, s, (size_t)n);
    T.len += n;
    T.caret += n;
}

static void text_delete(int from, int to) {
    memmove(T.buf + from, T.buf + to, (size_t)(T.len - to + 1));
    T.len -= to - from;
    if (T.caret > to) T.caret -= to - from;
    else if (T.caret > from) T.caret = from;
}

static int text_prev(int off) { if (off <= 0) return 0; off--; while (off > 0 && (T.buf[off] & 0xC0) == 0x80) off--; return off; }
static int text_next(int off) { if (off >= T.len) return T.len; off++; while (off < T.len && (T.buf[off] & 0xC0) == 0x80) off++; return off; }

void tools_text_keys(KUI *ui) {
    if (!T.active || ui->focus || S.modal || ui->menu_open) return;
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool shift = k->mods & MOD_SHIFT, ctrl = k->mods & (MOD_CTRL | MOD_ALT);
        bool used = true;
        if (k->code == KEY_ESC) T.active = false;
        else if (k->code == KEY_ENTER || k->code == KEY_KPENTER) {
            if (shift) text_insert("\n", 1);
            else text_commit();
        } else if (k->code == KEY_BACKSPACE) { if (T.caret > 0) text_delete(text_prev(T.caret), T.caret); }
        else if (k->code == KEY_DELETE) { if (T.caret < T.len) text_delete(T.caret, text_next(T.caret)); }
        else if (k->code == KEY_LEFT) T.caret = text_prev(T.caret);
        else if (k->code == KEY_RIGHT) T.caret = text_next(T.caret);
        else if (k->code == KEY_HOME) { int l, c; text_caret_pos(&l, &c); T.caret = text_offset_for(l, 0); }
        else if (k->code == KEY_END) { int l, c; text_caret_pos(&l, &c); T.caret = text_offset_for(l, 100000); }
        else if (k->code == KEY_UP || k->code == KEY_DOWN) {
            int l, c;
            text_caret_pos(&l, &c);
            T.caret = text_offset_for(k->code == KEY_UP ? (l > 0 ? l - 1 : 0) : l + 1, c);
        } else if (k->ch >= 32 && k->ch != 127 && !ctrl) {
            char enc[4];
            int n = kc_utf8_encode(k->ch, enc);
            text_insert(enc, n);
        } else used = false;
        if (used) { k->down = 0; k->code = 0; if (!T.active) break; }
    }
}

/* ---------------------------------------------------------------- pointer events */

void tools_pointer_press(KUI *ui, int button, int x, int y, int mods) {
    (void)ui;
    int ix = canvas_to_ix(x), iy = canvas_to_iy(y);
    if (S.tool != T_SELECT) commit_float();
    if (S.tool != T_TEXT) text_commit();
    memset(&D, 0, sizeof D);
    D.active = true;
    D.button = button;
    D.prim = button == 1 ? S.fg : S.bg;
    D.sec = button == 1 ? S.bg : S.fg;
    D.x0 = D.lx = D.cx = ix;
    D.y0 = D.ly = D.cy = iy;
    D.mods = mods;
    switch (S.tool) {
    case T_PENCIL: case T_BRUSH: case T_ERASER: stroke_begin(ix, iy); break;
    case T_SPRAY:
        hist_begin("Spray can");
        g_isink.color = D.prim;
        rs_spray(&g_isink, ix, iy, S.size[T_SPRAY], S.size[T_SPRAY] * S.size[T_SPRAY] / 4 + 4);
        kapp_set_tick(S.app, 30);
        break;
    case T_LINE: case T_RECT: case T_ELLIPSE: break;
    case T_FILL:
        hist_begin("Fill");
        if (fill_flood(ix, iy, D.prim)) hist_commit(); else hist_abort();
        D.active = false;
        break;
    case T_PICKER: pick(ix, iy); break;
    case T_TEXT: text_click(ix, iy); D.active = false; break;
    case T_SELECT: select_press(ix, iy, mods); break;
    default: break;
    }
}

void tools_pointer_move(KUI *ui, int x, int y, int mods) {
    (void)ui;
    if (!D.active) return;
    D.cx = canvas_to_ix(x);
    D.cy = canvas_to_iy(y);
    D.mods = mods;
    switch (S.tool) {
    case T_PENCIL: case T_BRUSH: case T_ERASER: stroke_to(D.cx, D.cy); break;
    case T_PICKER: pick(D.cx, D.cy); break;
    case T_SELECT: select_move(D.cx, D.cy); break;
    default: break;
    }
}

void tools_pointer_release(KUI *ui, int button, int x, int y, int mods) {
    (void)ui;
    if (!D.active || button != D.button) return;
    D.cx = canvas_to_ix(x);
    D.cy = canvas_to_iy(y);
    D.mods = mods;
    switch (S.tool) {
    case T_PENCIL: case T_BRUSH: case T_ERASER:
        stroke_to(D.cx, D.cy);
        hist_commit();
        break;
    case T_SPRAY:
        hist_commit();
        kapp_set_tick(S.app, 0);
        break;
    case T_LINE: case T_RECT: case T_ELLIPSE:
        hist_begin(g_def[S.tool].name);
        shape_draw(&g_isink, mods);
        hist_commit();
        break;
    case T_SELECT: select_release(); break;
    default: break;
    }
    D.active = false;
}

void tools_cancel_drag(void) {
    if (!D.active) return;
    switch (S.tool) {
    case T_PENCIL: case T_BRUSH: case T_ERASER: hist_abort(); break;
    case T_SPRAY: hist_abort(); kapp_set_tick(S.app, 0); break;
    case T_SELECT:
        if (Z.dragging_new) { Z.dragging_new = false; Z.active = false; }
        if (Z.moving) { Z.moving = false; abort_float(); }
        break;
    default: break;
    }
    D.active = false;
}

/* ---------------------------------------------------------------- per frame */

void tools_frame(KUI *ui) {
    if (D.active) D.mods = ui->mods;
    tools_text_keys(ui);
    if (D.active && S.tool == T_SPRAY && (ui->buttons & (D.button == 1 ? 1 : 2))) {
        int r = S.size[T_SPRAY];
        g_isink.color = D.prim;
        rs_spray(&g_isink, D.cx, D.cy, r, r * r / 4 + 4);
        ui->redraw_soon = true;
    }
    /* the hint under the picture follows what is going on */
    const char *hint = g_def[S.tool].hint;
    char buf[200];
    if (T.active) hint = "Type; Enter places the text, Shift+Enter: new line, Esc cancels";
    else if (Z.floating) hint = "Drag to move; click outside to place; Delete discards";
    else if (D.active && is_shape_tool()) {
        int x0, y0, x1, y1;
        shape_coords(D.mods, &x0, &y0, &x1, &y1);
        if (S.tool == T_LINE) snprintf(buf, sizeof buf, "Line from %d, %d to %d, %d (length %d)", x0, y0, x1, y1, sk_max(abs(x1 - x0), abs(y1 - y0)) + 1);
        else snprintf(buf, sizeof buf, "%s %d x %d at %d, %d", g_def[S.tool].name, abs(x1 - x0) + 1, abs(y1 - y0) + 1, sk_min(x0, x1), sk_min(y0, y1));
        hint = buf;
    } else if (Z.active && S.tool == T_SELECT) {
        snprintf(buf, sizeof buf, "Selection %d x %d at %d, %d. Drag to move (Ctrl: copy)", Z.w, Z.h, Z.x, Z.y);
        hint = buf;
    }
    snprintf(S.hint, sizeof S.hint, "%s", hint);
}

unsigned tools_cursor(int ix, int iy) {
    switch (S.tool) {
    case T_TEXT: return FC_TEXT;
    case T_SELECT: return in_selection(ix, iy) || Z.moving ? FC_MOVE : FC_CROSS;
    default: return FC_CROSS;
    }
}

/* ---------------------------------------------------------------- overlays */

static void draw_ants(KCanvas *cv, KRect r, uint64_t now) {
    int phase = (int)((now / 140) % 8);
    int k = 0;
    for (int x = r.x; x < r.x + r.w; x++, k++) {
        uint32_t c = ((k + phase) / 4) & 1 ? 0xFFFFFF : 0x000000;
        kc_pixel(cv, x, r.y, c);
        kc_pixel(cv, x, r.y + r.h - 1, ((k + phase + 2) / 4) & 1 ? 0xFFFFFF : 0x000000);
    }
    for (int y = r.y + 1; y < r.y + r.h - 1; y++, k++) {
        uint32_t c = ((k + phase) / 4) & 1 ? 0xFFFFFF : 0x000000;
        kc_pixel(cv, r.x, y, c);
        kc_pixel(cv, r.x + r.w - 1, y, ((k + phase + 2) / 4) & 1 ? 0xFFFFFF : 0x000000);
    }
}

void tools_draw_overlays(KUI *ui, KCanvas *cv) {
    int z = S.zoom;
    ScreenSink ss;
    ssink_init(&ss, cv, S.ox, S.oy, z);

    if (D.active && is_shape_tool()) shape_draw(&ss.base, ui->mods);

    if (Z.floating && Z.fpx) canvas_blit_image(cv, Z.fpx, Z.w, Z.h, Z.x, Z.y);

    if (Z.active) {
        KRect r = kr(S.ox + Z.x * z - 1, S.oy + Z.y * z - 1, Z.w * z + 2, Z.h * z + 2);
        draw_ants(cv, r, ui->now_ms);
        ui->redraw_soon = true;
    }

    if (T.active) {
        rs_text(&ss.base, T.x, T.y, T.buf, T.scale, T.color, T.opaque, T.bgcol);
        int cols, lines;
        text_metrics(T.buf, &cols, &lines);
        if (cols < 1) cols = 1;
        KRect box = kr(S.ox + T.x * z - 1, S.oy + T.y * z - 1, cols * FONT_W * T.scale * z + 2, lines * FONT_H * T.scale * z + 2);
        draw_ants(cv, box, 0);
        int l, c;
        text_caret_pos(&l, &c);
        if (((ui->now_ms / 450) & 1) == 0) {
            int cx = S.ox + (T.x + c * FONT_W * T.scale) * z, cy = S.oy + (T.y + l * FONT_H * T.scale) * z;
            kc_fill(cv, kr(cx, cy, z > 1 ? 2 : 1, FONT_H * T.scale * z), 0x000000);
            kc_fill(cv, kr(cx + (z > 1 ? 2 : 1), cy, 1, FONT_H * T.scale * z), 0xFFFFFF);
        }
        ui->redraw_soon = true;
    }

    /* brush footprint under the pointer */
    bool over = ui->mouse_inside && !ui->menu_open && kr_contains(S.view, ui->mx, ui->my);
    if (over && !D.active && !S.modal && (S.tool == T_BRUSH || S.tool == T_ERASER || S.tool == T_SPRAY)) {
        int ix = canvas_to_ix(ui->mx), iy = canvas_to_iy(ui->my);
        ss.base.alpha = 110;
        if (S.tool == T_SPRAY) {
            ss.base.color = 0x000000;
            rs_ellipse(&ss.base, ix - S.size[T_SPRAY], iy - S.size[T_SPRAY], ix + S.size[T_SPRAY], iy + S.size[T_SPRAY], 1, 0x000000, 0, S_OUTLINE);
        } else {
            ss.base.color = S.tool == T_ERASER ? 0xFFFFFF : S.fg;
            rs_stamp(&ss.base, ix, iy, S.size[S.tool], S.tool == T_ERASER);
        }
        ss.base.alpha = 255;
    }
}

/* ---------------------------------------------------------------- commands */

void tools_commit_all(void) {
    commit_float();
    text_commit();
}

bool tools_cancel_overlays(void) {
    if (Z.floating) { abort_float(); return true; }
    if (T.active) { T.active = false; return true; }
    return false;
}

void tools_reset(void) {
    if (Z.floating) abort_float();
    free(Z.fpx);
    memset(&Z, 0, sizeof Z);
    T.active = false;
    D.active = false;
}

bool tools_has_selection(void) { return Z.active && !Z.dragging_new; }
bool tools_float_active(void) { return Z.floating; }

void tools_get_selection(int *x, int *y, int *w, int *h) {
    if (x) *x = Z.x;
    if (y) *y = Z.y;
    if (w) *w = Z.w;
    if (h) *h = Z.h;
}

void tools_nudge(int dx, int dy) {
    if (!Z.active || Z.dragging_new || D.active) return;
    if (!Z.floating) lift(false);
    if (!Z.floating) return;
    Z.x += dx;
    Z.y += dy;
}

bool clip_has_content(void) { return g_clip.px != NULL; }

void cmd_copy(void) {
    text_commit();
    if (!Z.active || Z.dragging_new) return;
    uint32_t *buf = Z.floating ? dup_pixels(Z.fpx, Z.w, Z.h) : img_make_crop(Z.x, Z.y, Z.w, Z.h);
    if (!buf) return;
    free(g_clip.px);
    g_clip.px = buf;
    g_clip.w = Z.w;
    g_clip.h = Z.h;
}

void cmd_delete(void) {
    text_commit();
    if (Z.floating) {
        if (Z.from_paste) { abort_float(); Z.active = false; return; }
        hist_commit();                                   /* keep the hole that lifting left behind */
        free(Z.fpx);
        Z.fpx = NULL;
        Z.floating = false;
        Z.active = false;
        return;
    }
    if (!Z.active || Z.dragging_new) return;
    hist_begin("Clear selection");
    g_isink.color = S.bg;
    rs_fill_rect(&g_isink, Z.x, Z.y, Z.x + Z.w - 1, Z.y + Z.h - 1);
    hist_commit();
}

void cmd_cut(void) {
    cmd_copy();
    if (Z.active) cmd_delete();
}

void cmd_paste(void) {
    if (!g_clip.px) return;
    tools_commit_all();
    tool_set(T_SELECT);
    uint32_t *buf = dup_pixels(g_clip.px, g_clip.w, g_clip.h);
    if (!buf) return;
    free(Z.fpx);
    memset(&Z, 0, sizeof Z);
    Z.fpx = buf;
    Z.w = g_clip.w;
    Z.h = g_clip.h;
    int vx = canvas_to_ix(S.view.x), vy = canvas_to_iy(S.view.y);
    Z.x = sk_clamp(vx, 0, sk_max(0, g_doc.w - Z.w));
    Z.y = sk_clamp(vy, 0, sk_max(0, g_doc.h - Z.h));
    Z.ox = Z.x;
    Z.oy = Z.y;
    Z.floating = true;
    Z.from_paste = true;
    Z.active = true;
}

void cmd_select_all(void) {
    tools_commit_all();
    tool_set(T_SELECT);
    Z.active = true;
    Z.dragging_new = Z.moving = false;
    Z.x = Z.y = 0;
    Z.w = g_doc.w;
    Z.h = g_doc.h;
}

void cmd_deselect(void) {
    commit_float();
    Z.active = false;
    Z.dragging_new = Z.moving = false;
}

void cmd_crop_to_selection(void) {
    if (!Z.active || Z.dragging_new) return;
    tools_commit_all();
    uint32_t *np = img_make_crop(Z.x, Z.y, Z.w, Z.h);
    if (!np) return;
    int nw = Z.w, nh = Z.h;
    Z.active = false;
    hist_replace("Crop", np, nw, nh);
}
