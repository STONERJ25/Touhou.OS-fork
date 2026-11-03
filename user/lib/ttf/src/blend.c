/*
 * Blending coverage bitmaps into 0x00RRGGBB pixels, and the coverage tables that make the blend look right.
 *
 * Colours blend correctly in linear light, but pixels hold sRGB values. For a text colour F over a background B and a coverage c,
 * the linear-light result is srgb(c * lin(F) + (1 - c) * lin(B)). A plain alpha blend in sRGB computes B + a * (F - B); setting
 * the two equal gives the alpha a that makes the cheap blend give the right answer, for greys. Text colours are judged by their
 * luma, which is exact for grey text and a good approximation for coloured text.
 *
 * The sRGB transfer function is the one of IEC 61966-2-1; the table below holds round(65535 * lin(i / 255)).
 */
#include "ttfint.h"

static const uint16_t srgb_to_lin16[256] = {
        0,    20,    40,    60,    80,    99,   119,   139,
      159,   179,   199,   219,   241,   264,   288,   313,
      340,   367,   396,   427,   458,   491,   526,   562,
      599,   637,   677,   718,   761,   805,   851,   898,
      947,   997,  1048,  1101,  1156,  1212,  1270,  1330,
     1391,  1453,  1517,  1583,  1651,  1720,  1790,  1863,
     1937,  2013,  2090,  2170,  2250,  2333,  2418,  2504,
     2592,  2681,  2773,  2866,  2961,  3058,  3157,  3258,
     3360,  3464,  3570,  3678,  3788,  3900,  4014,  4129,
     4247,  4366,  4488,  4611,  4736,  4864,  4993,  5124,
     5257,  5392,  5530,  5669,  5810,  5953,  6099,  6246,
     6395,  6547,  6700,  6856,  7014,  7174,  7335,  7500,
     7666,  7834,  8004,  8177,  8352,  8528,  8708,  8889,
     9072,  9258,  9445,  9635,  9828, 10022, 10219, 10417,
    10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090,
    12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
    14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878,
    16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
    18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281,
    20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
    23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
    28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033,
    31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
    34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429,
    37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
    41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534,
    45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
    48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369,
    52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
    57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955,
    61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
};

/* sRGB value (0..255, fractional allowed) to linear light (0..1). */
static float to_linear(float v) {
    if (v <= 0.0f) return 0.0f;
    if (v >= 255.0f) return 1.0f;
    int i = (int)v;
    float t = v - (float)i;
    return ((float)srgb_to_lin16[i] + t * (float)(srgb_to_lin16[i + 1] - srgb_to_lin16[i])) / 65535.0f;
}

/* Linear light (0..1) to an sRGB value (0..255): the inverse, by bisection on the table. */
static float to_srgb(float lin) {
    if (lin <= 0.0f) return 0.0f;
    if (lin >= 1.0f) return 255.0f;
    float target = lin * 65535.0f;
    int lo = 0, hi = 255;                                   /* table[lo] <= target < table[hi] */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if ((float)srgb_to_lin16[mid] <= target) lo = mid; else hi = mid;
    }
    float a = (float)srgb_to_lin16[lo], b = (float)srgb_to_lin16[hi];
    return (float)lo + (b > a ? (target - a) / (b - a) : 0.0f);
}

static float luma_of(uint32_t rgb) {
    return (54.0f * (float)((rgb >> 16) & 255) + 183.0f * (float)((rgb >> 8) & 255) + 19.0f * (float)(rgb & 255)) / 256.0f;
}

void ttf_coverage_lut(uint8_t lut[256], uint32_t fg, uint32_t bg, int gamma, int contrast) {
    if (gamma < 0) gamma = 0;
    if (gamma > 100) gamma = 100;
    if (contrast < 0) contrast = 0;
    if (contrast > 100) contrast = 100;
    float lf = luma_of(fg), lb = luma_of(bg);
    float diff = lf > lb ? lf - lb : lb - lf;
    float trust = diff / 64.0f;                             /* near-equal colours: the division below is ill-conditioned, keep plain coverage */
    if (trust > 1.0f) trust = 1.0f;
    float g = (float)gamma / 100.0f * trust, k = (float)contrast / 100.0f;
    float linf = to_linear(lf), linb = to_linear(lb);
    for (int i = 0; i < 256; i++) {
        float c = (float)i / 255.0f;
        float a = c;
        if (g > 0.0f) {
            float o = to_srgb(c * linf + (1.0f - c) * linb);        /* what the sRGB value of the true blend is */
            float exact = (o - lb) / (lf - lb);                      /* the alpha that reaches it by a plain blend */
            a = c + g * (exact - c);
        }
        if (k > 0.0f) a += k * (a * a * (3.0f - 2.0f * a) - a);      /* toward the smoothstep: steeper around one half */
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        lut[i] = (uint8_t)(a * 255.0f + 0.5f);
    }
    lut[0] = 0;
    lut[255] = 255;
}

/* round(x / 255) for 0 <= x <= 65280 */
static inline uint32_t div255(uint32_t x) { x += 128; return (x + (x >> 8)) >> 8; }

static inline uint32_t mix8(uint32_t d, uint32_t s, uint32_t a) { return div255(d * (255 - a) + s * a); }

void ttf_blit(TtfSurface *dst, int x, int y, const TtfBitmap *bm, const TtfDrawStyle *st) {
    if (!dst || !dst->px || !bm || !st || !bm->data || bm->width <= 0 || bm->height <= 0) return;
    long long bx = (long long)x + bm->left, by = (long long)y - bm->top;
    long long cx0 = 0, cy0 = 0, cx1 = dst->width, cy1 = dst->height;
    if (st->clip_x1 > st->clip_x0 && st->clip_y1 > st->clip_y0) {
        if (st->clip_x0 > cx0) cx0 = st->clip_x0;
        if (st->clip_y0 > cy0) cy0 = st->clip_y0;
        if (st->clip_x1 < cx1) cx1 = st->clip_x1;
        if (st->clip_y1 < cy1) cy1 = st->clip_y1;
    }
    long long x0 = bx > cx0 ? bx : cx0, x1 = bx + bm->width < cx1 ? bx + bm->width : cx1;
    long long y0 = by > cy0 ? by : cy0, y1 = by + bm->height < cy1 ? by + bm->height : cy1;
    if (x0 >= x1 || y0 >= y1) return;
    const uint32_t fr = (st->color >> 16) & 255, fg = (st->color >> 8) & 255, fb = st->color & 255;
    const uint8_t *lut = st->lut;
    for (long long yy = y0; yy < y1; yy++) {
        uint32_t *d = dst->px + (size_t)yy * (size_t)dst->stride + (size_t)x0;
        const uint8_t *s = bm->data + (size_t)(yy - by) * (size_t)bm->stride + (size_t)(x0 - bx) * (size_t)bm->bpp;
        if (bm->bpp == 1) {
            for (long long xx = x0; xx < x1; xx++, d++, s++) {
                uint32_t a = lut ? lut[*s] : *s;
                if (a == 0) continue;
                uint32_t top = *d & 0xFF000000u;
                if (a == 255) { *d = top | (st->color & 0xFFFFFFu); continue; }
                uint32_t r = mix8((*d >> 16) & 255, fr, a), g = mix8((*d >> 8) & 255, fg, a), b = mix8(*d & 255, fb, a);
                *d = top | (r << 16) | (g << 8) | b;
            }
        } else {
            for (long long xx = x0; xx < x1; xx++, d++, s += 3) {
                uint32_t ar = s[st->lcd_bgr ? 2 : 0], ag = s[1], ab = s[st->lcd_bgr ? 0 : 2];       /* stripes run left to right */
                if (lut) { ar = lut[ar]; ag = lut[ag]; ab = lut[ab]; }
                if ((ar | ag | ab) == 0) continue;
                uint32_t top = *d & 0xFF000000u;
                uint32_t r = mix8((*d >> 16) & 255, fr, ar), g = mix8((*d >> 8) & 255, fg, ag), b = mix8(*d & 255, fb, ab);
                *d = top | (r << 16) | (g << 8) | b;
            }
        }
    }
}
