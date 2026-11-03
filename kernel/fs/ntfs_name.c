/*
 * NTFS names and times: UTF-16 <-> UTF-8, the collation of directory indexes, FILETIME -> Unix seconds.
 *
 * A directory index is ordered by file name compared case-insensitively: each UTF-16 code unit is mapped through the volume's own $UpCase table (65536 words)
 * and the results are compared unit by unit, the shorter name first when one is a prefix of the other (libfsntfs, "index values"). The table comes from the
 * volume, so lookups agree with the order the volume's creator used; ntfs_upcase_builtin() is only the fallback when the table is unusable.
 */
#include "ntfs_priv.h"

/* UTF-8 (what the VFS and users type) -> UTF-16 units. Overlong forms, surrogates and values past U+10FFFF are refused. */
int ntfs_utf8_to_utf16(const char *s, u16 *out, u32 cap) {
    const u8 *p = (const u8 *)s;
    u32 n = 0;
    while (*p) {
        u32 c = *p++, cp, extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else return -1;
        for (u32 i = 0; i < extra; i++) {
            if ((*p & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return -1;     /* overlong */
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return -1;
        if (cp >= 0x10000) {
            if (n + 2 > cap) return -1;
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 + (cp >> 10));
            out[n++] = (u16)(0xDC00 + (cp & 0x3FF));
        } else {
            if (n + 1 > cap) return -1;
            out[n++] = (u16)cp;
        }
    }
    return (int)n;
}

/* UTF-16LE (unaligned bytes) -> UTF-8. A lone surrogate becomes U+FFFD. -1 when it does not fit in `cap` bytes or contains a NUL or a slash, which no
 * path component of this system can carry. */
int ntfs_utf16_to_utf8(const u8 *le, u32 n, char *out, u32 cap) {
    u32 o = 0;
    for (u32 i = 0; i < n; i++) {
        u32 c = nt_le16(le + 2 * i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && nt_le16(le + 2 * (i + 1)) >= 0xDC00 && nt_le16(le + 2 * (i + 1)) <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (nt_le16(le + 2 * (i + 1)) - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c == 0 || c == '/') return -1;
        u32 need = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (o + need > cap) return -1;
        if (need == 1) {
            out[o++] = (char)c;
        } else if (need == 2) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (need == 3) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return (int)o;
}

/* FILETIME (100 ns steps since 1601-01-01) -> seconds since 1970; anything before 1970 reads as 0. */
u64 ntfs_time_to_unix(u64 ft) {
    u64 secs = ft / 10000000ull;
    return secs < 11644473600ull ? 0 : secs - 11644473600ull;
}

/* The fallback table: ASCII, Latin-1, Greek and Cyrillic folding by arithmetic. Only used when the volume's $UpCase cannot be read. */
void ntfs_upcase_builtin(u16 *t) {
    for (u32 i = 0; i < 65536; i++) t[i] = (u16)i;
    for (u32 c = 'a'; c <= 'z'; c++) t[c] = (u16)(c - 32);
    for (u32 c = 0xE0; c <= 0xFE; c++) if (c != 0xF7) t[c] = (u16)(c - 32);
    t[0xFF] = 0x178;
    for (u32 c = 0x3B1; c <= 0x3C9; c++) if (c != 0x3C2) t[c] = (u16)(c - 32);
    t[0x3C2] = 0x3A3;
    for (u32 c = 0x430; c <= 0x44F; c++) t[c] = (u16)(c - 32);
    for (u32 c = 0x450; c <= 0x45F; c++) t[c] = (u16)(c - 80);
}

/* Collation order of the (already upper-cased) target against an on-disk name. */
int ntfs_name_cmp(const NtfsVol *v, const u16 *tup, u32 tn, const u8 *key, u32 kn) {
    u32 n = tn < kn ? tn : kn;
    for (u32 i = 0; i < n; i++) {
        u16 b = v->upcase[nt_le16(key + 2 * i)];
        if (tup[i] != b) return tup[i] < b ? -1 : 1;
    }
    return tn == kn ? 0 : (tn < kn ? -1 : 1);
}

bool ntfs_name_equal_exact(const u16 *target, u32 tn, const u8 *key, u32 kn) {
    if (tn != kn) return false;
    for (u32 i = 0; i < tn; i++) if (target[i] != nt_le16(key + 2 * i)) return false;
    return true;
}
