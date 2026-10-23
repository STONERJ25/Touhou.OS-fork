#include "yinyang.h"

static uint32_t mix(uint32_t a, uint32_t b, int num, int den) {
    /* (a * num + b * (den - num)) / den per channel */
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        out |= ((ca * (uint32_t)num + cb * (uint32_t)(den - num)) / (uint32_t)den & 0xFF) << shift;
    }
    return out;
}

void yinyang_render(uint32_t *dst, int size, int pitch, uint32_t left, uint32_t right, uint32_t bg) {
    const int SS = 4;                       /* subsamples per axis */
    long R = (long)size * SS / 2;           /* radius in subsample units */
    long half = R / 2;
    long rim = R - SS;                      /* thin outline ring */
    uint32_t outline = 0x1A1A24;

    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int in_orb = 0, c_left = 0, c_right = 0, c_out = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    long x = (long)(px * SS + sx) - R + SS / 2;     /* centred, subsample units */
                    long y = (long)(py * SS + sy) - R + SS / 2;
                    long r2 = x * x + y * y;
                    if (r2 > R * R) continue;
                    in_orb++;
                    if (r2 > rim * rim) { c_out++; continue; }
                    long dt = x * x + (y + half) * (y + half);      /* distance to top lobe centre */
                    long db = x * x + (y - half) * (y - half);      /* ... bottom lobe */
                    long lobe = half * half;
                    long dot = (R / 8) * (R / 8);
                    int is_left;
                    if (dt <= lobe) is_left = (dt <= dot) ? 1 : 0;          /* top lobe is "right" coloured, left dot */
                    else if (db <= lobe) is_left = (db <= dot) ? 0 : 1;     /* bottom lobe is "left" coloured, right dot */
                    else is_left = x < 0;
                    if (is_left) c_left++; else c_right++;
                }
            }
            uint32_t pixel = bg;
            int total = SS * SS;
            if (in_orb) {
                /* weighted mix of the sample colours over the orb, remainder is background */
                uint32_t body = left;
                int body_n = c_left + c_right;
                if (body_n) body = mix(left, right, c_left, body_n);
                uint32_t col = body;
                int solid = body_n;
                if (c_out) {
                    col = mix(outline, body, c_out, c_out + body_n);
                    solid = body_n + c_out;
                }
                pixel = mix(col, bg, solid, total);
            }
            dst[py * pitch + px] = pixel;
        }
    }
}
