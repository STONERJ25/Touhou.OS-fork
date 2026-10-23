/* Picture tools: imginfo convert wallpaper screenshot. */
#include "kappabox.h"
#include <img.h>
#include <z.h>
#include <fairy.h>
#include <kappa_media.h>

/* ---------------------------------------------------------------- imginfo */

static int read_whole(const char *path, uint8_t **data, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t cap = 65536, n = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (!b) { close(fd); return -1; }
        if (n == cap) {
            if (cap >= IMG_MAX_FILE) { free(b); close(fd); errno = EFBIG; return -1; }
            cap *= 2;
            b = realloc(b, cap);
            if (!b) { close(fd); return -1; }
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { free(b); close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    *data = b;
    *len = n;
    return 0;
}

/* CRC-32 of the decoded pixels as R, G, B, A bytes: lets two decoders (or the host) compare results without moving pictures around. */
static uint32_t pixel_crc(const Image *im) {
    uint32_t crc = 0;
    uint8_t buf[4096];
    size_t n = (size_t)im->w * (size_t)im->h, i = 0;
    while (i < n) {
        size_t k = 0;
        for (; k < sizeof buf / 4 && i < n; k++, i++) {
            uint32_t p = im->px[i];
            buf[4 * k] = (uint8_t)(p >> 16); buf[4 * k + 1] = (uint8_t)(p >> 8); buf[4 * k + 2] = (uint8_t)p; buf[4 * k + 3] = (uint8_t)(p >> 24);
        }
        crc = z_crc32(crc, buf, k * 4);
    }
    return crc;
}

APPLET(imginfo) {
    int sum = 0, c, rc = 0;
    optind = 1;
    while ((c = getopt(argc, argv, "s")) != -1) {
        if (c == 's') sum = 1;
        else { fprintf(stderr, "usage: imginfo [-s] FILE...\n"); return 2; }
    }
    if (optind >= argc) { fprintf(stderr, "usage: imginfo [-s] FILE...\n"); return 2; }
    for (int i = optind; i < argc; i++) {
        uint8_t *data;
        size_t len;
        if (read_whole(argv[i], &data, &len) < 0) { printf("%s: error: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        ImgInfo info;
        int r = img_probe(data, len, &info);
        if (r) { printf("%s: error: %s\n", argv[i], img_strerror(r)); rc = 1; free(data); continue; }
        printf("%s: %s %dx%d %s %d frame%s, %zu bytes", argv[i], img_format_name(info.format), info.width, info.height,
               info.has_alpha ? "alpha" : "opaque", info.frames, info.frames == 1 ? "" : "s", len);
        if (sum) {
            Image im;
            r = img_decode(data, len, &im);
            if (r) { printf(", error: %s\n", img_strerror(r)); rc = 1; free(data); continue; }
            printf(", crc32 %08x", pixel_crc(&im));
            img_free(&im);
        }
        putchar('\n');
        free(data);
    }
    return rc;
}

/* ---------------------------------------------------------------- convert */

static int parse_size(const char *s, int *w, int *h) {
    char *end;
    long a = strtol(s, &end, 10);
    if (*end != 'x' || a <= 0) return -1;
    long b = strtol(end + 1, &end, 10);
    if (*end || b <= 0 || a > IMG_MAX_DIM || b > IMG_MAX_DIM) return -1;
    *w = (int)a;
    *h = (int)b;
    return 0;
}

static int has_ext(const char *p, const char *ext) {
    size_t n = strlen(p), e = strlen(ext);
    return n >= e && !strcasecmp(p + n - e, ext);
}

static void swap_images(Image *a, Image *b) { Image t = *a; *a = *b; *b = t; }

APPLET(convert) {
    int rotate = 0, flip_h = 0, flip_v = 0, level = 6, rw = 0, rh = 0, fw = 0, fh = 0, pct = 0, c;
    uint32_t bg = 0xFFFFFF;
    optind = 1;
    /* long-looking options (-resize, -fit, ...) are handled by hand so the usual ImageMagick spelling works */
    int i = 1;
    const char *files[2];
    int nfiles = 0;
    (void)c;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-resize") && i + 1 < argc) { if (parse_size(argv[++i], &rw, &rh)) { kb_error("bad size '%s' (use WIDTHxHEIGHT)", argv[i]); return 2; } }
        else if (!strcmp(a, "-fit") && i + 1 < argc) { if (parse_size(argv[++i], &fw, &fh)) { kb_error("bad size '%s' (use WIDTHxHEIGHT)", argv[i]); return 2; } }
        else if (!strcmp(a, "-scale") && i + 1 < argc) { pct = atoi(argv[++i]); if (pct < 1 || pct > 1000) { kb_error("bad percentage"); return 2; } }
        else if (!strcmp(a, "-rotate") && i + 1 < argc) { rotate = atoi(argv[++i]); if (rotate % 90) { kb_error("-rotate takes a multiple of 90"); return 2; } rotate = ((rotate / 90) % 4 + 4) % 4; }
        else if (!strcmp(a, "-flip")) flip_v = 1;
        else if (!strcmp(a, "-flop")) flip_h = 1;
        else if (!strcmp(a, "-level") && i + 1 < argc) level = atoi(argv[++i]);
        else if (!strcmp(a, "-background") && i + 1 < argc) bg = (uint32_t)strtoul(argv[++i][0] == '#' ? argv[i] + 1 : argv[i], NULL, 16) & 0xFFFFFF;
        else if (a[0] == '-' && a[1] && strcmp(a, "-")) { kb_error("unknown option '%s'", a); goto usage; }
        else if (nfiles < 2) files[nfiles++] = a;
        else goto usage;
    }
    if (nfiles != 2) {
usage:
        fprintf(stderr, "usage: convert [-resize WxH | -fit WxH | -scale N] [-rotate 90|180|270] [-flip] [-flop] [-level 0-9] [-background RRGGBB] INPUT OUTPUT\n");
        return 2;
    }
    const char *in = files[0], *out = files[1];
    int kind = has_ext(out, ".png") ? 0 : has_ext(out, ".bmp") ? 1 : (has_ext(out, ".ppm") || has_ext(out, ".pnm")) ? 2 : -1;
    if (kind < 0) { kb_error("the output name must end in .png, .bmp or .ppm"); return 2; }
    Image im, t;
    int r = img_load(in, &im);
    if (r) { kb_error("%s: %s", in, img_strerror(r)); return 1; }
    if (rotate) { r = img_rotate90(&im, rotate, &t); if (!r) { img_free(&im); swap_images(&im, &t); } }
    if (!r && flip_h) { r = img_flip(&im, 1, &t); if (!r) { img_free(&im); swap_images(&im, &t); } }
    if (!r && flip_v) { r = img_flip(&im, 0, &t); if (!r) { img_free(&im); swap_images(&im, &t); } }
    int nw = im.w, nh = im.h;
    if (pct) { nw = (int)(((long long)im.w * pct + 50) / 100); nh = (int)(((long long)im.h * pct + 50) / 100); }
    if (fw) img_fit(im.w, im.h, fw, fh, 0, &nw, &nh);
    if (rw) { nw = rw; nh = rh; }
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (!r && (nw != im.w || nh != im.h)) {
        r = img_scale(&im, nw, nh, (nw < im.w || nh < im.h) ? IMG_AREA : IMG_BILINEAR, &t);
        if (!r) { img_free(&im); swap_images(&im, &t); }
    }
    if (!r) r = kind == 0 ? img_save_png(out, &im, level) : kind == 1 ? img_save_bmp(out, &im) : img_save_ppm(out, &im, bg);
    img_free(&im);
    if (r) { kb_error("%s: %s", r == IMG_E_IO ? out : in, img_strerror(r)); return 1; }
    return 0;
}

/* ---------------------------------------------------------------- wallpaper */

APPLET(wallpaper) {
    KWallMode mode = KW_COVER;
    int have_mode = 0, clear = 0, c;
    optind = 1;
    while ((c = getopt(argc, argv, "m:c")) != -1) {
        if (c == 'm') {
            if (!k_wall_mode_parse(optarg, &mode)) { kb_error("unknown mode '%s' (cover, fit, center, tile or stretch)", optarg); return 2; }
            have_mode = 1;
        } else if (c == 'c') clear = 1;
        else { fprintf(stderr, "usage: wallpaper [-m cover|fit|center|tile|stretch] IMAGE\n       wallpaper -c\n       wallpaper\n"); return 2; }
    }
    if (clear) {
        int r = k_wallpaper_set(NULL, KW_COVER);
        if (r) { kb_error("cannot remove the setting: %s", strerror(-r)); return 1; }
        puts("The wallpaper is Reimu's night scene again.");
        return 0;
    }
    if (optind >= argc) {
        KWallConfig cfg;
        bool have = k_wallpaper_config(&cfg);
        if (have_mode) {                                            /* only the mode changes */
            if (!have) { kb_error("no picture is set; give one: wallpaper -m %s IMAGE", k_wall_mode_name(mode)); return 1; }
            int r = k_wallpaper_set(cfg.path, mode);
            if (r) { kb_error("cannot save the setting: %s", strerror(-r)); return 1; }
            printf("Wallpaper: %s (%s)\n", cfg.path, k_wall_mode_name(mode));
            return 0;
        }
        if (have) printf("%s (%s)\n", cfg.path, k_wall_mode_name(cfg.mode));
        else puts("built-in night scene");
        return 0;
    }
    char path[PATH_MAX];
    if (!realpath(argv[optind], path)) { kb_error("%s: %s", argv[optind], strerror(errno)); return 1; }
    ImgInfo info;
    int r = img_probe_file(path, &info);
    if (r) { kb_error("%s: %s", argv[optind], img_strerror(r)); return 1; }
    if (!have_mode) {                                               /* keep the mode that was set before, if any */
        KWallConfig old;
        if (k_wallpaper_config(&old)) mode = old.mode;
    }
    r = k_wallpaper_set(path, mode);
    if (r) { kb_error("cannot save the setting: %s", strerror(-r)); return 1; }
    printf("Wallpaper: %s (%s, %dx%d). The desktop changes it within a moment.\n", path, k_wall_mode_name(mode), info.width, info.height);
    return 0;
}

/* ---------------------------------------------------------------- screenshot */

APPLET(screenshot) {
    int delay = 0, c;
    optind = 1;
    while ((c = getopt(argc, argv, "d:")) != -1) {
        if (c == 'd') delay = atoi(optarg);
        else { fprintf(stderr, "usage: screenshot [-d SECONDS] [FILE.png|FILE.bmp|FILE.ppm]\n"); return 2; }
    }
    char path[640];
    if (optind < argc) {
        if (!realpath(argv[optind], path)) {                        /* the file may not exist yet: resolve its directory */
            const char *name = strrchr(argv[optind], '/');
            char dir[PATH_MAX];
            if (name) { snprintf(dir, sizeof dir, "%.*s", (int)(name - argv[optind]) + 1, argv[optind]); name++; }
            else { strcpy(dir, "."); name = argv[optind]; }
            char real[PATH_MAX];
            if (!realpath(dir, real)) { kb_error("%s: %s", dir, strerror(errno)); return 1; }
            snprintf(path, sizeof path, "%s/%s", real, name);
        }
        if (!has_ext(path, ".png") && !has_ext(path, ".bmp") && !has_ext(path, ".ppm")) { kb_error("the file name must end in .png, .bmp or .ppm"); return 2; }
        unlink(path);
    } else k_screenshot_path(path, sizeof path);
    if (strlen(path) > 250) { kb_error("the path is too long for the window server"); return 1; }
    if (delay > 0) sleep((unsigned)delay);
    FairyDisplay *d = fairy_connect("screenshot");
    if (!d) { kb_error("no window server answered (is the desktop running?)"); return 1; }
    fairy_screenshot(d, path);
    struct stat st;
    int ok = 0;
    for (int i = 0; i < 600; i++) {                                 /* the server writes PNGs in a child process and renames when done */
        if (stat(path, &st) == 0 && st.st_size > 0) { ok = 1; break; }
        usleep(100000);
    }
    fairy_disconnect(d);
    if (!ok) { kb_error("the window server did not produce %s", path); return 1; }
    printf("Aya took a photo of the screen: %s\n", path);
    return 0;
}
