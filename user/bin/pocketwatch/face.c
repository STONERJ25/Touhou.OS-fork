/* The analog face: a pocket watch with a gold case, roman numerals, tapered hands and (when time is stopped) a ring of knives. */
#include "watch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Fills a convex polygon given as x0,y0,x1,y1,... ; alpha < 255 blends it over what is there (used for soft shadows). */
void fill_poly(KCanvas *cv, const int *xy, int n, uint32_t color, int alpha) {
    if (n < 3) return;
    int ymin = xy[1], ymax = xy[1];
    for (int i = 1; i < n; i++) {
        if (xy[2 * i + 1] < ymin) ymin = xy[2 * i + 1];
        if (xy[2 * i + 1] > ymax) ymax = xy[2 * i + 1];
    }
    for (int y = ymin; y <= ymax; y++) {
        double yc = y + 0.5, xmin = 1e9, xmax = -1e9;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            double x0 = xy[2 * i], y0 = xy[2 * i + 1], x1 = xy[2 * j], y1 = xy[2 * j + 1];
            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
                double x = x0 + (yc - y0) * (x1 - x0) / (y1 - y0);
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
            }
        }
        if (xmax < xmin) continue;
        int xa = (int)floor(xmin + 0.5), xb = (int)floor(xmax + 0.5);
        if (xb < xa) xb = xa;
        if (alpha >= 255) kc_hline(cv, xa, y, xb - xa + 1, color);
        else for (int x = xa; x <= xb; x++) kc_blend_pixel(cv, x, y, color, alpha);
    }
}

static const double DEG = 3.14159265358979323846 / 180.0;

/* a straight bar from (x0,y0) to (x1,y1), w pixels wide */
static void thick_line(KCanvas *cv, double x0, double y0, double x1, double y1, double w, uint32_t color) {
    double dx = x1 - x0, dy = y1 - y0, len = sqrt(dx * dx + dy * dy);
    if (len < 0.5) return;
    double px = -dy / len * w / 2, py = dx / len * w / 2;
    int p[8] = { (int)floor(x0 + px + 0.5), (int)floor(y0 + py + 0.5), (int)floor(x1 + px + 0.5), (int)floor(y1 + py + 0.5),
                 (int)floor(x1 - px + 0.5), (int)floor(y1 - py + 0.5), (int)floor(x0 - px + 0.5), (int)floor(y0 - py + 0.5) };
    fill_poly(cv, p, 4, color, 255);
}

/* A hand pointing at `deg` degrees clockwise from twelve o'clock: `front` long, `back` of counterweight, tapering from wb to wt. */
static void hand(KCanvas *cv, int cx, int cy, double deg, double front, double back, double wb, double wt, uint32_t color, int alpha, int ox, int oy) {
    double a = deg * DEG, dx = sin(a), dy = -cos(a), px = cos(a), py = sin(a);
    double bx = cx + ox - dx * back, by = cy + oy - dy * back, fx = cx + ox + dx * front, fy = cy + oy + dy * front;
    int p[8] = { (int)floor(bx + px * wb + 0.5), (int)floor(by + py * wb + 0.5), (int)floor(bx - px * wb + 0.5), (int)floor(by - py * wb + 0.5),
                 (int)floor(fx - px * wt + 0.5), (int)floor(fy - py * wt + 0.5), (int)floor(fx + px * wt + 0.5), (int)floor(fy + py * wt + 0.5) };
    fill_poly(cv, p, 4, color, alpha);
}

static void ring(KCanvas *cv, int cx, int cy, int r, int thickness, uint32_t color) {
    for (int i = 0; i < thickness; i++) kc_circle(cv, cx, cy, r - i, color);
}

/* a thin silver knife hanging in the air, point towards the centre */
static void knife(KCanvas *cv, int cx, int cy, double deg, double r_out, double len, uint32_t blade, uint32_t edge) {
    double a = deg * DEG, dx = sin(a), dy = -cos(a), px = cos(a), py = sin(a);
    double tipx = cx + dx * (r_out - len), tipy = cy + dy * (r_out - len);
    double hx = cx + dx * (r_out - len * 0.25), hy = cy + dy * (r_out - len * 0.25);
    double ex = cx + dx * r_out, ey = cy + dy * r_out;
    int p[8] = { (int)floor(tipx + 0.5), (int)floor(tipy + 0.5), (int)floor(hx + px * 2.5 + 0.5), (int)floor(hy + py * 2.5 + 0.5),
                 (int)floor(hx + dx * 2 + 0.5), (int)floor(hy + dy * 2 + 0.5), (int)floor(hx - px * 2.5 + 0.5), (int)floor(hy - py * 2.5 + 0.5) };
    fill_poly(cv, p, 4, blade, 255);
    kc_line(cv, (int)floor(tipx + 0.5), (int)floor(tipy + 0.5), (int)floor(hx + 0.5), (int)floor(hy + 0.5), edge);
    thick_line(cv, hx + dx * 2, hy + dy * 2, ex, ey, 2.5, edge);                  /* the handle */
}

void face_draw(KCanvas *cv, int cx, int cy, int R, const struct tm *t, bool frozen) {
    if (R < 20) return;
    uint32_t case_dark = frozen ? 0x3D4968 : 0x6B4F12, case_light = frozen ? 0xC4D0EC : 0xEDC662, case_mid = frozen ? 0x8796BE : 0xB98A25;
    uint32_t dial = frozen ? 0xE4EDF9 : 0xFBF7E6, ink = frozen ? 0x1F2A48 : 0x1A1A24, tick_minor = frozen ? 0x5A6A94 : 0x4A4638;
    uint32_t red = frozen ? 0x8FB0F0 : 0xC8102E;

    /* bow and crown */
    int bow_r = R / 9 + 3;
    int bow_y = cy - R - bow_r + 2;
    ring(cv, cx, bow_y, bow_r, 3, case_dark);
    ring(cv, cx, bow_y, bow_r - 1, 1, case_light);
    kc_fill(cv, kr(cx - R / 10 - 2, cy - R - 3, R / 5 + 4, 7), case_mid);
    kc_outline(cv, kr(cx - R / 10 - 2, cy - R - 3, R / 5 + 4, 7), case_dark);

    /* the case */
    kc_circle_fill(cv, cx, cy, R, case_dark);
    kc_circle_fill(cv, cx, cy, R - 2, case_light);
    kc_circle_fill(cv, cx, cy, R - 5, case_mid);
    kc_circle_fill(cv, cx, cy, R - 7, case_dark);
    kc_circle_fill(cv, cx, cy, R - 8, dial);
    kc_circle(cv, cx, cy, R - 10, kc_shade(dial, 90));
    int Rd = R - 10;                                                    /* the dial's usable radius */

    /* ticks */
    for (int i = 0; i < 60; i++) {
        double a = i * 6 * DEG, dx = sin(a), dy = -cos(a);
        bool major = i % 5 == 0;
        double r1 = Rd - 1, r2 = Rd - (major ? 10.0 : 5.0);
        if (R < 60 && !major) continue;
        thick_line(cv, cx + dx * r1, cy + dy * r1, cx + dx * r2, cy + dy * r2, major ? 2.6 : 1.0, major ? ink : tick_minor);
    }

    /* roman numerals */
    static const char *const roman[12] = { "XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI" };
    if (R >= 60) {
        int scale = R >= 150 ? 2 : 1;
        double rn = Rd - 12 - (scale == 2 ? 24 : 16) - (R >= 96 ? 6 : 0);
        for (int h = 0; h < 12; h++) {
            if (R < 96 && h % 3) continue;
            double a = h * 30 * DEG;
            int w = (int)strlen(roman[h]) * 8 * scale, hh = 16 * scale;
            int x = cx + (int)floor(sin(a) * rn + 0.5) - w / 2, y = cy - (int)floor(cos(a) * rn + 0.5) - hh / 2;
            kc_text_scaled(cv, x + 1, y, roman[h], ink, scale);
            kc_text_scaled(cv, x, y, roman[h], ink, scale);
        }
    }

    /* the maker's mark and the time standard */
    if (R >= 90) {
        int scale = R >= 150 ? 2 : 1;
        const char *brand = "SAKUYA";
        int bw = (int)strlen(brand) * 8 * scale;
        kc_text_scaled(cv, cx - bw / 2, cy - (int)(R * 0.42) - 8 * scale, brand, case_mid, scale);
        kc_text_scaled(cv, cx - bw / 2 + 1, cy - (int)(R * 0.42) - 8 * scale, brand, case_mid, scale);
        const char *z = t->tm_zone && t->tm_zone[0] ? t->tm_zone : "UTC";
        int zw = (int)strlen(z) * 8 * scale;
        kc_text_scaled(cv, cx - zw / 2, cy + (int)(R * 0.36), z, tick_minor, scale);
        kc_text_scaled(cv, cx - zw / 2 + 1, cy + (int)(R * 0.36), z, tick_minor, scale);
    }

    /* hands */
    double ha = ((t->tm_hour % 12) + t->tm_min / 60.0 + t->tm_sec / 3600.0) * 30.0;
    double ma = (t->tm_min + t->tm_sec / 60.0) * 6.0;
    double sa = t->tm_sec * 6.0;
    double hl = Rd * 0.52, ml = Rd * 0.80, sl = Rd * 0.88;
    double hw = R >= 60 ? 4.5 : 3.0, mw = R >= 60 ? 3.2 : 2.2;
    for (int pass = 0; pass < 2; pass++) {
        bool shadow = pass == 0;
        int ox = shadow ? 3 : 0, oy = shadow ? 3 : 0;
        int alpha = shadow ? 55 : 255;
        hand(cv, cx, cy, ha, hl, Rd * 0.10, hw, 1.2, shadow ? 0x000000 : ink, alpha, ox, oy);
        hand(cv, cx, cy, ma, ml, Rd * 0.12, mw, 0.8, shadow ? 0x000000 : ink, alpha, ox, oy);
        hand(cv, cx, cy, sa, sl, Rd * 0.20, 1.1, 0.5, shadow ? 0x000000 : red, alpha, ox, oy);
    }
    kc_circle_fill(cv, cx, cy, R >= 60 ? 6 : 4, case_mid);
    kc_circle_fill(cv, cx, cy, R >= 60 ? 4 : 2, red);
    kc_circle_fill(cv, cx, cy, 1, case_dark);

    if (frozen) {                                                       /* knives hold the moment in place */
        for (int i = 0; i < 12; i++) knife(cv, cx, cy, i * 30.0 + 15.0, R + 20, 17, 0xDDE6F6, 0x5A6A94);
        static const int spark[5][2] = { { -1, -1 }, { 1, -1 }, { 0, 1 }, { -1, 1 }, { 1, 0 } };
        for (int i = 0; i < 5; i++) {
            int sx = cx + spark[i][0] * (R * 6 / 10), sy = cy + spark[i][1] * (R * 5 / 10) - (i == 4 ? 10 : 0);
            kc_hline(cv, sx - 3, sy, 7, 0xFFFFFF);
            kc_vline(cv, sx, sy - 3, 7, 0xFFFFFF);
            kc_blend_pixel(cv, sx - 1, sy - 1, 0x9DB6F0, 140);
            kc_blend_pixel(cv, sx + 1, sy + 1, 0x9DB6F0, 140);
        }
    }
}
