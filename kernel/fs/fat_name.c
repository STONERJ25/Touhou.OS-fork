/* FAT names: UTF-8 <-> UTF-16, case folding, 8.3 short names, VFAT long-name slots. Pure functions, no I/O. */
#include "fatfs.h"

/* ------------------------------------------------------------------------------------------------ unicode */

/* Decodes one code point. Returns the number of bytes used, or 0 for malformed input. */
static int u8_decode(const u8 *s, size_t len, u32 *cp) {
    if (!len) return 0;
    u8 c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (len < 2 || (s[1] & 0xC0) != 0x80) return 0;
        *cp = ((u32)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (len < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        u32 v = ((u32)(c & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        *cp = v;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (len < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        u32 v = ((u32)(c & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12) | ((u32)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return 0;
        *cp = v;
        return 4;
    }
    return 0;
}

/* Appends a code point as UTF-8; returns the byte count (0 if it does not fit). */
static int u8_encode(u32 cp, char *out, size_t room) {
    if (cp < 0x80) { if (room < 1) return 0; out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        if (room < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (room < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (room < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* UTF-8 -> UTF-16 code units. Returns the unit count, -EINVAL for malformed input, -ENAMETOOLONG if it exceeds cap. */
int fatn_utf8_to_utf16(const char *s, size_t len, u16 *out, int cap) {
    int n = 0;
    const u8 *p = (const u8 *)s;
    while (len) {
        u32 cp;
        int k = u8_decode(p, len, &cp);
        if (!k) return -EINVAL;
        p += k;
        len -= (size_t)k;
        if (cp >= 0x10000) {
            if (n + 2 > cap) return -ENAMETOOLONG;
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 | (cp >> 10));
            out[n++] = (u16)(0xDC00 | (cp & 0x3FF));
        } else {
            if (n + 1 > cap) return -ENAMETOOLONG;
            out[n++] = (u16)cp;
        }
    }
    return n;
}

/* UTF-16 -> NUL-terminated UTF-8. Lone surrogates become U+FFFD. Returns the length or -ENAMETOOLONG. */
int fatn_utf16_to_utf8(const u16 *in, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        u32 cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        int k = u8_encode(cp, out + o, (size_t)(cap - 1 - o));
        if (!k) return -ENAMETOOLONG;
        o += k;
    }
    out[o] = 0;
    return o;
}

/* Simple case folding: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic. */
u32 fatn_fold(u32 c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c < 0x100) return (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c;
    if (c < 0x180) {
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        if (c <= 0x137) return c | 1;
        if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;
        if (c >= 0x14A && c <= 0x177) return c | 1;
        if (c == 0x178) return 0xFF;
        if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
        return c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c == 0x3C2) return 0x3C3;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

/* Walks a UTF-8 string yielding folded code points; undecodable bytes are returned as themselves (above the Unicode range). */
static u32 next_folded(const u8 **p) {
    u32 cp;
    int k = u8_decode(*p, 4, &cp);              /* the string is NUL terminated, so a short read cannot go past it */
    if (!k) { cp = 0x110000u + **p; k = 1; }
    else cp = fatn_fold(cp);
    *p += k;
    return cp;
}

u32 fatn_hash(const char *utf8) {
    const u8 *p = (const u8 *)utf8;
    u32 h = 2166136261u;
    while (*p) {
        u32 c = next_folded(&p);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

bool fatn_equal(const char *a, const char *b) {
    const u8 *p = (const u8 *)a, *q = (const u8 *)b;
    while (*p && *q) {
        if (next_folded(&p) != next_folded(&q)) return false;
    }
    return *p == 0 && *q == 0;
}

/* ------------------------------------------------------------------------------------------------ short names */

u8 fatn_checksum(const u8 sfn[11]) {
    u8 s = 0;
    for (int i = 0; i < 11; i++) s = (u8)((((s & 1) << 7) | (s >> 1)) + sfn[i]);
    return s;
}

static bool sfn_ok(u32 c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return c && c < 0x80 && strchr("$%'-_@~!(){}^#&", (int)c) != NULL;
}

/* Characters never allowed in a long name either. */
static bool lfn_illegal(u32 c) {
    return c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|' || c == 0x7F;
}

bool fatn_valid16(const u16 *name, int n) {
    if (n < 1 || n > FAT_MAX_NAME) return false;
    bool all_dots = true;
    for (int i = 0; i < n; i++) {
        if (lfn_illegal(name[i])) return false;
        if (name[i] != '.') all_dots = false;
    }
    if (all_dots) return false;                         /* ".", ".." and friends */
    if (name[n - 1] == '.' || name[n - 1] == ' ') return false;
    return true;
}

static u32 upper_ascii(u32 c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* A legal 8.3 name in a single case per part needs no long-name slots. */
static bool plain_83(const u16 *name, int n, u8 sfn[11], u8 *ntres) {
    int dot = -1;
    for (int i = 0; i < n; i++) if (name[i] == '.') dot = i;
    int base_len = dot < 0 ? n : dot, ext_len = dot < 0 ? 0 : n - dot - 1;
    if (base_len < 1 || base_len > 8 || ext_len > 3) return false;
    bool lower[2] = { false, false }, upper[2] = { false, false };
    for (int i = 0; i < n; i++) {
        if (i == dot) continue;
        if (i < dot && name[i] == '.') return false;
        u32 c = name[i];
        int part = (dot >= 0 && i > dot) ? 1 : 0;
        if (c >= 'a' && c <= 'z') lower[part] = true;
        else if (c >= 'A' && c <= 'Z') upper[part] = true;
        if (c >= 0x80 || !sfn_ok(upper_ascii(c))) return false;
    }
    if ((lower[0] && upper[0]) || (lower[1] && upper[1])) return false;
    memset(sfn, ' ', 11);
    for (int i = 0; i < base_len; i++) sfn[i] = (u8)upper_ascii(name[i]);
    for (int i = 0; i < ext_len; i++) sfn[8 + i] = (u8)upper_ascii(name[dot + 1 + i]);
    *ntres = (u8)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return true;
}

int fatn_make_sfn(const u16 *name, int n, u8 sfn[11], u8 *ntres, bool *need_lfn,
                  bool (*taken)(void *ctx, const u8 sfn[11]), void *ctx) {
    if (plain_83(name, n, sfn, ntres)) {
        *need_lfn = false;
        return taken(ctx, sfn) ? -EEXIST : 0;
    }
    *need_lfn = true;
    *ntres = 0;
    bool lossy = false;
    int start = 0;
    while (start < n && (name[start] == '.' || name[start] == ' ')) start++;
    if (start) lossy = true;

    /* split at the last dot, dropping spaces and the other dots */
    int dot = -1;
    for (int i = start; i < n; i++) if (name[i] == '.') dot = i;
    char base[16], ext[8];
    int bl = 0, el = 0;
    for (int i = start; i < n; i++) {
        u32 c = name[i];
        if (c == ' ') { lossy = true; continue; }
        if (c == '.' && i != dot) { lossy = true; continue; }
        if (i == dot) continue;
        char out;
        u32 u = upper_ascii(c);
        if (c < 0x80 && sfn_ok(u)) out = (char)u;
        else { out = '_'; lossy = true; }
        if (dot >= 0 && i > dot) { if (el < 7) ext[el++] = out; else el++; }
        else { if (bl < 15) base[bl++] = out; else bl++; }
    }
    if (bl == 0) { base[bl++] = '_'; lossy = true; }
    if (bl > 8 || el > 3) lossy = true;
    if (bl > 8) bl = 8;
    if (el > 3) el = 3;

    if (!lossy) {
        memset(sfn, ' ', 11);
        memcpy(sfn, base, (size_t)bl);
        memcpy(sfn + 8, ext, (size_t)el);
        if (!taken(ctx, sfn)) return 0;
    }
    for (u32 k = 1; k < 1000000; k++) {
        char tail[10];
        int tl = snprintf(tail, sizeof tail, "~%u", k);
        int keep = 8 - tl;
        if (keep > bl) keep = bl;
        memset(sfn, ' ', 11);
        memcpy(sfn, base, (size_t)keep);
        memcpy(sfn + keep, tail, (size_t)tl);
        memcpy(sfn + 8, ext, (size_t)el);
        if (!taken(ctx, sfn)) return 0;
    }
    return -EEXIST;
}

/* OEM code page 437 for bytes 0x80..0xFF (names written by DOS and old Windows without a long name). */
static const u16 cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* "BASE    EXT" with the NT case flags -> "base.ext" in UTF-8 */
void fatn_sfn_to_utf8(const u8 sfn[11], u8 ntres, char *out, size_t cap) {
    size_t o = 0;
    for (int part = 0; part < 2; part++) {
        int from = part ? 8 : 0, len = part ? 3 : 8;
        while (len > 0 && sfn[from + len - 1] == ' ') len--;
        if (part && len > 0 && o + 1 < cap) out[o++] = '.';
        bool lower = (ntres & (part ? 0x10 : 0x08)) != 0;
        for (int i = 0; i < len; i++) {
            u8 b = sfn[from + i];
            if (i == 0 && part == 0 && b == 0x05) b = 0xE5;
            u32 cp = b < 0x80 ? b : cp437_hi[b - 0x80];
            if (lower) cp = fatn_fold(cp);
            int k = o + 4 < cap ? u8_encode(cp, out + o, cap - 1 - o) : 0;
            if (!k) break;
            o += (size_t)k;
        }
    }
    out[o] = 0;
}

/* Converts a looked-up name to its 11-byte short form, if it could be one (alias lookups such as PROGRA~1). */
bool fatn_name_to_sfn(const char *utf8, u8 sfn[11]) {
    size_t len = strlen(utf8);
    if (len == 0 || len > 12) return false;
    const char *dot = NULL;
    for (const char *p = utf8; *p; p++) if (*p == '.') dot = p;
    size_t bl = dot ? (size_t)(dot - utf8) : len, el = dot ? len - bl - 1 : 0;
    if (bl < 1 || bl > 8 || el > 3) return false;
    memset(sfn, ' ', 11);
    for (size_t i = 0; i < len; i++) {
        if (utf8 + i == dot) continue;
        u32 c = upper_ascii((u8)utf8[i]);
        if ((u8)utf8[i] >= 0x80 || !sfn_ok(c)) return false;
        if (dot && utf8 + i > dot) sfn[8 + (i - bl - 1)] = (u8)c;
        else sfn[i] = (u8)c;
    }
    return true;
}

/* Writes the long-name slots (last piece first, as stored on disk) and returns how many. */
u32 fatn_pack_lfn(const u16 *name, int n, u8 checksum, u8 *out) {
    u32 count = (u32)(n + 12) / 13;
    u16 units[13 * 20 + 13];
    for (int i = 0; i < n; i++) units[i] = name[i];
    if (n % 13) {
        units[n] = 0x0000;
        for (u32 i = (u32)n + 1; i < count * 13; i++) units[i] = 0xFFFF;
    }
    for (u32 k = 0; k < count; k++) {
        u32 seq = count - k;                            /* the piece stored in this slot */
        u8 *e = out + k * 32;
        memset(e, 0, 32);
        e[0] = (u8)(seq | (k == 0 ? 0x40 : 0));
        const u16 *u = units + (seq - 1) * 13;
        for (int j = 0; j < 5; j++) { e[1 + 2 * j] = (u8)u[j]; e[2 + 2 * j] = (u8)(u[j] >> 8); }
        e[11] = FAT_ATTR_LFN;
        e[13] = checksum;
        for (int j = 0; j < 6; j++) { e[14 + 2 * j] = (u8)u[5 + j]; e[15 + 2 * j] = (u8)(u[5 + j] >> 8); }
        for (int j = 0; j < 2; j++) { e[28 + 2 * j] = (u8)u[11 + j]; e[29 + 2 * j] = (u8)(u[11 + j] >> 8); }
    }
    return count;
}
