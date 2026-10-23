/* Procedural tool icons, 20x20 pixels, drawn from canvas primitives. */
#include "sketchbook.h"

typedef struct { int x, y; } Pt;

static void poly(KCanvas *c, const Pt *p, int n, uint32_t rgb) {
    int ymin = p[0].y, ymax = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
    for (int y = ymin; y <= ymax; y++) {
        int xs[16], nx = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if ((p[i].y <= y && p[j].y > y) || (p[j].y <= y && p[i].y > y)) {
                if (nx < 16) xs[nx++] = p[i].x + (y - p[i].y) * (p[j].x - p[i].x) / (p[j].y - p[i].y);
            }
        }
        for (int a = 1; a < nx; a++)
            for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int k = 0; k + 1 < nx; k += 2) kc_hline(c, xs[k], y, xs[k + 1] - xs[k] + 1, rgb);
    }
}

static void thick(KCanvas *c, int x0, int y0, int x1, int y1, int t, uint32_t col) {
    for (int dy = 0; dy < t; dy++)
        for (int dx = 0; dx < t; dx++) kc_line(c, x0 + dx - t / 2, y0 + dy - t / 2, x1 + dx - t / 2, y1 + dy - t / 2, col);
}

void icon_tool(KCanvas *c, Tool t, int X, int Y) {
    const uint32_t ink = 0x23232F;
    switch (t) {
    case T_PENCIL: {
        Pt body[4] = { { X + 5, Y + 14 }, { X + 13, Y + 6 }, { X + 15, Y + 8 }, { X + 7, Y + 16 } };
        poly(c, body, 4, 0xF0C030);
        kc_line(c, X + 5, Y + 14, X + 13, Y + 6, 0x8A5A10);
        kc_line(c, X + 7, Y + 16, X + 15, Y + 8, 0x8A5A10);
        Pt tip[3] = { { X + 3, Y + 18 }, { X + 5, Y + 14 }, { X + 7, Y + 16 } };
        poly(c, tip, 3, 0xF2DCB0);
        kc_pixel(c, X + 3, Y + 18, ink);
        kc_pixel(c, X + 4, Y + 17, ink);
        Pt er[4] = { { X + 13, Y + 6 }, { X + 15, Y + 8 }, { X + 17, Y + 6 }, { X + 15, Y + 4 } };
        poly(c, er, 4, 0xE0707F);
        kc_line(c, X + 13, Y + 6, X + 15, Y + 4, 0x8A2A3A);
        kc_line(c, X + 15, Y + 8, X + 17, Y + 6, 0x8A2A3A);
        break;
    }
    case T_BRUSH:
        thick(c, X + 17, Y + 2, X + 10, Y + 9, 3, 0x8B5A2B);
        thick(c, X + 10, Y + 9, X + 8, Y + 11, 4, 0xB8BEC6);
        kc_circle_fill(c, X + 5, Y + 14, 3, 0x3A7BD5);
        kc_circle(c, X + 5, Y + 14, 3, 0x1E3F7A);
        kc_pixel(c, X + 2, Y + 17, 0x3A7BD5);
        break;
    case T_ERASER: {
        Pt blk[4] = { { X + 2, Y + 13 }, { X + 8, Y + 7 }, { X + 17, Y + 13 }, { X + 11, Y + 19 } };
        poly(c, blk, 4, 0xF08CA0);
        Pt sleeve[4] = { { X + 8, Y + 7 }, { X + 10, Y + 9 }, { X + 17, Y + 13 }, { X + 15, Y + 11 } };
        poly(c, sleeve, 4, 0xC9CFD8);
        for (int i = 0; i < 4; i++) kc_line(c, blk[i].x, blk[i].y, blk[(i + 1) % 4].x, blk[(i + 1) % 4].y, 0x7A3140);
        break;
    }
    case T_LINE:
        thick(c, X + 3, Y + 16, X + 16, Y + 3, 2, ink);
        kc_fill(c, kr(X + 1, Y + 15, 4, 4), 0x3A7BD5);
        kc_fill(c, kr(X + 14, Y + 1, 4, 4), 0x3A7BD5);
        break;
    case T_RECT:
        kc_outline(c, kr(X + 2, Y + 4, 16, 12), ink);
        kc_outline(c, kr(X + 3, Y + 5, 14, 10), ink);
        break;
    case T_ELLIPSE: {
        ScreenSink ss;
        ssink_init(&ss, c, X, Y, 1);
        rs_ellipse(&ss.base, 1, 3, 18, 16, 2, ink, 0, S_OUTLINE);
        break;
    }
    case T_FILL: {
        Pt body[4] = { { X + 2, Y + 9 }, { X + 9, Y + 2 }, { X + 16, Y + 9 }, { X + 9, Y + 16 } };
        poly(c, body, 4, 0xC4CBD6);
        Pt paint[4] = { { X + 2, Y + 9 }, { X + 9, Y + 2 }, { X + 12, Y + 5 }, { X + 5, Y + 12 } };
        poly(c, paint, 4, 0x3A7BD5);
        for (int i = 0; i < 4; i++) kc_line(c, body[i].x, body[i].y, body[(i + 1) % 4].x, body[(i + 1) % 4].y, 0x4A525C);
        kc_circle_fill(c, X + 16, Y + 15, 2, 0x3A7BD5);
        kc_pixel(c, X + 16, Y + 12, 0x3A7BD5);
        break;
    }
    case T_PICKER:
        kc_circle_fill(c, X + 15, Y + 4, 3, 0x555C6B);
        thick(c, X + 13, Y + 6, X + 7, Y + 12, 3, 0xBFD6EE);
        kc_line(c, X + 12, Y + 8, X + 8, Y + 12, 0x7AA0D0);
        thick(c, X + 7, Y + 12, X + 3, Y + 16, 1, ink);
        kc_pixel(c, X + 2, Y + 17, ink);
        break;
    case T_TEXT:
        kc_text_bold(c, X + 5, Y + 2, "A", ink);
        break;
    case T_SELECT:
        for (int x = 3; x <= 16; x++) if (((x / 2) & 1) == 0) { kc_pixel(c, X + x, Y + 4, ink); kc_pixel(c, X + x, Y + 15, ink); }
        for (int y = 4; y <= 15; y++) if (((y / 2) & 1) == 0) { kc_pixel(c, X + 3, Y + y, ink); kc_pixel(c, X + 16, Y + y, ink); }
        break;
    case T_SPRAY:
        kc_fill(c, kr(X + 4, Y + 8, 9, 11), 0x9AA3B0);
        kc_outline(c, kr(X + 4, Y + 8, 9, 11), 0x4A525C);
        kc_fill(c, kr(X + 6, Y + 5, 5, 3), 0xD04A5A);
        kc_fill(c, kr(X + 10, Y + 4, 2, 2), 0xD04A5A);
        kc_pixel(c, X + 13, Y + 3, 0x3A7BD5);
        kc_pixel(c, X + 15, Y + 4, 0x3A7BD5);
        kc_pixel(c, X + 15, Y + 1, 0x3A7BD5);
        kc_pixel(c, X + 17, Y + 3, 0x3A7BD5);
        kc_pixel(c, X + 14, Y + 6, 0x3A7BD5);
        kc_pixel(c, X + 17, Y + 6, 0x3A7BD5);
        kc_pixel(c, X + 12, Y + 1, 0x3A7BD5);
        break;
    default: break;
    }
}

void icon_swap(KCanvas *c, int X, int Y) {
    const uint32_t ink = 0x23232F;
    kc_hline(c, X + 2, Y + 4, 9, ink);
    kc_line(c, X + 9, Y + 2, X + 11, Y + 4, ink);
    kc_line(c, X + 9, Y + 6, X + 11, Y + 4, ink);
    kc_hline(c, X + 3, Y + 9, 9, ink);
    kc_line(c, X + 5, Y + 7, X + 3, Y + 9, ink);
    kc_line(c, X + 5, Y + 11, X + 3, Y + 9, ink);
}
