/* Title screen art, the lacquered frame around the field and the portraits. */
#include "gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

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
        for (int yy = y < 0 ? 0 : y; yy < s->h; yy++) s->px[yy * s->w + x] = ks_lerp_px(top, bottom, (yy - y) * 256 / (s->h - y + 1));
    }
}

static void bamboo(KSprite *s, int x, int base, int height, int width, uint32_t body, uint32_t joint, uint32_t seed) {
    ks_rect(s, (float)x, (float)(base - height), (float)width, (float)height, body, 255);
    ks_rect(s, (float)x, (float)(base - height), (float)width / 3.f + 1.f, (float)height, ks_lerp_px(body, 0x6FD39A, 70), 255);
    int seg = 36 + (int)(hash32(seed) % 18);
    for (int y = base - seg; y > base - height; y -= seg) {
        ks_rect(s, (float)x - 1.f, (float)y, (float)width + 2.f, 3.f, joint, 255);
        if (hash32((uint32_t)y * 31 + seed) & 1) {
            int dir = (hash32((uint32_t)y + seed) & 2) ? 1 : -1;
            for (int k = 0; k < 3; k++)
                ks_line(s, (float)x + (float)width / 2.f, (float)y, (float)x + (float)width / 2.f + (float)dir * (6.f + (float)k * 7.f), (float)y - 4.f + (float)k * 3.f, 1.4f, joint, 255);
        }
    }
}

static void lantern(KSprite *s, float x, float y, float k, uint32_t col) {
    ks_glow(s, x, y, 16.f * k, col, 120);
    ks_ellipse(s, x, y, 5.f * k, 6.f * k, 0.f, 0x3A1008, 255);
    ks_ellipse(s, x, y, 4.2f * k, 5.2f * k, 0.f, col, 255);
    ks_ellipse(s, x - 1.f * k, y - 1.5f * k, 2.f * k, 2.4f * k, 0.f, 0xFFF0C0, 200);
    ks_rect(s, x - 2.5f * k, y - 6.6f * k, 5.f * k, 1.6f * k, 0x3A1008, 255);
    ks_rect(s, x - 2.5f * k, y + 5.f * k, 5.f * k, 1.6f * k, 0x3A1008, 255);
}

static KSprite *make_title_bg(void) {
    KSprite *s = ks_new(WIN_W, WIN_H);
    for (int y = 0; y < WIN_H; y++) {
        uint32_t c = y < 300 ? ks_lerp_px(0x050820, 0x1A2462, y * 256 / 300) : ks_lerp_px(0x1A2462, 0x5A3E84, (y - 300) * 256 / 180);
        ks_fill32(s->px + (size_t)y * WIN_W, c, WIN_W);
    }
    for (int i = 0; i < 260; i++) {
        uint32_t r = hash32((uint32_t)i * 977 + 13);
        int x = (int)(r % WIN_W), y = (int)((r >> 12) % 300);
        int b = 120 + (int)((r >> 4) & 135);
        ks_put(s, x, y, KRGB(b, b, b + 20 > 255 ? 255 : b + 20), 255);
        if ((r & 31) == 0) { ks_put(s, x + 1, y, 0xB0B8E0, 255); ks_put(s, x - 1, y, 0xB0B8E0, 255); ks_put(s, x, y + 1, 0xB0B8E0, 255); ks_put(s, x, y - 1, 0xB0B8E0, 255); }
    }
    ks_glow(s, 470.f, 150.f, 180.f, 0xAEB8F0, 80);
    ks_disc(s, 470.f, 150.f, 72.f, 0xF3EEDA, 255);
    ks_disc(s, 448.f, 128.f, 14.f, 0xE4DDC0, 255);
    ks_disc(s, 492.f, 168.f, 9.f, 0xE4DDC0, 255);
    ks_disc(s, 462.f, 186.f, 6.f, 0xE9E3C8, 255);
    ks_disc(s, 500.f, 122.f, 5.f, 0xE9E3C8, 255);
    ridge(s, 330, 170, 190, 5, 0x1C2656, 0x10173A);
    ridge(s, 370, 120, 120, 99, 0x151C48, 0x0A0E26);
    /* the lake with the moon's reflection and the lanterns floating on it */
    for (int y = 372; y < WIN_H; y++) ks_fill32(s->px + (size_t)y * WIN_W, ks_lerp_px(0x1A2860, 0x060A1E, (y - 372) * 256 / 108), WIN_W);
    for (int i = 0; i < 46; i++) {
        int y = 376 + i * 2;
        int span = 30 + i * 2 + (int)(hash32((uint32_t)i) % 18);
        ks_rect(s, 470.f - (float)span / 2.f + (float)(hash32((uint32_t)i + 3) % 14) - 7.f, (float)y, (float)span, 1.f, 0xCFD6FF, 60);
    }
    for (int i = 0; i < 22; i++) {
        uint32_t r = hash32((uint32_t)i * 7919 + 4);
        float x = 80.f + (float)(r % 480), y = 392.f + (float)((r >> 10) % 70);
        lantern(s, x, y, 0.75f + (float)((r >> 20) % 5) * 0.1f, (r & 1) ? 0xF28A30 : 0xF2603C);
        ks_rect(s, x - 3.f, y + 9.f, 6.f, 1.5f, 0xF28A30, 70);
    }
    for (int i = 0; i < 5; i++) {
        bamboo(s, 6 + i * 22 + (int)(hash32((uint32_t)i) % 8), WIN_H - 8, 280 + (int)(hash32((uint32_t)i + 9) % 90), 8, 0x1B4A33, 0x0E2A1D, (uint32_t)i + 40);
        bamboo(s, WIN_W - 26 - i * 22 - (int)(hash32((uint32_t)i + 77) % 8), WIN_H - 8, 270 + (int)(hash32((uint32_t)i + 99) % 100), 8, 0x1B4A33, 0x0E2A1D, (uint32_t)i + 140);
    }
    for (int i = 0; i < 3; i++) {
        bamboo(s, 2 + i * 40, WIN_H, 420 - (int)(hash32((uint32_t)i + 5) % 80), 12, 0x2C6B47, 0x16402B, (uint32_t)i + 1);
        bamboo(s, WIN_W - 16 - i * 40, WIN_H, 410 - (int)(hash32((uint32_t)i + 55) % 90), 12, 0x2C6B47, 0x16402B, (uint32_t)i + 200);
    }
    for (int i = 0; i < 18; i++) {
        uint32_t r = hash32((uint32_t)i * 4231 + 9);
        float x = (float)(r % WIN_W), y = 120.f + (float)((r >> 10) % 250);
        ks_ellipse(s, x, y, 3.f, 2.f, 0.4f, 0xF6B4C8, 220);
    }
    return s;
}

static KSprite *make_logo(void) {
    KSprite *t = ks_text("DANMAKU", 6, 0xFFFFFF, 0x2A0A22);
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++)
            if (t->px[y * t->w + x] == 0xFFFFFF) {
                int k = y * 256 / t->h;
                t->px[y * t->w + x] = k < 128 ? ks_lerp_px(0xFFF4C0, 0xFFC0A0, k * 2) : ks_lerp_px(0xFFC0A0, 0xE8445C, (k - 128) * 2);
            }
    KSprite *logo = ks_new(t->w + 24, t->h + 24);
    ks_glow(logo, logo->w / 2.f, logo->h / 2.f, (float)logo->w / 2.f, 0xFF6080, 60);
    KSprite *shadow = ks_clone(t);
    for (int i = 0; i < shadow->w * shadow->h; i++) if (shadow->px[i] != KS_KEY) shadow->px[i] = 0x0C0418;
    KCanvas cv;
    kc_init(&cv, logo->px, logo->w, logo->h, logo->w);
    ks_blit_key(&cv, 16, 17, shadow);
    ks_blit_key(&cv, 10, 10, t);
    ks_free(shadow);
    ks_free(t);
    logo->ax = logo->w / 2;
    logo->ay = 0;
    return logo;
}

static KSprite *make_frame(void) {
    KSprite *s = ks_new(WIN_W, WIN_H);
    for (int y = 0; y < WIN_H; y++) ks_fill32(s->px + (size_t)y * WIN_W, ks_lerp_px(0x7A1022, 0x4A0814, y * 256 / WIN_H), WIN_W);
    /* waves of gold in the margins */
    KSprite *tile = ks_new(36, 18);
    ks_fill(tile, 0x000000);
    for (int i = 0; i < 3; i++) ks_ring(tile, 18.f, 18.f, 16.f - (float)i * 5.f, 1.2f, 0xE8B850, 255);
    for (int i = 0; i < 3; i++) ks_ring(tile, 0.f, 9.f, 14.f - (float)i * 5.f, 1.2f, 0xE8B850, 255);
    for (int i = 0; i < 3; i++) ks_ring(tile, 36.f, 9.f, 14.f - (float)i * 5.f, 1.2f, 0xE8B850, 255);
    for (int y = 0; y < WIN_H; y += 18)
        for (int x = (y / 18 & 1) ? -18 : 0; x < WIN_W; x += 36) {
            bool margin = x < PFX - 2 || y < PFY || x > PFX + PFW || y > PFY + PFH - 4;
            if (!margin) continue;
            for (int ty = 0; ty < tile->h; ty++)
                for (int tx = 0; tx < tile->w; tx++) {
                    uint32_t p = tile->px[ty * tile->w + tx];
                    int dx = x + tx, dy = y + ty;
                    if (!p || dx < 0 || dy < 0 || dx >= WIN_W || dy >= WIN_H) continue;
                    s->px[dy * WIN_W + dx] = ks_lerp_px(s->px[dy * WIN_W + dx], 0xE8B850, (int)((p >> 8 & 255) * 110 / 255));
                }
        }
    ks_free(tile);
    /* the HUD panel */
    KRect hp = kr(PFX + PFW + 14, PFY, WIN_W - (PFX + PFW + 14) - 16, PFH);
    for (int y = hp.y; y < hp.y + hp.h; y++) ks_fill32(s->px + (size_t)y * WIN_W + hp.x, ks_lerp_px(0x0E1434, 0x1C2860, (y - hp.y) * 256 / hp.h), (size_t)hp.w);
    KCanvas cv;
    kc_init(&cv, s->px, WIN_W, WIN_H, WIN_W);
    kc_outline(&cv, kr_grow(hp, 1), 0x2A0610);
    kc_outline(&cv, hp, 0xE8B850);
    kc_outline(&cv, kr_inset(hp, 3), 0x8A6A28);
    for (int y = hp.y + 5; y < hp.y + hp.h - 5; y += 4) ks_stipple_rect(&cv, kr(hp.x + 5, y, hp.w - 10, 1), 0x1E2A64, y / 4);
    /* the field's gold border */
    KRect f = kr(PFX - 4, PFY - 4, PFW + 8, PFH + 8);
    kc_outline(&cv, kr_grow(f, 1), 0x2A0610);
    kc_outline(&cv, f, 0xE8B850);
    kc_outline(&cv, kr_inset(f, 1), 0x8A6A28);
    kc_outline(&cv, kr_inset(f, 2), 0x1A0408);
    kc_outline(&cv, kr_inset(f, 3), 0x1A0408);
    /* punch the hole */
    for (int y = PFY; y < PFY + PFH; y++)
        for (int x = PFX; x < PFX + PFW; x++) s->px[y * WIN_W + x] = KS_KEY;
    return s;
}

void titleart_build(int part) {
    if (part == 0) {
        A.logo = make_logo();
        A.logo_sub = ks_text("Gensokyo Lantern Festival", 2, 0xFFF0C8, 0x2A0A22);
        A.title_bg = make_title_bg();
    } else if (part == 1) {
        A.frame = make_frame();
        KSprite *o = ks_new(56, 56);
        const float m = 28.f, R = 25.f;
        ks_glow(o, m, m, 28.f, 0xFF6080, 60);
        ks_disc(o, m, m, R + 1.5f, 0xE8B850, 255);
        ks_disc(o, m, m, R, 0xFFF6EA, 255);
        float half[44];
        int n = 0;
        for (int i = 0; i <= 20; i++) {
            float a = 1.5708f + (float)i * 3.14159265f / 20.f;
            half[n * 2] = m + R * cosf(a);
            half[n * 2 + 1] = m + R * sinf(a);
            n++;
        }
        ks_poly(o, half, n, 0xC8102E, 255);
        ks_disc(o, m, m - R / 2.f, R / 2.f, 0xC8102E, 255);
        ks_disc(o, m, m + R / 2.f, R / 2.f, 0xFFF6EA, 255);
        ks_disc(o, m, m - R / 2.f, R / 6.f, 0xFFF6EA, 255);
        ks_disc(o, m, m + R / 2.f, R / 6.f, 0xC8102E, 255);
        ks_flatten(o, 0x20102C);
        A.hud_orb = o;
        KSprite *l = ks_new(15, 15);
        ks_star(l, 7.5f, 7.9f, 7.2f, 3.2f, 5, 0.f, 0x5A0A18, 255);
        ks_star(l, 7.5f, 7.9f, 6.2f, 2.6f, 5, 0.f, 0xF04A64, 255);
        ks_disc(l, 7.5f, 7.8f, 1.6f, 0xFFD0D8, 255);
        ks_flatten(l, 0x101030);
        A.life_icon = l;
        KSprite *b = ks_new(15, 15);
        ks_star(b, 7.5f, 7.9f, 7.2f, 3.2f, 5, 0.f, 0x0A4E1C, 255);
        ks_star(b, 7.5f, 7.9f, 6.2f, 2.6f, 5, 0.f, 0x3CD060, 255);
        ks_disc(b, 7.5f, 7.8f, 1.6f, 0xD0FFD8, 255);
        ks_flatten(b, 0x101030);
        A.bomb_icon = b;
    } else {
        for (int t = 0; t < SHOT_COUNT; t++) A.portrait[t] = spr_scale_nn(A.player[t][0], 3);
    }
}
