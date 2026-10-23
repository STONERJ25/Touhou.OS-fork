/* The picture and its history.
 *
 * Undo works on 64x64 tiles: before a painting operation writes pixels it announces the rectangle (hist_touch) and the
 * history saves the old content of every tile it covers, once per operation. Undo swaps the saved tiles with the picture,
 * which turns the entry into a redo entry. A 4096x4096 canvas therefore costs nothing until something is painted, and a
 * pencil stroke costs a few kilobytes. Whole-image operations are recorded as the operation itself (flip, rotate and
 * invert are exact inverses of each other) or, for resizing, as the old pixel buffer. */
#include "sketchbook.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

Doc g_doc;

typedef enum { H_TILES, H_SNAPSHOT, H_OP } HKind;

typedef struct HEntry {
    HKind kind;
    int op;                                 /* H_OP */
    char label[32];
    uint32_t id;
    int ntiles, cap;                        /* H_TILES */
    int *tile;
    uint32_t *data;                         /* ntiles * 64 * 64 saved pixels, rows of 64 */
    int sw, sh;                             /* H_SNAPSHOT: the other picture */
    uint32_t *spx;
} HEntry;

#define MAX_LEVELS 200
#define MAX_BYTES  ((size_t)96 << 20)
#define TILE_PX    (SK_TILE * SK_TILE)

static HEntry **g_undo, **g_redo;
static int g_nundo, g_nredo, g_cap_undo, g_cap_redo;
static HEntry *g_pend;
static uint8_t *g_tflag;
static int g_tw, g_th;
static uint32_t g_next_id = 1;
static size_t g_bytes;

static size_t entry_bytes(const HEntry *e) {
    if (e->kind == H_TILES) return (size_t)e->ntiles * (TILE_PX * 4 + sizeof(int));
    if (e->kind == H_SNAPSHOT) return (size_t)e->sw * (size_t)e->sh * 4;
    return 0;
}

static void entry_free(HEntry *e) {
    if (!e) return;
    free(e->tile);
    free(e->data);
    free(e->spx);
    free(e);
}

static void list_push(HEntry ***a, int *n, int *cap, HEntry *e) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        HEntry **na = realloc(*a, (size_t)nc * sizeof **a);
        if (!na) { entry_free(e); return; }
        *a = na;
        *cap = nc;
    }
    (*a)[(*n)++] = e;
}

/* ---------------------------------------------------------------- document */

void doc_free(void) {
    free(g_doc.px);
    g_doc.px = NULL;
    g_doc.w = g_doc.h = 0;
}

bool doc_new(int w, int h, uint32_t fill) {
    w = sk_clamp(w, 1, SK_MAX_DIM);
    h = sk_clamp(h, 1, SK_MAX_DIM);
    uint32_t *np = malloc((size_t)w * (size_t)h * 4);
    if (!np) return false;
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) np[i] = fill;
    hist_clear();
    free(g_doc.px);
    g_doc.px = np;
    g_doc.w = w;
    g_doc.h = h;
    return true;
}

bool doc_adopt(uint32_t *px, int w, int h) {
    if (!px || w < 1 || h < 1) return false;
    hist_clear();
    free(g_doc.px);
    g_doc.px = px;
    g_doc.w = w;
    g_doc.h = h;
    return true;
}

/* ---------------------------------------------------------------- tiles */

static void tile_geom(int t, int *x, int *y, int *vw, int *vh) {
    int tx = t % g_tw, ty = t / g_tw;
    *x = tx * SK_TILE;
    *y = ty * SK_TILE;
    *vw = sk_min(SK_TILE, g_doc.w - *x);
    *vh = sk_min(SK_TILE, g_doc.h - *y);
}

static void save_tile(HEntry *e, int t) {
    if (e->ntiles == e->cap) {
        int nc = e->cap ? e->cap * 2 : 8;
        int *nt = realloc(e->tile, (size_t)nc * sizeof(int));
        if (nt) e->tile = nt;
        uint32_t *nd = realloc(e->data, (size_t)nc * TILE_PX * 4);
        if (nd) e->data = nd;
        if (!nt || !nd) return;                           /* out of memory: this tile cannot be undone */
        e->cap = nc;
    }
    int x, y, vw, vh;
    tile_geom(t, &x, &y, &vw, &vh);
    uint32_t *dst = e->data + (size_t)e->ntiles * TILE_PX;
    for (int r = 0; r < vh; r++) memcpy(dst + r * SK_TILE, g_doc.px + (size_t)(y + r) * g_doc.w + x, (size_t)vw * 4);
    e->tile[e->ntiles++] = t;
}

static void swap_tiles(HEntry *e) {
    for (int k = 0; k < e->ntiles; k++) {
        int x, y, vw, vh;
        tile_geom(e->tile[k], &x, &y, &vw, &vh);
        uint32_t *buf = e->data + (size_t)k * TILE_PX;
        for (int r = 0; r < vh; r++) {
            uint32_t *a = g_doc.px + (size_t)(y + r) * g_doc.w + x, *b = buf + r * SK_TILE;
            for (int i = 0; i < vw; i++) { uint32_t t = a[i]; a[i] = b[i]; b[i] = t; }
        }
    }
}

void hist_begin(const char *label) {
    if (g_pend) hist_commit();
    g_pend = calloc(1, sizeof *g_pend);
    if (!g_pend) return;
    g_pend->kind = H_TILES;
    snprintf(g_pend->label, sizeof g_pend->label, "%s", label ? label : "");
    g_tw = (g_doc.w + SK_TILE - 1) / SK_TILE;
    g_th = (g_doc.h + SK_TILE - 1) / SK_TILE;
    free(g_tflag);
    g_tflag = calloc((size_t)g_tw * (size_t)g_th, 1);
    if (!g_tflag) { free(g_pend); g_pend = NULL; }
}

void hist_touch(int x0, int y0, int x1, int y1) {
    if (!g_pend || !g_tflag) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g_doc.w) x1 = g_doc.w - 1;
    if (y1 >= g_doc.h) y1 = g_doc.h - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int ty = y0 / SK_TILE; ty <= y1 / SK_TILE; ty++)
        for (int tx = x0 / SK_TILE; tx <= x1 / SK_TILE; tx++) {
            uint8_t *f = &g_tflag[ty * g_tw + tx];
            if (!*f) { *f = 1; save_tile(g_pend, ty * g_tw + tx); }
        }
}

bool hist_pending(void) { return g_pend != NULL; }
bool hist_pending_dirty(void) { return g_pend != NULL && g_pend->ntiles > 0; }

static void trim_history(void) {
    while (g_nundo > 1 && (g_nundo > MAX_LEVELS || g_bytes > MAX_BYTES)) {
        HEntry *e = g_undo[0];
        g_bytes -= entry_bytes(e);
        entry_free(e);
        memmove(g_undo, g_undo + 1, (size_t)(g_nundo - 1) * sizeof *g_undo);
        g_nundo--;
    }
}

static void drop_redo(void) {
    for (int i = 0; i < g_nredo; i++) { g_bytes -= entry_bytes(g_redo[i]); entry_free(g_redo[i]); }
    g_nredo = 0;
}

static void push_undo(HEntry *e) {
    e->id = g_next_id++;
    drop_redo();
    g_bytes += entry_bytes(e);
    list_push(&g_undo, &g_nundo, &g_cap_undo, e);
    trim_history();
}

void hist_commit(void) {
    if (!g_pend) return;
    HEntry *e = g_pend;
    g_pend = NULL;
    free(g_tflag);
    g_tflag = NULL;
    if (e->ntiles == 0) { entry_free(e); return; }
    push_undo(e);
}

void hist_abort(void) {
    if (!g_pend) return;
    swap_tiles(g_pend);                                   /* puts the old pixels back */
    entry_free(g_pend);
    g_pend = NULL;
    free(g_tflag);
    g_tflag = NULL;
}

void hist_clear(void) {
    if (g_pend) { entry_free(g_pend); g_pend = NULL; }
    free(g_tflag);
    g_tflag = NULL;
    for (int i = 0; i < g_nundo; i++) entry_free(g_undo[i]);
    for (int i = 0; i < g_nredo; i++) entry_free(g_redo[i]);
    g_nundo = g_nredo = 0;
    g_bytes = 0;
}

uint32_t hist_version(void) { return g_nundo ? g_undo[g_nundo - 1]->id : 0; }
const char *hist_undo_label(void) { return g_nundo ? g_undo[g_nundo - 1]->label : NULL; }
const char *hist_redo_label(void) { return g_nredo ? g_redo[g_nredo - 1]->label : NULL; }

/* ---------------------------------------------------------------- whole-image operations */

static void img_flip_h(void) {
    for (int y = 0; y < g_doc.h; y++) {
        uint32_t *row = g_doc.px + (size_t)y * g_doc.w;
        for (int x = 0, e = g_doc.w - 1; x < e; x++, e--) { uint32_t t = row[x]; row[x] = row[e]; row[e] = t; }
    }
}

static void img_flip_v(void) {
    for (int y = 0, e = g_doc.h - 1; y < e; y++, e--) {
        uint32_t *a = g_doc.px + (size_t)y * g_doc.w, *b = g_doc.px + (size_t)e * g_doc.w;
        for (int x = 0; x < g_doc.w; x++) { uint32_t t = a[x]; a[x] = b[x]; b[x] = t; }
    }
}

static bool img_rotate(bool clockwise) {
    int w = g_doc.w, h = g_doc.h;
    uint32_t *np = malloc((size_t)w * (size_t)h * 4);
    if (!np) return false;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = g_doc.px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int dx = clockwise ? h - 1 - y : y, dy = clockwise ? x : w - 1 - x;      /* the new picture is h wide, w tall */
            np[(size_t)dy * h + dx] = src[x];
        }
    }
    free(g_doc.px);
    g_doc.px = np;
    g_doc.w = h;
    g_doc.h = w;
    return true;
}

static void img_invert(void) {
    size_t n = (size_t)g_doc.w * (size_t)g_doc.h;
    for (size_t i = 0; i < n; i++) g_doc.px[i] ^= 0x00FFFFFFu;
}

static bool apply_op(int op, bool undo) {
    switch (op) {
    case HOP_FLIP_H: img_flip_h(); return true;
    case HOP_FLIP_V: img_flip_v(); return true;
    case HOP_ROT_CW: return img_rotate(undo ? false : true);
    case HOP_ROT_CCW: return img_rotate(undo ? true : false);
    case HOP_INVERT: img_invert(); return true;
    default: return false;
    }
}

bool hist_op(int op, const char *label) {
    if (g_pend) hist_commit();
    HEntry *e = calloc(1, sizeof *e);
    if (!e) return false;
    e->kind = H_OP;
    e->op = op;
    snprintf(e->label, sizeof e->label, "%s", label);
    if (!apply_op(op, false)) { entry_free(e); return false; }
    push_undo(e);
    return true;
}

bool hist_replace(const char *label, uint32_t *newpx, int nw, int nh) {
    if (g_pend) hist_commit();
    HEntry *e = calloc(1, sizeof *e);
    if (!e || !newpx) { free(e); free(newpx); return false; }
    e->kind = H_SNAPSHOT;
    snprintf(e->label, sizeof e->label, "%s", label);
    e->spx = g_doc.px;
    e->sw = g_doc.w;
    e->sh = g_doc.h;
    g_doc.px = newpx;
    g_doc.w = nw;
    g_doc.h = nh;
    push_undo(e);
    return true;
}

static void snapshot_swap(HEntry *e) {
    uint32_t *px = g_doc.px;
    int w = g_doc.w, h = g_doc.h;
    g_doc.px = e->spx;
    g_doc.w = e->sw;
    g_doc.h = e->sh;
    e->spx = px;
    e->sw = w;
    e->sh = h;
}

static void apply_entry(HEntry *e, bool undo) {
    g_bytes -= entry_bytes(e);
    switch (e->kind) {
    case H_TILES: swap_tiles(e); break;
    case H_SNAPSHOT: snapshot_swap(e); break;
    case H_OP: apply_op(e->op, undo); break;
    }
    g_bytes += entry_bytes(e);
}

bool hist_undo(void) {
    if (g_pend || !g_nundo) return false;
    HEntry *e = g_undo[--g_nundo];
    apply_entry(e, true);
    list_push(&g_redo, &g_nredo, &g_cap_redo, e);
    return true;
}

bool hist_redo(void) {
    if (g_pend || !g_nredo) return false;
    HEntry *e = g_redo[--g_nredo];
    apply_entry(e, false);
    list_push(&g_undo, &g_nundo, &g_cap_undo, e);
    return true;
}

/* ---------------------------------------------------------------- new pixel buffers for resizing and cropping */

uint32_t *img_make_resized(int nw, int nh, uint32_t fill, bool stretch) {
    nw = sk_clamp(nw, 1, SK_MAX_DIM);
    nh = sk_clamp(nh, 1, SK_MAX_DIM);
    uint32_t *np = malloc((size_t)nw * (size_t)nh * 4);
    if (!np) return NULL;
    if (stretch) {
        int *xmap = malloc((size_t)nw * sizeof(int));
        if (!xmap) { free(np); return NULL; }
        for (int x = 0; x < nw; x++) xmap[x] = (int)((long long)x * g_doc.w / nw);
        for (int y = 0; y < nh; y++) {
            const uint32_t *src = g_doc.px + (size_t)((long long)y * g_doc.h / nh) * g_doc.w;
            uint32_t *dst = np + (size_t)y * nw;
            for (int x = 0; x < nw; x++) dst[x] = src[xmap[x]];
        }
        free(xmap);
        return np;
    }
    for (size_t i = 0, n = (size_t)nw * (size_t)nh; i < n; i++) np[i] = fill;
    int cw = sk_min(nw, g_doc.w), ch = sk_min(nh, g_doc.h);
    for (int y = 0; y < ch; y++) memcpy(np + (size_t)y * nw, g_doc.px + (size_t)y * g_doc.w, (size_t)cw * 4);
    return np;
}

uint32_t *img_make_crop(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_doc.w) w = g_doc.w - x;
    if (y + h > g_doc.h) h = g_doc.h - y;
    if (w < 1 || h < 1) return NULL;
    uint32_t *np = malloc((size_t)w * (size_t)h * 4);
    if (!np) return NULL;
    for (int r = 0; r < h; r++) memcpy(np + (size_t)r * w, g_doc.px + (size_t)(y + r) * g_doc.w + x, (size_t)w * 4);
    return np;
}
