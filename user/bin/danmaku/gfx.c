/* Procedural sprites: bullets in nine colours and eight shapes, player shots, items and effects. */
#include "gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

Art A;

const BPal BPAL[BC_COUNT] = {
    { 0xE62A44, 0xFFA6B0, 0x5A0A18 },   /* red */
    { 0xF28A1C, 0xFFD494, 0x6A3004 },   /* orange */
    { 0xF2D41C, 0xFFF6A8, 0x645004 },   /* yellow */
    { 0x38C452, 0xB0F4B8, 0x0A4E1C },   /* green */
    { 0x24CCDC, 0xB0F6FF, 0x084A58 },   /* cyan */
    { 0x3466F4, 0xA8C0FF, 0x0E1A68 },   /* blue */
    { 0x9444DC, 0xDCB8FF, 0x341060 },   /* purple */
    { 0xF264B4, 0xFFBCE4, 0x66143E },   /* pink */
    { 0xE6E8F2, 0xFFFFFF, 0x545470 },   /* white */
};

static const uint32_t BACKDROP = 0x101030;                       /* what the soft edges of keyed sprites are blended with */

typedef struct Rot { float cx, cy, ca, sa; } Rot;
static Rot make_rot(float cx, float cy, float ang) { Rot r = { cx, cy, cosf(ang), sinf(ang) }; return r; }
static void rp(const Rot *r, float u, float v, float *x, float *y) { *x = r->cx + u * r->ca - v * r->sa; *y = r->cy + u * r->sa + v * r->ca; }

static void rpoly(KSprite *s, const Rot *r, const float *uv, int n, uint32_t col) {
    float pts[40];
    for (int i = 0; i < n && i < 20; i++) rp(r, uv[2 * i], uv[2 * i + 1], &pts[2 * i], &pts[2 * i + 1]);
    ks_poly(s, pts, n, col, 255);
}

static void rpoly_scaled(KSprite *s, const Rot *r, const float *uv, int n, float k, uint32_t col) {
    float sc[40];
    for (int i = 0; i < n && i < 20; i++) { sc[2 * i] = uv[2 * i] * k; sc[2 * i + 1] = uv[2 * i + 1] * k; }
    rpoly(s, r, sc, n, col);
}

/* a dotted halo: every other pixel of a ring, which reads as a soft glow without any blending */
static void dither_ring(KSprite *s, float cx, float cy, float r0, float r1, uint32_t col) {
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            if (((x + y) & 1) != 0) continue;
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= r0 && d <= r1 && s->px[y * s->w + x] == KS_KEY) s->px[y * s->w + x] = col & 0xFFFFFF;
        }
}

/* ---------------------------------------------------------------- bullets */

static KSprite *make_pellet(int c) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(9, 9);
    ks_disc(s, 4.5f, 4.5f, 4.4f, p->dark, 255);
    ks_disc(s, 4.5f, 4.5f, 3.5f, p->base, 255);
    ks_disc(s, 4.1f, 4.1f, 1.9f, p->light, 255);
    ks_flatten(s, BACKDROP);
    return s;
}

static KSprite *make_orb(int c) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(19, 19);
    float m = 9.5f;
    ks_disc(s, m, m, 7.6f, p->dark, 255);
    ks_disc_grad(s, m, m, 6.7f, p->light, p->base, -0.3f, -0.35f);
    ks_disc(s, m, m, 3.3f, 0xFFFFFF, 255);
    ks_disc(s, m, m, 2.2f, 0xFFFFFF, 255);
    ks_flatten(s, BACKDROP);
    dither_ring(s, m, m, 7.7f, 9.2f, p->base);
    return s;
}

static KSprite *make_big(int c) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(31, 31);
    float m = 15.5f;
    ks_disc(s, m, m, 14.4f, p->dark, 255);
    ks_disc_grad(s, m, m, 13.2f, p->light, p->base, -0.35f, -0.4f);
    ks_ring(s, m, m, 9.5f, 1.4f, p->light, 140);
    ks_disc(s, m, m, 6.2f, 0xFFFFFF, 190);
    ks_disc(s, m, m, 3.8f, 0xFFFFFF, 255);
    ks_disc(s, m - 5.f, m - 6.f, 2.4f, 0xFFFFFF, 220);
    ks_flatten(s, BACKDROP);
    dither_ring(s, m, m, 14.5f, 15.4f, p->base);
    return s;
}

static KSprite *make_rice(int c, int rot) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(17, 17);
    float a = (float)rot * 6.2831853f / (float)NROT;
    ks_ellipse(s, 8.5f, 8.5f, 7.4f, 4.2f, a, p->dark, 255);
    ks_ellipse(s, 8.5f, 8.5f, 6.5f, 3.3f, a, p->base, 255);
    Rot r = make_rot(8.5f, 8.5f, a);
    float x0, y0, x1, y1;
    rp(&r, -3.6f, 0.f, &x0, &y0);
    rp(&r, 3.6f, 0.f, &x1, &y1);
    ks_line(s, x0, y0, x1, y1, 2.2f, p->light, 255);
    ks_flatten(s, BACKDROP);
    return s;
}

static KSprite *make_kunai(int c, int rot) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(19, 19);
    float a = (float)rot * 6.2831853f / (float)NROT;
    Rot r = make_rot(9.5f, 9.5f, a);
    static const float body[] = { 9.f, 0.f, 3.f, 3.8f, -5.f, 2.2f, -8.f, 0.f, -5.f, -2.2f, 3.f, -3.8f };
    rpoly_scaled(s, &r, body, 6, 1.0f, p->dark);
    static const float inner[] = { 8.f, 0.f, 3.f, 2.7f, -4.5f, 1.4f, -6.5f, 0.f, -4.5f, -1.4f, 3.f, -2.7f };
    rpoly_scaled(s, &r, inner, 6, 1.0f, p->base);
    float x0, y0, x1, y1;
    rp(&r, -4.f, 0.f, &x0, &y0);
    rp(&r, 6.f, 0.f, &x1, &y1);
    ks_line(s, x0, y0, x1, y1, 1.4f, p->light, 255);
    ks_flatten(s, BACKDROP);
    return s;
}

static KSprite *make_star(int c) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(17, 17);
    ks_star(s, 8.5f, 8.7f, 7.8f, 3.6f, 5, 0.f, p->dark, 255);
    ks_star(s, 8.5f, 8.7f, 6.6f, 2.9f, 5, 0.f, p->base, 255);
    ks_disc(s, 8.5f, 8.7f, 2.2f, p->light, 255);
    ks_flatten(s, BACKDROP);
    return s;
}

static KSprite *make_bill(int c, int rot) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(19, 19);
    float a = (float)rot * 6.2831853f / (float)NROT;
    Rot r = make_rot(9.5f, 9.5f, a);
    static const float outer[] = { 7.5f, -4.2f, 7.5f, 4.2f, -7.5f, 4.2f, -7.5f, -4.2f };
    static const float paper[] = { 6.6f, -3.3f, 6.6f, 3.3f, -6.6f, 3.3f, -6.6f, -3.3f };
    rpoly(s, &r, outer, 4, p->dark);
    rpoly(s, &r, paper, 4, 0xFAF4E4);
    float x0, y0, x1, y1;
    rp(&r, -5.f, 0.f, &x0, &y0);
    rp(&r, 5.f, 0.f, &x1, &y1);
    ks_line(s, x0, y0, x1, y1, 1.8f, p->base, 255);
    rp(&r, -1.f, -2.4f, &x0, &y0);
    rp(&r, -1.f, 2.4f, &x1, &y1);
    ks_line(s, x0, y0, x1, y1, 1.4f, p->base, 255);
    rp(&r, 3.f, -2.4f, &x0, &y0);
    rp(&r, 3.f, 2.4f, &x1, &y1);
    ks_line(s, x0, y0, x1, y1, 1.2f, p->base, 255);
    ks_flatten(s, BACKDROP);
    return s;
}

static KSprite *make_beam(int c, int rot) {
    const BPal *p = &BPAL[c];
    KSprite *s = ks_new(19, 19);
    float a = (float)rot * 6.2831853f / (float)NROT;
    Rot r = make_rot(9.5f, 9.5f, a);
    float x0, y0, x1, y1;
    rp(&r, -6.f, 0.f, &x0, &y0);
    rp(&r, 6.f, 0.f, &x1, &y1);
    ks_line(s, x0, y0, x1, y1, 6.2f, p->dark, 255);
    ks_line(s, x0, y0, x1, y1, 5.0f, p->base, 255);
    ks_line(s, x0, y0, x1, y1, 2.6f, p->light, 255);
    ks_line(s, x0, y0, x1, y1, 1.2f, 0xFFFFFF, 255);
    ks_flatten(s, BACKDROP);
    return s;
}

static void build_bullets(int part) {
    /* part 0: round shapes, 1: directional shapes, 2: the rest */
    for (int c = 0; c < BC_COUNT; c++) {
        switch (part) {
        case 0:
            A.bullet[BK_PELLET][c][0] = make_pellet(c);
            A.bullet[BK_ORB][c][0] = make_orb(c);
            A.bullet[BK_BIG][c][0] = make_big(c);
            A.bullet[BK_STAR][c][0] = make_star(c);
            break;
        case 1:
            for (int r = 0; r < NROT; r++) {
                A.bullet[BK_RICE][c][r] = make_rice(c, r);
                A.bullet[BK_KUNAI][c][r] = make_kunai(c, r);
            }
            break;
        default:
            for (int r = 0; r < NROT; r++) {
                A.bullet[BK_BILL][c][r] = make_bill(c, r);
                A.bullet[BK_BEAM][c][r] = make_beam(c, r);
            }
            break;
        }
    }
}

/* ---------------------------------------------------------------- shots, items, effects */

static void build_shots_items(void) {
    /* the shrine maiden's amulet */
    KSprite *s = ks_new(9, 16);
    ks_rect(s, 0.5f, 0.5f, 8.f, 15.f, 0xB5122B, 255);
    ks_rect(s, 1.5f, 1.5f, 6.f, 13.f, 0xFFF6EA, 255);
    ks_rect(s, 3.8f, 3.f, 1.4f, 10.f, 0xC8102E, 255);
    ks_rect(s, 2.2f, 5.f, 4.6f, 1.2f, 0xC8102E, 255);
    ks_flatten(s, 0x303050);
    A.pshot[PS_AMULET] = s;
    s = ks_new(7, 13);                                   /* homing needle */
    static const float nd[] = { 3.5f, 0.5f, 6.f, 6.f, 3.5f, 12.5f, 1.f, 6.f };
    ks_poly(s, nd, 4, 0xFF5070, 255);
    ks_line(s, 3.5f, 2.f, 3.5f, 11.f, 1.4f, 0xFFFFFF, 255);
    ks_flatten(s, 0x303050);
    A.pshot[PS_NEEDLE] = s;
    s = ks_new(13, 13);                                  /* the witch's star */
    ks_star(s, 6.5f, 6.8f, 6.2f, 2.8f, 5, 0.f, 0xF2B820, 255);
    ks_star(s, 6.5f, 6.8f, 4.6f, 2.0f, 5, 0.f, 0xFFF0A0, 255);
    ks_disc(s, 6.5f, 6.8f, 1.6f, 0xFFFFFF, 255);
    ks_flatten(s, 0x303050);
    A.pshot[PS_STAR] = s;
    s = ks_new(9, 18);                                   /* focused: a thin beam of light */
    ks_ellipse(s, 4.5f, 9.f, 3.4f, 8.6f, 0.f, 0x4C7CF4, 255);
    ks_ellipse(s, 4.5f, 9.f, 2.2f, 7.4f, 0.f, 0xB4D0FF, 255);
    ks_ellipse(s, 4.5f, 9.f, 1.0f, 6.f, 0.f, 0xFFFFFF, 255);
    ks_flatten(s, 0x303050);
    A.pshot[PS_BEAM] = s;

    /* items: coloured tiles with a letter, and a few icons */
    static const struct { uint32_t col, rim; char ch; int size; } IT[IT_COUNT] = {
        { 0xE0304A, 0xFFB0B8, 'P', 18 }, { 0xE0304A, 0xFFD870, 'P', 26 }, { 0x3A6AF0, 0xB8D0FF, 0, 18 }, { 0x2CB04A, 0xB8F4C0, 'B', 20 },
        { 0xF060A8, 0xFFC0E0, 0, 22 }, { 0xF2D41C, 0xFFF8B0, 0, 11 }, { 0xE0304A, 0xFFD870, 'F', 26 },
    };
    for (int i = 0; i < IT_COUNT; i++) {
        int sz = IT[i].size;
        KSprite *t = ks_new(sz, sz);
        float m = (float)sz / 2.f;
        if (i == IT_STAR) {
            ks_star(t, m, m + 0.3f, m - 0.5f, 2.3f, 5, 0.f, 0xB88A00, 255);
            ks_star(t, m, m + 0.3f, m - 1.5f, 1.9f, 5, 0.f, IT[i].col, 255);
            ks_disc(t, m, m, 1.2f, 0xFFFFFF, 255);
        } else if (i == IT_LIFE) {
            ks_disc(t, m - 4.2f, m - 2.f, 5.6f, 0x8E1450, 255);
            ks_disc(t, m + 4.2f, m - 2.f, 5.6f, 0x8E1450, 255);
            static const float h[] = { 0.f, 0.f, 0.f, 0.f };
            (void)h;
            float tri[6] = { m - 9.6f, m - 0.5f, m + 9.6f, m - 0.5f, m, m + 9.5f };
            ks_poly(t, tri, 3, 0x8E1450, 255);
            ks_disc(t, m - 4.0f, m - 2.2f, 4.5f, IT[i].col, 255);
            ks_disc(t, m + 4.0f, m - 2.2f, 4.5f, IT[i].col, 255);
            float tri2[6] = { m - 8.4f, m - 0.3f, m + 8.4f, m - 0.3f, m, m + 7.8f };
            ks_poly(t, tri2, 3, IT[i].col, 255);
            ks_disc(t, m - 4.5f, m - 3.5f, 1.5f, 0xFFFFFF, 220);
        } else {
            ks_rect(t, 0.5f, 0.5f, (float)sz - 1.f, (float)sz - 1.f, 0x20203A, 255);
            ks_rect(t, 1.5f, 1.5f, (float)sz - 3.f, (float)sz - 3.f, IT[i].rim, 255);
            ks_rect(t, 2.5f, 2.5f, (float)sz - 5.f, (float)sz - 5.f, IT[i].col, 255);
            if (IT[i].ch) {
                KCanvas cv;
                kc_init(&cv, t->px, sz, sz, sz);
                int gx = (sz - 8) / 2, gy = (sz - 16) / 2;
                kc_glyph(&cv, gx, gy, (uint32_t)IT[i].ch, 0xFFFFFF, 1);
                kc_glyph(&cv, gx + 1, gy, (uint32_t)IT[i].ch, 0xFFFFFF, 1);
            } else {                                     /* the point item: a sparkle */
                ks_star(t, m, m, m - 3.f, 1.6f, 4, 0.f, 0xFFFFFF, 255);
            }
        }
        ks_flatten(t, 0x303050);
        A.item[i] = t;
    }
}

static KSprite *glow_disc(int size, uint32_t col, float power) {
    KSprite *s = ks_new(size, size);
    float m = (float)size / 2.f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = (float)x + 0.5f - m, dy = (float)y + 0.5f - m;
            float d = sqrtf(dx * dx + dy * dy) / m;
            if (d >= 1.f) continue;
            float k = 1.f - d;
            k = powf(k, power);
            int lv = (int)(k * 256.f);
            s->px[y * size + x] = ks_mul_px(col, lv);
        }
    s->ax = size / 2;
    s->ay = size / 2;
    return s;
}

static void build_effects(void) {
    for (int i = 0; i < 3; i++) {
        int sz = 5 + i * 2;
        KSprite *s = ks_new(sz, sz);
        ks_star(s, sz / 2.f, sz / 2.f, sz / 2.f, sz / 7.f, 4, 0.f, 0xFFFFFF, 255);
        ks_flatten(s, 0x202040);
        A.spark[i] = s;
    }
    for (int c = 0; c < BC_COUNT; c++)
        for (int i = 0; i < 3; i++) A.puff[c][i] = glow_disc(14 + i * 14, BPAL[c].base, 1.5f);
    A.halo = glow_disc(36, 0x909090, 2.0f);
    static const uint32_t ORBS[7] = { 0xF0303C, 0xF08A20, 0xF2E030, 0x38C452, 0x30B8E8, 0x4A5CF0, 0xC050E0 };
    for (int i = 0; i < 7; i++) {
        KSprite *s = glow_disc(44, ORBS[i], 1.2f);
        for (int y = 14; y < 30; y++)                      /* a bright core */
            for (int x = 14; x < 30; x++) {
                float dx = (float)x - 21.5f, dy = (float)y - 21.5f;
                float d = sqrtf(dx * dx + dy * dy);
                if (d < 8.f) s->px[y * 44 + x] = ks_add_px(s->px[y * 44 + x] == KS_KEY ? 0 : s->px[y * 44 + x], ks_mul_px(0xFFFFFF, (int)((1.f - d / 8.f) * 200.f)));
            }
        A.orb[i] = s;
    }
    A.petal = ks_new(7, 5);
    ks_ellipse(A.petal, 3.5f, 2.5f, 3.1f, 1.9f, 0.3f, 0xF8B8D0, 255);
    ks_flatten(A.petal, 0x303050);

    /* the magic circle: six frames, because the pattern repeats every 60 degrees */
    for (int f = 0; f < 6; f++) {
        const int S = 168;
        KSprite *s = ks_new(S, S);
        ks_fill(s, 0x000000);
        float m = S / 2.f, rot = (float)f * 10.f * 3.14159265f / 180.f;
        ks_ring(s, m, m, 78.f, 2.f, 0xA0A0A0, 255);
        ks_ring(s, m, m, 68.f, 1.4f, 0x707070, 255);
        ks_ring(s, m, m, 40.f, 1.4f, 0x808080, 255);
        for (int k = 0; k < 6; k++) {                          /* a hexagram between the rings */
            float a0 = rot + (float)k * 3.14159265f / 3.f * 2.f, a1 = a0 + 3.14159265f / 3.f * 2.f;
            ks_line(s, m + 68.f * cosf(a0), m + 68.f * sinf(a0), m + 68.f * cosf(a1), m + 68.f * sinf(a1), 1.5f, 0xB0B0B0, 255);
        }
        for (int k = 0; k < 36; k++) {                         /* tick marks and rune dots */
            float a = rot + (float)k * 3.14159265f / 18.f;
            float r0 = (k % 3 == 0) ? 70.f : 74.f;
            ks_line(s, m + r0 * cosf(a), m + r0 * sinf(a), m + 78.f * cosf(a), m + 78.f * sinf(a), 1.2f, 0x909090, 255);
            if (k % 3 == 0) ks_disc(s, m + 54.f * cosf(a + 0.1f), m + 54.f * sinf(a + 0.1f), 1.8f, 0xC0C0C0, 255);
        }
        for (int i = 0; i < S * S; i++) if (s->px[i] == 0) s->px[i] = KS_KEY;
        s->ax = S / 2;
        s->ay = S / 2;
        A.circle[f] = s;
    }
    /* the hitbox marker */
    KSprite *h = ks_new(11, 11);
    ks_disc(h, 5.5f, 5.5f, 5.2f, 0xC8102E, 255);
    ks_disc(h, 5.5f, 5.5f, 4.0f, 0xFFFFFF, 255);
    ks_disc(h, 5.5f, 5.5f, 2.6f, 0xFF4060, 255);
    ks_disc(h, 5.5f, 5.5f, 1.4f, 0xFFFFFF, 255);
    ks_flatten(h, 0x303050);
    A.hitbox = h;
}

/* ---------------------------------------------------------------- helpers */

KSprite *spr_scale_nn(const KSprite *s, int k) {
    KSprite *d = ks_new(s->w * k, s->h * k);
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) d->px[y * d->w + x] = s->px[(y / k) * s->w + x / k];
    d->ax = s->ax * k;
    d->ay = s->ay * k;
    return d;
}

void spr_shear(KSprite *s, int lean) {
    uint32_t *copy = malloc((size_t)s->w * (size_t)s->h * 4);
    if (!copy) return;
    memcpy(copy, s->px, (size_t)s->w * (size_t)s->h * 4);
    ks_clear(s);
    for (int y = 0; y < s->h; y++) {
        int dx = lean * (s->h - 1 - 2 * y) / (2 * (s->h - 1));
        for (int x = 0; x < s->w; x++) {
            int nx = x + dx;
            if (nx >= 0 && nx < s->w) s->px[y * s->w + nx] = copy[y * s->w + x];
        }
    }
    free(copy);
}

/* ---------------------------------------------------------------- the build driver */

static const char *const STAGE_LABELS[GFX_STAGES] = {
    "Rolling the pellets", "Polishing the kunai", "Folding the ofuda", "Dressing the fairies", "Waking the bosses", "Brushing the heroines",
    "Hanging the lanterns", "Painting the night sky", "Teaching the fairies to dance",
};
const char *gfx_stage_label(int stage) { return STAGE_LABELS[stage < 0 || stage >= GFX_STAGES ? 0 : stage]; }

void gfx_build_stage(int stage) {
    switch (stage) {
    case 0: build_bullets(0); break;
    case 1: build_bullets(1); break;
    case 2: build_bullets(2); build_shots_items(); build_effects(); break;
    case 3: chars_build(0); break;
    case 4: chars_build(1); break;
    case 5: chars_build(2); break;
    case 6: titleart_build(1); break;
    case 7: titleart_build(0); bg_build(0); break;
    default: titleart_build(2); bg_build(1); bg_build(2); break;
    }
}

void gfx_free(void) {
    memset(&A, 0, sizeof A);                                    /* the process exits right after: nothing to give back */
    bg_free();
}
