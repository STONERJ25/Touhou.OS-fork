/* Scrolling backgrounds: for every stage a tall tile (copied with rep movsq), a darker version for spell cards, and sparse parallax
 * layers of colour-keyed sprites that scroll faster. */
#include "gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LAYER_ITEMS 40

typedef struct LayerItem { int x, y0, spd16; const KSprite *spr; bool additive; bool spell_ok; bool soft; } LayerItem;

typedef struct StageBg {
    KSprite *base, *dark;
    LayerItem items[MAX_LAYER_ITEMS];
    int n;
    KSprite *owned[24];
    int nowned;
} StageBg;

static StageBg g_bg[N_STAGES];
static KSprite *g_lantern_cache;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

static KSprite *own(StageBg *b, KSprite *s) {
    if (b->nowned < 24) b->owned[b->nowned++] = s;
    return s;
}

static void add_item(StageBg *b, int x, int y0, int spd16, const KSprite *spr, bool additive) {
    if (b->n >= MAX_LAYER_ITEMS) return;
    b->items[b->n++] = (LayerItem){ x, y0, spd16, spr, additive, additive || spr->w <= 12, false };   /* only glows and sparkles survive the darker spell backdrop */
}

/* clouds and lanterns keep their soft translucent edges: they are blended instead of colour-keyed */
static void add_soft(StageBg *b, int x, int y0, int spd16, const KSprite *spr) {
    add_item(b, x, y0, spd16, spr, false);
    if (b->n > 0) b->items[b->n - 1].soft = true;
}

/* draws a soft disc and repeats it a tile height above and below, so that the tile wraps vertically */
static void wdisc(KSprite *s, float x, float y, float r, uint32_t col, int alpha) {
    for (int k = -1; k <= 1; k++) ks_glow(s, x, y + (float)k * BG_H, r * 1.5f, col, alpha * 2);
}

static void vgradient(KSprite *s, uint32_t a, uint32_t b, uint32_t c) {      /* a at the top and bottom, b in the middle, c at 3/4 */
    (void)c;
    for (int y = 0; y < s->h; y++) {
        int k = y < s->h / 2 ? y * 512 / s->h : (s->h - 1 - y) * 512 / s->h;
        ks_fill32(s->px + (size_t)y * s->w, ks_lerp_px(a, b, k), (size_t)s->w);
    }
}

static KSprite *darken(const KSprite *base, uint32_t tint, int level) {
    KSprite *d = ks_clone(base);
    for (int i = 0; i < d->w * d->h; i++) d->px[i] = ks_lerp_px(ks_mul_px(d->px[i], level), tint, 40);
    return d;
}

/* ---------------------------------------------------------------- stage 1: the cloud road */

static KSprite *cloud_sprite(StageBg *b, int w, int h, uint32_t col, uint32_t backdrop, uint32_t seed) {
    KSprite *s = ks_new(w, h);
    for (int i = 0; i < 9; i++) {
        uint32_t r = hash32(seed * 31 + (uint32_t)i);
        float x = (float)w * 0.14f + (float)(r % (uint32_t)(w * 70 / 100)), rad = (float)h * (0.28f + (float)((r >> 8) % 30) / 100.f);
        float y = (float)h * 0.6f - (float)((r >> 16) % (uint32_t)(h / 4 + 1));
        ks_glow(s, x, y, rad * 1.6f, col, 230);
        ks_disc(s, x, y, rad * 0.7f, col, 120);
    }
    (void)backdrop;
    return own(b, s);
}

static void build_stage1(void) {
    StageBg *b = &g_bg[0];
    KSprite *t = ks_new(BG_W, BG_H);
    vgradient(t, 0x0C1040, 0x4A3C8C, 0);
    for (int i = 0; i < 120; i++) {
        uint32_t r = hash32((uint32_t)i * 977 + 3);
        ks_put(t, (int)(r % BG_W), (int)((r >> 12) % BG_H), KRGB(150 + (r & 63), 150 + (r & 63), 190 + (r & 63) / 2), 255);
    }
    for (int i = 0; i < 18; i++) {                                  /* big soft clouds baked into the tile */
        uint32_t r = hash32((uint32_t)i * 4231 + 17);
        float cx = (float)(r % BG_W), cy = (float)((r >> 10) % BG_H);
        for (int k = 0; k < 6; k++) {
            uint32_t q = hash32(r + (uint32_t)k * 7919);
            wdisc(t, cx + (float)((q % 90) - 45), cy + (float)(((q >> 8) % 24) - 12), 16.f + (float)((q >> 16) % 22), i & 1 ? 0xC8B8F0 : 0xF0C0D8, 34);
        }
    }
    b->base = t;
    b->dark = darken(t, 0x701850, 105);
    for (int i = 0; i < 9; i++) {
        KSprite *c = cloud_sprite(b, 96 + (i % 3) * 24, 44, i & 1 ? 0xDCD4FF : 0xFFD8E8, 0x4A3C8C, (uint32_t)i + 1);
        add_soft(b, (int)(hash32((uint32_t)i * 53 + 5) % (BG_W - 40)), (int)(hash32((uint32_t)i * 29 + 9) % 600), 22 + (i % 3) * 5, c);
    }
    KSprite *spark = own(b, ks_new(9, 9));
    ks_star(spark, 4.5f, 4.5f, 4.4f, 0.9f, 4, 0.f, 0xFFFFFF, 255);
    ks_flatten(spark, 0x1A1850);
    KSprite *spark2 = own(b, ks_new(7, 7));
    ks_star(spark2, 3.5f, 3.5f, 3.4f, 0.8f, 4, 0.f, 0xFFE8A0, 255);
    ks_flatten(spark2, 0x1A1850);
    for (int i = 0; i < 18; i++)
        add_item(b, (int)(hash32((uint32_t)i * 71 + 2) % BG_W), (int)(hash32((uint32_t)i * 13 + 1) % 600), 52 + (i % 4) * 10, i & 1 ? spark : spark2, false);
}

/* ---------------------------------------------------------------- stage 2: the bamboo hollow */

static void stalk(KSprite *s, int x, int w, uint32_t body, uint32_t hi, uint32_t joint, int seg, int phase) {
    ks_rect(s, (float)x, 0, (float)w, (float)s->h, body, 255);
    ks_rect(s, (float)x, 0, (float)w / 3.f + 1.f, (float)s->h, hi, 255);
    for (int y = phase % seg; y < s->h; y += seg) ks_rect(s, (float)x - 1.f, (float)y, (float)w + 2.f, 3.f, joint, 255);
}

static void build_stage2(void) {
    StageBg *b = &g_bg[1];
    KSprite *t = ks_new(BG_W, BG_H);
    vgradient(t, 0x061A14, 0x0E3A2A, 0);
    for (int i = 0; i < 16; i++) {                                  /* distant stalks, thin and dim */
        uint32_t r = hash32((uint32_t)i * 313 + 5);
        int x = (int)(r % BG_W), w = 4 + (int)((r >> 8) % 5);
        int seg = 64;
        ks_rect(t, (float)x, 0, (float)w, BG_H, ks_lerp_px(0x0E3A2A, 0x1C5A40, 100 + (int)(r >> 20 & 63)), 255);
        for (int y = (int)((r >> 4) % 64); y < BG_H; y += seg) ks_rect(t, (float)x - 1.f, (float)y, (float)w + 2.f, 2.f, 0x0A2A1C, 255);
    }
    for (int i = 0; i < 7; i++) {                                   /* mist */
        uint32_t r = hash32((uint32_t)i * 977 + 41);
        float y = (float)((r >> 8) % BG_H);
        for (int k = 0; k < 7; k++) wdisc(t, (float)((r + (uint32_t)k * 63) % BG_W), y + (float)(k % 3) * 6.f, 44.f, 0x7AC8A0, 12);
    }
    b->base = t;
    b->dark = darken(t, 0x082820, 85);
    for (int i = 0; i < 5; i++) {                                   /* mid stalks, one long keyed strip each */
        KSprite *st = own(b, ks_new(16, BG_H));
        stalk(st, 3, 10, 0x2A7A4A, 0x4CB070, 0x124A28, 70 + i * 6, i * 23);
        ks_flatten(st, 0x0A2A1C);
        add_item(b, 10 + i * 78 + (int)(hash32((uint32_t)i + 8) % 30), 0, 48, st, false);
    }
    KSprite *leaf = own(b, ks_new(24, 10));
    ks_ellipse(leaf, 12.f, 5.f, 11.f, 3.4f, 0.35f, 0x3AA860, 255);
    ks_line(leaf, 2.f, 8.f, 22.f, 2.f, 0.8f, 0x8AE8A8, 255);
    ks_flatten(leaf, 0x0E3A2A);
    for (int i = 0; i < 12; i++) add_item(b, (int)(hash32((uint32_t)i * 91 + 7) % BG_W), (int)(hash32((uint32_t)i * 17 + 4) % 600), 85 + (i % 4) * 14, leaf, false);
    KSprite *fly = own(b, ks_new(15, 15));
    for (int y = 0; y < 15; y++)
        for (int x = 0; x < 15; x++) {
            float dx = (float)x - 7.f, dy = (float)y - 7.f, d = sqrtf(dx * dx + dy * dy) / 7.5f;
            if (d < 1.f) fly->px[y * 15 + x] = ks_mul_px(0xE8FF80, (int)((1.f - d) * (1.f - d) * 220.f));
        }
    for (int i = 0; i < 10; i++) add_item(b, (int)(hash32((uint32_t)i * 37 + 3) % BG_W), (int)(hash32((uint32_t)i * 101 + 6) % 600), 30 + (i % 3) * 12, fly, true);
}

/* ---------------------------------------------------------------- stage 3: the lantern lake */

static KSprite *paper_lantern(StageBg *b, uint32_t col, float k) {
    int w = (int)(30.f * k), h = (int)(38.f * k);
    KSprite *s = ks_new(w, h);
    float cx = (float)w / 2.f, cy = (float)h / 2.f;
    ks_glow(s, cx, cy, (float)w / 2.f, col, 130);
    ks_ellipse(s, cx, cy, 8.5f * k, 11.f * k, 0.f, 0x3A1008, 255);
    ks_ellipse_grad(s, cx, cy, 7.6f * k, 10.2f * k, 0.f, 0xFFF0B0, col, -0.15f, -0.2f);
    for (int i = -2; i <= 2; i++) ks_line(s, cx - 7.f * k, cy + (float)i * 4.f * k, cx + 7.f * k, cy + (float)i * 4.f * k, 0.9f * k, 0x7A2810, 160);
    ks_rect(s, cx - 3.5f * k, cy - 12.f * k, 7.f * k, 2.f * k, 0x3A1008, 255);
    ks_rect(s, cx - 3.5f * k, cy + 10.f * k, 7.f * k, 2.f * k, 0x3A1008, 255);
    return own(b, s);
}

static void build_stage3(void) {
    StageBg *b = &g_bg[2];
    KSprite *t = ks_new(BG_W, BG_H);
    vgradient(t, 0x06092A, 0x142468, 0);
    for (int i = 0; i < 140; i++) {                                 /* star reflections */
        uint32_t r = hash32((uint32_t)i * 977 + 11);
        ks_put(t, (int)(r % BG_W), (int)((r >> 12) % BG_H), KRGB(110 + (r & 63), 130 + (r & 63), 190 + (r & 63) / 2), 255);
    }
    for (int i = 0; i < 26; i++) {                                  /* ripples: wide flat rings */
        uint32_t r = hash32((uint32_t)i * 4231 + 17);
        float cx = (float)(r % BG_W), cy = (float)((r >> 10) % BG_H), rx = 14.f + (float)((r >> 4) % 40), ry = rx * 0.28f;
        for (int k = -1; k <= 1; k++) {
            ks_ellipse(t, cx, cy + (float)k * BG_H, rx, ry, 0.f, 0x5878C8, 38);
            ks_ellipse(t, cx, cy + (float)k * BG_H, rx - 1.6f, ry - 0.5f, 0.f, 0x142468, 255);
        }
    }
    for (int i = 0; i < 30; i++) {                                  /* streaks of moonlight */
        uint32_t r = hash32((uint32_t)i * 313 + 29);
        ks_rect(t, 170.f + (float)(r % 120) - 60.f, (float)((r >> 9) % BG_H), 10.f + (float)((r >> 4) % 70), 1.f, 0xB8C8FF, 70);
    }
    b->base = t;
    b->dark = darken(t, 0x30106A, 80);
    static const uint32_t LC[4] = { 0xF28A30, 0xF2603C, 0xF2B040, 0xE8507C };
    for (int i = 0; i < 12; i++) {
        KSprite *l = paper_lantern(b, LC[i % 4], 0.8f + (float)(i % 3) * 0.2f);
        add_soft(b, (int)(hash32((uint32_t)i * 53 + 5) % (BG_W - 30)), (int)(hash32((uint32_t)i * 29 + 9) % 600), 34 + (i % 4) * 7, l);
    }
    KSprite *ring = own(b, ks_new(34, 12));
    ks_fill(ring, 0x000000);
    ks_ellipse(ring, 17.f, 6.f, 15.f, 4.6f, 0.f, 0x4868B8, 255);
    ks_ellipse(ring, 17.f, 6.f, 13.4f, 3.8f, 0.f, 0x000000, 255);
    for (int i = 0; i < 34 * 12; i++) if (ring->px[i] == 0) ring->px[i] = KS_KEY;
    for (int i = 0; i < 9; i++) add_item(b, (int)(hash32((uint32_t)i * 77 + 1) % (BG_W - 20)), (int)(hash32((uint32_t)i * 19 + 2) % 600), 70 + (i % 3) * 14, ring, true);
}

void bg_build(int stage) {
    (void)g_lantern_cache;
    switch (stage) {
    case 0: build_stage1(); break;
    case 1: build_stage2(); break;
    default: build_stage3(); break;
    }
}

void bg_free(void) {
    for (int i = 0; i < N_STAGES; i++) {
        StageBg *b = &g_bg[i];
        ks_free(b->base);
        ks_free(b->dark);
        for (int k = 0; k < b->nowned; k++) ks_free(b->owned[k]);
        memset(b, 0, sizeof *b);
    }
}

void bg_draw(KCanvas *cv, int ox, int oy, int stage, bool spell, int tint, int scroll, int t) {
    (void)tint;
    StageBg *b = &g_bg[stage < 0 || stage >= N_STAGES ? 0 : stage];
    const KSprite *base = spell ? b->dark : b->base;
    if (!base) { ks_fill_rect(cv, kr(PFX, PFY, PFW, PFH), 0x101030); return; }
    int y0 = ((BG_H - (scroll >> 1) % BG_H) % BG_H);
    int sx = BG_MARGIN - ox;                                         /* the tile has 8 spare pixels on either side for the screen shake */
    for (int r = 0; r < PFH; r++) {
        int sy = (y0 + r - oy + BG_H * 2) % BG_H;
        ks_copy32(cv->px + (size_t)(PFY + r) * cv->stride + PFX, base->px + (size_t)sy * base->w + sx, PFW);
    }
    KRect clip = cv->clip;
    kc_set_clip(cv, kr_intersect(clip, kr(PFX, PFY, PFW, PFH)));
    for (int i = 0; i < b->n; i++) {
        const LayerItem *it = &b->items[i];
        if (spell && !it->spell_ok) continue;
        bool strip = it->spr->h >= BG_H;                              /* a full-height strip wraps seamlessly */
        int span = strip ? BG_H : BG_H + 100;
        int y = (it->y0 + (t * it->spd16) / 16) % span;
        if (!strip) y -= 60;
        int sprh = it->spr->h;
        for (int pass = 0; pass < 2; pass++) {                       /* tall strips wrap around */
            int yy = pass ? y - span : y;
            if (yy + sprh < 0 || yy > PFH) continue;
            if (it->additive) ks_blit_add(cv, PFX + it->x + ox, PFY + yy + oy, it->spr);
            else if (it->soft) ks_blit(cv, PFX + it->x + ox, PFY + yy + oy, it->spr);
            else ks_blit_key(cv, PFX + it->x + ox, PFY + yy + oy, it->spr);
        }
    }
    cv->clip = clip;
}
