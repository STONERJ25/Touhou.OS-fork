/* The compositor: paints desktop, windows, frames, the Alt+Tab switcher and the pointer into a back buffer, then copies the
 * damaged part to the framebuffer. */
#include "fairyd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <strings.h>
#include <img.h>
#include <kappa_media.h>
#include <gensokyo.h>

static KCanvas g_cv;

bool rect_empty(Rect r) { return r.w <= 0 || r.h <= 0; }

Rect rect_intersect(Rect a, Rect b) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    Rect r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return r;
}

Rect rect_union(Rect a, Rect b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    Rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* ---------------------------------------------------------------- pointer sprites (generated at start-up) */

#define SPR 26
typedef struct { uint8_t px[SPR * SPR]; int hotx, hoty; } Sprite;
static Sprite g_spr[FC_NONE + 1];

typedef struct { int x, y; } Pt;

static void mask_poly(uint8_t *m, const Pt *p, int n) {
    for (int y = 0; y < SPR; y++) {
        int xs[32], nx = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if ((p[i].y <= y && p[j].y > y) || (p[j].y <= y && p[i].y > y))
                if (nx < 32) xs[nx++] = p[i].x + (y - p[i].y) * (p[j].x - p[i].x) / (p[j].y - p[i].y);
        }
        for (int a = 1; a < nx; a++)
            for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int k = 0; k + 1 < nx; k += 2)
            for (int x = xs[k]; x <= xs[k + 1] && x < SPR; x++) if (x >= 0) m[y * SPR + x] = 1;
    }
}

static void mask_rect(uint8_t *m, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) if (i >= 0 && j >= 0 && i < SPR && j < SPR) m[j * SPR + i] = 1;
}

static void finish_sprite(Sprite *s, int hotx, int hoty) {
    uint8_t out[SPR * SPR];
    memcpy(out, s->px, sizeof out);
    for (int y = 0; y < SPR; y++)
        for (int x = 0; x < SPR; x++) {
            if (s->px[y * SPR + x]) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < SPR && ny < SPR && s->px[ny * SPR + nx] == 1) out[y * SPR + x] = 2;
                }
        }
    memcpy(s->px, out, sizeof out);
    s->hotx = hotx;
    s->hoty = hoty;
}

static void rotate_pts(Pt *p, int n, int cx, int cy, int quarter_turns_45) {
    static const int c[8] = { 1000, 707, 0, -707, -1000, -707, 0, 707 };
    static const int s[8] = { 0, 707, 1000, 707, 0, -707, -1000, -707 };
    int k = quarter_turns_45 & 7;
    for (int i = 0; i < n; i++) {
        int dx = p[i].x - cx, dy = p[i].y - cy;
        p[i].x = cx + (dx * c[k] - dy * s[k]) / 1000;
        p[i].y = cy + (dx * s[k] + dy * c[k]) / 1000;
    }
}

static void build_cursors(void) {
    memset(g_spr, 0, sizeof g_spr);
    {   /* arrow */
        Pt p[] = { { 1, 1 }, { 1, 18 }, { 5, 14 }, { 8, 20 }, { 11, 19 }, { 8, 13 }, { 13, 13 } };
        mask_poly(g_spr[FC_ARROW].px, p, 7);
        finish_sprite(&g_spr[FC_ARROW], 1, 1);
    }
    {   /* text I-beam */
        uint8_t *m = g_spr[FC_TEXT].px;
        mask_rect(m, 5, 2, 2, 17); mask_rect(m, 2, 1, 8, 2); mask_rect(m, 2, 18, 8, 2);
        finish_sprite(&g_spr[FC_TEXT], 6, 10);
    }
    {   /* hand */
        Pt p[] = { { 7, 1 }, { 10, 1 }, { 10, 9 }, { 12, 8 }, { 14, 8 }, { 16, 9 }, { 18, 9 }, { 20, 11 }, { 20, 17 }, { 17, 21 }, { 9, 21 }, { 5, 15 }, { 3, 12 }, { 5, 11 }, { 7, 13 } };
        mask_poly(g_spr[FC_HAND].px, p, 15);
        finish_sprite(&g_spr[FC_HAND], 8, 1);
    }
    {   /* busy: hourglass */
        Pt top[] = { { 5, 2 }, { 19, 2 }, { 13, 11 }, { 11, 11 } };
        Pt bot[] = { { 11, 12 }, { 13, 12 }, { 19, 21 }, { 5, 21 } };
        mask_poly(g_spr[FC_BUSY].px, top, 4);
        mask_poly(g_spr[FC_BUSY].px, bot, 4);
        finish_sprite(&g_spr[FC_BUSY], 12, 11);
    }
    {   /* resize arrows */
        Pt h[] = { { 1, 12 }, { 7, 6 }, { 7, 10 }, { 17, 10 }, { 17, 6 }, { 23, 12 }, { 17, 18 }, { 17, 14 }, { 7, 14 }, { 7, 18 } };
        Pt hh[10]; memcpy(hh, h, sizeof h);
        mask_poly(g_spr[FC_RESIZE_H].px, hh, 10);
        finish_sprite(&g_spr[FC_RESIZE_H], 12, 12);
        Pt v[10]; memcpy(v, h, sizeof h);
        rotate_pts(v, 10, 12, 12, 2);
        mask_poly(g_spr[FC_RESIZE_V].px, v, 10);
        finish_sprite(&g_spr[FC_RESIZE_V], 12, 12);
        Pt d1[10]; memcpy(d1, h, sizeof h);
        rotate_pts(d1, 10, 12, 12, 1);
        mask_poly(g_spr[FC_RESIZE_NWSE].px, d1, 10);
        finish_sprite(&g_spr[FC_RESIZE_NWSE], 12, 12);
        Pt d2[10]; memcpy(d2, h, sizeof h);
        rotate_pts(d2, 10, 12, 12, 3);
        mask_poly(g_spr[FC_RESIZE_NESW].px, d2, 10);
        finish_sprite(&g_spr[FC_RESIZE_NESW], 12, 12);
        Pt m1[10]; memcpy(m1, h, sizeof h);
        Pt m2[10]; memcpy(m2, h, sizeof h);
        rotate_pts(m2, 10, 12, 12, 2);
        mask_poly(g_spr[FC_MOVE].px, m1, 10);
        mask_poly(g_spr[FC_MOVE].px, m2, 10);
        finish_sprite(&g_spr[FC_MOVE], 12, 12);
    }
    {   /* crosshair */
        uint8_t *m = g_spr[FC_CROSS].px;
        mask_rect(m, 11, 3, 2, 18); mask_rect(m, 3, 11, 18, 2);
        finish_sprite(&g_spr[FC_CROSS], 12, 12);
    }
}

static Rect cursor_rect(void) {
    Sprite *sp = &g_spr[S.cursor_shape <= FC_NONE ? S.cursor_shape : FC_ARROW];
    Rect r = { S.cursor_x - sp->hotx, S.cursor_y - sp->hoty, SPR, SPR };
    return r;
}

void cursor_move_to(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= S.screen_w) x = S.screen_w - 1;
    if (y >= S.screen_h) y = S.screen_h - 1;
    if (x == S.cursor_x && y == S.cursor_y) return;
    comp_damage(cursor_rect());
    S.cursor_x = x;
    S.cursor_y = y;
    comp_damage(cursor_rect());
}

void cursor_set_shape(unsigned shape) {
    if (shape > FC_NONE) shape = FC_ARROW;
    if (shape == S.cursor_shape) return;
    comp_damage(cursor_rect());
    S.cursor_shape = shape;
    comp_damage(cursor_rect());
}

static void draw_cursor(Rect clip) {
    if (S.cursor_shape == FC_NONE) return;
    Sprite *sp = &g_spr[S.cursor_shape];
    Rect cr = cursor_rect();
    Rect v = rect_intersect(cr, clip);
    for (int y = v.y; y < v.y + v.h; y++)
        for (int x = v.x; x < v.x + v.w; x++) {
            uint8_t p = sp->px[(y - cr.y) * SPR + (x - cr.x)];
            if (p) S.back[y * S.screen_w + x] = p == 1 ? 0xFFFFFF : 0x000000;
        }
}

/* ---------------------------------------------------------------- init and damage */

void comp_init(int w, int h, uint32_t *fb, int pitch, bool swap_rb) {
    S.screen_w = w;
    S.screen_h = h;
    S.back = calloc((size_t)w * (size_t)h, 4);
    S.fb = fb;
    S.fb_pitch = pitch;
    S.swap_rb = swap_rb;
    kc_init(&g_cv, S.back, w, h, w);
    S.cursor_x = w / 2;
    S.cursor_y = h / 2;
    S.cursor_shape = FC_ARROW;
    build_cursors();
    Rect all = { 0, 0, w, h };
    S.dirty = all;
}

void comp_damage(Rect r) {
    Rect screen = { 0, 0, S.screen_w, S.screen_h };
    r = rect_intersect(r, screen);
    if (rect_empty(r)) return;
    S.dirty = rect_union(S.dirty, r);
}

void comp_damage_win(const Win *w) {
    Rect o = win_outer(w);
    if (win_has_decor(w) && w->state != FS_MAXIMIZED) { o.w += 6; o.h += 6; }      /* drop shadow */
    comp_damage(o);
}

/* ---------------------------------------------------------------- window painting */

static const KTheme *TH = &k_theme_shrine;

static void draw_frame(const Win *w, Rect clip) {
    KCanvas *cv = &g_cv;
    kc_set_clip(cv, clip);
    Rect o = win_outer(w);
    bool focused = S.focus == w;
    bool maxed = w->state == FS_MAXIMIZED;
    KRect orect = kr(o.x, o.y, o.w, o.h);
    kc_fill(cv, orect, kc_shade(TH->window_bg, 94));
    if (!maxed) kc_bevel(cv, orect, KB_RAISED);
    int tb = maxed ? 0 : BORDER;
    KRect title = kr(o.x + tb, o.y + tb, o.w - 2 * tb, TITLE_H);
    uint32_t a = focused ? TH->title_active_a : TH->title_inactive_a, b = focused ? TH->title_active_b : TH->title_inactive_b;
    kc_gradient_h(cv, title, a, b);
    kc_hline(cv, title.x, title.y + title.h - 1, title.w, kc_shade(b, 70));
    k_icon(cv, KI_YINYANG, title.x + 3, title.y + 3, 16);
    /* buttons, right to left */
    static const struct { int zone; KIcon icon; } btn[3] = { { Z_CLOSE, KI_CLOSE }, { Z_MAX, KI_MAXIMIZE }, { Z_MIN, KI_MINIMIZE } };
    int bx = title.x + title.w - 3;
    for (int i = 0; i < 3; i++) {
        Rect br;
        if (!win_button(w, btn[i].zone, &br)) continue;
        bool dim = btn[i].zone == Z_CLOSE && (w->flags & FW_NO_CLOSE);
        kc_panel(cv, br, KB_RAISED, dim ? kc_shade(TH->button_bg, 85) : TH->button_bg);
        KIcon ic = btn[i].icon;
        if (btn[i].zone == Z_MAX && maxed) ic = KI_RESTORE;
        k_icon(cv, ic, br.x + 1, br.y + 1, 16);
        bx = br.x - 2;
    }
    int tx = title.x + 24;
    int avail = bx - tx;
    if (avail > 8) {
        KRect old = cv->clip;
        kc_set_clip(cv, kr_intersect(old, kr(tx, title.y, avail, TITLE_H)));
        char buf[80];
        int maxcells = avail / 8;
        size_t n = kc_utf8_len(w->title);
        if ((int)n > maxcells) {
            size_t off = kc_utf8_offset(w->title, (size_t)(maxcells > 3 ? maxcells - 3 : 0));
            memcpy(buf, w->title, off < 76 ? off : 76);
            strcpy(buf + (off < 76 ? off : 76), "...");
        } else {
            strncpy(buf, w->title, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
        }
        uint32_t fg = focused ? TH->title_text : TH->title_text_inactive;
        kc_text(cv, tx + 1, title.y + 4, buf, kc_shade(b, 60));
        kc_text_bold(cv, tx, title.y + 3, buf, fg);
        cv->clip = old;
    }
}

static void shade_rect(Rect r, int pct) {
    r = rect_intersect(r, (Rect){ 0, 0, S.screen_w, S.screen_h });
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = S.back + (size_t)y * S.screen_w;
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t p = row[x];
            row[x] = (((p >> 16) & 255) * pct / 100) << 16 | (((p >> 8) & 255) * pct / 100) << 8 | ((p & 255) * pct / 100);
        }
    }
}

static void draw_window(const Win *w, Rect clip) {
    Rect o = win_outer(w);
    bool decor = win_has_decor(w);
    if (decor && w->state != FS_MAXIMIZED) {
        Rect sh = { o.x + 5, o.y + 5, o.w, o.h };
        Rect v = rect_intersect(sh, clip);
        if (!rect_empty(v)) shade_rect(v, 62);
    }
    Rect vis = rect_intersect(o, clip);
    if (rect_empty(vis)) return;
    if (decor) draw_frame(w, vis);
    Rect c = win_content(w);
    Rect cv = rect_intersect(c, clip);
    if (rect_empty(cv)) return;
    for (int y = cv.y; y < cv.y + cv.h; y++) {
        uint32_t *dst = S.back + (size_t)y * S.screen_w + cv.x;
        int sy = y - c.y;
        int sx = cv.x - c.x;
        if (w->buf && sy < w->bh) {
            int n = cv.w;
            int have = w->bw - sx;
            if (have > n) have = n;
            if (have > 0) memcpy(dst, w->buf + (size_t)sy * w->bstride + sx, (size_t)have * 4);
            if (have < n) for (int i = have > 0 ? have : 0; i < n; i++) dst[i] = TH->window_bg;
        } else {
            for (int i = 0; i < cv.w; i++) dst[i] = TH->window_bg;
        }
    }
}

static void draw_switcher(Rect clip) {
    if (!S.switching || S.switch_n == 0) return;
    int rows = S.switch_n;
    int w = 360, h = 40 + rows * 26;
    Rect box = { (S.screen_w - w) / 2, (S.screen_h - h) / 2, w, h };
    if (rect_empty(rect_intersect(box, clip))) return;
    KCanvas *cv = &g_cv;
    kc_set_clip(cv, clip);
    kc_fill_alpha(cv, kr(box.x + 4, box.y + 4, box.w, box.h), 0x000000, 120);
    kc_panel(cv, kr(box.x, box.y, box.w, box.h), KB_RAISED, TH->window_bg);
    kc_text_bold(cv, box.x + 14, box.y + 10, "Switch window", TH->accent_dark);
    for (int i = 0; i < rows; i++) {
        KRect rr = kr(box.x + 8, box.y + 32 + i * 26, box.w - 16, 24);
        bool sel = i == S.switch_index;
        if (sel) kc_fill(cv, rr, TH->select_bg);
        k_icon(cv, KI_YINYANG, rr.x + 4, rr.y + 4, 16);
        kc_text_in(cv, kr(rr.x + 28, rr.y, rr.w - 32, rr.h), S.switch_list[i]->title, sel ? TH->select_text : TH->text, KA_LEFT);
    }
}

static void draw_background(Rect r) {
    KCanvas *cv = &g_cv;
    kc_set_clip(cv, r);
    kc_gradient_v(cv, kr(0, 0, S.screen_w, S.screen_h), 0x0A0F2A, 0x24356E);
}

static void compose(Rect r) {
    draw_background(r);
    for (int i = 0; i < S.nwins; i++) {
        const Win *w = S.wins[i];
        if (!w->visible) continue;
        draw_window(w, r);
    }
    draw_switcher(r);
    draw_cursor(r);
}

static void flush_rect(Rect r) {
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = S.back + (size_t)y * S.screen_w + r.x;
        uint32_t *dst = (uint32_t *)((uint8_t *)S.fb + (size_t)y * S.fb_pitch) + r.x;
        if (!S.swap_rb) memcpy(dst, src, (size_t)r.w * 4);
        else for (int x = 0; x < r.w; x++) {
            uint32_t p = src[x];
            dst[x] = (p & 0xFF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        }
    }
}

void comp_flush(void) {
    if (rect_empty(S.dirty)) return;
    Rect r = S.dirty;
    S.dirty = (Rect){ 0, 0, 0, 0 };
    compose(r);
    flush_rect(r);
}

static void snapshot_ppm(const char *path) {
    char tmp[sizeof(((FmPath *)0)->path) + 8];                      /* written under another name and renamed, so the file is never seen half-done */
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", S.screen_w, S.screen_h);
    uint8_t *row = malloc((size_t)S.screen_w * 3);
    if (row) {
        for (int y = 0; y < S.screen_h; y++) {
            for (int x = 0; x < S.screen_w; x++) {
                uint32_t p = S.back[(size_t)y * S.screen_w + x];
                row[x * 3] = (uint8_t)(p >> 16); row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)p;
            }
            fwrite(row, 1, (size_t)S.screen_w * 3, f);
        }
        free(row);
    }
    if (fclose(f) == 0) rename(tmp, path);
    else unlink(tmp);
}

/* The screen as a libimg picture, then PNG (kind 0) or BMP. */
static int snapshot_image(const char *path, int bmp) {
    Image im;
    if (img_alloc(&im, S.screen_w, S.screen_h) != IMG_OK) return -1;
    for (size_t i = 0; i < (size_t)S.screen_w * S.screen_h; i++) im.px[i] = 0xFF000000u | S.back[i];
    int rc = bmp ? img_save_bmp(path, &im) : img_save_png(path, &im, 2);
    img_free(&im);
    return rc;
}

/* Compressing a full screen takes a while on a slow machine, so a child process (a copy of the screen as it was at this
 * moment) does it and the compositor goes on painting. img_save_png writes "name.part" and renames, so the file only
 * appears under its real name when it is complete. */
static void snapshot_png_in_background(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        gap_setname("aya");
        int rc = snapshot_image(path, 0);
        char note[400];
        if (rc == 0) snprintf(note, sizeof note, "Aya took a photo of the screen (%dx%d): %s", S.screen_w, S.screen_h, path);
        else snprintf(note, sizeof note, "Aya's film jammed: could not write %s", path);
        gap_log(note);
        _exit(rc == 0 ? 0 : 1);
    }
    if (pid < 0) snapshot_image(path, 0);                           /* no process to spare: do it here */
}

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), e = strlen(ext);
    return n >= e && !strcasecmp(s + n - e, ext);
}

/* path "" = the defaults: /tmp/screenshot.ppm (what the tests and the Sketchbook use) and a PNG named by date in ~/Pictures. */
void comp_screenshot(const char *path) {
    if (!path || !path[0]) {
        snapshot_ppm("/tmp/screenshot.ppm");
        char png[640];
        k_screenshot_path(png, sizeof png);
        snapshot_png_in_background(png);
    } else if (ends_with(path, ".png")) snapshot_png_in_background(path);
    else if (ends_with(path, ".bmp")) snapshot_image(path, 1);
    else snapshot_ppm(path);
}
