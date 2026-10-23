/*
 * Gensokyo Gallery (gallery): a picture viewer for the HakureiDE desktop.
 *
 *   gallery [--trace] [--slideshow SECONDS] [FILE | FOLDER]
 *   gallery --info FILE...       print what imginfo would, without opening a window
 *
 * Opens a picture (or the first picture of a folder) and lets you walk through the folder: Left/Right, PageUp/PageDown,
 * Space/Backspace, Home/End. The wheel zooms around the pointer, dragging pans, double-click switches between "fit" and
 * 100%. R and L rotate, H and V flip, F5 starts a slideshow, W makes the picture the wallpaper, I shows its details,
 * Ctrl+O opens another one. Animated GIFs play. PNG, JPEG, GIF, BMP, PNM and TGA are understood (libimg).
 */
#include <kappa.h>
#include <kappa_media.h>
#include <img.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>

#define MENU_H 24
#define TOOL_H 32
#define STATUS_H 24
#define BG_VIEW 0x14182C
#define MAX_FILES 20000

typedef struct {
    KApp *app;
    char dir[512];
    char **names;
    int nnames, cur;
    char path[700];
    char name[300];
    /* the loaded picture */
    bool have;
    char error[200];
    Image src, shown;                    /* shown = src after rotation / flips; unused (empty) when they are the identity */
    int rot, flip;                       /* the picture is shown flipped horizontally if `flip`, then rotated clockwise by rot quarter turns */
    ImgFormat format;
    long long file_bytes;
    int frames;
    /* animation */
    ImgAnim *anim;
    uint8_t *anim_data;
    uint64_t next_frame_ms;
    int delay_ms;
    /* view */
    int fit;                             /* 1: shrink to fit the window; 0: use zoom */
    int zoom;                            /* percent, when fit is 0 */
    int pan_x, pan_y;                    /* screen pixels from the centred position */
    bool dragging;
    int drag_x, drag_y, pan_x0, pan_y0;
    Image cache;                         /* the picture pre-scaled for a zoom below 100% */
    int cache_w, cache_h;
    bool cache_ok;
    /* loading */
    bool want_load, load_shown;
    /* slideshow */
    bool slideshow;
    int slide_secs;
    uint64_t next_slide_ms;
    /* misc */
    bool info, trace;
    char status[240];
    uint64_t status_until;
    int menu_was_open;
} Gallery;

static Gallery G;

static void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace(const char *fmt, ...) {
    if (!G.trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(G.status, sizeof G.status, fmt, ap);
    va_end(ap);
    G.status_until = k_now_ms() + 6000;
}

/* ---------------------------------------------------------------- the folder */

static bool is_picture_name(const char *n) {
    static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".jpe", ".gif", ".bmp", ".ppm", ".pgm", ".pbm", ".pnm", ".tga" };
    size_t l = strlen(n);
    for (unsigned i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        size_t e = strlen(ext[i]);
        if (l > e && !strcasecmp(n + l - e, ext[i])) return true;
    }
    return false;
}

static int cmp_names(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    while (*x && *y) {                                               /* case-insensitive, digits compared as numbers: pic2 before pic10 */
        if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
            unsigned long long nx = 0, ny = 0;
            while (*x >= '0' && *x <= '9') nx = nx * 10 + (unsigned)(*x++ - '0');
            while (*y >= '0' && *y <= '9') ny = ny * 10 + (unsigned)(*y++ - '0');
            if (nx != ny) return nx < ny ? -1 : 1;
            continue;
        }
        int cx = tolower((unsigned char)*x), cy = tolower((unsigned char)*y);
        if (cx != cy) return cx < cy ? -1 : 1;
        x++; y++;
    }
    return *x ? 1 : *y ? -1 : 0;
}

static void free_names(void) {
    for (int i = 0; i < G.nnames; i++) free(G.names[i]);
    free(G.names);
    G.names = NULL;
    G.nnames = 0;
}

static void scan_dir(const char *dir) {
    free_names();
    snprintf(G.dir, sizeof G.dir, "%s", dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    int cap = 0;
    while ((e = readdir(d)) && G.nnames < MAX_FILES) {
        if (e->d_name[0] == '.' || !is_picture_name(e->d_name)) continue;
        if (G.nnames == cap) { cap = cap ? cap * 2 : 64; G.names = realloc(G.names, (size_t)cap * sizeof *G.names); if (!G.names) { G.nnames = 0; break; } }
        G.names[G.nnames++] = strdup(e->d_name);
    }
    closedir(d);
    if (G.nnames > 1) qsort(G.names, (size_t)G.nnames, sizeof *G.names, cmp_names);
}

static void make_abs(const char *in, char *out, size_t cap) {
    if (in[0] == '/') { snprintf(out, cap, "%s", in); return; }
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    while (in[0] == '.' && in[1] == '/') in += 2;
    snprintf(out, cap, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", in);
}

/* ---------------------------------------------------------------- loading */

static void drop_picture(void) {
    img_free(&G.src);
    img_free(&G.shown);
    img_free(&G.cache);
    if (G.anim) { img_anim_close(G.anim); G.anim = NULL; }
    free(G.anim_data);
    G.anim_data = NULL;
    G.have = false;
    G.cache_ok = false;
    G.frames = 1;
}

static void rebuild_shown(void) {
    img_free(&G.shown);
    img_free(&G.cache);
    G.cache_ok = false;
    if (!G.have || (!G.rot && !G.flip)) return;
    Image a = { 0 }, b = { 0 };
    int have_a = G.flip && img_flip(&G.src, 1, &a) == IMG_OK;
    if (G.rot && img_rotate90(have_a ? &a : &G.src, G.rot, &b) == IMG_OK) { img_free(&a); G.shown = b; return; }
    if (have_a) G.shown = a;
}

static const Image *current_image(void) {
    if (!G.have) return NULL;
    return G.shown.px ? &G.shown : &G.src;
}

static int read_file(const char *path, uint8_t **data, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return IMG_E_IO;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return IMG_E_IO; }
    if (st.st_size > (long long)IMG_MAX_FILE) { close(fd); return IMG_E_TOOBIG; }
    size_t cap = (size_t)st.st_size + 1, n = 0;
    uint8_t *b = malloc(cap);
    if (!b) { close(fd); return IMG_E_NOMEM; }
    for (;;) {
        if (n == cap) { cap *= 2; uint8_t *nb = realloc(b, cap); if (!nb) { free(b); close(fd); return IMG_E_NOMEM; } b = nb; }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { free(b); close(fd); return IMG_E_IO; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    *data = b;
    *len = n;
    return 0;
}

static void reset_view(void) {
    G.fit = 1;
    G.zoom = 100;
    G.pan_x = G.pan_y = 0;
    G.cache_ok = false;
}

static void set_title(void) {
    char t[400];
    if (G.path[0]) snprintf(t, sizeof t, "Gallery - %s", G.name);
    else snprintf(t, sizeof t, "Gallery");
    kapp_set_title(G.app, t);
}

static void do_load(void) {
    drop_picture();
    G.error[0] = 0;
    G.rot = G.flip = 0;
    reset_view();
    uint8_t *data;
    size_t len;
    int rc = read_file(G.path, &data, &len);
    if (rc) { snprintf(G.error, sizeof G.error, "%s", img_strerror(rc)); trace("error %s: %s", G.path, G.error); set_title(); return; }
    G.file_bytes = (long long)len;
    G.format = img_sniff(data, len);
    ImgInfo info;
    if (G.format == IMG_GIF && img_probe(data, len, &info) == IMG_OK && info.frames > 1) {
        ImgAnim *a;
        rc = img_anim_open(data, len, &a);
        if (rc == IMG_OK) {
            const Image *first;
            int delay;
            rc = img_anim_next(a, &first, &delay);
            if (rc == IMG_OK && img_alloc(&G.src, first->w, first->h) == IMG_OK) {
                memcpy(G.src.px, first->px, (size_t)first->w * first->h * 4);
                G.src.has_alpha = first->has_alpha;
                G.anim = a;
                G.anim_data = data;                                  /* the animation reads from this buffer for as long as it lives */
                G.frames = info.frames;
                G.delay_ms = delay;
                G.next_frame_ms = k_now_ms() + (uint64_t)delay;
                G.have = true;
                data = NULL;
            } else {
                if (rc == IMG_OK) rc = IMG_E_NOMEM;
                img_anim_close(a);
            }
        }
    }
    if (!G.have) {
        if (data) {
            rc = img_decode(data, len, &G.src);
            free(data);
        }
        if (rc) { snprintf(G.error, sizeof G.error, "%s", img_strerror(rc)); trace("error %s: %s", G.path, G.error); set_title(); return; }
        G.have = true;
        G.frames = 1;
    }
    trace("open %s %s %dx%d", G.path, img_format_name(G.format), G.src.w, G.src.h);
    set_title();
}

static void request_load(const char *path) {
    snprintf(G.path, sizeof G.path, "%s", path);
    const char *b = strrchr(path, '/');
    snprintf(G.name, sizeof G.name, "%s", b ? b + 1 : path);
    G.want_load = true;
    G.load_shown = false;
    set_title();
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size < 600 * 1024) { do_load(); G.want_load = false; }    /* small: no "loading" frame needed */
}

static void open_index(int idx) {
    if (G.nnames == 0) return;
    G.cur = ((idx % G.nnames) + G.nnames) % G.nnames;
    char p[900];
    snprintf(p, sizeof p, "%s%s%s", G.dir, strcmp(G.dir, "/") ? "/" : "", G.names[G.cur]);
    request_load(p);
}

static void open_target(const char *arg) {
    char full[700];
    make_abs(arg, full, sizeof full);
    struct stat st;
    if (stat(full, &st) < 0) { snprintf(G.error, sizeof G.error, "%s: %s", full, strerror(errno)); G.path[0] = 0; set_title(); return; }
    if (S_ISDIR(st.st_mode)) {
        size_t l = strlen(full);
        while (l > 1 && full[l - 1] == '/') full[--l] = 0;
        scan_dir(full);
        if (G.nnames == 0) { drop_picture(); G.path[0] = 0; snprintf(G.error, sizeof G.error, "There are no pictures in %s.", full); set_title(); return; }
        open_index(0);
        return;
    }
    char dir[700];
    snprintf(dir, sizeof dir, "%s", full);
    char *slash = strrchr(dir, '/');
    if (slash == dir) dir[1] = 0; else if (slash) *slash = 0;
    scan_dir(dir);
    const char *base = strrchr(full, '/') + 1;
    int idx = -1;
    for (int i = 0; i < G.nnames; i++) if (!strcmp(G.names[i], base)) idx = i;
    if (idx < 0) {                                                   /* a picture whose name does not look like one */
        G.names = realloc(G.names, (size_t)(G.nnames + 1) * sizeof *G.names);
        G.names[G.nnames] = strdup(base);
        idx = G.nnames++;
    }
    open_index(idx);
}

/* ---------------------------------------------------------------- the view */

/* Where the picture goes in the view rectangle, and at what scale (percent * 100 in `pct`). */
static void place(const Image *im, KRect view, int *dw, int *dh, int *x0, int *y0, long long *pct) {
    long long p;
    if (G.fit) {
        long long px = (long long)view.w * 10000 / im->w, py = (long long)view.h * 10000 / im->h;
        p = px < py ? px : py;
        if (p > 10000) p = 10000;
        if (p < 100) p = 100;
    } else p = (long long)G.zoom * 100;
    *pct = p;
    *dw = (int)((long long)im->w * p / 10000);
    *dh = (int)((long long)im->h * p / 10000);
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
    int maxpx = *dw > view.w ? (*dw - view.w) / 2 + 40 : 0, maxpy = *dh > view.h ? (*dh - view.h) / 2 + 40 : 0;
    if (G.pan_x > maxpx) G.pan_x = maxpx;
    if (G.pan_x < -maxpx) G.pan_x = -maxpx;
    if (G.pan_y > maxpy) G.pan_y = maxpy;
    if (G.pan_y < -maxpy) G.pan_y = -maxpy;
    *x0 = view.x + (view.w - *dw) / 2 + G.pan_x;
    *y0 = view.y + (view.h - *dh) / 2 + G.pan_y;
}

static uint32_t checker(int x, int y) { return ((x >> 3) + (y >> 3)) & 1 ? 0x9A9AA6 : 0xC8C8D2; }

static void draw_picture(KCanvas *cv, const Image *im, KRect view) {
    int dw, dh, x0, y0;
    long long pct;
    place(im, view, &dw, &dh, &x0, &y0, &pct);
    kc_fill(cv, view, BG_VIEW);
    KRect old = cv->clip;
    kc_set_clip(cv, view);
    const uint32_t *src = im->px;
    int sw = im->w, sh = im->h, sstride = im->w;
    int ox = x0, oy = y0;                                            /* screen position of src (0,0) at scale 1:1 after any pre-scaling */
    long long mag = pct;                                             /* >100 * 100: enlarge by nearest neighbour */
    if (pct < 10000) {                                               /* shrink once with an area average, then copy */
        if (!G.cache_ok || G.cache_w != dw || G.cache_h != dh) {
            img_free(&G.cache);
            if (img_scale(im, dw, dh, IMG_AREA, &G.cache) == IMG_OK) { G.cache_ok = true; G.cache_w = dw; G.cache_h = dh; }
            else G.cache_ok = false;
        }
        if (G.cache_ok) { src = G.cache.px; sw = G.cache.w; sh = G.cache.h; sstride = G.cache.w; mag = 10000; }
    }
    int has_alpha = im->has_alpha;
    int vx0 = view.x > cv->clip.x ? view.x : cv->clip.x;
    for (int y = view.y; y < view.y + view.h; y++) {
        long long sy = mag == 10000 ? y - oy : ((long long)(y - oy) * 10000) / mag;
        if (sy < 0 || sy >= sh || y < 0 || y >= cv->h) continue;
        uint32_t *dst = cv->px + (size_t)y * cv->stride;
        const uint32_t *row = src + (size_t)sy * sstride;
        for (int x = vx0; x < view.x + view.w && x < cv->w; x++) {
            long long sx = mag == 10000 ? x - ox : ((long long)(x - ox) * 10000) / mag;
            if (sx < 0 || sx >= sw) continue;
            uint32_t p = row[sx];
            if (has_alpha && (p >> 24) != 255) {
                uint32_t a = p >> 24, bgc = checker(x, y);
                uint32_t r = (((p >> 16) & 255) * a + ((bgc >> 16) & 255) * (255 - a)) / 255;
                uint32_t g = (((p >> 8) & 255) * a + ((bgc >> 8) & 255) * (255 - a)) / 255;
                uint32_t b = ((p & 255) * a + (bgc & 255) * (255 - a)) / 255;
                p = r << 16 | g << 8 | b;
            }
            dst[x] = p & 0x00FFFFFFu;
        }
    }
    cv->clip = old;
}

static void zoom_to(int percent, int anchor_x, int anchor_y, KRect view) {
    const Image *im = current_image();
    if (!im) return;
    int dw, dh, x0, y0;
    long long pct;
    place(im, view, &dw, &dh, &x0, &y0, &pct);
    static const int steps[] = { 5, 10, 16, 25, 33, 50, 66, 75, 100, 150, 200, 300, 400, 600, 800, 1200, 1600, 3200 };
    if (percent < steps[0]) percent = steps[0];
    if (percent > steps[sizeof steps / sizeof steps[0] - 1]) percent = steps[sizeof steps / sizeof steps[0] - 1];
    /* keep the picture point under the anchor where it is */
    double fx = dw ? (double)(anchor_x - x0) / dw : 0.5, fy = dh ? (double)(anchor_y - y0) / dh : 0.5;
    G.fit = 0;
    G.zoom = percent;
    int ndw = (int)((long long)im->w * percent / 100), ndh = (int)((long long)im->h * percent / 100);
    int nx0 = anchor_x - (int)(fx * ndw), ny0 = anchor_y - (int)(fy * ndh);
    G.pan_x = nx0 - (view.x + (view.w - ndw) / 2);
    G.pan_y = ny0 - (view.y + (view.h - ndh) / 2);
    G.cache_ok = false;
    trace("zoom %d", percent);
}

static int next_step(int cur, int dir) {
    static const int steps[] = { 5, 10, 16, 25, 33, 50, 66, 75, 100, 150, 200, 300, 400, 600, 800, 1200, 1600, 3200 };
    int n = (int)(sizeof steps / sizeof steps[0]);
    if (dir > 0) { for (int i = 0; i < n; i++) if (steps[i] > cur) return steps[i]; return steps[n - 1]; }
    for (int i = n - 1; i >= 0; i--) if (steps[i] < cur) return steps[i];
    return steps[0];
}

static int effective_zoom(KRect view) {
    const Image *im = current_image();
    if (!im) return 100;
    int dw, dh, x0, y0;
    long long pct;
    place(im, view, &dw, &dh, &x0, &y0, &pct);
    return (int)(pct / 100);
}

/* ---------------------------------------------------------------- commands */

static void cmd_open_dialog(void);

static void cmd_goto(int idx) {
    if (G.nnames == 0) return;
    trace("goto %d", ((idx % G.nnames) + G.nnames) % G.nnames);
    open_index(idx);
}

static void cmd_rotate(int turns) {
    if (!G.have) return;
    G.rot = ((G.rot + turns) % 4 + 4) % 4;
    rebuild_shown();
    reset_view();
    trace("rotate %d", G.rot * 90);
}

/* Flips act on what is on the screen: after a flip the later rotation runs the other way, and a vertical flip is a horizontal
 * flip plus a half turn. */
static void cmd_flip(int horizontal) {
    if (!G.have) return;
    G.flip = !G.flip;
    G.rot = (4 - G.rot) % 4;
    if (!horizontal) G.rot = (G.rot + 2) % 4;
    rebuild_shown();
    reset_view();
    trace("flip %s", horizontal ? "horizontal" : "vertical");
}

static void cmd_wallpaper(void) {
    if (!G.have || !G.path[0]) return;
    int rc = k_wallpaper_set(G.path, KW_COVER);
    if (rc) say("Could not set the wallpaper: %s", strerror(-rc));
    else say("Wallpaper: %s (the desktop changes it within a moment)", G.name);
    trace("wallpaper %s", G.path);
}

static void cmd_slideshow(void) {
    G.slideshow = !G.slideshow;
    G.next_slide_ms = k_now_ms() + (uint64_t)G.slide_secs * 1000;
    say(G.slideshow ? "Slideshow: a new picture every %d seconds (Esc stops it)" : "Slideshow stopped", G.slide_secs);
    trace("slideshow %s", G.slideshow ? "on" : "off");
}

static void on_picked(const char *path, void *user) {
    (void)user;
    if (path) open_target(path);
}

static void cmd_open_dialog(void) {
    k_file_dialog(G.app, 0, "Open picture", G.dir[0] ? G.dir : (getenv("HOME") ? getenv("HOME") : "/"), NULL, on_picked, NULL);
}

static void cmd_about(void) {
    const char *b[] = { "OK" };
    k_messagebox(kapp_display(G.app), "About Gallery",
                 "Gensokyo Gallery\n\nAya's photographs and everything else: PNG, JPEG, GIF, BMP, PNM and TGA pictures, one folder at a time.\n\nPress F1 for the keys.",
                 KI_IMAGE, b, 1);
}

static void cmd_keys(void) {
    const char *b[] = { "OK" };
    k_messagebox(kapp_display(G.app), "Gallery keys",
                 "Left / Right, PageUp / PageDown, Backspace / Space: previous / next picture\nHome / End: first / last\n"
                 "+ / - or the wheel: zoom;  0 or F: fit;  1: 100%;  2..4: 200% to 400%\nDrag: pan;  double-click: fit / 100%\n"
                 "R / L: rotate;  H / V: flip\nF5 or S: slideshow;  Esc stops it\nW: use as wallpaper;  I: details\nCtrl+O: open;  Ctrl+Q: quit",
                 KI_INFO, b, 1);
}

/* ---------------------------------------------------------------- drawing */

static void draw_text_centered(KCanvas *cv, KRect r, const char *s, uint32_t col) {
    kc_text_in(cv, r, s, col, KA_CENTER);
}

static void format_size(long long b, char *out, size_t n) {
    if (b < 1024) snprintf(out, n, "%lld bytes", b);
    else if (b < 1024 * 1024) snprintf(out, n, "%.1f KiB", (double)b / 1024.0);
    else snprintf(out, n, "%.1f MiB", (double)b / (1024.0 * 1024.0));
}

static void draw_info(KUI *ui, KRect view) {
    KCanvas *cv = &ui->cv;
    const Image *im = current_image();
    if (!im) return;
    char lines[8][400];
    char sz[48];
    format_size(G.file_bytes, sz, sizeof sz);
    snprintf(lines[0], sizeof lines[0], "%s", G.name);
    snprintf(lines[1], sizeof lines[1], "%s, %d x %d pixels%s", img_format_name(G.format), G.src.w, G.src.h, G.src.has_alpha ? ", transparency" : "");
    snprintf(lines[2], sizeof lines[2], "%s%s", sz, G.frames > 1 ? ", animated" : "");
    snprintf(lines[3], sizeof lines[3], "Picture %d of %d in %s", G.cur + 1, G.nnames, G.dir);
    snprintf(lines[4], sizeof lines[4], "Zoom %d%%%s", effective_zoom(view), G.fit ? " (fits the window)" : "");
    int n = 5, w = 0;
    for (int i = 0; i < n; i++) { int tw = kc_text_width(lines[i]); if (tw > w) w = tw; }
    KRect box = kr(view.x + 12, view.y + 12, w + 24, n * 18 + 16);
    kc_fill_alpha(cv, box, 0x05081A, 200);
    kc_outline(cv, box, KC_GOLD);
    for (int i = 0; i < n; i++) kc_text(cv, box.x + 12, box.y + 8 + i * 18, lines[i], i == 0 ? 0xFFE9A8 : 0xDDE3F8);
}

static void draw_toolbar(KUI *ui, KRect r, KRect view) {
    KCanvas *cv = &ui->cv;
    kc_gradient_v(cv, r, 0xD6CFBB, 0xC4BCA6);
    kc_hline(cv, r.x, r.y + r.h - 1, r.w, 0x8E8672);
    int x = r.x + 6, y = r.y + 3, h = r.h - 6;
    struct { const char *label; const char *tip; int id; } b[] = {
        { "Open", "Open a picture or folder (Ctrl+O)", 1 }, { "<", "Previous picture (Left)", 2 }, { ">", "Next picture (Right)", 3 },
        { "Fit", "Fit to window (0)", 4 }, { "100%", "Actual size (1)", 5 }, { "-", "Zoom out (-)", 6 }, { "+", "Zoom in (+)", 7 },
        { "Rot L", "Rotate left (L)", 8 }, { "Rot R", "Rotate right (R)", 9 }, { "Flip", "Flip horizontally (H)", 10 },
        { "Show", "Slideshow (F5)", 11 }, { "Wall", "Use as wallpaper (W)", 12 },
    };
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) {
        int w = kc_text_width(b[i].label) + 16;
        if (w < 28) w = 28;
        if (x + w > r.x + r.w - 4) break;
        KRect br = kr(x, y, w, h);
        bool sel = (b[i].id == 11 && G.slideshow) || (b[i].id == 4 && G.fit) || (b[i].id == 5 && !G.fit && G.zoom == 100);
        if (k_button_flat(ui, br, b[i].label, sel)) {
            switch (b[i].id) {
            case 1: cmd_open_dialog(); break;
            case 2: cmd_goto(G.cur - 1); break;
            case 3: cmd_goto(G.cur + 1); break;
            case 4: reset_view(); trace("fit"); break;
            case 5: zoom_to(100, view.x + view.w / 2, view.y + view.h / 2, view); break;
            case 6: zoom_to(next_step(effective_zoom(view), -1), view.x + view.w / 2, view.y + view.h / 2, view); break;
            case 7: zoom_to(next_step(effective_zoom(view), 1), view.x + view.w / 2, view.y + view.h / 2, view); break;
            case 8: cmd_rotate(3); break;
            case 9: cmd_rotate(1); break;
            case 10: cmd_flip(1); break;
            case 11: cmd_slideshow(); break;
            case 12: cmd_wallpaper(); break;
            }
        }
        if (k_hover(ui, br)) k_tooltip_text(ui, b[i].tip);
        x += w + 3;
        if (i == 0 || i == 3 || i == 6 || i == 9) { kc_vline(cv, x + 1, y + 2, h - 4, 0x8E8672); x += 6; }
    }
}

static void draw_status(KUI *ui, KRect r, KRect view) {
    KCanvas *cv = &ui->cv;
    kc_panel(cv, r, KB_RAISED, ui->theme->window_bg);
    char text[420];
    if (G.status[0] && ui->now_ms < G.status_until) snprintf(text, sizeof text, "%s", G.status);
    else if (G.have) {
        char sz[48];
        format_size(G.file_bytes, sz, sizeof sz);
        snprintf(text, sizeof text, "%s   %d x %d   %s   %s   %d / %d   %d%%%s", G.name, G.src.w, G.src.h, img_format_name(G.format), sz,
                 G.cur + 1, G.nnames, effective_zoom(view), G.slideshow ? "   slideshow" : "");
    } else snprintf(text, sizeof text, "%s", G.error[0] ? G.error : "Open a picture with Ctrl+O");
    kc_text_in(cv, kr(r.x + 8, r.y, r.w - 16, r.h), text, ui->theme->text, KA_LEFT);
}

static void run_menubar(KUI *ui, int W, KRect view) {
    static const char *const titles[] = { "File", "View", "Image", "Go", "Help" };
    KMenuItem file[] = {
        { "Open...", 0, false, false, "Ctrl+O" }, { "Set as Wallpaper", 0, false, !G.have, "W" }, { "", 0, true, false, NULL }, { "Quit", 0, false, false, "Ctrl+Q" },
    };
    KMenuItem viewm[] = {
        { "Fit to Window", 0, false, !G.have, "0" }, { "Actual Size", 0, false, !G.have, "1" }, { "Zoom In", 0, false, !G.have, "+" }, { "Zoom Out", 0, false, !G.have, "-" },
        { "", 0, true, false, NULL }, { G.slideshow ? "Stop Slideshow" : "Start Slideshow", 0, false, G.nnames < 2, "F5" }, { G.info ? "Hide Details" : "Show Details", 0, false, false, "I" },
    };
    KMenuItem image[] = {
        { "Rotate Right", 0, false, !G.have, "R" }, { "Rotate Left", 0, false, !G.have, "L" }, { "Flip Horizontally", 0, false, !G.have, "H" }, { "Flip Vertically", 0, false, !G.have, "V" },
    };
    KMenuItem go[] = {
        { "Next Picture", 0, false, G.nnames < 2, "Right" }, { "Previous Picture", 0, false, G.nnames < 2, "Left" },
        { "First Picture", 0, false, G.nnames < 2, "Home" }, { "Last Picture", 0, false, G.nnames < 2, "End" },
    };
    KMenuItem help[] = { { "Keyboard Shortcuts", 0, false, false, "F1" }, { "About Gallery", 0, false, false, NULL } };
    const KMenuItem *menus[] = { file, viewm, image, go, help };
    int counts[] = { 4, 7, 4, 4, 2 };
    int r = k_menubar(ui, kr(0, 0, W, MENU_H), titles, menus, counts, 5);
    if (G.menu_was_open && !ui->menu_open) kapp_redraw(G.app);
    G.menu_was_open = ui->menu_open;
    if (!r) return;
    kapp_redraw(G.app);
    int m = (r >> 8) - 1, it = (r & 255) - 1;
    int cx = view.x + view.w / 2, cy = view.y + view.h / 2;
    switch (m) {
    case 0: if (it == 0) cmd_open_dialog(); else if (it == 1) cmd_wallpaper(); else if (it == 3) kapp_quit(G.app, 0); break;
    case 1:
        if (it == 0) { reset_view(); trace("fit"); }
        else if (it == 1) zoom_to(100, cx, cy, view);
        else if (it == 2) zoom_to(next_step(effective_zoom(view), 1), cx, cy, view);
        else if (it == 3) zoom_to(next_step(effective_zoom(view), -1), cx, cy, view);
        else if (it == 5) cmd_slideshow();
        else if (it == 6) G.info = !G.info;
        break;
    case 2: if (it == 0) cmd_rotate(1); else if (it == 1) cmd_rotate(3); else if (it == 2) cmd_flip(1); else if (it == 3) cmd_flip(0); break;
    case 3: if (it == 0) cmd_goto(G.cur + 1); else if (it == 1) cmd_goto(G.cur - 1); else if (it == 2) cmd_goto(0); else if (it == 3) cmd_goto(G.nnames - 1); break;
    case 4: if (it == 0) cmd_keys(); else cmd_about(); break;
    }
}

/* ---------------------------------------------------------------- input */

static void handle_keys(KUI *ui, KRect view) {
    if (ui->menu_open) return;
    int cx = view.x + view.w / 2, cy = view.y + view.h / 2;
    for (int i = 0; i < ui->nkeys; i++) {
        KKey *k = &ui->keys[i];
        if (!k->down || !k->code) continue;
        bool ctrl = (k->mods & MOD_CTRL) != 0, alt = (k->mods & MOD_ALT) != 0, shift = (k->mods & MOD_SHIFT) != 0;
        bool used = true;
        if (alt) continue;
        if (ctrl) {
            switch (k->code) {
            case KEY_O: cmd_open_dialog(); break;
            case KEY_Q: case KEY_W: kapp_quit(G.app, 0); break;
            case KEY_EQUAL: zoom_to(next_step(effective_zoom(view), 1), cx, cy, view); break;
            case KEY_MINUS: zoom_to(next_step(effective_zoom(view), -1), cx, cy, view); break;
            case KEY_0: reset_view(); break;
            default: used = false; break;
            }
        } else switch (k->code) {
            case KEY_RIGHT: case KEY_PAGEDOWN: case KEY_SPACE: cmd_goto(G.cur + 1); break;
            case KEY_LEFT: case KEY_PAGEUP: case KEY_BACKSPACE: cmd_goto(G.cur - 1); break;
            case KEY_HOME: cmd_goto(0); break;
            case KEY_END: cmd_goto(G.nnames - 1); break;
            case KEY_F1: cmd_keys(); break;
            case KEY_F5: cmd_slideshow(); break;
            case KEY_ESC: if (G.slideshow) cmd_slideshow(); else kapp_quit(G.app, 0); break;
            case KEY_UP: G.pan_y += 40; break;
            case KEY_DOWN: G.pan_y -= 40; break;
            default:
                if (k->ch == '+' || k->ch == '=') zoom_to(next_step(effective_zoom(view), 1), cx, cy, view);
                else if (k->ch == '-' || k->code == KEY_KPMINUS) zoom_to(next_step(effective_zoom(view), -1), cx, cy, view);
                else if (k->ch == '0' || k->ch == 'f' || k->ch == 'F') { reset_view(); trace("fit"); }
                else if (k->ch == '1') zoom_to(100, cx, cy, view);
                else if (k->ch == '2') zoom_to(200, cx, cy, view);
                else if (k->ch == '3') zoom_to(300, cx, cy, view);
                else if (k->ch == '4') zoom_to(400, cx, cy, view);
                else if (k->ch == 'r' || k->ch == 'R') cmd_rotate(shift ? 3 : 1);
                else if (k->ch == 'l' || k->ch == 'L') cmd_rotate(3);
                else if (k->ch == 'h' || k->ch == 'H') cmd_flip(1);
                else if (k->ch == 'v' || k->ch == 'V') cmd_flip(0);
                else if (k->ch == 's' || k->ch == 'S') cmd_slideshow();
                else if (k->ch == 'w' || k->ch == 'W') cmd_wallpaper();
                else if (k->ch == 'i' || k->ch == 'I') G.info = !G.info;
                else used = false;
                break;
            }
        if (used) { k->down = 0; k->code = 0; kapp_redraw(G.app); }
    }
}

static void handle_pointer(KUI *ui, KRect view) {
    if (ui->menu_open) { G.dragging = false; return; }
    bool inside = kr_contains(view, ui->mx, ui->my);
    if (ui->wheel && inside && G.have) {
        int steps = ui->wheel > 0 ? 1 : -1;
        zoom_to(next_step(effective_zoom(view), steps), ui->mx, ui->my, view);
    }
    if (ui->pressed[0] && inside) {
        if (ui->double_click) {
            if (G.fit) zoom_to(100, ui->mx, ui->my, view); else reset_view();
            G.dragging = false;
        } else {
            G.dragging = true;
            G.drag_x = ui->mx; G.drag_y = ui->my;
            G.pan_x0 = G.pan_x; G.pan_y0 = G.pan_y;
        }
    }
    if (G.dragging) {
        if (!(ui->buttons & 1)) G.dragging = false;
        else { G.pan_x = G.pan_x0 + ui->mx - G.drag_x; G.pan_y = G.pan_y0 + ui->my - G.drag_y; ui->cursor_shape = FC_MOVE; }
    }
}

static void advance_timers(KUI *ui) {
    uint64_t now = ui->now_ms;
    if (G.anim && now >= G.next_frame_ms) {
        const Image *fr;
        int delay;
        if (img_anim_next(G.anim, &fr, &delay) == IMG_OK && fr->w == G.src.w && fr->h == G.src.h) {
            memcpy(G.src.px, fr->px, (size_t)fr->w * fr->h * 4);
            G.src.has_alpha = fr->has_alpha;
            if (G.rot || G.flip) rebuild_shown();
            G.cache_ok = false;
            G.delay_ms = delay;
            G.next_frame_ms = now + (uint64_t)delay;
        } else G.next_frame_ms = now + 1000;
    }
    if (G.slideshow && now >= G.next_slide_ms && G.nnames > 1 && !G.want_load) {
        G.next_slide_ms = now + (uint64_t)G.slide_secs * 1000;
        cmd_goto(G.cur + 1);
    }
    if (G.anim || G.slideshow) {
        kapp_set_tick(G.app, G.anim ? 20 : 250);
        ui->redraw_soon = true;
    } else kapp_set_tick(G.app, 1000);
}

static void draw(KApp *app, KUI *ui, void *user) {
    (void)app; (void)user;
    KCanvas *cv = &ui->cv;
    int W = cv->w, H = cv->h;
    KRect view = kr(0, MENU_H + TOOL_H, W, H - MENU_H - TOOL_H - STATUS_H);
    k_clear(ui);
    if (G.want_load) {
        if (!G.load_shown) {
            kc_fill(cv, view, BG_VIEW);
            draw_text_centered(cv, view, "Loading...", 0xDDE3F8);
            draw_toolbar(ui, kr(0, MENU_H, W, TOOL_H), view);
            draw_status(ui, kr(0, H - STATUS_H, W, STATUS_H), view);
            G.load_shown = true;
            kapp_redraw(G.app);
            return;
        }
        do_load();
        G.want_load = false;
        G.load_shown = false;
    }
    advance_timers(ui);
    handle_keys(ui, view);
    handle_pointer(ui, view);
    const Image *im = current_image();
    if (im) draw_picture(cv, im, view);
    else {
        kc_fill(cv, view, BG_VIEW);
        k_icon(cv, KI_IMAGE, view.x + view.w / 2 - 24, view.y + view.h / 2 - 50, 48);
        draw_text_centered(cv, kr(view.x, view.y + view.h / 2 + 8, view.w, 20), G.error[0] ? G.error : "No picture open", 0xDDE3F8);
        if (!G.error[0]) draw_text_centered(cv, kr(view.x, view.y + view.h / 2 + 30, view.w, 20), "Press Ctrl+O to choose one", 0x9AA3D0);
    }
    if (G.info) draw_info(ui, view);
    draw_toolbar(ui, kr(0, MENU_H, W, TOOL_H), view);
    draw_status(ui, kr(0, H - STATUS_H, W, STATUS_H), view);
    run_menubar(ui, W, view);
}

/* ---------------------------------------------------------------- start */

static int text_info(int argc, char **argv) {
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        ImgInfo info;
        int r = img_probe_file(argv[i], &info);
        if (r) { printf("%s: error: %s\n", argv[i], img_strerror(r)); rc = 1; continue; }
        printf("%s: %s %dx%d %s %d frame%s\n", argv[i], img_format_name(info.format), info.width, info.height, info.has_alpha ? "alpha" : "opaque", info.frames, info.frames == 1 ? "" : "s");
    }
    return rc;
}

int main(int argc, char **argv) {
    memset(&G, 0, sizeof G);
    G.slide_secs = 4;
    G.fit = 1;
    G.zoom = 100;
    G.frames = 1;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) G.trace = true;
        else if (!strcmp(argv[i], "--info")) return text_info(argc - i - 1, argv + i + 1);
        else if (!strcmp(argv[i], "--slideshow") && i + 1 < argc) { G.slideshow = true; G.slide_secs = atoi(argv[++i]); if (G.slide_secs < 1) G.slide_secs = 1; }
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "usage: gallery [--trace] [--slideshow SECONDS] [FILE | FOLDER]\n       gallery --info FILE...\n"); return 2; }
        else target = argv[i];
    }
    KApp *app = kapp_new("Gallery", 800, 600, FW_RESIZABLE);
    if (!app) return 1;
    G.app = app;
    kapp_set_draw(app, draw, NULL);
    kapp_set_minsize(app, 520, 340);
    kapp_set_tick(app, 1000);
    if (target) open_target(target);
    else {
        const char *home = getenv("HOME");
        if (home) { char p[600]; snprintf(p, sizeof p, "%s/Pictures", home); struct stat st; if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) scan_dir(p); }
    }
    if (G.slideshow) G.next_slide_ms = k_now_ms() + (uint64_t)G.slide_secs * 1000;
    set_title();
    int rc = kapp_run(app);
    drop_picture();
    free_names();
    kapp_destroy(app);
    return rc;
}
