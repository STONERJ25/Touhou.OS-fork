/*
 * libttf internals shared by the source files. Not for users of the library: include <ttf.h>.
 *
 * The one rule of this code base: font bytes are hostile. Every read goes through a Span, which refuses to read outside the
 * bytes it was made for and answers 0 instead; callers check counts against the span length before they loop, so a lie in a
 * font costs an error or a blank glyph and never a wild pointer.
 */
#ifndef GAP_TTFINT_H
#define GAP_TTFINT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ttf.h"

/* Allocation goes through these so the host fuzzer (tests/host/ttftool.c, -DTTF_ALLOC_HOOKS) can count what the library asks for
 * and fail it on demand. */
#ifdef TTF_ALLOC_HOOKS
void *ttf_hook_malloc(size_t n);
void *ttf_hook_realloc(void *p, size_t n);
void  ttf_hook_free(void *p);
#define TTF_MALLOC(n)      ttf_hook_malloc(n)
#define TTF_REALLOC(p, n)  ttf_hook_realloc((p), (n))
#define TTF_FREE(p)        ttf_hook_free(p)
#else
#include <stdlib.h>
#define TTF_MALLOC(n)      malloc(n)
#define TTF_REALLOC(p, n)  realloc((p), (n))
#define TTF_FREE(p)        free(p)
#endif

/* ---------------------------------------------------------------- bounds-checked reading */

typedef struct Span { const uint8_t *p; size_t n; } Span;

static inline Span sp_make(const uint8_t *p, size_t n) { Span s; s.p = p; s.n = n; return s; }
static inline Span sp_table(const TtfFont *f, TtfTable t) { return sp_make(f->data + t.off, t.len); }
static inline Span sp_from(Span s, size_t off) { return off >= s.n ? sp_make(s.p + s.n, 0) : sp_make(s.p + off, s.n - off); }
static inline int  sp_has(Span s, size_t off, size_t len) { return off <= s.n && len <= s.n - off; }
static inline uint32_t sp_u8(Span s, size_t o) { return o < s.n ? s.p[o] : 0u; }
static inline uint32_t sp_u16(Span s, size_t o) { return sp_has(s, o, 2) ? ((uint32_t)s.p[o] << 8) | s.p[o + 1] : 0u; }
static inline int32_t  sp_i16(Span s, size_t o) { return (int16_t)sp_u16(s, o); }
static inline uint32_t sp_u32(Span s, size_t o) {
    return sp_has(s, o, 4) ? ((uint32_t)s.p[o] << 24) | ((uint32_t)s.p[o + 1] << 16) | ((uint32_t)s.p[o + 2] << 8) | s.p[o + 3] : 0u;
}
static inline int32_t  sp_i32(Span s, size_t o) { return (int32_t)sp_u32(s, o); }

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---------------------------------------------------------------- scratch memory */

/* Points of the glyph being built (TrueType glyphs are collected as points first so that composites can place a component by
 * matching points, then turned into path operations). */
typedef struct TtfPts {
    float *x, *y;
    uint8_t *on;
    uint32_t *ends;                     /* index of the last point of each contour */
    uint32_t n, ncont;
    uint32_t cap_n, cap_c;
} TtfPts;

struct TtfRaster {
    TtfOutline outline;
    TtfPts pts;
    float *acc;                         /* signed-area accumulator, (width + 1) * height cells */
    size_t acc_cap;
    uint8_t *gray;                      /* 3x wide coverage of an LCD render before filtering */
    size_t gray_cap;
    uint8_t *bmp;                       /* the result of ttf_render */
    size_t bmp_cap;
};

/* Grows *buf (elements of `elem` bytes, *cap of them now) to hold at least `need`. 0 on success, -1 when memory or the size
 * arithmetic fails (the old buffer is then untouched). */
int ttfi_reserve(void **buf, uint32_t *cap, uint32_t need, size_t elem);
int ttfi_reserve_sz(void **buf, size_t *cap, size_t need, size_t elem);

/* Path building (outline.c). They return 0, or TTF_E_NOMEM / TTF_E_TOOBIG. */
void ttfi_outline_reset(TtfOutline *o);
int  ttfi_path_move(TtfOutline *o, float x, float y);
int  ttfi_path_line(TtfOutline *o, float x, float y);
int  ttfi_path_quad(TtfOutline *o, float cx, float cy, float x, float y);
int  ttfi_path_cubic(TtfOutline *o, float c1x, float c1y, float c2x, float c2y, float x, float y);
int  ttfi_path_close(TtfOutline *o);
void ttfi_pts_reset(TtfPts *p);
int  ttfi_pts_reserve(TtfPts *p, uint32_t total);   /* room for `total` points in all, TTF_E_TOOBIG beyond TTF_MAX_POINTS */
int  ttfi_pts_add(TtfPts *p, float x, float y, int on);
int  ttfi_pts_end_contour(TtfPts *p);               /* the contour just added ends at the last point; ignores an empty one */
int  ttfi_pts_to_path(const TtfPts *p, TtfOutline *o);

/* Per-format decoders. Each appends the glyph to an outline that ttfi_outline_reset emptied. */
int ttfi_glyf_outline(TtfRaster *r, const TtfFont *f, uint32_t glyph);
int ttfi_cff_open(TtfFont *f);                       /* validates the CFF table and fills the cff_* fields */
int ttfi_cff_outline(TtfRaster *r, const TtfFont *f, uint32_t glyph);
void ttfi_cmap_select(TtfFont *f);
void ttfi_otl_open(TtfFont *f);                      /* finds the kern subtables, the GPOS 'kern' lookups and the GSUB 'liga' lookups */

#endif
