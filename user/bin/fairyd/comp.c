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

/* The sprites are drawn at the ui scale (26 x 26 physical pixels at scale 1, 52 x 52 at scale 2): the same shapes at a finer
 * resolution, not enlarged pixels. */
#define SPR1 26
#define SPR_MAX_SCALE 4
typedef struct { uint8_t px[SPR1 * SPR_MAX_SCALE * SPR1 * SPR_MAX_SCALE]; int hotx, hoty; } Sprite;
static Sprite g_spr[FC_NONE + 1];
static int SPR = SPR1;                          /* sprite side in physical pixels: SPR1 * the sprite scale */
static int g_k = 1;                             /* the sprite scale */

typedef struct { int x, y; } Pt;

static void scale_pts(Pt *p, int n) { for (int i = 0; i < n; i++) { p[i].x *= g_k; p[i].y *= g_k; } }

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
    static uint8_t out[sizeof s->px];
    size_t bytes = (size_t)SPR * SPR;
    memcpy(out, s->px, bytes);
    for (int y = 0; y < SPR; y++)
        for (int x = 0; x < SPR; x++) {
            if (s->px[y * SPR + x]) continue;
            for (int dy = -g_k; dy <= g_k; dy++)                 /* the black outline is one logical pixel wide */
                for (int dx = -g_k; dx <= g_k; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < SPR && ny < SPR && s->px[ny * SPR + nx] == 1) out[y * SPR + x] = 2;
                }
        }
    memcpy(s->px, out, bytes);
    s->hotx = hotx * g_k;
    s->hoty = hoty * g_k;
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

static void build_cursors(int scale) {
    memset(g_spr, 0, sizeof g_spr);
    g_k = scale < 1 ? 1 : scale > SPR_MAX_SCALE ? SPR_MAX_SCALE : scale;
    SPR = SPR1 * g_k;
    const int K = g_k;
    {   /* arrow */
        Pt p[] = { { 1, 1 }, { 1, 18 }, { 5, 14 }, { 8, 20 }, { 11, 19 }, { 8, 13 }, { 13, 13 } };
        scale_pts(p, 7);
        mask_poly(g_spr[FC_ARROW].px, p, 7);
        finish_sprite(&g_spr[FC_ARROW], 1, 1);
    }
    {   /* text I-beam */
        uint8_t *m = g_spr[FC_TEXT].px;
        mask_rect(m, 5 * K, 2 * K, 2 * K, 17 * K); mask_rect(m, 2 * K, 1 * K, 8 * K, 2 * K); mask_rect(m, 2 * K, 18 * K, 8 * K, 2 * K);
        finish_sprite(&g_spr[FC_TEXT], 6, 10);
    }
    {   /* hand */
        Pt p[] = { { 7, 1 }, { 10, 1 }, { 10, 9 }, { 12, 8 }, { 14, 8 }, { 16, 9 }, { 18, 9 }, { 20, 11 }, { 20, 17 }, { 17, 21 }, { 9, 21 }, { 5, 15 }, { 3, 12 }, { 5, 11 }, { 7, 13 } };
        scale_pts(p, 15);
        mask_poly(g_spr[FC_HAND].px, p, 15);
        finish_sprite(&g_spr[FC_HAND], 8, 1);
    }
    {   /* busy: hourglass */
        Pt top[] = { { 5, 2 }, { 19, 2 }, { 13, 11 }, { 11, 11 } };
        Pt bot[] = { { 11, 12 }, { 13, 12 }, { 19, 21 }, { 5, 21 } };
        scale_pts(top, 4);
        scale_pts(bot, 4);
        mask_poly(g_spr[FC_BUSY].px, top, 4);
        mask_poly(g_spr[FC_BUSY].px, bot, 4);
        finish_sprite(&g_spr[FC_BUSY], 12, 11);
    }
    {   /* resize arrows */
        Pt h[] = { { 1, 12 }, { 7, 6 }, { 7, 10 }, { 17, 10 }, { 17, 6 }, { 23, 12 }, { 17, 18 }, { 17, 14 }, { 7, 14 }, { 7, 18 } };
        Pt hh[10]; memcpy(hh, h, sizeof h);
        scale_pts(hh, 10);
        mask_poly(g_spr[FC_RESIZE_H].px, hh, 10);
        finish_sprite(&g_spr[FC_RESIZE_H], 12, 12);
        Pt v[10]; memcpy(v, h, sizeof h);
        rotate_pts(v, 10, 12, 12, 2);
        scale_pts(v, 10);
        mask_poly(g_spr[FC_RESIZE_V].px, v, 10);
        finish_sprite(&g_spr[FC_RESIZE_V], 12, 12);
        Pt d1[10]; memcpy(d1, h, sizeof h);
        rotate_pts(d1, 10, 12, 12, 1);
        scale_pts(d1, 10);
        mask_poly(g_spr[FC_RESIZE_NWSE].px, d1, 10);
        finish_sprite(&g_spr[FC_RESIZE_NWSE], 12, 12);
        Pt d2[10]; memcpy(d2, h, sizeof h);
        rotate_pts(d2, 10, 12, 12, 3);
        scale_pts(d2, 10);
        mask_poly(g_spr[FC_RESIZE_NESW].px, d2, 10);
        finish_sprite(&g_spr[FC_RESIZE_NESW], 12, 12);
        Pt m1[10]; memcpy(m1, h, sizeof h);
        Pt m2[10]; memcpy(m2, h, sizeof h);
        rotate_pts(m2, 10, 12, 12, 2);
        scale_pts(m1, 10);
        scale_pts(m2, 10);
        mask_poly(g_spr[FC_MOVE].px, m1, 10);
        mask_poly(g_spr[FC_MOVE].px, m2, 10);
        finish_sprite(&g_spr[FC_MOVE], 12, 12);
    }
    {   /* crosshair */
        uint8_t *m = g_spr[FC_CROSS].px;
        mask_rect(m, 11 * K, 3 * K, 2 * K, 18 * K); mask_rect(m, 3 * K, 11 * K, 18 * K, 2 * K);
        finish_sprite(&g_spr[FC_CROSS], 12, 12);
    }
}

/* The pointer sprite in physical pixels. */
static Rect cursor_rect_phys(void) {
    Sprite *sp = &g_spr[S.cursor_shape <= FC_NONE ? S.cursor_shape : FC_ARROW];
    Rect r = { S.ptr_x - sp->hotx, S.ptr_y - sp->hoty, SPR, SPR };
    return r;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* Physical rectangle -> the logical rectangle that covers it (damage is kept in logical pixels). */
static Rect logical_cover(Rect p) {
    int s = S.scale;
    int x0 = floor_div(p.x, s), y0 = floor_div(p.y, s);
    int x1 = floor_div(p.x + p.w + s - 1, s), y1 = floor_div(p.y + p.h + s - 1, s);
    return (Rect){ x0, y0, x1 - x0, y1 - y0 };
}

static Rect cursor_rect(void) { return logical_cover(cursor_rect_phys()); }

/* Moves the pointer to physical (px, py); the logical position is the logical pixel under it. */
static void pointer_set_phys(int px, int py) {
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= S.phys_w) px = S.phys_w - 1;
    if (py >= S.phys_h) py = S.phys_h - 1;
    if (px == S.ptr_x && py == S.ptr_y) return;
    comp_damage(cursor_rect());
    S.ptr_x = px;
    S.ptr_y = py;
    S.cursor_x = px / S.scale;
    S.cursor_y = py / S.scale;
    comp_damage(cursor_rect());
}

void cursor_move_to(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= S.screen_w) x = S.screen_w - 1;
    if (y >= S.screen_h) y = S.screen_h - 1;
    pointer_set_phys(x * S.scale + S.scale / 2, y * S.scale + S.scale / 2);
}

void cursor_move_by(int dx, int dy) { pointer_set_phys(S.ptr_x + dx, S.ptr_y + dy); }

void cursor_set_shape(unsigned shape) {
    if (shape > FC_NONE) shape = FC_ARROW;
    if (shape == S.cursor_shape) return;
    comp_damage(cursor_rect());
    S.cursor_shape = shape;
    comp_damage(cursor_rect());
}

static Rect to_phys(Rect r) { return (Rect){ r.x * S.scale, r.y * S.scale, r.w * S.scale, r.h * S.scale }; }

static void draw_cursor(Rect clip) {
    if (S.cursor_shape == FC_NONE) return;
    Sprite *sp = &g_spr[S.cursor_shape];
    Rect cr = cursor_rect_phys();
    Rect v = rect_intersect(cr, to_phys(clip));
    for (int y = v.y; y < v.y + v.h; y++)
        for (int x = v.x; x < v.x + v.w; x++) {
            uint8_t p = sp->px[(y - cr.y) * SPR + (x - cr.x)];
            if (p) S.back[y * S.phys_w + x] = p == 1 ? 0xFFFFFF : 0x000000;
        }
}

/* ---------------------------------------------------------------- init and damage */

/* Pending damage is a short list of rectangles, not one bounding box: a pointer in one corner and a clock tick in the opposite one
 * must not cost a full-screen compose and a full-screen write to the framebuffer. S.dirty stays the bounding box of the list (the
 * main loop only asks whether it is empty). fairyd.damage=bbox on the kernel command line goes back to the single rectangle,
 * for comparing. */
#define MAX_DAMAGE 12
static Rect g_dmg[MAX_DAMAGE];
static int g_ndmg;
static bool g_dmg_bbox_only;

/* The framebuffer is display memory mapped write-combining (or worse, uncached): the copy to it must be wide stores, sequential,
 * and never read back. fairyd.copy=rep uses the plain byte-string copy instead, for comparing. */
static bool g_copy_nt = true;

static struct {
    uint64_t frames, rects, pixels, compose_cy, flush_cy, worst_cy;
    uint64_t tsc0, ms0, last_report_ms;
} g_st;

static inline uint64_t rdtsc_u(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int g_scale_asked;                      /* ui.scale=N on the kernel command line, 0 = choose by the screen size */

static void read_knobs(void) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return;
    char line[400];
    if (fgets(line, sizeof line, f)) {
        for (char *p = strtok(line, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
            if (!strcmp(p, "fairyd.copy=rep")) g_copy_nt = false;
            else if (!strcmp(p, "fairyd.damage=bbox")) g_dmg_bbox_only = true;
            else if (!strncmp(p, "ui.scale=", 9)) g_scale_asked = atoi(p + 9);
        }
    }
    fclose(f);
}

/* The ui scale (physical pixels per logical pixel). Automatic: 2 when the screen is at least 2560 wide or 1440 high (a 27-inch
 * 2560x1440 monitor, 4K), else 1. ui.scale=N on the kernel command line forces N (1 to 4). Either way the logical screen must
 * stay at least 640x480, which every application promises to fit in: a scale that would make it smaller is lowered. */
#define UI_SCALE_AUTO_W 2560
#define UI_SCALE_AUTO_H 1440
#define UI_SCALE_MAX    SPR_MAX_SCALE
static int choose_scale(int w, int h, const char **why) {
    int s;
    if (g_scale_asked >= 1) { s = g_scale_asked > UI_SCALE_MAX ? UI_SCALE_MAX : g_scale_asked; *why = "ui.scale"; }
    else { s = (w >= UI_SCALE_AUTO_W || h >= UI_SCALE_AUTO_H) ? 2 : 1; *why = "automatic"; }
    while (s > 1 && (w / s < 640 || h / s < 480)) s--;
    return s;
}

void comp_init(int w, int h, uint32_t *fb, int pitch, bool swap_rb) {
    read_knobs();
    const char *why;
    int s = choose_scale(w, h, &why);
    S.scale = s;
    S.screen_w = w / s;
    S.screen_h = h / s;
    S.phys_w = S.screen_w * s;
    S.phys_h = S.screen_h * s;
    S.back = calloc((size_t)S.phys_w * (size_t)S.phys_h, 4);
    S.fb = fb;
    S.fb_pitch = pitch;
    S.swap_rb = swap_rb;
    kc_init_scaled(&g_cv, S.back, S.screen_w, S.screen_h, S.phys_w, s);
    S.ptr_x = w / 2 < S.phys_w ? w / 2 : S.phys_w - 1;
    S.ptr_y = h / 2 < S.phys_h ? h / 2 : S.phys_h - 1;
    S.cursor_x = S.ptr_x / s;
    S.cursor_y = S.ptr_y / s;
    S.cursor_shape = FC_ARROW;
    build_cursors(s);
    w = S.screen_w;
    h = S.screen_h;
    fprintf(stderr, "fairyd: ui scale %d (%s)\n", s, why);
    Rect all = { 0, 0, w, h };
    S.dirty = all;
    g_dmg[0] = all;
    g_ndmg = 1;
    g_st.tsc0 = rdtsc_u();
    g_st.ms0 = k_now_ms();
    fprintf(stderr, "fairyd: framebuffer copy: %s, damage: %s\n", g_copy_nt ? "non-temporal 16-byte stores" : "rep movsb",
            g_dmg_bbox_only ? "one bounding box" : "rectangle list");
}

static long rect_area(Rect r) { return (long)r.w * r.h; }

/* Damage is kept in logical pixels; compose() and flush_rect() turn it into physical ones. */
void comp_damage(Rect r) {
    Rect screen = { 0, 0, S.screen_w, S.screen_h };
    r = rect_intersect(r, screen);
    if (rect_empty(r)) return;
    S.dirty = rect_union(S.dirty, r);
    if (g_dmg_bbox_only) { g_dmg[0] = S.dirty; g_ndmg = 1; return; }
    for (;;) {
        /* Merge r with any rectangle it overlaps or touches when that wastes little; a rectangle inside r simply disappears. */
        int best = -1;
        long best_waste = 0;
        for (int i = 0; i < g_ndmg; i++) {
            Rect u = rect_union(g_dmg[i], r);
            Rect x = rect_intersect(g_dmg[i], r);
            long waste = rect_area(u) - (rect_area(g_dmg[i]) + rect_area(r) - rect_area(x));
            long allowed = (rect_area(g_dmg[i]) + rect_area(r)) / 4;
            if (allowed < 4096) allowed = 4096;
            if (waste <= allowed && (best < 0 || waste < best_waste)) { best = i; best_waste = waste; }
        }
        if (best < 0 && g_ndmg == MAX_DAMAGE) {             /* the list is full: fold r into whatever it grows the least */
            for (int i = 0; i < g_ndmg; i++) {
                Rect u = rect_union(g_dmg[i], r);
                long waste = rect_area(u) - rect_area(g_dmg[i]) - rect_area(r);
                if (best < 0 || waste < best_waste) { best = i; best_waste = waste; }
            }
        }
        if (best < 0) { g_dmg[g_ndmg++] = r; return; }
        r = rect_union(g_dmg[best], r);
        g_dmg[best] = g_dmg[--g_ndmg];                      /* r replaces it; look again, the bigger r may now reach other rectangles */
    }
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
    r = to_phys(rect_intersect(r, (Rect){ 0, 0, S.screen_w, S.screen_h }));
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = S.back + (size_t)y * S.phys_w;
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t p = row[x];
            row[x] = (((p >> 16) & 255) * pct / 100) << 16 | (((p >> 8) & 255) * pct / 100) << 8 | ((p & 255) * pct / 100);
        }
    }
}

/* A window whose buffer has fewer pixels than the screen (a program that draws its own pixels and did not ask for FW_HIDPI):
 * every buffer pixel becomes a block of pixels on the screen (nearest neighbour). c is the content rectangle, cv the part to
 * paint, both logical. */
static void draw_content_enlarged(const Win *w, Rect c, Rect cv) {
    int k = S.scale / w->bscale;                                /* screen pixels per buffer pixel */
    if (k < 1) k = 1;
    Rect pc = to_phys(c), pv = to_phys(cv);
    for (int y = pv.y; y < pv.y + pv.h; y++) {
        uint32_t *dst = S.back + (size_t)y * S.phys_w;
        int sy = (y - pc.y) / k;
        const uint32_t *src = w->buf && sy < w->bh ? w->buf + (size_t)sy * w->bstride : NULL;
        int x = pv.x, x_end = pv.x + pv.w;
        int sx = (x - pc.x) / k, sub = (x - pc.x) % k;
        while (x < x_end) {
            uint32_t p = src && sx < w->bw ? src[sx] : TH->window_bg;
            int run = k - sub;
            if (run > x_end - x) run = x_end - x;
            for (int i = 0; i < run; i++) dst[x + i] = p;
            x += run;
            sx++;
            sub = 0;
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
    if (w->bscale != S.scale) { draw_content_enlarged(w, c, cv); return; }
    c = to_phys(c);                                             /* the buffer is at the screen's scale: copy it pixel for pixel */
    cv = to_phys(cv);
    for (int y = cv.y; y < cv.y + cv.h; y++) {
        uint32_t *dst = S.back + (size_t)y * S.phys_w + cv.x;
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

/* Copies `n` pixels to the framebuffer with 16-byte non-temporal stores (movntdq): on write-combining memory each group of four
 * fills a quarter of a cache line and the line goes out as one burst; on uncached memory it is still a quarter of the bus
 * transactions of the dword stores a byte-string copy ends up doing there. Sequential and never reading the destination. */
static void copy_to_fb(uint32_t *dst, const uint32_t *src, size_t n) {
    while (n && ((uintptr_t)dst & 15)) { *dst++ = *src++; n--; }
    while (n >= 16) {
        __asm__ volatile (
            "movdqu    (%1), %%xmm0\n\t"
            "movdqu  16(%1), %%xmm1\n\t"
            "movdqu  32(%1), %%xmm2\n\t"
            "movdqu  48(%1), %%xmm3\n\t"
            "movntdq %%xmm0,   (%0)\n\t"
            "movntdq %%xmm1, 16(%0)\n\t"
            "movntdq %%xmm2, 32(%0)\n\t"
            "movntdq %%xmm3, 48(%0)\n\t"
            : : "r"(dst), "r"(src) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
        dst += 16; src += 16; n -= 16;
    }
    while (n >= 4) {
        __asm__ volatile ("movdqu (%1), %%xmm0\n\tmovntdq %%xmm0, (%0)" : : "r"(dst), "r"(src) : "xmm0", "memory");
        dst += 4; src += 4; n -= 4;
    }
    while (n) { *dst++ = *src++; n--; }
}

static void flush_rect(Rect r) {
    r = to_phys(r);
    static uint32_t *swapped;                                       /* one row with red and blue exchanged, for RGBX displays */
    static int swapped_len;
    if (S.swap_rb && swapped_len < r.w) {
        uint32_t *bigger = realloc(swapped, (size_t)r.w * 4);
        if (!bigger) return;
        swapped = bigger;
        swapped_len = r.w;
    }
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *src = S.back + (size_t)y * S.phys_w + r.x;
        uint32_t *dst = (uint32_t *)((uint8_t *)S.fb + (size_t)y * S.fb_pitch) + r.x;
        if (S.swap_rb) {
            for (int x = 0; x < r.w; x++) {
                uint32_t p = src[x];
                swapped[x] = (p & 0xFF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
            }
            src = swapped;
        }
        if (g_copy_nt) copy_to_fb(dst, src, (size_t)r.w);
        else memcpy(dst, src, (size_t)r.w * 4);
    }
}

/* /tmp/fairyd.stat: what a frame costs, rewritten every two seconds. On a PC whose desktop feels slow this says whether the time goes
 * into painting (compose) or into the framebuffer writes (flush), and how fast those writes really are. */
static void report_stats(void) {
    uint64_t ms = k_now_ms() - g_st.ms0;
    if (ms < 500 || !g_st.frames) return;
    uint64_t cy_per_us = (rdtsc_u() - g_st.tsc0) / (ms * 1000);
    if (!cy_per_us) return;
    uint64_t compose_us = g_st.compose_cy / cy_per_us, flush_us = g_st.flush_cy / cy_per_us, worst_us = g_st.worst_cy / cy_per_us;
    uint64_t mbps = flush_us ? g_st.pixels * 4 / flush_us : 0;                 /* bytes per microsecond is MB/s */
    FILE *f = fopen("/tmp/fairyd.stat", "w");
    if (!f) return;
    fprintf(f, "screen %dx%d, copy: %s, damage: %s, ui scale %d\n", S.phys_w, S.phys_h, g_copy_nt ? "nt" : "rep", g_dmg_bbox_only ? "bbox" : "list", S.scale);
    fprintf(f, "frames %llu, rectangles %llu, %llu kpixels (%llu per frame)\n", (unsigned long long)g_st.frames, (unsigned long long)g_st.rects,
            (unsigned long long)(g_st.pixels / 1000), (unsigned long long)(g_st.pixels / 1000 / g_st.frames));
    fprintf(f, "compose %llu us per frame, flush %llu us per frame (~%llu MB/s), worst frame %llu us\n", (unsigned long long)(compose_us / g_st.frames),
            (unsigned long long)(flush_us / g_st.frames), (unsigned long long)mbps, (unsigned long long)worst_us);
    fclose(f);
}

void comp_flush(void) {
    if (g_ndmg == 0) return;
    Rect list[MAX_DAMAGE];
    int n = g_ndmg;
    memcpy(list, g_dmg, (size_t)n * sizeof(Rect));
    g_ndmg = 0;
    S.dirty = (Rect){ 0, 0, 0, 0 };
    uint64_t t0 = rdtsc_u();
    for (int i = 0; i < n; i++) compose(list[i]);
    uint64_t t1 = rdtsc_u();
    for (int i = 0; i < n; i++) flush_rect(list[i]);
    __asm__ volatile ("sfence" ::: "memory");                       /* push out the last write-combining buffers now, not when something evicts them */
    uint64_t t2 = rdtsc_u();
    g_st.frames++;
    g_st.rects += (uint64_t)n;
    for (int i = 0; i < n; i++) g_st.pixels += (uint64_t)rect_area(list[i]) * (uint64_t)(S.scale * S.scale);
    g_st.compose_cy += t1 - t0;
    g_st.flush_cy += t2 - t1;
    if (t2 - t0 > g_st.worst_cy) g_st.worst_cy = t2 - t0;
    if (S.now_ms - g_st.last_report_ms >= 2000) {
        g_st.last_report_ms = S.now_ms;
        report_stats();
    }
}

static void snapshot_ppm(const char *path) {
    char tmp[sizeof(((FmPath *)0)->path) + 8];                      /* written under another name and renamed, so the file is never seen half-done */
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", S.phys_w, S.phys_h);
    uint8_t *row = malloc((size_t)S.phys_w * 3);
    if (row) {
        for (int y = 0; y < S.phys_h; y++) {
            for (int x = 0; x < S.phys_w; x++) {
                uint32_t p = S.back[(size_t)y * S.phys_w + x];
                row[x * 3] = (uint8_t)(p >> 16); row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)p;
            }
            fwrite(row, 1, (size_t)S.phys_w * 3, f);
        }
        free(row);
    }
    if (fclose(f) == 0) rename(tmp, path);
    else unlink(tmp);
}

/* The screen as a libimg picture, then PNG (kind 0) or BMP. */
static int snapshot_image(const char *path, int bmp) {
    Image im;
    if (img_alloc(&im, S.phys_w, S.phys_h) != IMG_OK) return -1;
    for (size_t i = 0; i < (size_t)S.phys_w * S.phys_h; i++) im.px[i] = 0xFF000000u | S.back[i];
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
        if (rc == 0) snprintf(note, sizeof note, "Aya took a photo of the screen (%dx%d): %s", S.phys_w, S.phys_h, path);
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
