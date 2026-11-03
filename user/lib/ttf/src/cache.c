/*
 * The glyph cache and text drawing.
 *
 * Rendered bitmaps are kept in a hash table keyed by (font id, glyph, size in 1/64 pixel, sub-pixel step, mode), and chained in a
 * doubly linked list in order of use. Memory is bounded twice: by the sum of the entries' sizes (header and pixels) and by their
 * number. When either limit is exceeded the least recently used entries are freed, never the one just handed out, so a single bitmap
 * larger than the whole budget is still returned (and goes at the next call). A glyph whose outline cannot be decoded is cached as an
 * empty bitmap, so a broken glyph in a long text is tried once, not once per occurrence.
 */
#include "ttfint.h"

typedef struct Entry {
    struct Entry *hnext;                /* hash chain */
    struct Entry *prev, *next;          /* use order: head is the most recent */
    uint32_t font_id, glyph, size64;
    uint8_t sub, mode;
    size_t bytes;                       /* what this entry counts against max_bytes */
    TtfBitmap bm;                       /* bm.data points just past this struct */
} Entry;

struct TtfCache {
    Entry **buckets;
    size_t nbuckets;                    /* a power of two */
    Entry *head, *tail;
    size_t max_bytes, max_entries;
    size_t bytes, entries, peak;
    uint64_t hits, misses, evictions;
};

#define DEFAULT_BYTES   (4u << 20)
#define DEFAULT_ENTRIES 4096u

TtfCache *ttf_cache_new(size_t max_bytes, size_t max_entries) {
    if (max_bytes == 0) max_bytes = DEFAULT_BYTES;
    if (max_entries == 0) max_entries = DEFAULT_ENTRIES;
    TtfCache *c = (TtfCache *)TTF_MALLOC(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    size_t nb = 16;
    while (nb < max_entries && nb < (1u << 16)) nb <<= 1;
    c->buckets = (Entry **)TTF_MALLOC(nb * sizeof(Entry *));
    if (!c->buckets) { TTF_FREE(c); return NULL; }
    memset(c->buckets, 0, nb * sizeof(Entry *));
    c->nbuckets = nb;
    c->max_bytes = max_bytes;
    c->max_entries = max_entries;
    return c;
}

static size_t hash_of(uint32_t font_id, uint32_t glyph, uint32_t size64, unsigned sub, unsigned mode) {
    uint32_t h = font_id * 0x9E3779B1u;
    h ^= glyph * 0x85EBCA6Bu + 0x7F4A7C15u;
    h = (h << 13) | (h >> 19);
    h ^= size64 * 0xC2B2AE35u;
    h ^= (sub * 0x27D4EB2Fu) + mode * 0x165667B1u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

static void unlink_lru(TtfCache *c, Entry *e) {
    if (e->prev) e->prev->next = e->next; else c->head = e->next;
    if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
    e->prev = e->next = NULL;
}

static void push_front(TtfCache *c, Entry *e) {
    e->prev = NULL;
    e->next = c->head;
    if (c->head) c->head->prev = e;
    c->head = e;
    if (!c->tail) c->tail = e;
}

static void drop(TtfCache *c, Entry *e) {
    size_t b = hash_of(e->font_id, e->glyph, e->size64, e->sub, e->mode) & (c->nbuckets - 1);
    for (Entry **pp = &c->buckets[b]; *pp; pp = &(*pp)->hnext) {
        if (*pp == e) { *pp = e->hnext; break; }
    }
    unlink_lru(c, e);
    c->bytes -= e->bytes;
    c->entries--;
    TTF_FREE(e);
}

static int evict_one(TtfCache *c) {
    if (!c->tail || c->tail == c->head) return 0;                   /* the newest entry stays */
    drop(c, c->tail);
    c->evictions++;
    return 1;
}

void ttf_cache_clear(TtfCache *c) {
    if (!c) return;
    while (c->head) drop(c, c->tail);
}

void ttf_cache_free(TtfCache *c) {
    if (!c) return;
    ttf_cache_clear(c);
    TTF_FREE(c->buckets);
    TTF_FREE(c);
}

void ttf_cache_stats(const TtfCache *c, TtfCacheStats *st) {
    memset(st, 0, sizeof *st);
    if (!c) return;
    st->hits = c->hits;
    st->misses = c->misses;
    st->evictions = c->evictions;
    st->bytes = c->bytes;
    st->entries = c->entries;
    st->peak_bytes = c->peak;
}

const TtfBitmap *ttf_cache_get(TtfCache *c, TtfRaster *r, const TtfFont *f, uint32_t glyph, float size_px, int subpixel, int mode) {
    if (!c || !r || !f) return NULL;
    if (subpixel < 0) subpixel = 0;
    if (subpixel >= TTF_SUBPIXEL_STEPS) subpixel = TTF_SUBPIXEL_STEPS - 1;
    if (mode != TTF_MODE_LCD) mode = TTF_MODE_GRAY;
    if (!(size_px >= 1.0f)) size_px = 1.0f;
    if (size_px > TTF_MAX_SIZE_PX) size_px = TTF_MAX_SIZE_PX;
    const uint32_t size64 = (uint32_t)(size_px * 64.0f + 0.5f);
    const size_t b = hash_of(f->id, glyph, size64, (unsigned)subpixel, (unsigned)mode) & (c->nbuckets - 1);

    for (Entry *e = c->buckets[b]; e; e = e->hnext) {
        if (e->font_id == f->id && e->glyph == glyph && e->size64 == size64 && e->sub == subpixel && e->mode == mode) {
            if (c->head != e) { unlink_lru(c, e); push_front(c, e); }
            c->hits++;
            return &e->bm;
        }
    }
    c->misses++;

    TtfRenderParams p;
    memset(&p, 0, sizeof p);
    p.size_px = (float)size64 / 64.0f;
    p.sub_x = (float)subpixel / (float)TTF_SUBPIXEL_STEPS;
    p.mode = mode;
    TtfBitmap bm;
    if (ttf_render(r, f, glyph, &p, &bm) != TTF_OK) {
        memset(&bm, 0, sizeof bm);
        bm.bpp = mode == TTF_MODE_LCD ? 3 : 1;
    }
    size_t data = bm.width > 0 && bm.height > 0 ? (size_t)bm.stride * (size_t)bm.height : 0;
    Entry *e = (Entry *)TTF_MALLOC(sizeof(Entry) + data);
    while (!e) {                                                    /* out of memory: give back what the cache holds and retry */
        if (!evict_one(c)) {
            if (!c->head) return NULL;
            drop(c, c->head);                                       /* nothing but the last returned entry is left, and nothing else fits */
            c->evictions++;
        }
        e = (Entry *)TTF_MALLOC(sizeof(Entry) + data);
    }
    e->hnext = NULL;
    e->prev = e->next = NULL;
    e->font_id = f->id;
    e->glyph = glyph;
    e->size64 = size64;
    e->sub = (uint8_t)subpixel;
    e->mode = (uint8_t)mode;
    e->bytes = sizeof(Entry) + data;
    e->bm = bm;
    e->bm.data = data ? (uint8_t *)(e + 1) : NULL;
    if (data) memcpy(e->bm.data, bm.data, data);
    e->hnext = c->buckets[b];
    c->buckets[b] = e;
    push_front(c, e);
    c->bytes += e->bytes;
    c->entries++;
    if (c->bytes > c->peak) c->peak = c->bytes;
    while ((c->bytes > c->max_bytes || c->entries > c->max_entries) && evict_one(c)) {}
    return &e->bm;
}

/* ---------------------------------------------------------------- drawing text */

float ttf_draw_text(TtfCache *c, TtfRaster *r, const TtfFontSet *set, float size_px, const char *utf8, size_t len,
                    float x, int baseline_y, const TtfDrawStyle *style, TtfSurface *dst) {
    float width = 0;
    if (!r || !set || !style || !dst || !dst->px || !utf8) return 0;
    TtfGlyphPos stack[128];
    TtfGlyphPos *pos = stack, *heap = NULL;
    size_t n = ttf_layout(set, size_px, utf8, len, style->layout_flags, stack, 128, &width);
    if (n > 128) {
        heap = (TtfGlyphPos *)TTF_MALLOC(n * sizeof *heap);
        if (!heap) return width;
        ttf_layout(set, size_px, utf8, len, style->layout_flags, heap, n, &width);
        pos = heap;
    }
    int cx0 = 0, cy0 = 0, cx1 = dst->width, cy1 = dst->height;
    if (style->clip_x1 > style->clip_x0 && style->clip_y1 > style->clip_y0) {
        cx0 = style->clip_x0; cy0 = style->clip_y0; cx1 = style->clip_x1; cy1 = style->clip_y1;
    }
    float margin = size_px * 2.0f + 2.0f;                           /* glyphs reach about this far from their pen position */
    int mode = style->mode == TTF_MODE_LCD ? TTF_MODE_LCD : TTF_MODE_GRAY;
    if ((float)baseline_y - margin <= (float)cy1 && (float)baseline_y + margin >= (float)cy0) {
        for (size_t i = 0; i < n; i++) {
            float X = x + pos[i].x;
            if (X > (float)cx1 + margin || X + pos[i].advance + margin < (float)cx0) continue;
            if (!(X > -1e8f && X < 1e8f)) continue;
            int ix = (int)X;
            if ((float)ix > X) ix--;                                /* floor */
            int sub = (int)((X - (float)ix) * (float)TTF_SUBPIXEL_STEPS + 0.5f);
            if (sub >= TTF_SUBPIXEL_STEPS) { sub = 0; ix++; }
            const TtfFont *f = set->fonts[pos[i].font];
            const TtfBitmap *bm;
            TtfBitmap local;
            if (c) {
                bm = ttf_cache_get(c, r, f, pos[i].glyph, size_px, sub, mode);
                if (!bm) continue;
            } else {
                TtfRenderParams p;
                memset(&p, 0, sizeof p);
                p.size_px = size_px;
                p.sub_x = (float)sub / (float)TTF_SUBPIXEL_STEPS;
                p.mode = mode;
                if (ttf_render(r, f, pos[i].glyph, &p, &local) != TTF_OK) continue;
                bm = &local;
            }
            ttf_blit(dst, ix, baseline_y, bm, style);
        }
    }
    TTF_FREE(heap);
    return width;
}
