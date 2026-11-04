/*
 * gunzip for the kernel: one gzip member (RFC 1952) holding a DEFLATE stream (RFC 1951), read from memory and handed to a callback in pieces.
 *
 * It exists so that initrd.img can be shipped compressed (docs/UPDATE.md, "The compressed initrd"): the loader hands the kernel the file as it
 * is on the ESP, and kernel/fs/initrd.c feeds the unpacked tar to the file system as it comes out. Nothing here depends on the kernel: the one
 * source file, kernel/lib/inflate.c, is also compiled for the host by tests/host/test_inflate.py (with AddressSanitizer and UBSan), and it
 * includes nothing but <stdint.h> and <stddef.h>, so that header can be found with -iquote and the host's own <string.h> is left alone.
 *
 * Usage:
 *
 *     void *work = kmalloc(GZ_WORK_128K);                          // the tables and the window; any size from GZ_MIN_WORK up, 8-byte aligned
 *     GzInfo info;
 *     int rc = gunzip(file, file_size, work, GZ_WORK_128K, 256u << 20, sink, ctx, &info);
 *     if (rc) panic("%s", gunzip_strerror(rc));
 *
 * The sink gets the output in order, in pieces of up to a window's worth. THE OUTPUT IS NOT TRUSTED UNTIL gunzip() RETURNS 0: the trailer
 * (CRC-32 and length) is checked after the last byte has been delivered, and a damaged stream can deliver a lot before it is found out.
 * A caller that must not act on damaged data has to be able to throw away what it did, or to halt (the kernel halts).
 *
 * What is refused, each with its own result (and a short reason in GzInfo.why for GZ_E_DATA): a header that is not a gzip member or sets a
 * reserved flag; invalid DEFLATE data (a block type that does not exist, a stored block whose length check fails, Huffman code lengths that
 * over-subscribe or leave gaps, a code that is not assigned, a distance that reaches before the start of the output); a stream that ends early;
 * a CRC-32 or length that does not match the trailer; ANY byte after the trailer (a second gzip member counts: a single member is the format of
 * initrd.img); and more output than `limit` (a decompression bomb needs a few kilobytes of input per gigabyte of output, so the ceiling is the
 * only defence, and because the output is streamed it costs time but never memory).
 */
#ifndef K_INFLATE_H
#define K_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#define GZ_OK           0
#define GZ_E_FORMAT   (-1)      /* not a gzip member: wrong magic, a method other than 8 (deflate), a reserved flag set, a header cut short */
#define GZ_E_HCRC     (-2)      /* the header's own CRC-16 (FHCRC) does not match */
#define GZ_E_DATA     (-3)      /* invalid DEFLATE data (GzInfo.why says what) */
#define GZ_E_TRUNC    (-4)      /* the input ends inside the stream */
#define GZ_E_CRC      (-5)      /* the trailer's CRC-32 does not match the output */
#define GZ_E_SIZE     (-6)      /* the trailer's length (mod 2^32) does not match the output */
#define GZ_E_TRAILING (-7)      /* bytes after the trailer */
#define GZ_E_LIMIT    (-8)      /* the output would be longer than the limit */
#define GZ_E_SINK     (-9)      /* the sink returned non-zero */
#define GZ_E_ARG     (-10)      /* a work area that is too small or not 8-byte aligned, or no input */

#define GZ_HISTORY     32768u   /* how far back DEFLATE may look: the window always keeps this much of what it has delivered */
#define GZ_STATE_BYTES 16384u   /* the decoder's tables, at the start of the work area (inflate.c checks that its state fits) */
#define GZ_MIN_WORK    (GZ_STATE_BYTES + GZ_HISTORY + 1024u)        /* the smallest work area gunzip() accepts: tables, history, room for a match */
#define GZ_WORK_128K   (128u * 1024u)       /* a work area of one power of two, which is what the kernel's page allocator hands out without waste (120 KiB window) */

/* Gets the next piece of output (data stays valid only during the call); non-zero stops the decoding with GZ_E_SINK. NULL sink: just check the stream. */
typedef int (*GzSink)(void *ctx, const uint8_t *data, size_t n);

typedef struct {
    uint64_t in_pos;            /* input bytes used so far; on an error, about where the decoder was */
    uint64_t out_len;           /* bytes delivered to the sink */
    uint32_t crc;               /* CRC-32 of those bytes */
    const char *why;            /* GZ_E_DATA / GZ_E_FORMAT: a short reason (a string constant); otherwise NULL */
} GzInfo;

/* Decodes src[0..srclen) completely, or fails. work/work_size: the memory for the tables and the window (GZ_MIN_WORK or more, 8-byte aligned; a bigger one
 * means fewer, larger pieces for the sink). limit: the most output bytes allowed. info may be NULL. */
int gunzip(const uint8_t *src, size_t srclen, void *work, size_t work_size, uint64_t limit, GzSink sink, void *ctx, GzInfo *info);

const char *gunzip_strerror(int rc);

#endif
