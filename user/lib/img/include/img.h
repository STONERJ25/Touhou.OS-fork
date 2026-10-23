/*
 * libimg: image decoding, encoding and the small set of transformations a viewer or a wallpaper needs.
 *
 * Decoders turn a file (in memory) into an Image: w x h pixels of 0xAARRGGBB with straight (not premultiplied) alpha,
 * alpha 255 for opaque pixels. Everything that parses untrusted bytes is bounds-checked: a truncated or hostile file
 * produces an error code, never an over-read, and sizes are capped (IMG_MAX_DIM per side, IMG_MAX_PIXELS in total)
 * before anything is allocated.
 *
 *      Image im;
 *      if (img_load("/tmp/photo.png", &im) == IMG_OK) {
 *          Image small;
 *          img_scale(&im, 160, 120, IMG_AREA, &small);
 *          ...
 *          img_free(&small);
 *      }
 *      img_free(&im);
 *
 * Formats read: PNG (all colour types and bit depths, Adam7, tRNS), BMP (1/4/8/16/24/32 bit, RLE, bitfields).
 * Formats written: PNG, BMP, PPM.
 */
#ifndef GAP_IMG_H
#define GAP_IMG_H

#include <stddef.h>
#include <stdint.h>

typedef enum { IMG_UNKNOWN = 0, IMG_PNG, IMG_JPEG, IMG_GIF, IMG_BMP, IMG_PNM, IMG_TGA } ImgFormat;

/* return codes (0 or negative) */
#define IMG_OK            0
#define IMG_E_FORMAT    (-1)        /* not a format this library knows */
#define IMG_E_TRUNC     (-2)        /* the data ends before the image does */
#define IMG_E_CORRUPT   (-3)        /* the data contradicts itself */
#define IMG_E_TOOBIG    (-4)        /* larger than IMG_MAX_DIM / IMG_MAX_PIXELS */
#define IMG_E_NOMEM     (-5)
#define IMG_E_UNSUPPORTED (-6)      /* valid, but a feature this library does not do */
#define IMG_E_IO        (-7)
#define IMG_E_ARG       (-8)

#define IMG_MAX_DIM     16384
#define IMG_MAX_PIXELS  (64u * 1024u * 1024u)
#define IMG_MAX_FILE    (256u * 1024u * 1024u)

typedef struct ImgInfo {
    ImgFormat format;
    int width, height;              /* as the image will be displayed (after the JPEG orientation tag, if any) */
    int has_alpha;
    int frames;                     /* 1 for stills; animated GIFs report their frame count when it is cheap to know */
} ImgInfo;

typedef struct Image {
    int w, h;
    uint32_t *px;                   /* w * h pixels, row after row, 0xAARRGGBB */
    int has_alpha;                  /* false: every alpha is 255 */
} Image;

ImgFormat   img_sniff(const void *data, size_t len);          /* by signature */
const char *img_format_name(ImgFormat f);                      /* "PNG", "JPEG", ... */
const char *img_strerror(int rc);

/* Size and kind without decoding the pixels. */
int  img_probe(const void *data, size_t len, ImgInfo *info);
int  img_probe_file(const char *path, ImgInfo *info);

/* Decoding. img_load reads the whole file; img_decode works on a buffer. On failure *out is zeroed. */
int  img_decode(const void *data, size_t len, Image *out);
int  img_load(const char *path, Image *out);
void img_free(Image *im);
int  img_alloc(Image *im, int w, int h);                       /* zeroed pixels; checks the limits */

/* Transformations; `out` receives a new image (it may not be `src`). */
typedef enum { IMG_NEAREST, IMG_BILINEAR, IMG_AREA } ImgScale;
int  img_scale(const Image *src, int new_w, int new_h, ImgScale mode, Image *out);   /* IMG_AREA averages: use it to shrink */
int  img_rotate90(const Image *src, int quarter_turns_cw, Image *out);               /* 1 = 90 degrees clockwise, 2 = 180, 3 = 270 */
int  img_flip(const Image *src, int horizontal, Image *out);                         /* horizontal mirror, else vertical */
void img_fit(int src_w, int src_h, int box_w, int box_h, int allow_upscale, int *out_w, int *out_h);   /* aspect-preserving size */
/* Alpha-composites over a solid 0x00RRGGBB colour in place; the result is opaque. */
void img_composite_over(Image *im, uint32_t background);

/* Encoding. Writers send their output through a sink that returns 0 or a negative number. */
typedef int (*ImgSink)(void *user, const void *data, size_t n);
int  img_write_png(const Image *im, int level, ImgSink sink, void *user);   /* level 0..9 like gzip; 6 is a good default, 2 is quick */
int  img_write_bmp(const Image *im, ImgSink sink, void *user);              /* 24-bit, or 32-bit with alpha when the image has some */
int  img_write_ppm(const Image *im, uint32_t background, ImgSink sink, void *user);   /* P6; transparency is flattened onto `background` */
int  img_save_png(const char *path, const Image *im, int level);
int  img_save_bmp(const char *path, const Image *im);
int  img_save_ppm(const char *path, const Image *im, uint32_t background);

#endif
