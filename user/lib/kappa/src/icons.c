/* Procedural icons: everything is drawn from primitives on a 32 unit grid and scales to any size. No image files. */
#include <kappa.h>
#include <yinyang.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int x, y; } Pt;

/* Scanline polygon fill (even-odd). Points are in absolute pixels. */
static void poly(KCanvas *c, const Pt *p, int n, uint32_t rgb) {
    int ymin = p[0].y, ymax = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].y < ymin) ymin = p[i].y; if (p[i].y > ymax) ymax = p[i].y; }
    for (int y = ymin; y <= ymax; y++) {
        int xs[32], nx = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            if ((p[i].y <= y && p[j].y > y) || (p[j].y <= y && p[i].y > y)) {
                if (nx < 32) xs[nx++] = p[i].x + (y - p[i].y) * (p[j].x - p[i].x) / (p[j].y - p[i].y);
            }
        }
        for (int a = 1; a < nx; a++)
            for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) { int t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t; }
        for (int k = 0; k + 1 < nx; k += 2) kc_hline(c, xs[k], y, xs[k + 1] - xs[k] + 1, rgb);
    }
}

/* Five-pointed star centred on (cx, cy), pointing up. */
static void star(KCanvas *c, int cx, int cy, int ro, int ri, uint32_t rgb) {
    static const int sinv[10] = { 0, 588, 951, 951, 588, 0, -588, -951, -951, -588 };
    static const int cosv[10] = { 1000, 809, 309, -309, -809, -1000, -809, -309, 309, 809 };
    Pt st[10];
    for (int i = 0; i < 10; i++) {
        int r = i % 2 ? ri : ro, k = (i + 8) % 10;
        st[i].x = cx + r * cosv[k] / 1000;
        st[i].y = cy + r * sinv[k] / 1000;
    }
    poly(c, st, 10, rgb);
}

/* A ring of outer radius ro and thickness t; pixels within `gap` columns of the axis in the given direction are skipped
 * (dir: 0 none, 1 top, 2 top-right). */
static void ring(KCanvas *c, int cx, int cy, int ro, int t, int gap, int dir, uint32_t rgb) {
    int ri = ro - t;
    for (int y = -ro; y <= ro; y++)
        for (int x = -ro; x <= ro; x++) {
            int d = x * x + y * y;
            if (d > ro * ro || d < ri * ri) continue;
            if (dir == 1 && y < 0 && abs(x) < gap) continue;
            if (dir == 2 && x > ro / 4 && y < -ro / 4 && x + gap * 0 > 0) continue;
            kc_pixel(c, cx + x, cy + y, rgb);
        }
}

static void thick_line(KCanvas *c, int x0, int y0, int x1, int y1, int t, uint32_t rgb) {
    for (int dy = 0; dy < t; dy++)
        for (int dx = 0; dx < t; dx++) kc_line(c, x0 + dx - t / 2, y0 + dy - t / 2, x1 + dx - t / 2, y1 + dy - t / 2, rgb);
}

static const char *const g_names[KI_COUNT] = {
    "folder", "folder-open", "file", "text", "image", "terminal", "calculator", "clock", "gear", "spellcard", "news", "notebook",
    "trash", "home", "drive", "yinyang", "power", "search", "star", "up", "back", "forward", "plus", "close", "minimize", "maximize",
    "restore", "check", "arrow-down", "arrow-right", "refresh", "game", "monitor", "info", "warning", "error", "question", "torii",
    "bamboo", "petal", "moon", "lock", "user", "sketch",
};

const char *k_icon_name(KIcon i) { return i >= 0 && i < KI_COUNT ? g_names[i] : "?"; }

void k_icon(KCanvas *c, KIcon icon, int X, int Y, int s) {
#define U(v) ((v) * s / 32)
#define X_(v) (X + U(v))
#define Y_(v) (Y + U(v))
#define R_(x, y, w, h) kr(X_(x), Y_(y), U(w) > 0 ? U(w) : 1, U(h) > 0 ? U(h) : 1)
    const uint32_t ink = 0x23232F, white = 0xFFFFFF, red = KC_SHRINE;
    switch (icon) {
    case KI_FOLDER: case KI_FOLDER_OPEN: {
        uint32_t back = 0xC99A2E, front = 0xF0C65A;
        kc_fill(c, R_(3, 7, 12, 5), back);
        kc_fill(c, R_(3, 10, 26, 17), back);
        if (icon == KI_FOLDER) kc_fill(c, R_(3, 12, 26, 15), front);
        else {
            Pt p[4] = { { X_(7), Y_(14) }, { X_(31), Y_(14) }, { X_(26), Y_(27) }, { X_(2), Y_(27) } };
            poly(c, p, 4, front);
        }
        kc_hline(c, X_(3), Y_(12), U(26), 0xFFE9A8);
        kc_outline(c, R_(3, 10, 26, 17), 0x8B6A1C);
        break;
    }
    case KI_FILE: case KI_TEXT: {
        kc_fill(c, R_(7, 3, 18, 26), white);
        kc_outline(c, R_(7, 3, 18, 26), 0x55555F);
        Pt fold[3] = { { X_(19), Y_(3) }, { X_(25), Y_(9) }, { X_(19), Y_(9) } };
        poly(c, fold, 3, 0xCFCFD6);
        if (icon == KI_TEXT) for (int i = 0; i < 5; i++) kc_fill(c, R_(10, 12 + i * 3, 12 - (i == 4) * 5, 1), 0x6A6A78);
        break;
    }
    case KI_IMAGE: {
        kc_fill(c, R_(3, 5, 26, 22), 0x7EC8F0);
        Pt m1[3] = { { X_(3), Y_(27) }, { X_(12), Y_(13) }, { X_(21), Y_(27) } };
        Pt m2[3] = { { X_(14), Y_(27) }, { X_(22), Y_(16) }, { X_(29), Y_(27) } };
        poly(c, m1, 3, 0x3E8F4E);
        poly(c, m2, 3, 0x56A863);
        kc_circle_fill(c, X_(23), Y_(11), U(3), 0xFFE06A);
        kc_outline(c, R_(3, 5, 26, 22), ink);
        break;
    }
    case KI_TERMINAL: {
        kc_fill(c, R_(3, 5, 26, 22), 0x14141C);
        kc_outline(c, R_(3, 5, 26, 22), 0x70707C);
        kc_fill(c, R_(3, 5, 26, 4), 0x50505E);
        thick_line(c, X_(7), Y_(14), X_(12), Y_(18), 2, 0x6CFF8A);
        thick_line(c, X_(12), Y_(18), X_(7), Y_(22), 2, 0x6CFF8A);
        kc_fill(c, R_(15, 22, 8, 2), 0x6CFF8A);
        break;
    }
    case KI_CALC: {
        kc_fill(c, R_(6, 3, 20, 26), 0xC7C2B2);
        kc_outline(c, R_(6, 3, 20, 26), 0x55524A);
        kc_fill(c, R_(9, 6, 14, 6), 0xAEE6B0);
        for (int r = 0; r < 3; r++)
            for (int q = 0; q < 3; q++) kc_fill(c, R_(9 + q * 5, 15 + r * 4, 4, 3), r == 2 && q == 2 ? red : 0x4A4A56);
        break;
    }
    case KI_CLOCK: {
        kc_circle_fill(c, X_(16), Y_(16), U(13), 0x5A3B1E);
        kc_circle_fill(c, X_(16), Y_(16), U(11), 0xFBF7EA);
        for (int h = 0; h < 12; h++) {
            static const int dx[12] = { 0, 5, 9, 10, 9, 5, 0, -5, -9, -10, -9, -5 };
            static const int dy[12] = { -10, -9, -5, 0, 5, 9, 10, 9, 5, 0, -5, -9 };
            kc_fill(c, kr(X_(16) + U(dx[h]), Y_(16) + U(dy[h]), 1 + (s >= 28), 1 + (s >= 28)), ink);
        }
        thick_line(c, X_(16), Y_(16), X_(16), Y_(8), s >= 28 ? 2 : 1, ink);
        thick_line(c, X_(16), Y_(16), X_(22), Y_(19), s >= 28 ? 2 : 1, red);
        break;
    }
    case KI_GEAR: {
        for (int t = 0; t < 8; t++) {
            static const int dx[8] = { 0, 9, 13, 9, 0, -9, -13, -9 }, dy[8] = { -13, -9, 0, 9, 13, 9, 0, -9 };
            kc_circle_fill(c, X_(16) + U(dx[t]) * 9 / 10, Y_(16) + U(dy[t]) * 9 / 10, U(3), 0x5C6B7A);
        }
        kc_circle_fill(c, X_(16), Y_(16), U(10), 0x7A8A9A);
        kc_circle_fill(c, X_(16), Y_(16), U(4), KC_PAPER);
        kc_circle(c, X_(16), Y_(16), U(10), 0x3E4A56);
        break;
    }
    case KI_SPELLCARD: {
        kc_fill(c, R_(7, 2, 18, 28), 0xF6EFD6);
        kc_outline(c, R_(7, 2, 18, 28), red);
        kc_outline(c, R_(9, 4, 14, 24), 0xD9A441);
        star(c, X_(16), Y_(15), U(7), U(3), 0xE8B83C);
        kc_fill(c, R_(11, 24, 10, 2), red);
        break;
    }
    case KI_NEWS: {
        kc_fill(c, R_(4, 4, 24, 24), 0xF1EEE4);
        kc_outline(c, R_(4, 4, 24, 24), 0x55555F);
        kc_fill(c, R_(7, 7, 18, 4), ink);
        kc_fill(c, R_(7, 13, 8, 7), 0x8FA6C8);
        for (int i = 0; i < 4; i++) kc_fill(c, R_(17, 13 + i * 3, 8, 1), 0x6A6A78);
        for (int i = 0; i < 2; i++) kc_fill(c, R_(7, 22 + i * 3, 18, 1), 0x6A6A78);
        break;
    }
    case KI_NOTEBOOK: {
        kc_fill(c, R_(6, 3, 20, 26), 0x6C3FA3);
        kc_fill(c, R_(6, 3, 3, 26), 0x4B2B78);
        kc_outline(c, R_(6, 3, 20, 26), 0x2E1A4A);
        kc_fill(c, R_(12, 8, 10, 8), 0xF2E7B8);
        kc_circle_fill(c, X_(17), Y_(12), U(3), 0xB78FE0);
        kc_fill(c, R_(21, 24, 3, 7), red);
        break;
    }
    case KI_TRASH: {
        kc_fill(c, R_(8, 9, 16, 19), 0xA9B1BC);
        for (int i = 0; i < 3; i++) kc_fill(c, R_(11 + i * 4, 12, 1, 13), 0x6C747F);
        kc_fill(c, R_(6, 6, 20, 3), 0x8B939E);
        kc_fill(c, R_(13, 4, 6, 2), 0x8B939E);
        kc_outline(c, R_(8, 9, 16, 19), 0x4A525C);
        break;
    }
    case KI_HOME: {
        Pt roof[3] = { { X_(16), Y_(4) }, { X_(30), Y_(16) }, { X_(2), Y_(16) } };
        poly(c, roof, 3, red);
        kc_fill(c, R_(6, 16, 20, 12), 0xEFE3C4);
        kc_outline(c, R_(6, 16, 20, 12), 0x6B5D3A);
        kc_fill(c, R_(13, 20, 6, 8), 0x7A4B2A);
        break;
    }
    case KI_DRIVE: {
        kc_fill(c, R_(3, 10, 26, 13), 0xA7AEB8);
        kc_outline(c, R_(3, 10, 26, 13), 0x4A525C);
        kc_fill(c, R_(5, 13, 16, 2), 0x6C747F);
        kc_circle_fill(c, X_(25), Y_(19), U(1) + 1, 0x4CFF6A);
        break;
    }
    case KI_YINYANG: {
        uint32_t *buf = malloc((size_t)s * (size_t)s * 4);
        if (buf) {
            yinyang_render(buf, s, s, 0xF4F4F4, red, 0xFF000000u);        /* background marker: skipped below */
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++)
                    if (buf[y * s + x] != 0xFF000000u) kc_pixel(c, X + x, Y + y, buf[y * s + x]);
            free(buf);
        }
        break;
    }
    case KI_POWER: {
        ring(c, X_(16), Y_(17), U(11), s >= 24 ? 3 : 2, U(4), 1, red);
        kc_fill(c, R_(15, 4, 3, 13), red);
        break;
    }
    case KI_SEARCH: {
        kc_circle(c, X_(13), Y_(13), U(8), ink);
        kc_circle(c, X_(13), Y_(13), U(7), ink);
        thick_line(c, X_(19), Y_(19), X_(28), Y_(28), s >= 24 ? 3 : 2, ink);
        break;
    }
    case KI_STAR: star(c, X_(16), Y_(17), U(14), U(6), KC_GOLD); break;
    case KI_UP: thick_line(c, X_(8), Y_(20), X_(16), Y_(10), 2, ink); thick_line(c, X_(24), Y_(20), X_(16), Y_(10), 2, ink); break;
    case KI_BACK: thick_line(c, X_(20), Y_(7), X_(10), Y_(16), 2, ink); thick_line(c, X_(20), Y_(25), X_(10), Y_(16), 2, ink); break;
    case KI_FORWARD: thick_line(c, X_(12), Y_(7), X_(22), Y_(16), 2, ink); thick_line(c, X_(12), Y_(25), X_(22), Y_(16), 2, ink); break;
    case KI_PLUS: kc_fill(c, R_(14, 6, 4, 20), ink); kc_fill(c, R_(6, 14, 20, 4), ink); break;
    case KI_CLOSE: thick_line(c, X_(8), Y_(8), X_(24), Y_(24), 2, ink); thick_line(c, X_(24), Y_(8), X_(8), Y_(24), 2, ink); break;
    case KI_MINIMIZE: kc_fill(c, R_(8, 22, 16, 3), ink); break;
    case KI_MAXIMIZE: kc_outline(c, R_(8, 8, 16, 16), ink); kc_hline(c, X_(8), Y_(9), U(16), ink); break;
    case KI_RESTORE:                                            /* the visible parts of the back square, then the front one */
        kc_hline(c, X_(11), Y_(7), U(13), ink);
        kc_vline(c, X_(23), Y_(7), U(13), ink);
        kc_vline(c, X_(11), Y_(7), U(5), ink);
        kc_hline(c, X_(21), Y_(19), U(3), ink);
        kc_outline(c, R_(8, 11, 13, 13), ink);
        break;
    case KI_CHECK: thick_line(c, X_(7), Y_(17), X_(13), Y_(23), 3, 0x1E8A3C); thick_line(c, X_(13), Y_(23), X_(25), Y_(9), 3, 0x1E8A3C); break;
    case KI_ARROW_DOWN: {
        Pt t[3] = { { X_(9), Y_(12) }, { X_(23), Y_(12) }, { X_(16), Y_(21) } };
        poly(c, t, 3, ink);
        break;
    }
    case KI_ARROW_RIGHT: {
        Pt t[3] = { { X_(12), Y_(9) }, { X_(12), Y_(23) }, { X_(21), Y_(16) } };
        poly(c, t, 3, ink);
        break;
    }
    case KI_REFRESH: {
        ring(c, X_(16), Y_(17), U(10), s >= 24 ? 3 : 2, U(3), 2, ink);
        Pt t[3] = { { X_(18), Y_(2) }, { X_(28), Y_(9) }, { X_(17), Y_(12) } };
        poly(c, t, 3, ink);
        break;
    }
    case KI_GAME: {
        kc_fill(c, R_(3, 10, 26, 14), 0x6B6B7E);
        kc_circle_fill(c, X_(6), Y_(18), U(5), 0x6B6B7E);
        kc_circle_fill(c, X_(26), Y_(18), U(5), 0x6B6B7E);
        kc_fill(c, R_(8, 14, 6, 2), white); kc_fill(c, R_(10, 12, 2, 6), white);
        kc_circle_fill(c, X_(21), Y_(14), U(2), red);
        kc_circle_fill(c, X_(25), Y_(18), U(2), 0x3A7BD5);
        break;
    }
    case KI_MONITOR: {
        kc_fill(c, R_(3, 4, 26, 18), 0x30303C);
        kc_fill(c, R_(5, 6, 22, 14), 0x2B3F7A);
        kc_fill(c, R_(13, 22, 6, 4), 0x6B6B7E);
        kc_fill(c, R_(9, 26, 14, 2), 0x6B6B7E);
        break;
    }
    case KI_INFO: kc_circle_fill(c, X_(16), Y_(16), U(13), 0x2D6CC0); kc_circle_fill(c, X_(16), Y_(9), U(2), white); kc_fill(c, R_(14, 13, 4, 11), white); break;
    case KI_QUESTION:
        kc_circle_fill(c, X_(16), Y_(16), U(13), 0x2D6CC0);
        kc_text_scaled(c, X_(16) - (s >= 24 ? 8 : 4), Y_(16) - (s >= 24 ? 16 : 8), "?", white, s >= 24 ? 2 : 1);
        break;
    case KI_WARNING: {
        Pt t[3] = { { X_(16), Y_(3) }, { X_(30), Y_(28) }, { X_(2), Y_(28) } };
        poly(c, t, 3, 0xF2C230);
        kc_fill(c, R_(15, 11, 3, 9), ink); kc_fill(c, R_(15, 22, 3, 3), ink);
        break;
    }
    case KI_ERROR:
        kc_circle_fill(c, X_(16), Y_(16), U(13), 0xC02D2D);
        thick_line(c, X_(10), Y_(10), X_(22), Y_(22), 3, white); thick_line(c, X_(22), Y_(10), X_(10), Y_(22), 3, white);
        break;
    case KI_TORII: {
        kc_fill(c, R_(2, 5, 28, 4), red);
        kc_fill(c, R_(0, 3, 32, 3), 0x2A2A33);
        kc_fill(c, R_(6, 11, 20, 3), red);
        kc_fill(c, R_(8, 9, 4, 20), red);
        kc_fill(c, R_(20, 9, 4, 20), red);
        break;
    }
    case KI_BAMBOO: {
        for (int i = 0; i < 3; i++) {
            int bx = 6 + i * 9;
            kc_fill(c, R_(bx, 2, 5, 28), 0x58A55F);
            kc_fill(c, R_(bx, 2, 2, 28), 0x7DC884);
            for (int k = 0; k < 3; k++) kc_fill(c, R_(bx - 1, 9 + k * 9 + i, 7, 2), 0x2F7036);
        }
        break;
    }
    case KI_PETAL: {
        kc_ellipse_fill(c, X_(16), Y_(16), U(6), U(11), 0xF6B4C8);
        kc_ellipse_fill(c, X_(16), Y_(14), U(3), U(6), 0xFAD3E0);
        break;
    }
    case KI_MOON: {
        kc_circle_fill(c, X_(15), Y_(16), U(12), KC_MOON);
        kc_circle_fill(c, X_(21), Y_(12), U(10), KC_NIGHT);
        break;
    }
    case KI_LOCK: {
        kc_circle(c, X_(16), Y_(12), U(6), ink); kc_circle(c, X_(16), Y_(12), U(5), ink);
        kc_fill(c, R_(7, 13, 18, 14), KC_GOLD);
        kc_outline(c, R_(7, 13, 18, 14), 0x7A5C16);
        kc_fill(c, R_(15, 18, 3, 5), ink);
        break;
    }
    case KI_USER: {
        kc_circle_fill(c, X_(16), Y_(11), U(6), 0xF0CFA8);
        Pt sh[6] = { { X_(5), Y_(29) }, { X_(8), Y_(21) }, { X_(12), Y_(18) }, { X_(20), Y_(18) }, { X_(24), Y_(21) }, { X_(27), Y_(29) } };
        poly(c, sh, 6, red);
        break;
    }
    case KI_SKETCH: {
        kc_ellipse_fill(c, X_(15), Y_(17), U(13), U(10), 0xD8B27A);
        kc_circle_fill(c, X_(9), Y_(15), U(2), red); kc_circle_fill(c, X_(14), Y_(11), U(2), 0x3A7BD5);
        kc_circle_fill(c, X_(20), Y_(12), U(2), 0x3FB26B); kc_circle_fill(c, X_(23), Y_(18), U(2), KC_GOLD);
        thick_line(c, X_(26), Y_(4), X_(14), Y_(26), 2, 0x6B4A2A);
        break;
    }
    default: break;
    }
#undef U
#undef X_
#undef Y_
#undef R_
}
