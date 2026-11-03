/*
 * LZNT1 decompression, the compression NTFS uses for compressed files. Written from MS-XCA section 2.5 ("LZNT1 Algorithm Details"), whose worked example (a
 * 142-byte string that compresses to 59 bytes) is one of the tests, and checked against the output of Windows' own RtlCompressBuffer.
 *
 * A stream is a series of chunks. A chunk starts with a 16-bit little-endian header: bit 15 says the data is compressed, bits 14..12 are always 3, bits 11..0
 * are the size of the chunk counted with the header minus 3, i.e. the number of data bytes after the header minus one. A header of 0 ends the stream. An
 * uncompressed chunk is just the bytes. A compressed chunk is groups of one flag byte (least significant bit first) and up to eight items: a clear bit is a
 * literal byte, a set bit a 2-byte token that copies earlier output of the same chunk. In the token (displacement-1) is in the high M bits and (length-3) in
 * the low 16-M bits, where M is the smallest value >= 4 with 2**M >= the number of bytes the chunk has produced so far; a copy may overlap its own output.
 * A chunk produces at most 4096 bytes; copies never reach into the previous chunk.
 */
#include "ntfs_priv.h"

int ntfs_lznt1(const u8 *src, size_t slen, u8 *dst, size_t dcap, size_t *dlen) {
    size_t sp = 0, dp = 0;
    while (slen - sp >= 2) {
        u32 h = nt_le16(src + sp);
        if (h == 0) break;
        if (((h >> 12) & 7) != 3) return -EIO;
        size_t csz = (size_t)(h & 0xFFF) + 1;
        sp += 2;
        if (csz > slen - sp) return -EIO;                          /* the chunk runs past the end of the data */
        const u8 *c = src + sp;
        sp += csz;
        if (!(h & 0x8000)) {                                       /* stored as it is */
            if (csz > 4096 || csz > dcap - dp) return -EIO;
            memcpy(dst + dp, c, csz);
            dp += csz;
            continue;
        }
        size_t start = dp, ci = 0;
        while (ci < csz) {
            u32 flags = c[ci++];
            for (u32 bit = 0; bit < 8 && ci < csz; bit++) {
                size_t done = dp - start;
                if (!(flags & (1u << bit))) {
                    if (dp >= dcap || done >= 4096) return -EIO;
                    dst[dp++] = c[ci++];
                    continue;
                }
                if (csz - ci < 2 || done == 0) return -EIO;        /* half a token, or a copy with nothing to copy from */
                u32 tok = nt_le16(c + ci);
                ci += 2;
                u32 m = 4;
                while (((size_t)1 << m) < done) m++;
                size_t disp = (size_t)(tok >> (16 - m)) + 1, len = (size_t)(tok & ((1u << (16 - m)) - 1)) + 3;
                if (disp > done || len > dcap - dp || len > 4096 - done) return -EIO;
                for (size_t k = 0; k < len; k++) {                 /* byte by byte: the source may overlap the destination */
                    dst[dp] = dst[dp - disp];
                    dp++;
                }
            }
        }
    }
    *dlen = dp;
    return 0;
}
