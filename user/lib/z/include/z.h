/*
 * libz: DEFLATE compression for Gensokyo (RFC 1951), with the zlib (RFC 1950) and gzip (RFC 1952) wrappers.
 *
 *  - inflate: a streaming decompressor. Feed it input in pieces of any size and give it output space in pieces of any
 *    size; it keeps its own 32 KiB history, so the caller's buffers can be reused freely. Every length, distance and
 *    table is checked: hostile input produces an error code, never an over-read.
 *  - deflate: a streaming compressor (LZ77 with hash chains, lazy matching, dynamic Huffman blocks, levels 0..9,
 *    stored-block fallback when compression would only make the data bigger).
 *  - crc32 / adler32.
 *
 * The streaming interface looks like this (the shape is the one every zlib user knows):
 *
 *      ZStream s;
 *      z_inflate_init(&s, Z_AUTO);                       // zlib or gzip, whichever the data starts with
 *      for (;;) {
 *          s.next_in = in;  s.avail_in = n;             // refill when avail_in reaches 0
 *          s.next_out = out; s.avail_out = sizeof out;  // drain when avail_out reaches 0
 *          int rc = z_inflate(&s);                       // Z_OK: more input or output space needed, Z_STREAM_END: done
 *          ...
 *      }
 *      z_inflate_end(&s);
 *
 * When Z_STREAM_END is returned, bytes after the end of the stream (trailing data, the next gzip member) are still in
 * next_in / avail_in.
 */
#ifndef GAP_Z_H
#define GAP_Z_H

#include <stddef.h>
#include <stdint.h>

/* return codes */
#define Z_OK           0
#define Z_STREAM_END   1
#define Z_ERR_DATA    (-1)      /* the compressed data is corrupt */
#define Z_ERR_MEM     (-2)
#define Z_ERR_FORMAT  (-3)      /* not a zlib / gzip stream */
#define Z_ERR_CHECK   (-4)      /* checksum or length in the trailer does not match */
#define Z_ERR_LIMIT   (-5)      /* output limit exceeded (decompression bomb guard of the one-shot helpers) */
#define Z_ERR_ARG     (-6)
#define Z_ERR_TRUNC   (-7)      /* the input ended in the middle of the stream */
#define Z_BUF_ERROR   (-8)      /* no progress possible: the caller must supply input or output space */

/* stream formats */
#define Z_RAW   0               /* bare DEFLATE */
#define Z_ZLIB  1
#define Z_GZIP  2
#define Z_AUTO  3               /* inflate only: zlib or gzip, decided by the first bytes */

/* flush modes for z_deflate */
#define Z_NO_FLUSH    0
#define Z_SYNC_FLUSH  2         /* emit everything so far and byte-align (empty stored block marker) */
#define Z_FINISH      4

#define Z_DEFAULT_LEVEL 6

typedef struct ZGzipInfo {
    uint32_t mtime;             /* seconds since 1970, 0 = unknown */
    uint8_t  os;                /* 3 = Unix */
    uint8_t  xfl;
    char     name[256];         /* original file name, "" if absent (deflate: set before the first z_deflate call) */
} ZGzipInfo;

typedef struct ZStream {
    const uint8_t *next_in;  size_t avail_in;  uint64_t total_in;
    uint8_t       *next_out; size_t avail_out; uint64_t total_out;
    void          *state;
    ZGzipInfo      gzip;     /* inflate: filled in while the gzip header is read; deflate: used for the header */
    uint32_t       check;    /* running CRC-32 (gzip) / Adler-32 (zlib) of the uncompressed data */
} ZStream;

uint32_t z_crc32(uint32_t crc, const void *data, size_t n);        /* start from 0 */
uint32_t z_adler32(uint32_t adler, const void *data, size_t n);    /* start from 1 */

int  z_inflate_init(ZStream *s, int format);
int  z_inflate(ZStream *s);
int  z_inflate_reset(ZStream *s);               /* start the next stream (e.g. the next gzip member) with the same buffers */
void z_inflate_end(ZStream *s);

int  z_deflate_init(ZStream *s, int level, int format);   /* level 0..9, 0 = stored */
int  z_deflate(ZStream *s, int flush);          /* Z_OK, or Z_STREAM_END once Z_FINISH has been fully written */
void z_deflate_end(ZStream *s);
size_t z_deflate_bound(size_t source_len);      /* a safe output size for one-shot compression (raw/zlib/gzip all fit) */

/* One-shot helpers. The output is malloc'd (free() it). max_out caps what inflate may produce (0 = 256 MiB). */
int  z_compress(const void *src, size_t n, int level, int format, uint8_t **out, size_t *out_len);
int  z_uncompress(const void *src, size_t n, int format, size_t max_out, uint8_t **out, size_t *out_len);

const char *z_strerror(int rc);

#endif
