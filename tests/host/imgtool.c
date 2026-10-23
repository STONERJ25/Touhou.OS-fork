/* Host test driver for libimg (build: see tests/host/imgtest.py).
 *   imgtool info FILE            "FORMAT W H alpha frames"
 *   imgtool dump FILE OUT        raw RGBA bytes (R, G, B, A per pixel)
 *   imgtool frames FILE PREFIX   every GIF frame as PREFIX<n>.rgba, prints the delays
 *   imgtool convert IN OUT [LEVEL]   OUT's extension picks png / bmp / ppm
 *   imgtool scale IN OUT W H MODE    MODE 0 nearest, 1 bilinear, 2 area (raw RGBA out)
 *   imgtool rot IN OUT TURNS     rotate and write raw RGBA with a "W H" first line
 *   imgtool try FILE             decode, print the return code (for fuzzing)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <img.h>

static void write_rgba(const char *path, const Image *im, int header) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (header) fprintf(f, "%d %d\n", im->w, im->h);
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        uint32_t p = im->px[i];
        uint8_t b[4] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p, (uint8_t)(p >> 24) };
        fwrite(b, 1, 4, f);
    }
    fclose(f);
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        size_t r = fread(b + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    fclose(f);
    *n = len;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "info")) {
        ImgInfo in;
        int rc = img_probe_file(argv[2], &in);
        if (rc) { printf("error %d %s\n", rc, img_strerror(rc)); return 1; }
        printf("%s %d %d %d %d\n", img_format_name(in.format), in.width, in.height, in.has_alpha, in.frames);
        return 0;
    }
    if (!strcmp(cmd, "try")) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n);
        if (!d) return 2;
        Image im;
        ImgInfo in;
        int prc = img_probe(d, n, &in);
        int rc = img_decode(d, n, &im);
        printf("%d %d\n", prc, rc);
        if (!rc) img_free(&im);
        free(d);
        return 0;
    }
    if (!strcmp(cmd, "dump") && argc >= 4) {
        Image im;
        int rc = img_load(argv[2], &im);
        if (rc) { printf("error %d %s\n", rc, img_strerror(rc)); return 1; }
        printf("%d %d %d\n", im.w, im.h, im.has_alpha);
        write_rgba(argv[3], &im, 0);
        img_free(&im);
        return 0;
    }
    if (!strcmp(cmd, "frames") && argc >= 4) {
        size_t n;
        uint8_t *d = slurp(argv[2], &n);
        if (!d) return 2;
        ImgAnim *a;
        int rc = img_anim_open(d, n, &a);
        if (rc) { printf("error %d\n", rc); return 1; }
        int w, h, nf, loops;
        img_anim_info(a, &w, &h, &nf, &loops);
        printf("%d %d %d %d\n", w, h, nf, loops);
        for (int i = 0; i < nf; i++) {
            const Image *fr;
            int delay;
            rc = img_anim_next(a, &fr, &delay);
            if (rc) { printf("error %d at frame %d\n", rc, i); return 1; }
            char path[512];
            snprintf(path, sizeof path, "%s%d.rgba", argv[3], i);
            write_rgba(path, fr, 0);
            printf("%d\n", delay);
        }
        img_anim_close(a);
        free(d);
        return 0;
    }
    if (!strcmp(cmd, "convert") && argc >= 4) {
        Image im;
        int rc = img_load(argv[2], &im);
        if (rc) { printf("error %d %s\n", rc, img_strerror(rc)); return 1; }
        const char *out = argv[3];
        size_t l = strlen(out);
        int level = argc > 4 ? atoi(argv[4]) : 6;
        if (l > 4 && !strcmp(out + l - 4, ".bmp")) rc = img_save_bmp(out, &im);
        else if (l > 4 && !strcmp(out + l - 4, ".ppm")) rc = img_save_ppm(out, &im, 0xFFFFFF);
        else rc = img_save_png(out, &im, level);
        img_free(&im);
        if (rc) { printf("error %d %s\n", rc, img_strerror(rc)); return 1; }
        return 0;
    }
    if (!strcmp(cmd, "scale") && argc >= 7) {
        Image im, o;
        int rc = img_load(argv[2], &im);
        if (rc) return 1;
        rc = img_scale(&im, atoi(argv[4]), atoi(argv[5]), (ImgScale)atoi(argv[6]), &o);
        if (rc) { printf("error %d\n", rc); return 1; }
        printf("%d %d\n", o.w, o.h);
        write_rgba(argv[3], &o, 0);
        return 0;
    }
    if (!strcmp(cmd, "rot") && argc >= 5) {
        Image im, o;
        int rc = img_load(argv[2], &im);
        if (rc) return 1;
        rc = img_rotate90(&im, atoi(argv[4]), &o);
        if (rc) return 1;
        write_rgba(argv[3], &o, 1);
        return 0;
    }
    return 2;
}
