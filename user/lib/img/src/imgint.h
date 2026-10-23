/* libimg internals shared by the codec files. */
#ifndef IMGINT_H
#define IMGINT_H

#include <img.h>
#include <string.h>
#include <stdlib.h>

static inline uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t rd_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint32_t rd_le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t rd_be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

#define ARGB(a, r, g, b) ((uint32_t)(a) << 24 | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))

/* checks width/height against the limits (and overflow); returns 0 or IMG_E_TOOBIG / IMG_E_CORRUPT */
int img_check_size(long long w, long long h);

int png_probe(const uint8_t *d, size_t n, ImgInfo *info);
int png_decode(const uint8_t *d, size_t n, Image *out);
int bmp_probe(const uint8_t *d, size_t n, ImgInfo *info);
int bmp_decode(const uint8_t *d, size_t n, Image *out);

#endif
