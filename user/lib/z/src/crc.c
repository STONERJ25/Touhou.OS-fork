/* CRC-32 (slicing-by-4) and Adler-32. */
#include <z.h>

static uint32_t g_crc[4][256];
static int g_crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = g_crc[0][i];
        for (int t = 1; t < 4; t++) {
            c = g_crc[0][c & 0xFF] ^ (c >> 8);
            g_crc[t][i] = c;
        }
    }
    g_crc_ready = 1;
}

uint32_t z_crc32(uint32_t crc, const void *data, size_t n) {
    if (!g_crc_ready) crc_init();
    const uint8_t *p = data;
    crc = ~crc;
    while (n && ((uintptr_t)p & 3)) { crc = g_crc[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8); n--; }
    while (n >= 4) {
        uint32_t w = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        crc ^= w;
        crc = g_crc[3][crc & 0xFF] ^ g_crc[2][(crc >> 8) & 0xFF] ^ g_crc[1][(crc >> 16) & 0xFF] ^ g_crc[0][crc >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) crc = g_crc[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t z_adler32(uint32_t adler, const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
    while (n) {
        size_t chunk = n < 5552 ? n : 5552;                     /* the most bytes that cannot overflow 32 bits */
        n -= chunk;
        while (chunk--) { a += *p++; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

const char *z_strerror(int rc) {
    switch (rc) {
    case Z_OK: return "ok";
    case Z_STREAM_END: return "end of stream";
    case Z_ERR_DATA: return "compressed data is corrupt";
    case Z_ERR_MEM: return "out of memory";
    case Z_ERR_FORMAT: return "not in gzip or zlib format";
    case Z_ERR_CHECK: return "checksum mismatch (the data is damaged)";
    case Z_ERR_LIMIT: return "output too large";
    case Z_ERR_ARG: return "bad argument";
    case Z_ERR_TRUNC: return "unexpected end of file";
    case Z_BUF_ERROR: return "no progress possible";
    default: return "unknown error";
    }
}
