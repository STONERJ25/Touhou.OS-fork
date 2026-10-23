/* Pictures of the spell card of the day: the bullet pattern drawn as a diagram, with the caster above and the reader below. */
#include "news.h"
#include <math.h>
#include <string.h>

static const double PI_ = 3.14159265358979323846;

static const uint32_t k_colors[6] = { 0xFF7AA8, 0x6AE0FF, 0xFFE06A, 0x8AF29A, 0xC48AFF, 0xFFA05A };

static void bullet(KCanvas *cv, double x, double y, uint32_t color, int r) {
    int ix = (int)floor(x + 0.5), iy = (int)floor(y + 0.5);
    kc_circle_fill(cv, ix, iy, r, color);
    kc_circle_fill(cv, ix, iy, r > 2 ? r - 2 : 0, 0xFFFFFF);
}

void spellcard_picture(KUI *ui, KRect r, const SpellCard *c, long daynum) {
    KCanvas *cv = &ui->cv;
    int s = r.w < r.h ? r.w : r.h;
    KRect box = kr(r.x + (r.w - s) / 2, r.y, s, s);
    kc_gradient_v(cv, box, 0x070A1C, 0x1D2A5C);
    kc_outline(cv, box, PAGE_RULE);
    kc_outline(cv, kr_inset(box, 2), 0x3A4690);
    KRect old = cv->clip;
    kc_set_clip(cv, kr_intersect(old, kr_inset(box, 3)));

    double cx = box.x + s / 2.0, cy = box.y + s * 0.36, R = s * 0.40;
    uint32_t c1 = k_colors[(unsigned)((daynum + c->pattern) % 6)], c2 = k_colors[(unsigned)((daynum + c->pattern + 2) % 6)];
    int a = c->a, b = c->b, n = c->c;
    if (a < 1) a = 1;
    switch (c->pattern) {
    case 0:                                                           /* spiral arms */
        for (int arm = 0; arm < a; arm++)
            for (int j = 0; j < n * 2; j++) {
                double t = (double)j / (n * 2.0);
                double ang = arm * 2 * PI_ / a + t * b * PI_ * 0.6;
                bullet(cv, cx + cos(ang) * R * t, cy + sin(ang) * R * t, arm & 1 ? c2 : c1, 3);
            }
        break;
    case 1:                                                           /* rings */
        for (int ring = 0; ring < a; ring++) {
            double rad = R * (ring + 1) / a;
            int count = b < 4 ? 4 : b;
            for (int i = 0; i < count; i++) {
                double ang = 2 * PI_ * i / count + ring * PI_ / count;
                bullet(cv, cx + cos(ang) * rad, cy + sin(ang) * rad, ring & 1 ? c2 : c1, 3);
            }
        }
        break;
    case 2: {                                                         /* a rose of petals */
        int count = b < 12 ? 12 : b;
        for (int i = 0; i < count * 2; i++) {
            double th = 4 * PI_ * i / (count * 2);
            double rad = R * cos(a * th / 2.0);
            bullet(cv, cx + cos(th) * rad, cy + sin(th) * rad, i & 1 ? c2 : c1, 2);
        }
        break;
    }
    case 3:                                                           /* fans of bullets aimed down at the reader */
        for (int row = 0; row < (n < 1 ? 1 : n); row++)
            for (int i = 0; i < a; i++) {
                double ang = PI_ / 2 + ((a > 1 ? (double)i / (a - 1) : 0.5) - 0.5) * b * PI_ / 180.0;
                double dist = R * 0.35 + (row + 1) * R * 0.75 / ((n < 1 ? 1 : n) + 0.5);
                bullet(cv, cx + cos(ang) * dist, cy + sin(ang) * dist, row & 1 ? c2 : c1, 3);
            }
        break;
    case 4:                                                           /* rain in columns */
        for (int i = 0; i < a; i++) {
            double x = box.x + 12 + (a > 1 ? (double)i * (s - 24) / (a - 1) : s / 2.0);
            for (int j = 0; j < 6; j++) {
                double y = box.y + 26 + (double)(((j * 47 + i * 29 + (int)daynum * 7) % 100)) * (s - 70) / 100.0;
                bullet(cv, x, y, (i + j) & 1 ? c2 : c1, 2);
            }
        }
        break;
    case 5: {                                                         /* a lattice held by strings */
        int cols = a < 2 ? 2 : a, rows = b < 2 ? 2 : b;
        for (int j = 0; j < rows; j++)
            for (int i = 0; i < cols; i++) {
                double x = box.x + 14 + (double)i * (s - 28) / (cols - 1);
                double y = box.y + 30 + (double)j * (s - 80) / (rows - 1);
                if (i + 1 < cols) kc_line(cv, (int)x, (int)y, (int)(box.x + 14 + (double)(i + 1) * (s - 28) / (cols - 1)), (int)y, 0x3A4690);
                if (j + 1 < rows) kc_line(cv, (int)x, (int)y, (int)x, (int)(box.y + 30 + (double)(j + 1) * (s - 80) / (rows - 1)), 0x3A4690);
                bullet(cv, x, y, (i + j) & 1 ? c2 : c1, 3);
            }
        break;
    }
    case 6:                                                           /* drifting petals */
        for (int arm = 0; arm < a; arm++)
            for (int j = 0; j < b * 3; j++) {
                double t = (double)j / (b * 3.0);
                double ang = arm * 2 * PI_ / a + t * 3 * PI_ + sin(t * 6) * 0.35;
                bullet(cv, cx + cos(ang) * R * t, cy + sin(ang) * R * t, arm & 1 ? c2 : c1, 2);
            }
        break;
    default: {                                                        /* zigzag lanes */
        for (int lane = 0; lane < a; lane++) {
            double x0 = box.x + 14 + (a > 1 ? (double)lane * (s - 28) / (a - 1) : s / 2.0);
            for (int j = 0; j < 9; j++) {
                double y = box.y + 30 + j * (s - 70) / 9.0;
                bullet(cv, x0 + sin(j * 0.9 + lane) * 9, y, (lane + j) & 1 ? c2 : c1, 2);
            }
        }
        break;
    }
    }

    /* the caster above, the reader below */
    kc_circle_fill(cv, (int)cx, box.y + 14, 6, 0xD9A441);
    kc_circle_fill(cv, (int)cx, box.y + 14, 3, 0xFFF6E0);
    kc_circle_fill(cv, (int)cx, box.y + s - 18, 4, 0xC8102E);
    kc_circle_fill(cv, (int)cx, box.y + s - 18, 2, 0xFFFFFF);
    cv->clip = old;
}
