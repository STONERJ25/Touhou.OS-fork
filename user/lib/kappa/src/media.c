/* Wallpaper rendering and picture thumbnails, on top of libimg. */
#include <kappa_media.h>
#include <img.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- wallpaper */

static void fill(uint32_t *px, int w, int h, int stride, uint32_t c) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[(size_t)y * stride + x] = c;
}

/* Copies the part of `src` that falls inside the screen when src's top-left corner is at (ox, oy). */
static void blit(uint32_t *px, int w, int h, int stride, const Image *src, int ox, int oy) {
    for (int y = 0; y < src->h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= h) continue;
        for (int x = 0; x < src->w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= w) continue;
            px[(size_t)dy * stride + dx] = src->px[(size_t)y * src->w + x] & 0x00FFFFFFu;
        }
    }
}

static int shrink_or_grow(const Image *im, int tw, int th, Image *out) {
    return img_scale(im, tw, th, (tw < im->w || th < im->h) ? IMG_AREA : IMG_BILINEAR, out);
}

/* w, h: the size in logical pixels; px holds (w * scale) x (h * scale) pixels. A picture is scaled to the whole buffer (all its
 * pixels are used: center and tile show it pixel for pixel on the screen), the night scene is drawn on a scaled canvas so it
 * keeps its proportions. */
void k_wallpaper_render_scaled(uint32_t *px, int lw, int lh, int stride, int scale) {
    if (scale < 1) scale = 1;
    int w = lw * scale, h = lh * scale;
    KWallConfig cfg;
    Image im, sc;
    if (!k_wallpaper_config(&cfg) || img_load(cfg.path, &im) != IMG_OK) { k_scenery_night_scaled(px, lw, lh, stride, scale); return; }
    img_composite_over(&im, KC_NIGHT);
    fill(px, w, h, stride, KC_NIGHT);
    int rc = 0;
    switch (cfg.mode) {
    case KW_STRETCH:
        rc = shrink_or_grow(&im, w, h, &sc);
        if (!rc) { blit(px, w, h, stride, &sc, 0, 0); img_free(&sc); }
        break;
    case KW_FIT: {
        int tw, th;
        img_fit(im.w, im.h, w, h, 1, &tw, &th);
        rc = shrink_or_grow(&im, tw, th, &sc);
        if (!rc) { blit(px, w, h, stride, &sc, (w - tw) / 2, (h - th) / 2); img_free(&sc); }
        break;
    }
    case KW_CENTER:
        blit(px, w, h, stride, &im, (w - im.w) / 2, (h - im.h) / 2);
        break;
    case KW_TILE:
        for (int ty = 0; ty < h; ty += im.h)
            for (int tx = 0; tx < w; tx += im.w) blit(px, w, h, stride, &im, tx, ty);
        break;
    default: {                                                     /* cover: scale until both sides are covered, crop the middle */
        long long tw, th;
        if ((long long)im.w * h > (long long)im.h * w) { th = h; tw = ((long long)im.w * h + im.h - 1) / im.h; }
        else { tw = w; th = ((long long)im.h * w + im.w - 1) / im.w; }
        if (tw < w) tw = w;
        if (th < h) th = h;
        rc = shrink_or_grow(&im, (int)tw, (int)th, &sc);
        if (!rc) { blit(px, w, h, stride, &sc, (int)(w - tw) / 2, (int)(h - th) / 2); img_free(&sc); }
        break;
    }
    }
    img_free(&im);
    if (rc) k_scenery_night_scaled(px, lw, lh, stride, scale);
}

void k_wallpaper_render(uint32_t *px, int w, int h, int stride) { k_wallpaper_render_scaled(px, w, h, stride, 1); }

/* ---------------------------------------------------------------- thumbnails */

#define THUMB_SLOTS 32
#define THUMB_MAX_BYTES (8u << 20)
#define THUMB_MAX_PIXELS (24u << 20)

typedef struct {
    char path[256];
    int size, w, h, failed;
    long long mtime, fsize;
    uint32_t *px;
    unsigned long used;
} Thumb;

static Thumb g_thumbs[THUMB_SLOTS];
static unsigned long g_clock;

static Thumb *lookup(const char *path, int size, const struct stat *st) {
    for (int i = 0; i < THUMB_SLOTS; i++) {
        Thumb *t = &g_thumbs[i];
        if ((t->px || t->failed) && t->size == size && t->mtime == (long long)st->st_mtime && t->fsize == (long long)st->st_size && !strcmp(t->path, path)) {
            t->used = ++g_clock;
            return t;
        }
    }
    return NULL;
}

static bool give(Thumb *t, KCanvas *out) {
    if (t->failed || !t->px) return false;
    kc_init(out, t->px, t->w, t->h, t->w);
    return true;
}

bool k_image_thumbnail_cached(const char *path, int size, KCanvas *out) {
    struct stat st;
    if (stat(path, &st) < 0) return false;
    Thumb *t = lookup(path, size, &st);
    return t ? give(t, out) : false;
}

bool k_image_thumbnail(const char *path, int size, KCanvas *out) {
    struct stat st;
    if (size < 8 || size > 512 || strlen(path) >= sizeof g_thumbs[0].path || stat(path, &st) < 0 || !S_ISREG(st.st_mode)) return false;
    Thumb *t = lookup(path, size, &st);
    if (t) return give(t, out);
    Thumb *slot = &g_thumbs[0];
    for (int i = 0; i < THUMB_SLOTS; i++) {
        if (!g_thumbs[i].px && !g_thumbs[i].failed) { slot = &g_thumbs[i]; break; }
        if (g_thumbs[i].used < slot->used) slot = &g_thumbs[i];
    }
    free(slot->px);
    memset(slot, 0, sizeof *slot);
    strcpy(slot->path, path);
    slot->size = size;
    slot->mtime = (long long)st.st_mtime;
    slot->fsize = (long long)st.st_size;
    slot->used = ++g_clock;
    slot->failed = 1;
    ImgInfo info;
    if (st.st_size > (long long)THUMB_MAX_BYTES || img_probe_file(path, &info) != IMG_OK || (long long)info.width * info.height > (long long)THUMB_MAX_PIXELS) return false;
    Image im, sm;
    if (img_load(path, &im) != IMG_OK) return false;
    img_composite_over(&im, KC_PAPER);
    int tw, th;
    img_fit(im.w, im.h, size, size, 0, &tw, &th);
    if (img_scale(&im, tw, th, tw < im.w || th < im.h ? IMG_AREA : IMG_NEAREST, &sm) != IMG_OK) { img_free(&im); return false; }
    img_free(&im);
    slot->px = sm.px;                                              /* the scaled pixels become the cache entry */
    slot->w = sm.w;
    slot->h = sm.h;
    slot->failed = 0;
    return give(slot, out);
}

void k_image_thumbnail_flush(void) {
    for (int i = 0; i < THUMB_SLOTS; i++) { free(g_thumbs[i].px); memset(&g_thumbs[i], 0, sizeof g_thumbs[i]); }
}
