#include "art.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float RAD[NRAD] = { 10.f, 9.f, 8.f, 7.f, 6.f, 5.f };

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

static KCanvas wrap_canvas(KSprite *s) {
    KCanvas cv;
    kc_init(&cv, s->px, s->w, s->h, s->w);
    return cv;
}

/* ---------------------------------------------------------------- the garden */

static void build_floor(SArt *a) {
    KSprite *f = ks_new(FIELD_W, FIELD_H);
    for (int cy = 0; cy < SN_H; cy++)
        for (int cx = 0; cx < SN_W; cx++) {
            uint32_t base = ((cx + cy) & 1) ? 0x1B3C36 : 0x183631;
            for (int y = 0; y < CELL; y++) ks_fill32(f->px + (size_t)(cy * CELL + y) * FIELD_W + cx * CELL, base, CELL);
        }
    ks_glow(f, 150.f, 120.f, 150.f, 0x4FB09A, 36);
    ks_glow(f, 430.f, 330.f, 170.f, 0x3A8CB8, 32);
    ks_glow(f, 300.f, 210.f, 230.f, 0x2F7F6F, 22);
    for (int cy = 0; cy < SN_H; cy++)
        for (int cx = 0; cx < SN_W; cx++) {
            uint32_t h = hash32((uint32_t)(cx * 131 + cy * 977 + 5));
            float ox = (float)cx * CELL, oy = (float)cy * CELL;
            int roll = (int)(h % 100);
            float px = ox + 4.f + (float)((h >> 8) % 16), py = oy + 8.f + (float)((h >> 16) % 12);
            if (roll < 34) {
                ks_line(f, px, py, px - 2.f, py - 5.f, 1.2f, 0x2F7A4A, 255);
                ks_line(f, px, py, px + 0.5f, py - 6.f, 1.2f, 0x4C9C62, 255);
                ks_line(f, px, py, px + 2.5f, py - 4.f, 1.2f, 0x2F7A4A, 255);
            } else if (roll < 42) {
                ks_ellipse(f, px, py, 2.4f, 1.7f, 0.f, 0x56706C, 255);
                ks_ellipse(f, px - 0.4f, py - 0.5f, 1.5f, 0.9f, 0.f, 0x7B9690, 255);
            } else if (roll < 47) {
                ks_disc(f, px, py, 1.7f, (h & 0x10000) ? 0xF6D4E2 : 0xFFF3C8, 255);
                ks_disc(f, px, py, 0.7f, 0xF2B93B, 255);
            }
        }
    KCanvas cv = wrap_canvas(f);                             /* soft shadow along the edges */
    for (int i = 0; i < 14; i++) {
        int lv = 150 + i * 7;
        ks_shade_rect(&cv, kr(0, i, FIELD_W, 1), lv);
        ks_shade_rect(&cv, kr(0, FIELD_H - 1 - i, FIELD_W, 1), lv);
        ks_shade_rect(&cv, kr(i, 14, 1, FIELD_H - 28), lv);
        ks_shade_rect(&cv, kr(FIELD_W - 1 - i, 14, 1, FIELD_H - 28), lv);
    }
    /* a copy with 8 extra pixels on every side (the edges repeated) so that the picture can be shifted for a screen shake */
    KSprite *padded = ks_new(FIELD_W + 16, FIELD_H + 16);
    for (int y = 0; y < FIELD_H + 16; y++) {
        int sy = y - 8 < 0 ? 0 : y - 8 >= FIELD_H ? FIELD_H - 1 : y - 8;
        uint32_t *dst = padded->px + (size_t)y * padded->w;
        const uint32_t *src = f->px + (size_t)sy * FIELD_W;
        for (int x = 0; x < 8; x++) dst[x] = src[0];
        ks_copy32(dst + 8, src, FIELD_W);
        for (int x = 0; x < 8; x++) dst[8 + FIELD_W + x] = src[FIELD_W - 1];
    }
    ks_free(f);
    a->floor = padded;
}

static void build_borders(SArt *a) {
    KSprite *f = ks_new(FIELD_W, FIELD_H);
    ks_rect(f, 0, 0, FIELD_W, 7, 0x8E0B21, 255);
    ks_rect(f, 0, FIELD_H - 7, FIELD_W, 7, 0x8E0B21, 255);
    ks_rect(f, 0, 0, 7, FIELD_H, 0x8E0B21, 255);
    ks_rect(f, FIELD_W - 7, 0, 7, FIELD_H, 0x8E0B21, 255);
    ks_rect(f, 0, 0, FIELD_W, 2, 0xD8384F, 255);               /* lacquer highlights */
    ks_rect(f, 0, 0, 2, FIELD_H, 0xD8384F, 255);
    ks_rect(f, 0, FIELD_H - 2, FIELD_W, 2, 0x4F0613, 255);
    ks_rect(f, FIELD_W - 2, 0, 2, FIELD_H, 0x4F0613, 255);
    for (int i = 0; i <= FIELD_W; i += 48) {                   /* posts with gold caps */
        ks_rect(f, (float)i - 5.f, 0, 10, 10, 0xB5122B, 255);
        ks_rect(f, (float)i - 5.f, FIELD_H - 10, 10, 10, 0xB5122B, 255);
        ks_rect(f, (float)i - 3.f, 1, 6, 3, 0xD9A441, 255);
        ks_rect(f, (float)i - 3.f, FIELD_H - 4, 6, 3, 0xD9A441, 255);
    }
    for (int j = 0; j <= FIELD_H; j += 48) {
        ks_rect(f, 0, (float)j - 5.f, 10, 10, 0xB5122B, 255);
        ks_rect(f, FIELD_W - 10, (float)j - 5.f, 10, 10, 0xB5122B, 255);
        ks_rect(f, 1, (float)j - 3.f, 3, 6, 0xD9A441, 255);
        ks_rect(f, FIELD_W - 4, (float)j - 3.f, 3, 6, 0xD9A441, 255);
    }
    ks_flatten(f, 0x300810);
    a->fence = f;
    for (int ph = 0; ph < 2; ph++) {                             /* the open edge of the wrap-around field */
        KSprite *p = ks_new(FIELD_W, FIELD_H);
        for (int d = 0; d < 6; d++) {
            uint32_t col = ks_lerp_px(0xBFEFFF, 0x2A5C9A, d * 256 / 6);
            for (int x = 0; x < FIELD_W; x++) {
                if (((x / 4) + ph) & 1) continue;
                p->px[d * FIELD_W + x] = col;
                p->px[(FIELD_H - 1 - d) * FIELD_W + x] = col;
            }
            for (int y = 0; y < FIELD_H; y++) {
                if (((y / 4) + ph) & 1) continue;
                p->px[y * FIELD_W + d] = col;
                p->px[y * FIELD_W + FIELD_W - 1 - d] = col;
            }
        }
        a->portal[ph] = p;
    }
}

/* ---------------------------------------------------------------- dango, lantern */

static void ball(KSprite *s, float cx, float cy, float r, uint32_t light, uint32_t base, uint32_t rim) {
    ks_disc(s, cx, cy, r + 0.9f, rim, 255);
    ks_disc_grad(s, cx, cy, r, light, base, -0.38f, -0.42f);
    ks_disc(s, cx - r * 0.35f, cy - r * 0.40f, r * 0.22f, 0xFFFFFF, 210);
}

static void dango_sprite(KSprite *s, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t l1, uint32_t l2, uint32_t l3, uint32_t rim) {
    ks_line(s, 1.5f, 22.5f, 22.5f, 1.5f, 1.4f, 0xB07C3A, 255);
    ks_line(s, 2.f, 22.f, 22.f, 2.f, 0.6f, 0xE0B070, 255);
    ball(s, 6.8f, 17.2f, 5.0f, l1, c1, rim);
    ball(s, 12.0f, 12.0f, 5.2f, l2, c2, rim);
    ball(s, 17.2f, 6.8f, 5.0f, l3, c3, rim);
}

static void build_items(SArt *a) {
    a->dango = ks_new(CELL, CELL);
    dango_sprite(a->dango, 0x7CC85E, 0xF4F0E4, 0xF58CAB, 0xC4EFA0, 0xFFFFFF, 0xFFC2D4, 0x3C6B3A);
    ks_darken_edge(a->dango, 100);
    a->gold = ks_new(CELL, CELL);
    dango_sprite(a->gold, 0xE9A817, 0xFFC93A, 0xFFE27A, 0xFFE9A0, 0xFFF0B0, 0xFFF8D0, 0x8A5A00);
    a->gold_halo = ks_new(CELL + 16, CELL + 16);
    ks_glow(a->gold_halo, (CELL + 16) / 2.f, (CELL + 16) / 2.f, (CELL + 16) / 2.f, 0xFFD54A, 120);
    a->gold_halo->ax = a->gold_halo->w / 2;
    a->gold_halo->ay = a->gold_halo->h / 2;

    KSprite *s = ks_new(CELL, CELL);                          /* stone lantern */
    ks_rect(s, 5.f, 19.f, 14.f, 4.f, 0x4E5260, 255);
    ks_rect(s, 5.f, 19.f, 14.f, 1.2f, 0x8D92A3, 255);
    ks_rect(s, 9.5f, 14.5f, 5.f, 5.f, 0x6C7080, 255);
    ks_rect(s, 6.f, 7.5f, 12.f, 7.5f, 0x7B8090, 255);
    ks_glow(s, 12.f, 11.f, 9.f, 0xFFC050, 150);
    ks_rect(s, 8.f, 9.f, 8.f, 4.5f, 0xFFD36B, 255);
    ks_rect(s, 11.4f, 9.f, 1.2f, 4.5f, 0x7B8090, 255);
    ks_rect(s, 8.f, 10.9f, 8.f, 1.0f, 0x7B8090, 255);
    float roof[8] = { 2.5f, 8.f, 21.5f, 8.f, 17.5f, 3.2f, 6.5f, 3.2f };
    ks_poly_grad_v(s, roof, 4, 0xB9BECE, 0x8C91A2);
    ks_disc(s, 12.f, 2.4f, 1.7f, 0xA6ABBA, 255);
    ks_outline(s, 0x1C202C);
    a->stone = s;
}

/* ---------------------------------------------------------------- the serpent */

static void build_body(SArt *a) {
    static const uint32_t TONE[2][2] = { { 0x4ECB5E, 0x3AB04F }, { 0xE05A50, 0xC03A3A } };
    static const uint32_t LIGHT[2][2] = { { 0xB8F5A8, 0x98EA94 }, { 0xFFB0A0, 0xF59088 } };
    for (int i = 0; i < NRAD; i++) {
        int size = (int)(RAD[i] + 3.f) * 2;
        a->outline[i] = ks_new(size, size);
        ks_disc(a->outline[i], size / 2.f, size / 2.f, RAD[i] + 1.6f, 0x0F3322, 255);
        ks_flatten(a->outline[i], 0x0F3322);
        for (int hurt = 0; hurt < 2; hurt++)
            for (int tone = 0; tone < 2; tone++) {
                KSprite *s = ks_new(size, size);
                ks_disc(s, size / 2.f, size / 2.f, RAD[i], TONE[hurt][tone], 255);
                ks_disc(s, size / 2.f, size / 2.f - RAD[i] * 0.15f, RAD[i] * 0.75f, LIGHT[hurt][tone], 60);
                ks_flatten(s, 0x0F3322);
                a->fill[hurt][tone][i] = s;
            }
        a->shine[i] = ks_new(size, size);
        ks_disc(a->shine[i], size / 2.f - RAD[i] * 0.18f, size / 2.f - RAD[i] * 0.38f, RAD[i] * 0.42f, 0xFFFFFF, 70);
    }
}

typedef struct { float cx, cy, ca, sa, angle; } Xf;

static void xf_pt(const Xf *x, float u, float v, float *ox, float *oy) {
    *ox = x->cx + u * x->ca - v * x->sa;
    *oy = x->cy + u * x->sa + v * x->ca;
}
static void h_disc(KSprite *s, const Xf *x, float u, float v, float r, uint32_t col, int al) {
    float px, py;
    xf_pt(x, u, v, &px, &py);
    ks_disc(s, px, py, r, col, al);
}
static void h_ellipse(KSprite *s, const Xf *x, float u, float v, float ru, float rv, uint32_t col, int al) {
    float px, py;
    xf_pt(x, u, v, &px, &py);
    ks_ellipse(s, px, py, ru, rv, x->angle, col, al);
}
static void h_line(KSprite *s, const Xf *x, float u0, float v0, float u1, float v1, float w, uint32_t col) {
    float ax, ay, bx, by;
    xf_pt(x, u0, v0, &ax, &ay);
    xf_pt(x, u1, v1, &bx, &by);
    ks_line(s, ax, ay, bx, by, w, col, 255);
}

static KSprite *make_head(int dir, bool tongue, bool dead) {
    const int S = 44;
    KSprite *s = ks_new(S, S);
    float ang = ((float)dir - 1.f) * (float)M_PI_2;          /* RIGHT = 0 */
    Xf x = { S / 2.f, S / 2.f, cosf(ang), sinf(ang), ang };
    uint32_t body = dead ? 0xD05050 : 0x46C458, light = dead ? 0xFFB0A8 : 0xB8F5A8, rim = 0x0F3322;
    if (tongue && !dead) {
        h_line(s, &x, 12.f, 0.f, 18.6f, 0.f, 1.5f, 0xE02850);
        h_line(s, &x, 18.f, 0.f, 21.2f, -2.f, 1.3f, 0xE02850);
        h_line(s, &x, 18.f, 0.f, 21.2f, 2.f, 1.3f, 0xE02850);
    }
    h_ellipse(s, &x, 1.5f, 0.f, 13.2f, 11.8f, rim, 255);
    float hx, hy;
    xf_pt(&x, 1.5f, 0.f, &hx, &hy);
    ks_ellipse_grad(s, hx, hy, 11.8f, 10.4f, ang, light, body, -0.35f, -0.1f);
    h_ellipse(s, &x, 7.5f, 0.f, 5.2f, 5.6f, dead ? 0xE08080 : 0x8CE69A, 140);        /* the snout */
    if (!dead) {
        for (int side = -1; side <= 1; side += 2) {
            float v = (float)side * 5.6f;
            h_disc(s, &x, 4.6f, v, 3.7f, rim, 255);
            h_disc(s, &x, 4.6f, v, 3.1f, 0xFFFFFF, 255);
            h_disc(s, &x, 5.5f, v, 1.7f, 0x1A1224, 255);
            h_disc(s, &x, 5.9f, v - (float)side * 0.5f, 0.55f, 0xFFFFFF, 255);
            h_disc(s, &x, 11.8f, (float)side * 1.8f, 0.7f, 0x1F5A33, 255);
            h_disc(s, &x, 2.2f, (float)side * 8.6f, 1.6f, 0xF59AAE, 110);
        }
    } else {
        for (int side = -1; side <= 1; side += 2) {
            float v = (float)side * 5.6f;
            h_line(s, &x, 3.2f, v - 2.f, 6.4f, v + 2.f, 1.4f, 0x2A1018);
            h_line(s, &x, 3.2f, v + 2.f, 6.4f, v - 2.f, 1.4f, 0x2A1018);
        }
        h_line(s, &x, 10.5f, -2.5f, 10.5f, 2.5f, 1.3f, 0x2A1018);
    }
    /* the ribbon */
    h_ellipse(s, &x, -5.f, -9.2f, 3.2f, 1.8f, 0xD0182C, 255);
    h_ellipse(s, &x, -5.f, -5.8f, 3.2f, 1.8f, 0xD0182C, 255);
    h_disc(s, &x, -5.f, -7.5f, 1.5f, 0x8E0B21, 255);
    return s;
}

static void build_heads(SArt *a) {
    for (int d = 0; d < 4; d++) {
        a->head[d][0] = make_head(d, false, false);
        a->head[d][1] = make_head(d, true, false);
        a->head_dead[d] = make_head(d, false, true);
    }
    a->petal = ks_new(6, 5);
    ks_ellipse(a->petal, 3.f, 2.5f, 2.6f, 1.7f, 0.3f, 0xF6B4C8, 255);
    ks_ellipse(a->petal, 2.6f, 2.2f, 1.2f, 0.7f, 0.3f, 0xFFE3EC, 255);
    ks_flatten(a->petal, 0x3A5A6A);
    for (int i = 0; i < 3; i++) {
        int sz = 7 + i * 4;
        a->spark[i] = ks_new(sz, sz);
        ks_star(a->spark[i], sz / 2.f, sz / 2.f, sz / 2.f - 0.3f, sz / 7.f, 4, 0.f, i == 0 ? 0xFFFFFF : 0xFFE9A0, 255);
        ks_flatten(a->spark[i], 0x204040);
    }
}

/* ---------------------------------------------------------------- the title scene */

static int noise1(int x, int period, uint32_t seed) {
    int i = x / period, f = x % period;
    int a = (int)(hash32((uint32_t)i * 2654435761u + seed) & 1023);
    int b = (int)(hash32((uint32_t)(i + 1) * 2654435761u + seed) & 1023);
    int t = f * 256 / period;
    t = t * t * (768 - 2 * t) / 65536;
    return a + (b - a) * t / 256;
}

static void ridge(KSprite *s, int base, int amp, int period, uint32_t seed, uint32_t top, uint32_t bottom) {
    for (int x = 0; x < s->w; x++) {
        int n = noise1(x + 400, period, seed) + noise1(x + 3000, period / 2, seed + 7) / 2;
        int y = base - amp * n / 1536;
        for (int yy = y; yy < s->h; yy++) {
            if (yy < 0) continue;
            s->px[yy * s->w + x] = ks_lerp_px(top, bottom, (yy - y) * 256 / (s->h - y + 1));
        }
    }
}

static void bamboo(KSprite *s, int x, int base, int height, int width, uint32_t body, uint32_t joint, uint32_t seed) {
    ks_rect(s, (float)x, (float)(base - height), (float)width, (float)height, body, 255);
    ks_rect(s, (float)x, (float)(base - height), (float)width / 3.f + 1.f, (float)height, ks_lerp_px(body, 0x6FD39A, 70), 255);
    int seg = 34 + (int)(hash32(seed) % 18);
    for (int y = base - seg; y > base - height; y -= seg) {
        ks_rect(s, (float)x - 1.f, (float)y, (float)width + 2.f, 3.f, joint, 255);
        if (hash32((uint32_t)y * 31 + seed) & 1) {
            int dir = (hash32((uint32_t)y + seed) & 2) ? 1 : -1;
            for (int k = 0; k < 3; k++) {
                float lx = (float)x + (float)width / 2.f + (float)dir * (6.f + (float)k * 7.f), ly = (float)y - 4.f + (float)k * 3.f;
                ks_line(s, (float)x + (float)width / 2.f, (float)y, lx, ly, 1.4f, joint, 255);
            }
        }
    }
}

static void build_title(SArt *a) {
    KSprite *s = ks_new(WIN_W, WIN_H);
    for (int y = 0; y < WIN_H; y++) {
        uint32_t c = y < WIN_H / 2 ? ks_lerp_px(0x070B22, 0x131D4E, y * 512 / WIN_H) : ks_lerp_px(0x131D4E, 0x3B4B8C, (y - WIN_H / 2) * 400 / WIN_H);
        ks_fill32(s->px + (size_t)y * WIN_W, c, WIN_W);
    }
    for (int i = 0; i < 150; i++) {
        uint32_t r = hash32((uint32_t)i * 977 + 13);
        int x = (int)(r % WIN_W), y = (int)((r >> 12) % (WIN_H * 6 / 10));
        int b = 130 + (int)((r >> 4) & 120);
        ks_put(s, x, y, KRGB(b, b, b + 20 > 255 ? 255 : b + 20), 255);
        if ((r & 31) == 0) { ks_put(s, x + 1, y, 0xB0B8E0, 255); ks_put(s, x - 1, y, 0xB0B8E0, 255); ks_put(s, x, y + 1, 0xB0B8E0, 255); ks_put(s, x, y - 1, 0xB0B8E0, 255); }
    }
    ks_glow(s, 452.f, 112.f, 120.f, 0xAEB8F0, 70);
    ks_disc(s, 452.f, 112.f, 44.f, 0xF3EEDA, 255);
    ks_disc(s, 438.f, 100.f, 9.f, 0xE4DDC0, 255);
    ks_disc(s, 466.f, 122.f, 6.f, 0xE4DDC0, 255);
    ks_disc(s, 448.f, 132.f, 4.f, 0xE9E3C8, 255);
    ridge(s, WIN_H * 62 / 100, 150, 150, 5, 0x1A2352, 0x0F1538);
    ridge(s, WIN_H * 74 / 100, 110, 100, 99, 0x131A40, 0x0A0E26);
    for (int i = 0; i < 6; i++) {
        bamboo(s, 8 + i * 20 + (int)(hash32((uint32_t)i) % 8), WIN_H - 14, 190 + (int)(hash32((uint32_t)i + 9) % 80), 7, 0x1B4A33, 0x0E2A1D, (uint32_t)i + 40);
        bamboo(s, WIN_W - 24 - i * 22 - (int)(hash32((uint32_t)i + 77) % 8), WIN_H - 14, 180 + (int)(hash32((uint32_t)i + 99) % 90), 7, 0x1B4A33, 0x0E2A1D, (uint32_t)i + 140);
    }
    for (int i = 0; i < 3; i++) {
        bamboo(s, 4 + i * 38, WIN_H, 330 - (int)(hash32((uint32_t)i + 5) % 70), 11, 0x2C6B47, 0x16402B, (uint32_t)i + 1);
        bamboo(s, WIN_W - 18 - i * 40, WIN_H, 320 - (int)(hash32((uint32_t)i + 55) % 80), 11, 0x2C6B47, 0x16402B, (uint32_t)i + 200);
    }
    ks_rect(s, 0, WIN_H - 22, WIN_W, 22, 0x0A0F26, 255);
    a->title_bg = s;

    /* the logo: a gradient face, a dark outline and a soft drop shadow */
    KSprite *t = ks_text("EAT THE DANGO", 4, 0xFFFFFF, 0x2A0F2E);
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++)
            if (t->px[y * t->w + x] == 0xFFFFFF) t->px[y * t->w + x] = ks_lerp_px(0xFFD0E0, 0xF2709A, y * 256 / t->h);
    KSprite *logo = ks_new(t->w + 6, t->h + 6);
    KSprite *shadow = ks_clone(t);
    for (int i = 0; i < shadow->w * shadow->h; i++) if (shadow->px[i] != KS_KEY) shadow->px[i] = 0x100818;
    KCanvas cv = wrap_canvas(logo);
    ks_blit_key(&cv, 5, 6, shadow);
    ks_blit_key(&cv, 1, 1, t);
    ks_free(shadow);
    ks_free(t);
    logo->ax = 0;
    logo->ay = 0;
    a->logo = logo;
    a->logo_sub = ks_text("Snake for HakureiDE", 2, 0xE8E4D4, 0x14102A);
}

void sart_build_stage(SArt *a, int stage) {
    switch (stage) {
    case 0: build_floor(a); break;
    case 1: build_borders(a); break;
    case 2: build_items(a); break;
    case 3: build_body(a); break;
    case 4: build_heads(a); break;
    default: build_title(a); break;
    }
}

void sart_free(SArt *a) {
    ks_free(a->floor);
    ks_free(a->fence);
    ks_free(a->portal[0]);
    ks_free(a->portal[1]);
    ks_free(a->dango);
    ks_free(a->gold);
    ks_free(a->gold_halo);
    ks_free(a->stone);
    for (int i = 0; i < NRAD; i++) {
        ks_free(a->outline[i]);
        ks_free(a->shine[i]);
        for (int h = 0; h < 2; h++) for (int t = 0; t < 2; t++) ks_free(a->fill[h][t][i]);
    }
    for (int d = 0; d < 4; d++) { ks_free(a->head[d][0]); ks_free(a->head[d][1]); ks_free(a->head_dead[d]); }
    ks_free(a->petal);
    for (int i = 0; i < 3; i++) ks_free(a->spark[i]);
    ks_free(a->title_bg);
    ks_free(a->logo);
    ks_free(a->logo_sub);
    memset(a, 0, sizeof *a);
}
