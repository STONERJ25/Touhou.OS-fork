/*
 * Kyouko's browser (user/bin/browser): the first TouhouOS web browser. Phase 1: pages without JavaScript.
 *
 * Kyouko Kasodani is the yamabiko, the mountain echo: she shouts into the valley and an answer comes back, which is what
 * a browser does with a URL. The pieces around her keep their own names: Hatate (user/lib/web) looks at the page through
 * her camera, Kosuzu (user/lib/js) reads the scripts, Yukari opens the gaps the packets travel through. docs/BROWSER.md
 * says how the files of this program fit together.
 *
 * This header holds what several files share: the limits that keep a hostile page from taking the machine down, and the
 * two plain types of the pixel code (a surface and a rectangle). Nothing here depends on the window toolkit, so that the
 * font provider, the painter, the loader and the page logic also build for the host tests (tests/host/test_browser.py).
 */
#ifndef BROWSER_H
#define BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- limits */

#define BR_MAX_DOC_BYTES   (8u << 20)      /* the page itself: reading stops here and the status line says so */
#define BR_MAX_SUB_BYTES   (6u << 20)      /* one stylesheet or picture */
#define BR_MAX_SUBRES      400             /* subresources requested by one page */
#define BR_FETCH_THREADS   4               /* downloads running at once (the rest wait their turn) */
#define BR_TIMEOUT_MS      20000           /* connecting, and the longest silence while a body arrives */
#define BR_DEADLINE_MS     90000           /* the whole download of one resource, however slowly it trickles in */
#define BR_MEMORY_LIMIT    (160u << 20)    /* what one document may allocate (WebConfig.memory_limit) */
#define BR_MAX_REDIRECTS   10
#define BR_MAX_URL         4096

/* ---------------------------------------------------------------- pixels */

/* A pixel surface: 0x00RRGGBB (the toolkit's format; the alpha byte is ignored when writing). */
typedef struct BrSurface {
    uint32_t *px;
    int w, h, stride;                      /* stride in pixels */
} BrSurface;

/* A half-open rectangle of device pixels: x0 <= x < x1, y0 <= y < y1. */
typedef struct BrRect { int x0, y0, x1, y1; } BrRect;

static inline BrRect br_rect(int x, int y, int w, int h) { BrRect r = { x, y, x + w, y + h }; return r; }
static inline bool br_rect_empty(BrRect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static inline BrRect br_rect_and(BrRect a, BrRect b) {
    BrRect r = { a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1 };
    return r;
}

/* x / 255, rounded, for x up to 255 * 255 */
static inline unsigned br_div255(unsigned x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

/* Source-over of an opaque colour with coverage a (0..255) onto an opaque destination pixel. */
static inline uint32_t br_mix(uint32_t d, uint32_t rgb, unsigned a) {
    if (a >= 255) return rgb & 0xFFFFFFu;
    if (a == 0) return d & 0xFFFFFFu;
    unsigned ia = 255 - a;
    unsigned r = br_div255(((rgb >> 16) & 255) * a + ((d >> 16) & 255) * ia);
    unsigned g = br_div255(((rgb >> 8) & 255) * a + ((d >> 8) & 255) * ia);
    unsigned b = br_div255((rgb & 255) * a + (d & 255) * ia);
    return r << 16 | g << 8 | b;
}

#endif
