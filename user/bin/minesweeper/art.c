#include "art.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    GRASS = 0x66B05A, GRASS_HI = 0xB2E58F, GRASS_LO = 0x38783F, GRASS_DK = 0x28592F,
    OPEN_BG = 0xE9E2CB, OPEN_LINE = 0xC5BC9F, OPEN_HI = 0xF6F1E1,
};

static const uint32_t NUM_COLORS[9] = { 0, 0x2347D8, 0x1E8A2E, 0xD42424, 0x16168C, 0x8A2410, 0x0E8A8A, 0x202020, 0x707070 };

static void bevel(KSprite *t, int s, uint32_t hi, uint32_t lo) {
    for (int i = 0; i < 2; i++) {
        for (int x = i; x < s - i; x++) { t->px[i * s + x] = hi; t->px[(s - 1 - i) * s + x] = lo; }
        for (int y = i; y < s - i; y++) { t->px[y * s + i] = hi; t->px[y * s + s - 1 - i] = lo; }
    }
    /* the bottom/right edges overwrite the corners of the top/left ones: keep the classic mitred look */
}

static void blade(KSprite *t, float x, float y, float dx, float dy, uint32_t col) {
    ks_line(t, x, y, x + dx, y + dy, 1.3f, col, 255);
}

static KSprite *make_covered(int s, int variant) {
    KSprite *t = ks_new(s, s);
    float u = (float)s / 20.f;
    ks_fill(t, GRASS);
    bevel(t, s, GRASS_HI, GRASS_LO);
    /* a shaded inner edge makes the tile read as a raised tuft of turf */
    for (int x = 2; x < s - 2; x++) t->px[(s - 3) * s + x] = ks_lerp_px(GRASS, GRASS_LO, 90);
    for (int y = 2; y < s - 2; y++) t->px[y * s + s - 3] = ks_lerp_px(GRASS, GRASS_LO, 90);
    float bx = (variant == 0 ? 0.46f : variant == 1 ? 0.60f : 0.38f) * (float)s;
    float by = (variant == 0 ? 0.70f : variant == 1 ? 0.64f : 0.62f) * (float)s;
    blade(t, bx, by, -3.2f * u, -6.2f * u, 0x4F9A52);
    blade(t, bx, by, 0.4f * u, -7.4f * u, 0x8CCB6E);
    blade(t, bx, by, 3.4f * u, -5.2f * u, 0x4F9A52);
    blade(t, bx + 1.5f * u, by + 0.5f * u, 1.6f * u, -4.4f * u, 0x8CCB6E);
    if (variant == 2) {                                      /* a tiny flower */
        ks_disc(t, 0.70f * (float)s, 0.36f * (float)s, 1.7f * u, 0xFFF3F6, 255);
        ks_disc(t, 0.70f * (float)s, 0.36f * (float)s, 0.7f * u, 0xF2C230, 255);
    }
    return t;
}

static KSprite *make_open(int s) {
    KSprite *t = ks_new(s, s);
    ks_fill(t, OPEN_BG);
    for (int x = 0; x < s; x++) t->px[x] = OPEN_LINE;
    for (int y = 0; y < s; y++) t->px[y * s] = OPEN_LINE;
    for (int x = 1; x < s; x++) t->px[s + x] = OPEN_HI;
    for (int y = 1; y < s; y++) t->px[y * s + 1] = OPEN_HI;
    return t;
}

static void ofuda(KSprite *t, int s) {
    float u = (float)s / 20.f, cx = (float)s * 0.5f;
    ks_ellipse(t, cx, 17.2f * u, 4.6f * u, 1.5f * u, 0.f, 0x2B5A31, 160);              /* shadow in the turf */
    ks_line(t, cx, 11.f * u, cx, 17.f * u, 1.7f * u, 0x7A4B24, 255);                    /* the stick */
    ks_rect(t, cx - 3.6f * u, 3.f * u, 7.2f * u, 9.4f * u, 0xB8AE90, 255);              /* paper with a border */
    ks_rect(t, cx - 3.f * u, 3.6f * u, 6.f * u, 8.2f * u, 0xFFFBEA, 255);
    ks_rect(t, cx - 2.2f * u, 4.4f * u, 4.4f * u, 1.1f * u, 0xC8102E, 255);             /* red seal marks */
    ks_rect(t, cx - 0.5f * u, 5.6f * u, 1.f * u, 4.6f * u, 0xC8102E, 255);
    ks_rect(t, cx - 2.f * u, 7.f * u, 4.f * u, 0.9f * u, 0xC8102E, 255);
    ks_rect(t, cx - 1.6f * u, 9.4f * u, 3.2f * u, 1.0f * u, 0xC8102E, 255);
}

static void fairy_figure(KSprite *t, float cx, float cy, float u, uint32_t hair, uint32_t dress, uint32_t dress_dk) {
    /* wings */
    ks_ellipse(t, cx - 4.6f * u, cy - 1.4f * u, 3.5f * u, 1.9f * u, -0.75f, 0xDDF3FF, 235);
    ks_ellipse(t, cx + 4.6f * u, cy - 1.4f * u, 3.5f * u, 1.9f * u, 0.75f, 0xDDF3FF, 235);
    ks_ellipse(t, cx - 3.8f * u, cy + 1.4f * u, 2.6f * u, 1.4f * u, 0.55f, 0xC4E4FA, 230);
    ks_ellipse(t, cx + 3.8f * u, cy + 1.4f * u, 2.6f * u, 1.4f * u, -0.55f, 0xC4E4FA, 230);
    /* arms and dress */
    ks_line(t, cx - 2.f * u, cy + 1.2f * u, cx - 4.4f * u, cy + 3.2f * u, 1.3f * u, 0xFFDCC4, 255);
    ks_line(t, cx + 2.f * u, cy + 1.2f * u, cx + 4.4f * u, cy + 3.2f * u, 1.3f * u, 0xFFDCC4, 255);
    float dr[8] = { cx - 2.1f * u, cy + 0.4f * u, cx + 2.1f * u, cy + 0.4f * u, cx + 4.0f * u, cy + 6.6f * u, cx - 4.0f * u, cy + 6.6f * u };
    ks_poly_grad_v(t, dr, 4, dress, dress_dk);
    ks_rect(t, cx - 4.0f * u, cy + 5.8f * u, 8.0f * u, 0.9f * u, 0xFFF6E8, 255);
    /* head */
    ks_disc(t, cx, cy - 3.1f * u, 4.2f * u, hair, 255);
    ks_disc(t, cx, cy - 2.4f * u, 3.3f * u, 0xFFE3CF, 255);
    ks_ellipse(t, cx, cy - 5.2f * u, 3.7f * u, 1.7f * u, 0.f, hair, 255);
    ks_disc(t, cx - 1.3f * u, cy - 2.3f * u, 0.75f * u, 0x35243F, 255);
    ks_disc(t, cx + 1.3f * u, cy - 2.3f * u, 0.75f * u, 0x35243F, 255);
    ks_disc(t, cx - 2.2f * u, cy - 1.2f * u, 0.9f * u, 0xF6A0A8, 150);
    ks_disc(t, cx + 2.2f * u, cy - 1.2f * u, 0.9f * u, 0xF6A0A8, 150);
    /* a red ribbon */
    ks_ellipse(t, cx + 3.1f * u, cy - 5.6f * u, 1.7f * u, 1.0f * u, -0.5f, 0xD0182C, 255);
    ks_ellipse(t, cx + 4.3f * u, cy - 4.5f * u, 1.7f * u, 1.0f * u, 0.9f, 0xD0182C, 255);
}

static KSprite *make_fairy(int s, bool hit) {
    KSprite *t = make_open(s);
    if (hit) {
        ks_fill(t, 0xE02020);
        for (int x = 0; x < s; x++) t->px[x] = 0xA01818;
        for (int y = 0; y < s; y++) t->px[y * s] = 0xA01818;
    }
    float u = (float)s / 20.f;
    fairy_figure(t, (float)s * 0.5f, (float)s * 0.50f, u, hit ? 0x5B3A9E : 0x3E6FD0, hit ? 0xFFFFFF : 0x5AA7E8, hit ? 0xE8D8F0 : 0x2F6FB8);
    return t;
}

/* Paints src (with its transparent and translucent pixels) on top of the opaque tile dst. */
static void overlay(KSprite *dst, const KSprite *src) {
    KCanvas cv;
    kc_init(&cv, dst->px, dst->w, dst->h, dst->w);
    ks_blit(&cv, 0, 0, src);
}

static KSprite *make_flag(int s) {
    KSprite *t = make_covered(s, 0);
    KSprite *f = ks_new(s, s);
    ofuda(f, s);
    overlay(t, f);
    ks_free(f);
    return t;
}

static KSprite *make_wrong(int s) {
    KSprite *t = make_flag(s);
    float u = (float)s / 20.f;
    ks_line(t, 3.f * u, 3.f * u, 17.f * u, 17.f * u, 2.2f * u, 0xE02020, 255);
    ks_line(t, 17.f * u, 3.f * u, 3.f * u, 17.f * u, 2.2f * u, 0xE02020, 255);
    return t;
}

/* ---------------------------------------------------------------- the smiley */

static void arc(KSprite *t, float cx, float cy, float r, float a0, float a1, float width, uint32_t col) {
    int n = 14;
    float px = cx + r * cosf(a0), py = cy + r * sinf(a0);
    for (int i = 1; i <= n; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)n;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        ks_line(t, px, py, x, y, width, col, 255);
        px = x;
        py = y;
    }
}

static KSprite *make_face(int kind) {
    KSprite *t = ks_new(FACE_SIZE, FACE_SIZE);
    float cx = 16.f, cy = 17.f;
    ks_disc(t, cx, cy, 14.2f, 0x6A4A00, 255);
    ks_disc_grad(t, cx, cy, 13.2f, 0xFFF6A0, 0xF0B820, -0.35f, -0.4f);
    uint32_t ink = 0x241808;
    switch (kind) {
    case FACE_NORMAL:
        ks_disc(t, cx - 4.8f, cy - 2.6f, 1.9f, ink, 255);
        ks_disc(t, cx + 4.8f, cy - 2.6f, 1.9f, ink, 255);
        arc(t, cx, cy + 0.8f, 7.4f, 0.55f, 2.59f, 1.7f, ink);
        break;
    case FACE_OOH:
        ks_disc(t, cx - 4.8f, cy - 3.2f, 2.5f, ink, 255);
        ks_disc(t, cx + 4.8f, cy - 3.2f, 2.5f, ink, 255);
        ks_disc(t, cx - 5.2f, cy - 3.8f, 0.8f, 0xFFFFFF, 255);
        ks_disc(t, cx + 4.4f, cy - 3.8f, 0.8f, 0xFFFFFF, 255);
        ks_ellipse(t, cx, cy + 6.6f, 2.8f, 3.4f, 0.f, ink, 255);
        ks_ellipse(t, cx, cy + 6.9f, 1.5f, 2.0f, 0.f, 0xB03030, 255);
        break;
    case FACE_COOL:
        ks_rect(t, cx - 9.2f, cy - 5.6f, 8.4f, 5.2f, ink, 255);
        ks_rect(t, cx + 0.8f, cy - 5.6f, 8.4f, 5.2f, ink, 255);
        ks_rect(t, cx - 1.f, cy - 5.f, 2.f, 1.4f, ink, 255);
        ks_line(t, cx - 8.6f, cy - 4.6f, cx - 6.2f, cy - 4.6f, 1.f, 0x7080A0, 255);
        ks_line(t, cx + 1.4f, cy - 4.6f, cx + 3.8f, cy - 4.6f, 1.f, 0x7080A0, 255);
        arc(t, cx, cy + 1.f, 7.f, 0.6f, 2.54f, 1.7f, ink);
        break;
    default:                                                 /* dead */
        for (int side = -1; side <= 1; side += 2) {
            float ex = cx + (float)side * 4.8f, ey = cy - 2.6f;
            ks_line(t, ex - 2.f, ey - 2.f, ex + 2.f, ey + 2.f, 1.6f, ink, 255);
            ks_line(t, ex + 2.f, ey - 2.f, ex - 2.f, ey + 2.f, 1.6f, ink, 255);
        }
        arc(t, cx, cy + 10.5f, 6.6f, 3.86f, 5.56f, 1.7f, ink);   /* a frown */
        break;
    }
    /* the shrine maiden's red bow on top of the head */
    ks_ellipse(t, 8.8f, 5.2f, 4.2f, 2.4f, -0.55f, 0xD0182C, 255);
    ks_ellipse(t, 14.4f, 3.4f, 4.2f, 2.4f, 0.55f, 0xD0182C, 255);
    ks_ellipse(t, 8.2f, 4.7f, 2.0f, 0.9f, -0.55f, 0xF2606E, 255);
    ks_disc(t, 11.6f, 4.6f, 1.9f, 0x9E0F22, 255);
    return t;
}

/* ---------------------------------------------------------------- build / free */

void art_build(Art *a, int cell) {
    memset(a, 0, sizeof *a);
    a->cell = cell;
    for (int i = 0; i < 3; i++) a->covered[i] = make_covered(cell, i);
    a->pressed = make_open(cell);
    for (int n = 1; n <= 8; n++) {
        KSprite *t = make_open(cell);
        KCanvas cv;
        kc_init(&cv, t->px, cell, cell, cell);
        int ox = (cell - 8) / 2, oy = (cell - 16) / 2;
        kc_glyph(&cv, ox, oy, (uint32_t)('0' + n), NUM_COLORS[n], 1);
        kc_glyph(&cv, ox + 1, oy, (uint32_t)('0' + n), NUM_COLORS[n], 1);
        a->num[n] = t;
    }
    a->flag = make_flag(cell);
    a->fairy = make_fairy(cell, false);
    a->fairy_hit = make_fairy(cell, true);
    a->wrong = make_wrong(cell);
    for (int i = 0; i < FACE_COUNT; i++) a->face[i] = make_face(i);
}

void art_free(Art *a) {
    for (int i = 0; i < 3; i++) ks_free(a->covered[i]);
    ks_free(a->pressed);
    for (int i = 1; i <= 8; i++) ks_free(a->num[i]);
    ks_free(a->flag);
    ks_free(a->fairy);
    ks_free(a->fairy_hit);
    ks_free(a->wrong);
    for (int i = 0; i < FACE_COUNT; i++) ks_free(a->face[i]);
    memset(a, 0, sizeof *a);
}

/* ---------------------------------------------------------------- LED digits */

static const uint8_t SEG[11] = {            /* bits: a b c d e f g */
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x40
};

static KSprite *g_led[11];                  /* 14 x 25 digits, pre-rendered once */

static void hseg(KSprite *t, float x0, float x1, float y, uint32_t col) {
    float p[12] = { x0, y, x0 + 1.7f, y - 1.6f, x1 - 1.7f, y - 1.6f, x1, y, x1 - 1.7f, y + 1.6f, x0 + 1.7f, y + 1.6f };
    ks_poly(t, p, 6, col, 255);
}

static void vseg(KSprite *t, float x, float y0, float y1, uint32_t col) {
    float p[12] = { x, y0, x + 1.6f, y0 + 1.7f, x + 1.6f, y1 - 1.7f, x, y1, x - 1.6f, y1 - 1.7f, x - 1.6f, y0 + 1.7f };
    ks_poly(t, p, 6, col, 255);
}

static void build_led(void) {
    if (g_led[0]) return;
    for (int d = 0; d < 11; d++) {
        KSprite *t = ks_new(14, 25);
        ks_fill(t, 0x000000);
        for (int pass = 0; pass < 2; pass++) {                       /* ghost segments first, lit ones on top */
            for (int i = 0; i < 7; i++) {
                bool on = (SEG[d] >> i) & 1;
                if (on != (pass == 1)) continue;
                uint32_t col = on ? 0xFF3C2C : 0x2A0808;
                switch (i) {
                case 0: hseg(t, 2.4f, 11.6f, 2.5f, col); break;
                case 1: vseg(t, 12.2f, 3.4f, 11.6f, col); break;
                case 2: vseg(t, 12.2f, 13.4f, 21.6f, col); break;
                case 3: hseg(t, 2.4f, 11.6f, 22.5f, col); break;
                case 4: vseg(t, 1.8f, 13.4f, 21.6f, col); break;
                case 5: vseg(t, 1.8f, 3.4f, 11.6f, col); break;
                default: hseg(t, 2.4f, 11.6f, 12.5f, col); break;
                }
            }
        }
        g_led[d] = t;
    }
}

void art_draw_led(KCanvas *c, int x, int y, int value) {
    build_led();
    kc_fill(c, kr(x, y, LED_W, LED_H), 0x000000);
    kc_bevel(c, kr(x, y, LED_W, LED_H), KB_SUNKEN);
    if (value > 999) value = 999;
    if (value < -99) value = -99;
    int digits[3];
    if (value < 0) {
        int v = -value;
        digits[0] = 10;
        digits[1] = v / 10 % 10;
        digits[2] = v % 10;
    } else {
        digits[0] = value / 100;
        digits[1] = value / 10 % 10;
        digits[2] = value % 10;
    }
    for (int i = 0; i < 3; i++) ks_blit_key(c, x + 2 + i * 15, y + 1, g_led[digits[i]]);
}
