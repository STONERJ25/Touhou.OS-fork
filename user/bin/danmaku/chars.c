/* Procedural characters: fairies, spirits, the five bosses and the two heroines. Original designs, drawn from polygons and discs. */
#include "gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SKIN 0xFFE2CC
#define SKIN_SH 0xF0C4A8
#define INK  0x2A1E3C

static void blush(KSprite *s, float x, float y, float r) { ks_disc(s, x, y, r, 0xF58AA0, 120); }

static void bow(KSprite *s, float x, float y, float k, uint32_t col, uint32_t knot) {
    ks_ellipse(s, x - 2.6f * k, y, 3.2f * k, 1.9f * k, -0.5f, col, 255);
    ks_ellipse(s, x + 2.6f * k, y, 3.2f * k, 1.9f * k, 0.5f, col, 255);
    ks_disc(s, x, y, 1.3f * k, knot, 255);
}

/* ---------------------------------------------------------------- fairies */

typedef struct FairyStyle { uint32_t dress, dress_dk, hair, hair_hi, accent, wing; } FairyStyle;

static KSprite *make_fairy(const FairyStyle *st, int frame, float k, bool maid) {
    int w = (int)(30.f * k), h = (int)(32.f * k);
    KSprite *s = ks_new(w, h);
    float cx = (float)w / 2.f;
    /* wings */
    float wa = frame == 0 ? 0.65f : 0.2f, wy = (frame == 0 ? 11.f : 13.5f) * k;
    ks_ellipse(s, cx - 8.5f * k, wy, 7.6f * k, 3.7f * k, -wa, st->wing, 215);
    ks_ellipse(s, cx + 8.5f * k, wy, 7.6f * k, 3.7f * k, wa, st->wing, 215);
    ks_ellipse(s, cx - 7.f * k, wy + 6.5f * k, 5.2f * k, 2.7f * k, wa * 0.7f, st->wing, 190);
    ks_ellipse(s, cx + 7.f * k, wy + 6.5f * k, 5.2f * k, 2.7f * k, -wa * 0.7f, st->wing, 190);
    /* legs and shoes */
    ks_line(s, cx - 2.5f * k, 25.f * k, cx - 2.7f * k, 29.2f * k, 2.0f * k, SKIN, 255);
    ks_line(s, cx + 2.5f * k, 25.f * k, cx + 2.7f * k, 29.2f * k, 2.0f * k, SKIN, 255);
    ks_ellipse(s, cx - 2.8f * k, 30.2f * k, 2.4f * k, 1.2f * k, 0.f, INK, 255);
    ks_ellipse(s, cx + 2.8f * k, 30.2f * k, 2.4f * k, 1.2f * k, 0.f, INK, 255);
    /* arms */
    ks_line(s, cx - 3.4f * k, 14.f * k, cx - 7.2f * k, 19.5f * k, 2.2f * k, SKIN, 255);
    ks_line(s, cx + 3.4f * k, 14.f * k, cx + 7.2f * k, 19.5f * k, 2.2f * k, SKIN, 255);
    /* dress */
    float dr[8] = { cx - 3.8f * k, 13.f * k, cx + 3.8f * k, 13.f * k, cx + 7.6f * k, 26.f * k, cx - 7.6f * k, 26.f * k };
    ks_poly_grad_v(s, dr, 4, st->dress, st->dress_dk);
    ks_rect(s, cx - 7.6f * k, 25.f * k, 15.2f * k, 1.3f * k, 0xFFF6E8, 255);
    ks_ellipse(s, cx, 13.2f * k, 4.2f * k, 1.5f * k, 0.f, 0xFFF6E8, 255);
    if (maid) {                                                        /* a white apron and a frilled band */
        float ap[8] = { cx - 2.6f * k, 15.f * k, cx + 2.6f * k, 15.f * k, cx + 5.2f * k, 25.f * k, cx - 5.2f * k, 25.f * k };
        ks_poly(s, ap, 4, 0xFFFFFF, 235);
        ks_line(s, cx - 2.4f * k, 15.f * k, cx + 2.4f * k, 15.f * k, 1.2f * k, st->accent, 255);
    }
    /* head */
    ks_disc(s, cx, 8.6f * k, 6.9f * k, st->hair, 255);
    ks_disc(s, cx, 9.2f * k, 5.3f * k, SKIN, 255);
    ks_ellipse(s, cx, 5.8f * k, 5.8f * k, 2.7f * k, 0.f, st->hair, 255);
    ks_ellipse(s, cx - 2.f * k, 5.4f * k, 2.8f * k, 1.0f * k, -0.3f, st->hair_hi, 200);
    ks_disc(s, cx - 2.2f * k, 9.6f * k, 0.95f * k, INK, 255);
    ks_disc(s, cx + 2.2f * k, 9.6f * k, 0.95f * k, INK, 255);
    ks_disc(s, cx - 2.5f * k, 9.3f * k, 0.35f * k, 0xFFFFFF, 255);
    ks_disc(s, cx + 1.9f * k, 9.3f * k, 0.35f * k, 0xFFFFFF, 255);
    blush(s, cx - 3.6f * k, 11.3f * k, 1.2f * k);
    blush(s, cx + 3.6f * k, 11.3f * k, 1.2f * k);
    if (maid) {
        ks_ellipse(s, cx, 3.6f * k, 5.6f * k, 1.7f * k, 0.f, 0xFFFFFF, 255);
        ks_line(s, cx - 5.f * k, 3.4f * k, cx + 5.f * k, 3.4f * k, 0.8f * k, st->accent, 255);
    }
    bow(s, cx + 4.6f * k, 4.2f * k, k * 0.9f, st->accent, 0x802030);
    ks_outline(s, 0x241838);
    ks_flatten(s, 0x303050);
    s->ax = w / 2;
    s->ay = h / 2;
    return s;
}

static KSprite *make_orb_enemy(int frame, uint32_t a, uint32_t b) {
    const int S = 30;
    KSprite *s = ks_new(S, S);
    float m = S / 2.f, R = 11.2f, rot = frame ? 1.5708f : 0.f;
    float ca = cosf(rot), sa = sinf(rot);
    ks_glow(s, m, m, 14.8f, a, 90);
    ks_disc(s, m, m, R + 1.f, 0x20102C, 255);
    ks_disc(s, m, m, R, b, 255);
    /* local frame: u to the right, v down; the left half is colour a */
    float pts[44];
    int n = 0;
    for (int i = 0; i <= 20; i++) {
        float an = 1.5708f + (float)i * 3.14159265f / 20.f;           /* the left half circle, from the bottom to the top */
        float u = R * cosf(an), v = R * sinf(an);
        pts[n * 2] = m + u * ca - v * sa;
        pts[n * 2 + 1] = m + u * sa + v * ca;
        n++;
    }
    ks_poly(s, pts, n, a, 255);
    float tu = 0.f, tv = -R / 2.f, bu = 0.f, bv = R / 2.f;               /* the two lobes */
    ks_disc(s, m + tu * ca - tv * sa, m + tu * sa + tv * ca, R / 2.f, a, 255);
    ks_disc(s, m + bu * ca - bv * sa, m + bu * sa + bv * ca, R / 2.f, b, 255);
    ks_disc(s, m + tu * ca - tv * sa, m + tu * sa + tv * ca, R / 6.f + 0.4f, b, 255);
    ks_disc(s, m + bu * ca - bv * sa, m + bu * sa + bv * ca, R / 6.f + 0.4f, a, 255);
    ks_disc(s, m - 3.f, m - 4.f, 1.5f, 0xFFFFFF, 110);
    ks_flatten(s, 0x303050);
    s->ax = S / 2;
    s->ay = S / 2;
    return s;
}

static KSprite *make_wisp(int frame) {
    const int w = 30, h = 36;
    KSprite *s = ks_new(w, h);
    float cx = w / 2.f, sway = frame ? 2.2f : -2.2f;
    ks_glow(s, cx, 20.f, 15.f, 0x60C8FF, 100);
    float flame[] = { cx + sway, 1.f, cx + 5.f + sway * 0.4f, 10.f, cx + 9.f, 19.f, cx + 7.f, 29.f, cx, 33.f, cx - 7.f, 29.f, cx - 9.f, 19.f, cx - 5.f + sway * 0.4f, 10.f };
    ks_poly_grad_v(s, flame, 8, 0xB8F0FF, 0x2C8CE0);
    float inner[] = { cx + sway * 0.6f, 9.f, cx + 5.f, 19.f, cx + 3.5f, 27.f, cx, 30.f, cx - 3.5f, 27.f, cx - 5.f, 19.f };
    ks_poly_grad_v(s, inner, 6, 0xFFFFFF, 0xA0E0FF);
    ks_ellipse(s, cx - 3.2f, 20.f, 1.5f, 2.3f, 0.f, INK, 255);
    ks_ellipse(s, cx + 3.2f, 20.f, 1.5f, 2.3f, 0.f, INK, 255);
    ks_disc(s, cx - 3.5f, 19.f, 0.5f, 0xFFFFFF, 255);
    ks_disc(s, cx + 2.9f, 19.f, 0.5f, 0xFFFFFF, 255);
    blush(s, cx - 5.5f, 23.f, 1.4f);
    blush(s, cx + 5.5f, 23.f, 1.4f);
    ks_flatten(s, 0x203050);
    s->ax = w / 2;
    s->ay = 22;
    return s;
}

/* ---------------------------------------------------------------- bosses */

typedef struct BossStyle {
    uint32_t hair, hair_hi, dress, dress_dk, trim, wing, accent;
    int wing_kind, ears, long_hair;
} BossStyle;

static void boss_wings(KSprite *s, const BossStyle *b, int frame, float cx) {
    float a = frame ? 0.15f : 0.55f;
    if (b->wing_kind == 1) {                                          /* four fairy wings */
        ks_ellipse(s, cx - 15.f, 24.f, 16.f, 6.4f, -a - 0.1f, b->wing, 200);
        ks_ellipse(s, cx + 15.f, 24.f, 16.f, 6.4f, a + 0.1f, b->wing, 200);
        ks_ellipse(s, cx - 12.f, 36.f, 11.f, 4.6f, a, b->wing, 180);
        ks_ellipse(s, cx + 12.f, 36.f, 11.f, 4.6f, -a, b->wing, 180);
    } else if (b->wing_kind == 2) {                                    /* leaf-like wings */
        ks_ellipse(s, cx - 13.f, 28.f, 12.f, 4.6f, -a - 0.5f, b->wing, 210);
        ks_ellipse(s, cx + 13.f, 28.f, 12.f, 4.6f, a + 0.5f, b->wing, 210);
    } else if (b->wing_kind == 3) {                                    /* soft bat-like cloaks */
        float l[] = { cx - 4.f, 24.f, cx - 28.f, 14.f + (float)frame * 3.f, cx - 24.f, 40.f, cx - 8.f, 46.f };
        float r[] = { cx + 4.f, 24.f, cx + 28.f, 14.f + (float)frame * 3.f, cx + 24.f, 40.f, cx + 8.f, 46.f };
        ks_poly(s, l, 4, b->wing, 205);
        ks_poly(s, r, 4, b->wing, 205);
    }
}

static KSprite *make_boss_girl(const BossStyle *b, int frame, int variant) {
    const int w = 72, h = 92;
    KSprite *s = ks_new(w, h);
    float cx = w / 2.f;
    float sway = frame ? 1.5f : -1.5f;
    boss_wings(s, b, frame, cx);
    if (b->ears) {                                                     /* long rabbit ears */
        for (int side = -1; side <= 1; side += 2) {
            float ex = cx + (float)side * 8.f, tilt = (float)side * (0.28f + (frame ? 0.1f : 0.f));
            ks_ellipse(s, ex + (float)side * 2.f, 8.f, 4.2f, 14.f, tilt, 0xF4F0FA, 255);
            ks_ellipse(s, ex + (float)side * 2.f, 9.f, 2.3f, 11.f, tilt, 0xF59AB8, 255);
        }
    }
    if (variant == 0) {                                                /* a rayed halo behind the head */
        for (int k = 0; k < 12; k++) {
            float a = (float)k * 3.14159265f / 6.f;
            ks_line(s, cx + 11.f * cosf(a), 21.f + 11.f * sinf(a), cx + 20.f * cosf(a), 21.f + 20.f * sinf(a), 2.2f, 0xFFD080, 230);
        }
    }
    /* back hair */
    if (b->long_hair) {
        float hair[] = { cx - 11.f, 16.f, cx + 11.f, 16.f, cx + 15.f + sway, 52.f, cx + 7.f + sway, 62.f, cx - 7.f + sway, 62.f, cx - 15.f + sway, 52.f };
        ks_poly_grad_v(s, hair, 6, b->hair_hi, b->hair);
    }
    ks_disc(s, cx, 21.f, 11.f, b->hair, 255);
    /* skirt, bodice, arms */
    float skirt[] = { cx - 8.f, 38.f, cx + 8.f, 38.f, cx + 27.f, 82.f, cx - 27.f, 82.f };
    ks_poly_grad_v(s, skirt, 4, b->dress, b->dress_dk);
    ks_rect(s, cx - 27.f, 80.f, 54.f, 3.f, b->trim, 255);
    for (int k = -3; k <= 3; k++) ks_line(s, cx + (float)k * 2.6f, 44.f, cx + (float)k * 7.2f, 80.f, 1.0f, b->dress_dk, 140);
    float top[] = { cx - 7.f, 30.f, cx + 7.f, 30.f, cx + 8.f, 42.f, cx - 8.f, 42.f };
    ks_poly(s, top, 4, 0xFFF6EC, 255);
    ks_rect(s, cx - 8.f, 40.f, 16.f, 3.f, b->accent, 255);
    ks_line(s, cx - 7.f, 32.f, cx - 20.f, 48.f - (float)frame * 2.f, 4.4f, b->dress, 255);
    ks_line(s, cx + 7.f, 32.f, cx + 20.f, 48.f - (float)frame * 2.f, 4.4f, b->dress, 255);
    ks_disc(s, cx - 20.f, 49.f - (float)frame * 2.f, 2.8f, SKIN, 255);
    ks_disc(s, cx + 20.f, 49.f - (float)frame * 2.f, 2.8f, SKIN, 255);
    /* face */
    ks_disc(s, cx, 22.f, 9.2f, SKIN, 255);
    ks_ellipse(s, cx, 14.f, 10.6f, 5.2f, 0.f, b->hair, 255);
    ks_ellipse(s, cx - 3.f, 13.4f, 5.f, 1.6f, -0.3f, b->hair_hi, 210);
    ks_ellipse(s, cx - 8.f, 22.f, 2.6f, 7.f, 0.2f, b->hair, 255);
    ks_ellipse(s, cx + 8.f, 22.f, 2.6f, 7.f, -0.2f, b->hair, 255);
    for (int side = -1; side <= 1; side += 2) {
        float ex = cx + (float)side * 3.8f;
        ks_ellipse(s, ex, 22.6f, 1.7f, 2.3f, 0.f, 0x1C1030, 255);
        ks_disc(s, ex - 0.4f, 21.6f, 0.7f, 0xFFFFFF, 255);
        blush(s, cx + (float)side * 6.2f, 25.6f, 1.8f);
    }
    ks_line(s, cx - 1.4f, 26.6f, cx + 1.4f, 26.6f, 0.9f, 0xA04050, 255);
    bow(s, cx, 11.f, 1.6f, b->accent, 0x802030);
    if (variant == 1) {                                                /* a bamboo staff and a leaf in the hair */
        ks_line(s, cx + 24.f, 30.f, cx + 26.f, 86.f, 3.6f, 0x6EBE5A, 255);
        for (int k = 0; k < 4; k++) ks_rect(s, cx + 22.f, 40.f + (float)k * 14.f, 7.f, 1.8f, 0x2E7A3A, 255);
        ks_ellipse(s, cx + 7.f, 10.f, 6.f, 2.2f, 0.7f, 0x4CC060, 255);
    } else if (variant == 2) {                                         /* a mochi mallet */
        ks_line(s, cx + 22.f, 52.f, cx + 30.f, 22.f, 3.2f, 0xB07840, 255);
        ks_rect(s, cx + 22.f, 12.f, 16.f, 12.f, 0xC89A60, 255);
        ks_rect(s, cx + 22.f, 12.f, 16.f, 2.4f, 0xE8C890, 255);
        ks_rect(s, cx + 22.f, 22.f, 16.f, 2.f, 0x8A5A2C, 255);
    } else if (variant == 4) {                                         /* a crescent brooch and a few stars */
        ks_disc(s, cx, 35.f, 4.6f, 0xFFF0B0, 255);
        ks_disc(s, cx + 1.8f, 34.f, 4.1f, 0xE8E4FF, 255);
        ks_disc(s, cx + 1.8f, 34.f, 3.9f, b->dress, 255);
        for (int k = 0; k < 7; k++) ks_star(s, cx - 20.f + (float)((k * 37) % 40), 52.f + (float)((k * 23) % 26), 2.2f, 1.0f, 4, 0.f, 0xFFFFFF, 220);
    } else if (variant == 0) {
        ks_star(s, cx, 36.f, 4.4f, 2.0f, 6, 0.f, 0xFFD060, 255);
    }
    ks_outline(s, 0x241838);
    ks_flatten(s, 0x303050);
    s->ax = w / 2;
    s->ay = 44;
    return s;
}

static KSprite *make_boss_lantern(int frame) {
    const int w = 72, h = 92;
    KSprite *s = ks_new(w, h);
    float cx = w / 2.f, sway = frame ? 2.f : -2.f;
    /* a ghostly tail below the lantern */
    float tail[] = { cx - 12.f, 62.f, cx + 12.f, 62.f, cx + 6.f + sway, 80.f, cx + sway, 90.f, cx - 6.f + sway, 80.f };
    ks_poly_grad_v(s, tail, 5, 0xD8C8F8, 0x6A58C0);
    ks_glow(s, cx, 40.f, 34.f, 0xFF9040, 110);
    /* the lantern: a round paper body with ribs */
    ks_ellipse(s, cx, 38.f, 24.f, 26.f, 0.f, 0x3A1A18, 255);
    ks_ellipse_grad(s, cx, 38.f, 22.5f, 24.5f, 0.f, 0xFFD070, 0xD8401C, -0.2f, -0.2f);
    for (int k = -3; k <= 3; k++) {
        float yy = 38.f + (float)k * 6.6f, half = 22.5f * sqrtf(1.f - (float)(k * k) / 14.5f);
        ks_line(s, cx - half, yy, cx + half, yy, 1.2f, 0x7A1C10, 200);
    }
    ks_rect(s, cx - 13.f, 10.f, 26.f, 6.f, 0x24100C, 255);                 /* black caps */
    ks_rect(s, cx - 13.f, 60.f, 26.f, 6.f, 0x24100C, 255);
    ks_line(s, cx, 4.f, cx, 10.f, 1.6f, 0x24100C, 255);
    ks_rect(s, cx - 3.f, 66.f, 6.f, 9.f, 0xF0B040, 255);                   /* the tassel */
    /* the face on the paper */
    for (int side = -1; side <= 1; side += 2) {
        float ex = cx + (float)side * 8.f;
        ks_ellipse(s, ex, 34.f, 5.4f, 6.4f, 0.f, 0xFFF8E0, 255);
        ks_ellipse(s, ex, 35.f, 2.6f, 3.6f, 0.f, 0x5A1810, 255);
        ks_disc(s, ex - 1.f, 33.f, 1.0f, 0xFFFFFF, 255);
        blush(s, cx + (float)side * 14.f, 43.f, 2.6f);
    }
    ks_line(s, cx - 5.f, 46.f, cx - 2.f, 49.f, 1.4f, 0x5A1810, 255);
    ks_line(s, cx - 2.f, 49.f, cx + 2.f, 49.f, 1.4f, 0x5A1810, 255);
    ks_line(s, cx + 2.f, 49.f, cx + 5.f, 46.f, 1.4f, 0x5A1810, 255);
    ks_disc(s, cx - 24.f, 50.f - (float)frame * 2.f, 3.4f, 0xF4EEFF, 255);     /* tiny ghost hands */
    ks_disc(s, cx + 24.f, 50.f - (float)frame * 2.f, 3.4f, 0xF4EEFF, 255);
    ks_outline(s, 0x241838);
    ks_flatten(s, 0x303050);
    s->ax = w / 2;
    s->ay = 40;
    return s;
}

/* ---------------------------------------------------------------- the heroines */

static KSprite *make_player(int type, int lean) {
    const int w = 36, h = 46;
    KSprite *s = ks_new(w, h);
    float cx = w / 2.f;
    if (type == SHOT_A) {                                              /* the shrine maiden */
        float hair[] = { cx - 8.f, 10.f, cx + 8.f, 10.f, cx + 11.f, 34.f, cx - 11.f, 34.f };
        ks_poly_grad_v(s, hair, 4, 0x3A2C48, 0x180E22);
        ks_disc(s, cx, 14.f, 9.f, 0x241830, 255);
        float skirt[] = { cx - 8.f, 29.f, cx + 8.f, 29.f, cx + 16.f, 43.f, cx - 16.f, 43.f };
        ks_poly_grad_v(s, skirt, 4, 0xE02840, 0x9A1428);
        ks_rect(s, cx - 16.f, 41.5f, 32.f, 2.f, 0xFFF6EA, 255);
        ks_ellipse(s, cx - 15.f, 26.f, 4.4f, 8.f, 0.35f, 0xFFF6EA, 255);        /* detached sleeves */
        ks_ellipse(s, cx + 15.f, 26.f, 4.4f, 8.f, -0.35f, 0xFFF6EA, 255);
        ks_disc(s, cx - 17.f, 32.f, 2.2f, SKIN, 255);
        ks_disc(s, cx + 17.f, 32.f, 2.2f, SKIN, 255);
        float top[] = { cx - 6.f, 21.f, cx + 6.f, 21.f, cx + 8.f, 31.f, cx - 8.f, 31.f };
        ks_poly(s, top, 4, 0xFFF6EA, 255);
        ks_rect(s, cx - 8.f, 28.f, 16.f, 2.6f, 0xF2D030, 255);
        ks_ellipse(s, cx, 21.4f, 5.4f, 1.8f, 0.f, 0xE02840, 255);
        ks_disc(s, cx, 15.f, 6.6f, SKIN, 255);
        ks_ellipse(s, cx, 10.4f, 7.f, 3.6f, 0.f, 0x241830, 255);
        ks_ellipse(s, cx - 4.8f, 15.f, 1.6f, 5.f, 0.15f, 0x241830, 255);
        ks_ellipse(s, cx + 4.8f, 15.f, 1.6f, 5.f, -0.15f, 0x241830, 255);
        ks_disc(s, cx - 2.4f, 15.6f, 0.95f, INK, 255);
        ks_disc(s, cx + 2.4f, 15.6f, 0.95f, INK, 255);
        blush(s, cx - 4.2f, 17.6f, 1.2f);
        blush(s, cx + 4.2f, 17.6f, 1.2f);
        bow(s, cx, 6.2f, 2.2f, 0xE02840, 0x8E0B21);
    } else {                                                           /* the witch */
        float hair[] = { cx - 9.f, 11.f, cx + 9.f, 11.f, cx + 14.f, 33.f, cx + 8.f, 40.f, cx - 8.f, 38.f, cx - 14.f, 30.f };
        ks_poly_grad_v(s, hair, 6, 0xFFE890, 0xD8A838);
        ks_disc(s, cx, 14.f, 9.f, 0xF4D468, 255);
        float skirt[] = { cx - 7.f, 28.f, cx + 7.f, 28.f, cx + 15.f, 43.f, cx - 15.f, 43.f };
        ks_poly_grad_v(s, skirt, 4, 0x4A3C8C, 0x1E1648);
        float apron[] = { cx - 4.f, 30.f, cx + 4.f, 30.f, cx + 9.f, 43.f, cx - 9.f, 43.f };
        ks_poly(s, apron, 4, 0xFFFFFF, 240);
        ks_ellipse(s, cx - 13.f, 27.f, 4.f, 6.5f, 0.4f, 0x4A3C8C, 255);
        ks_ellipse(s, cx + 13.f, 27.f, 4.f, 6.5f, -0.4f, 0x4A3C8C, 255);
        ks_disc(s, cx - 16.f, 32.f, 2.2f, SKIN, 255);
        ks_disc(s, cx + 16.f, 32.f, 2.2f, SKIN, 255);
        float top[] = { cx - 6.f, 21.f, cx + 6.f, 21.f, cx + 7.f, 30.f, cx - 7.f, 30.f };
        ks_poly(s, top, 4, 0x4A3C8C, 255);
        ks_ellipse(s, cx, 21.6f, 4.4f, 1.6f, 0.f, 0xFFFFFF, 255);
        ks_disc(s, cx, 15.f, 6.6f, SKIN, 255);
        ks_ellipse(s, cx, 10.6f, 7.f, 3.4f, 0.f, 0xF4D468, 255);
        ks_ellipse(s, cx - 4.8f, 15.f, 1.6f, 5.f, 0.15f, 0xF4D468, 255);
        ks_ellipse(s, cx + 4.8f, 15.f, 1.6f, 5.f, -0.15f, 0xF4D468, 255);
        ks_disc(s, cx - 2.4f, 15.6f, 0.95f, INK, 255);
        ks_disc(s, cx + 2.4f, 15.6f, 0.95f, INK, 255);
        blush(s, cx - 4.2f, 17.6f, 1.2f);
        blush(s, cx + 4.2f, 17.6f, 1.2f);
        ks_ellipse(s, cx, 8.f, 13.f, 2.6f, 0.f, 0x1E1648, 255);                  /* the hat */
        float hat[] = { cx - 7.f, 8.f, cx + 7.f, 8.f, cx + 2.f, 0.8f, cx - 3.f, 0.5f };
        ks_poly_grad_v(s, hat, 4, 0x3A2C78, 0x1E1648);
        ks_rect(s, cx - 7.f, 6.4f, 14.f, 2.4f, 0xFFF6EA, 255);
        ks_star(s, cx + 6.f, 7.6f, 2.4f, 1.0f, 5, 0.f, 0xF2D030, 255);
    }
    ks_outline(s, 0x241838);
    ks_flatten(s, 0x303050);
    if (lean) spr_shear(s, lean);
    s->ax = w / 2;
    s->ay = 24;
    return s;
}

static KSprite *make_option(int type) {
    KSprite *s = ks_new(15, 15);
    if (type == SHOT_A) {
        ks_disc(s, 7.5f, 7.5f, 7.f, 0x20102C, 255);
        ks_disc(s, 7.5f, 7.5f, 6.1f, 0xFFF6EA, 255);
        float half[40];
        int n = 0;
        for (int i = 0; i <= 12; i++) {
            float a = -1.5708f + (float)i * 3.14159265f / 12.f;
            half[n * 2] = 7.5f + 6.1f * cosf(a + 3.14159265f);
            half[n * 2 + 1] = 7.5f + 6.1f * sinf(a + 3.14159265f);
            n++;
        }
        ks_poly(s, half, n, 0xE02840, 255);
        ks_disc(s, 7.5f, 4.4f, 3.f, 0xE02840, 255);
        ks_disc(s, 7.5f, 10.6f, 3.f, 0xFFF6EA, 255);
        ks_disc(s, 7.5f, 4.4f, 1.1f, 0xFFF6EA, 255);
        ks_disc(s, 7.5f, 10.6f, 1.1f, 0xE02840, 255);
    } else {
        ks_glow(s, 7.5f, 7.5f, 7.5f, 0xFFE070, 180);
        ks_star(s, 7.5f, 7.7f, 6.2f, 2.8f, 5, 0.f, 0xF2B820, 255);
        ks_star(s, 7.5f, 7.7f, 4.4f, 2.0f, 5, 0.f, 0xFFF4B0, 255);
    }
    ks_flatten(s, 0x303050);
    return s;
}

void chars_build(int part) {
    if (part == 0) {
        static const FairyStyle FS[4] = {
            { 0x4C82E6, 0x2A4CA8, 0x6AA6FF, 0xB8D6FF, 0xE8546A, 0xDDF2FF },     /* blue */
            { 0xE04A66, 0xA02444, 0xE8745A, 0xFFB090, 0xFFD858, 0xFFE0E4 },     /* red */
            { 0x44B864, 0x1E7A3E, 0x78DC84, 0xC4F6C8, 0xF28AB8, 0xE0FFE8 },     /* green */
            { 0xE6B834, 0xA67A10, 0xFFD864, 0xFFF0A8, 0x58A0F0, 0xFFF6D0 },     /* yellow */
        };
        for (int i = 0; i < 4; i++)
            for (int f = 0; f < 2; f++) A.enemy[i][f] = make_fairy(&FS[i], f, 1.f, false);
        static const FairyStyle MAID = { 0xE86AA8, 0xA83272, 0xFF9AC0, 0xFFD0E4, 0x58A0F0, 0xFFE8F4 };
        for (int f = 0; f < 2; f++) {
            A.enemy[ET_MAID][f] = make_fairy(&MAID, f, 1.45f, true);
            A.enemy[ET_ORB][f] = make_orb_enemy(f, 0xF04A3A, 0xFFF4E4);
            A.enemy[ET_WISP][f] = make_wisp(f);
        }
    } else if (part == 1) {
        static const BossStyle BS[5] = {
            { 0xFFC850, 0xFFEEA0, 0xF2784C, 0xB8381C, 0xFFE070, 0xFFD8A0, 0xE04060, 1, 0, 1 },     /* Asahi: dawn */
            { 0x3C9858, 0x78D890, 0x4CA85A, 0x1C6A34, 0xE8E0A0, 0xA8F0B8, 0xF2E070, 2, 0, 0 },     /* Takeru: bamboo */
            { 0xF4D8F0, 0xFFFFFF, 0xF2D868, 0xB88C20, 0xFFFFFF, 0xE8F4FF, 0xF47AA0, 0, 1, 1 },     /* Mochiko: rabbit */
            { 0 },                                                                                /* Chochin is drawn separately */
            { 0x3C2A74, 0x7A64C8, 0x4C3C98, 0x1C1448, 0xF0D8FF, 0x6A58C0, 0xC0A0FF, 3, 0, 1 },     /* Yozora: midnight */
        };
        for (int id = 0; id < 5; id++)
            for (int f = 0; f < 2; f++) A.boss[id][f] = id == 3 ? make_boss_lantern(f) : make_boss_girl(&BS[id], f, id);
        for (int id = 0; id < 5; id++) {                               /* a white-hot copy for the hit flash */
            KSprite *c = ks_clone(A.boss[id][0]);
            for (int i = 0; i < c->w * c->h; i++) if (c->px[i] != KS_KEY) c->px[i] = ks_lerp_px(c->px[i], 0xFFFFFF, 120);
            A.boss_flash[id] = c;
        }
    } else {
        for (int t = 0; t < SHOT_COUNT; t++) {
            A.player[t][0] = make_player(t, 0);
            A.player[t][1] = make_player(t, -4);
            A.player[t][2] = make_player(t, 4);
            A.option[t] = make_option(t);
        }
    }
}
