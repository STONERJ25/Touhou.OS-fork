/*
 * web_image_libimg: the WebImageDecoder built on user/lib/img. probe reads the size without decoding (img_probe), decode
 * gives an Image (0xAARRGGBB, straight alpha) whose pointer is the handle the display list carries, release frees it.
 * This is the only file of the engine that depends on another library; an embedder with its own decoder can leave it out.
 */
#include "web.h"

#include <img.h>
#include <stdlib.h>

static int probe(void *user, const void *data, size_t len, int *w, int *h) {
    (void)user;
    ImgInfo info;
    if (img_probe(data, len, &info) != IMG_OK) return -1;
    *w = info.width;
    *h = info.height;
    return 0;
}

static WebImageHandle decode(void *user, const void *data, size_t len) {
    (void)user;
    Image *im = malloc(sizeof *im);
    if (!im) return 0;
    if (img_decode(data, len, im) != IMG_OK) { free(im); return 0; }
    return (WebImageHandle)im;
}

static void release(void *user, WebImageHandle h) {
    (void)user;
    Image *im = (Image *)h;
    if (!im) return;
    img_free(im);
    free(im);
}

static const WebImageDecoder libimg_decoder = { NULL, probe, decode, release };

const WebImageDecoder *web_image_libimg(void) { return &libimg_decoder; }
