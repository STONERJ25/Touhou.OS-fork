/* The default wallpaper: a procedural Gensokyo night. Moon, stars, mountains, a torii on a hill and bamboo in the foreground. */
#include <kappa.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

/* smooth 1-D value noise in [0, 1023] */
static int noise1(int x, int period, uint32_t seed) {
    int i = x / period, f = x % period;
    int a = (int)(hash32((uint32_t)i * 2654435761u + seed) & 1023);
    int b = (int)(hash32((uint32_t)(i + 1) * 2654435761u + seed) & 1023);
    int t = f * 256 / period;
    t = t * t * (768 - 2 * t) / 65536;                       /* smoothstep, scaled to 0..256 */
    return a + (b - a) * t / 256;
}

static void ridge(KCanvas *cv, int w, int h, int base, int amp, int period, uint32_t seed, uint32_t top, uint32_t bottom) {
    for (int x = 0; x < w; x++) {
        int n = noise1(x + 1000, period, seed) + noise1(x + 5000, period / 2, seed + 7) / 2;
        int y = base - amp * n / 1536;
        kc_gradient_v(cv, kr(x, y, 1, h - y), top, bottom);
    }
}

static void bamboo(KCanvas *cv, int x, int base, int height, int width, uint32_t body, uint32_t joint, uint32_t light, uint32_t seed) {
    kc_fill(cv, kr(x, base - height, width, height), body);
    kc_fill(cv, kr(x, base - height, width / 3 + 1, height), light);
    int seg = 38 + (int)(hash32(seed) % 20);
    for (int y = base - seg; y > base - height; y -= seg + (int)(hash32((uint32_t)y + seed) % 8)) {
        kc_fill(cv, kr(x - 1, y, width + 2, 3), joint);
        /* leaf fronds hanging off the joints */
        if (hash32((uint32_t)y * 31 + seed) & 1) {
            int dir = (hash32((uint32_t)y + seed) & 2) ? 1 : -1;
            for (int k = 0; k < 4; k++) {
                int lx = x + width / 2 + dir * (6 + k * 7), ly = y - 4 + k * 3;
                kc_line(cv, x + width / 2, y, lx, ly, joint);
                kc_line(cv, x + width / 2, y + 1, lx + dir * 6, ly + 5, body);
            }
        }
    }
    kc_fill(cv, kr(x, base - height, width, 3), joint);
}

void k_scenery_night(uint32_t *px, int w, int h, int stride) { k_scenery_night_scaled(px, w, h, stride, 1); }

void k_scenery_night_scaled(uint32_t *px, int w, int h, int stride, int scale) {
    KCanvas cv;
    kc_init_scaled(&cv, px, w, h, stride, scale);
    int horizon = h * 62 / 100;

    /* sky */
    kc_gradient_v(&cv, kr(0, 0, w, horizon / 2), 0x060A20, 0x121C4C);
    kc_gradient_v(&cv, kr(0, horizon / 2, w, horizon - horizon / 2), 0x121C4C, 0x3A4A8A);

    /* stars */
    for (int i = 0; i < 260; i++) {
        uint32_t r = hash32((uint32_t)i * 977 + 13);
        int x = (int)(r % (uint32_t)w), y = (int)((r >> 12) % (uint32_t)(horizon - 40));
        int bright = 120 + (int)((r >> 4) & 127);
        uint32_t c = KRGB(bright, bright, bright + 20 > 255 ? 255 : bright + 20);
        kc_pixel(&cv, x, y, c);
        if ((r & 31) == 0) { kc_pixel(&cv, x + 1, y, c); kc_pixel(&cv, x - 1, y, c); kc_pixel(&cv, x, y + 1, c); kc_pixel(&cv, x, y - 1, c); }
    }

    /* the moon, with a soft halo */
    int mx = w * 76 / 100, my = h * 22 / 100, mr = h * 11 / 100;
    int halo = mr * 3;
    for (int dy = -halo; dy <= halo; dy++)
        for (int dx = -halo; dx <= halo; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 >= halo * halo || d2 <= mr * mr) continue;
            int a = (int)((halo - sqrt((double)d2)) * 30.0 / (double)(halo - mr));
            if (a > 0) kc_blend_pixel(&cv, mx + dx, my + dy, 0xAEB8F0, a);
        }
    kc_circle_fill(&cv, mx, my, mr, 0xF3EEDA);
    kc_circle_fill(&cv, mx - mr / 3, my - mr / 4, mr / 5, 0xE4DDC0);
    kc_circle_fill(&cv, mx + mr / 3, my + mr / 5, mr / 7, 0xE4DDC0);
    kc_circle_fill(&cv, mx - mr / 6, my + mr / 2, mr / 9, 0xE9E3C8);

    /* mountains */
    ridge(&cv, w, h, horizon - 10, h * 22 / 100, 170, 5, 0x1A2352, 0x0F1538);
    ridge(&cv, w, h, horizon + 24, h * 16 / 100, 110, 99, 0x131A40, 0x0A0E26);

    /* the hill and its torii */
    int hx = w * 27 / 100, hy = horizon + 40;
    for (int dx = -w / 5; dx <= w / 5; dx++) {
        int y = hy + (dx * dx) / (w / 6);
        kc_gradient_v(&cv, kr(hx + dx, y, 1, h - y), 0x0B1030, 0x070A1C);
    }
    int tw = h * 9 / 100, tx = hx - tw / 2, ty = hy - tw;
    uint32_t red = 0xB5122B, dark = 0x2A1520;
    kc_fill(&cv, kr(tx + tw / 6, ty + tw / 4, tw / 12 + 1, tw * 3 / 4), red);
    kc_fill(&cv, kr(tx + tw - tw / 6 - tw / 12, ty + tw / 4, tw / 12 + 1, tw * 3 / 4), red);
    kc_fill(&cv, kr(tx + tw / 8, ty + tw / 3, tw * 3 / 4, tw / 14 + 1), red);
    kc_fill(&cv, kr(tx - 2, ty + tw / 6 - 2, tw + 4, tw / 12 + 2), dark);
    kc_fill(&cv, kr(tx - 5, ty + tw / 6 - 5, 6, 5), dark);
    kc_fill(&cv, kr(tx + tw - 1, ty + tw / 6 - 5, 6, 5), dark);
    /* a faint reflection of the moon on the lake */
    for (int i = 0; i < 40; i++) {
        int y = horizon + 60 + i * 6;
        int span = 20 + i * 2 + (int)(hash32((uint32_t)i) % 18);
        kc_fill_alpha(&cv, kr(mx - span / 2 + (int)(hash32((uint32_t)i + 3) % 12) - 6, y, span, 2), 0xCFD6FF, 20);
    }

    /* foreground ground and bamboo on both sides */
    kc_gradient_v(&cv, kr(0, h - h * 12 / 100, w, h * 12 / 100), 0x0A0F26, 0x04060F);
    uint32_t bodyA = 0x2C6B47, jointA = 0x16402B, lightA = 0x4A9A6A;
    uint32_t bodyB = 0x1B4A33, jointB = 0x0E2A1D, lightB = 0x2F6E4C;
    for (int i = 0; i < 6; i++) {                                              /* distant layer */
        bamboo(&cv, 8 + i * 22 + (int)(hash32((uint32_t)i) % 8), h - 30, h * 55 / 100 - (int)(hash32((uint32_t)i + 9) % 60), 7, bodyB, jointB, lightB, (uint32_t)i + 40);
        bamboo(&cv, w - 20 - i * 24 - (int)(hash32((uint32_t)i + 77) % 8), h - 30, h * 52 / 100 - (int)(hash32((uint32_t)i + 99) % 70), 7, bodyB, jointB, lightB, (uint32_t)i + 140);
    }
    for (int i = 0; i < 4; i++) {                                              /* near layer */
        bamboo(&cv, 14 + i * 40, h - 6, h * 78 / 100 - (int)(hash32((uint32_t)i + 5) % 90), 11, bodyA, jointA, lightA, (uint32_t)i + 1);
        bamboo(&cv, w - 32 - i * 42, h - 6, h * 72 / 100 - (int)(hash32((uint32_t)i + 55) % 100), 11, bodyA, jointA, lightA, (uint32_t)i + 200);
    }

    /* a few petals drifting in the moonlight */
    for (int i = 0; i < 26; i++) {
        uint32_t r = hash32((uint32_t)i * 7919 + 4);
        int x = w / 4 + (int)(r % (uint32_t)(w / 2)), y = h / 3 + (int)((r >> 10) % (uint32_t)(h / 2));
        kc_ellipse_fill(&cv, x, y, 3, 2, 0xF6B4C8);
        kc_pixel(&cv, x - 1, y, 0xFDE3EC);
    }
}
