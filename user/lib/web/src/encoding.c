/*
 * Character encodings for HTML input (the WHATWG Encoding standard and HTML section 13.2.3): encoding labels, BOM sniffing,
 * the <meta charset> pre-scan of the first 1024 bytes, and decoders from UTF-8, windows-1252 (which is also what the
 * labels "iso-8859-1", "latin1" and "ascii" mean on the web), UTF-16LE/BE, Shift_JIS and EUC-JP (both with the JIS X 0208
 * index, EUC-JP also with JIS X 0212, jistables.c) into UTF-8.
 *
 * The decoders also do the HTML "preprocessing the input stream" step: CR LF and lone CR become LF, so the tokenizer only
 * ever sees LF. Every decoder is streaming (state between calls) and replaces malformed input with U+FFFD exactly where
 * the Encoding standard says (UTF-8: the "maximal subpart" rule).
 *
 * Not supported: the other single-byte encodings (ISO-8859-2..16, windows-1250..1258, KOI8), GBK/gb18030, Big5, EUC-KR,
 * ISO-2022-JP: their labels are treated as unknown (the parser then keeps sniffing or uses its default).
 */
#include "webint.h"

extern const uint16_t web_jis0208[];
extern const int web_jis0208_count;
extern const uint16_t web_jis0212[];
extern const int web_jis0212_count;

static const struct { const char *label; int enc; } labels[] = {
    {"unicode-1-1-utf-8", ENC_UTF8}, {"unicode11utf8", ENC_UTF8}, {"unicode20utf8", ENC_UTF8}, {"utf-8", ENC_UTF8},
    {"utf8", ENC_UTF8}, {"x-unicode20utf8", ENC_UTF8},
    {"ansi_x3.4-1968", ENC_WINDOWS_1252}, {"ascii", ENC_WINDOWS_1252}, {"cp1252", ENC_WINDOWS_1252},
    {"cp819", ENC_WINDOWS_1252}, {"csisolatin1", ENC_WINDOWS_1252}, {"ibm819", ENC_WINDOWS_1252},
    {"iso-8859-1", ENC_WINDOWS_1252}, {"iso-ir-100", ENC_WINDOWS_1252}, {"iso8859-1", ENC_WINDOWS_1252},
    {"iso88591", ENC_WINDOWS_1252}, {"iso_8859-1", ENC_WINDOWS_1252}, {"iso_8859-1:1987", ENC_WINDOWS_1252},
    {"l1", ENC_WINDOWS_1252}, {"latin1", ENC_WINDOWS_1252}, {"us-ascii", ENC_WINDOWS_1252},
    {"windows-1252", ENC_WINDOWS_1252}, {"x-cp1252", ENC_WINDOWS_1252},
    {"csunicode", ENC_UTF16LE}, {"iso-10646-ucs-2", ENC_UTF16LE}, {"ucs-2", ENC_UTF16LE}, {"unicode", ENC_UTF16LE},
    {"unicodefeff", ENC_UTF16LE}, {"utf-16", ENC_UTF16LE}, {"utf-16le", ENC_UTF16LE},
    {"unicodefffe", ENC_UTF16BE}, {"utf-16be", ENC_UTF16BE},
    {"csshiftjis", ENC_SHIFT_JIS}, {"ms932", ENC_SHIFT_JIS}, {"ms_kanji", ENC_SHIFT_JIS}, {"shift-jis", ENC_SHIFT_JIS},
    {"shift_jis", ENC_SHIFT_JIS}, {"sjis", ENC_SHIFT_JIS}, {"windows-31j", ENC_SHIFT_JIS}, {"x-sjis", ENC_SHIFT_JIS},
    {"cseucpkdfmtjapanese", ENC_EUC_JP}, {"euc-jp", ENC_EUC_JP}, {"x-euc-jp", ENC_EUC_JP},
    {"x-user-defined", ENC_X_USER_DEFINED},
};

static const char *const names[ENC_COUNT] = {
    "UTF-8", "windows-1252", "UTF-16LE", "UTF-16BE", "Shift_JIS", "EUC-JP", "ISO-2022-JP", "x-user-defined", "replacement",
};

static bool ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

int enc_from_label(const char *s, size_t n) {
    while (n && ws((unsigned char)*s)) { s++; n--; }
    while (n && ws((unsigned char)s[n - 1])) n--;
    if (!n || n > 32) return -1;
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
        if (strlen(labels[i].label) == n && strncasecmp(labels[i].label, s, n) == 0) return labels[i].enc;
    return -1;
}

const char *enc_name(int enc) { return enc >= 0 && enc < ENC_COUNT ? names[enc] : "unknown"; }

int enc_sniff_bom(const unsigned char *p, size_t n, size_t *bom_len) {
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { *bom_len = 3; return ENC_UTF8; }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { *bom_len = 2; return ENC_UTF16BE; }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { *bom_len = 2; return ENC_UTF16LE; }
    *bom_len = 0;
    return -1;
}

/* ------------------------------------------------------------------------------------------------ the pre-scan (13.2.3.2) */

typedef struct { const unsigned char *p; size_t n, pos; } Scan;

static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* "get an attribute": name (lowercased, at most 63 bytes kept) and value; false when there is none */
static bool get_attr(Scan *s, char *name, size_t *nn, char *val, size_t *vn) {
    *nn = *vn = 0;
    while (s->pos < s->n && (ws(s->p[s->pos]) || s->p[s->pos] == '/')) s->pos++;
    if (s->pos >= s->n || s->p[s->pos] == '>') return false;
    for (;;) {
        if (s->pos >= s->n) return true;
        int c = s->p[s->pos];
        if (c == '=' && *nn > 0) { s->pos++; break; }
        if (ws(c)) {
            while (s->pos < s->n && ws(s->p[s->pos])) s->pos++;
            if (s->pos >= s->n || s->p[s->pos] != '=') return true;
            s->pos++;
            break;
        }
        if (c == '/' || c == '>') return true;
        if (*nn < 63) name[(*nn)++] = (char)lowc(c);
        s->pos++;
    }
    while (s->pos < s->n && ws(s->p[s->pos])) s->pos++;
    if (s->pos >= s->n) return true;
    int c = s->p[s->pos];
    if (c == '"' || c == '\'') {
        int q = c;
        s->pos++;
        while (s->pos < s->n && s->p[s->pos] != q) {
            if (*vn < 255) val[(*vn)++] = (char)lowc(s->p[s->pos]);
            s->pos++;
        }
        if (s->pos < s->n) s->pos++;
        return true;
    }
    if (c == '>') return true;
    while (s->pos < s->n && !ws(s->p[s->pos]) && s->p[s->pos] != '>') {
        if (*vn < 255) val[(*vn)++] = (char)lowc(s->p[s->pos]);
        s->pos++;
    }
    return true;
}

/* "the algorithm for extracting a character encoding from a meta element" (content="text/html; charset=...") */
int enc_from_meta_content(const char *v, size_t n) {
    size_t i = 0;
    for (;;) {
        const char *f = NULL;
        for (size_t k = i; k + 7 <= n; k++)
            if (strncasecmp(v + k, "charset", 7) == 0) { f = v + k; break; }
        if (!f) return -1;
        i = (size_t)(f - v) + 7;
        while (i < n && ws((unsigned char)v[i])) i++;
        if (i < n && v[i] == '=') break;
    }
    i++;
    while (i < n && ws((unsigned char)v[i])) i++;
    if (i >= n) return -1;
    if (v[i] == '"' || v[i] == '\'') {
        char q = v[i++];
        size_t s = i;
        while (i < n && v[i] != q) i++;
        if (i >= n) return -1;
        return enc_from_label(v + s, i - s);
    }
    size_t s = i;
    while (i < n && !ws((unsigned char)v[i]) && v[i] != ';') i++;
    return enc_from_label(v + s, i - s);
}

int enc_prescan(const unsigned char *p, size_t n) {
    if (n > 1024) n = 1024;
    Scan s = { p, n, 0 };
    while (s.pos < s.n) {
        const unsigned char *q = s.p + s.pos;
        size_t left = s.n - s.pos;
        if (left >= 4 && memcmp(q, "<!--", 4) == 0) {
            size_t k = s.pos + 2;                              /* the dashes of "<!--" may end it: "<!-->" */
            while (k + 2 < s.n && !(s.p[k] == '-' && s.p[k + 1] == '-' && s.p[k + 2] == '>')) k++;
            if (k + 2 >= s.n) return -1;
            s.pos = k + 3;
            continue;
        }
        if (left >= 6 && q[0] == '<' && lowc(q[1]) == 'm' && lowc(q[2]) == 'e' && lowc(q[3]) == 't' && lowc(q[4]) == 'a' &&
            (ws(q[5]) || q[5] == '/')) {
            s.pos += 6;
            char seen[8][64];
            int nseen = 0;
            bool got_pragma = false;
            int need_pragma = -1;                              /* -1 null, 0 false, 1 true */
            int charset = -1;
            char name[64], val[256];
            size_t nn, vn;
            while (get_attr(&s, name, &nn, val, &vn)) {
                name[nn] = 0;
                bool dup = false;
                for (int i = 0; i < nseen; i++) if (strcmp(seen[i], name) == 0) dup = true;
                if (dup) continue;
                if (nseen < 8) { memcpy(seen[nseen], name, nn + 1); nseen++; }
                if (strcmp(name, "http-equiv") == 0) {
                    if (vn == 12 && memcmp(val, "content-type", 12) == 0) got_pragma = true;
                } else if (strcmp(name, "content") == 0) {
                    if (charset < 0) {
                        int e = enc_from_meta_content(val, vn);
                        if (e >= 0) { charset = e; need_pragma = 1; }
                    }
                } else if (strcmp(name, "charset") == 0) {
                    charset = enc_from_label(val, vn);
                    need_pragma = 0;
                }
            }
            if (need_pragma < 0 || (need_pragma == 1 && !got_pragma) || charset < 0) continue;
            if (charset == ENC_UTF16LE || charset == ENC_UTF16BE) charset = ENC_UTF8;
            if (charset == ENC_X_USER_DEFINED) charset = ENC_WINDOWS_1252;
            return charset;
        }
        if (left >= 2 && q[0] == '<' && ((q[1] | 0x20) >= 'a' && (q[1] | 0x20) <= 'z')) {
            s.pos += 1;
            while (s.pos < s.n && !ws(s.p[s.pos]) && s.p[s.pos] != '>') s.pos++;
            char name[64], val[256];
            size_t nn, vn;
            while (get_attr(&s, name, &nn, val, &vn)) {}
            continue;
        }
        if (left >= 3 && q[0] == '<' && q[1] == '/' && ((q[2] | 0x20) >= 'a' && (q[2] | 0x20) <= 'z')) {
            s.pos += 2;
            while (s.pos < s.n && !ws(s.p[s.pos]) && s.p[s.pos] != '>') s.pos++;
            char name[64], val[256];
            size_t nn, vn;
            while (get_attr(&s, name, &nn, val, &vn)) {}
            continue;
        }
        if (left >= 2 && q[0] == '<' && (q[1] == '!' || q[1] == '/' || q[1] == '?')) {
            while (s.pos < s.n && s.p[s.pos] != '>') s.pos++;
            s.pos++;
            continue;
        }
        s.pos++;
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------------ decoders */

static const uint16_t w1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

void dec_init(WebDecoder *d, int enc) {
    memset(d, 0, sizeof *d);
    d->enc = enc;
}

/* Emits a code point with the newline normalization. */
static bool put(WebDecoder *d, WebBuf *out, uint32_t cp) {
    if (cp == '\r') { d->cr = true; return buf_putc(out, '\n'); }
    if (cp == '\n' && d->cr) { d->cr = false; return true; }
    d->cr = false;
    if (cp < 0x80) return buf_putc(out, (char)cp);
    return buf_put_cp(out, cp);
}

/* ASCII without CR copied in one go: the common case of every decoder here */
static size_t ascii_run(WebDecoder *d, const unsigned char *p, size_t n, WebBuf *out) {
    size_t i = 0;
    while (i < n && p[i] < 0x80 && p[i] != '\r' && !(p[i] == '\n' && d->cr && i == 0)) i++;
    if (i) {
        d->cr = false;
        buf_put(out, p, i);
    }
    return i;
}

static bool utf8_dec(WebDecoder *d, const unsigned char *p, size_t n, bool last, WebBuf *out) {
    /* state[0] = code point so far, state[1] = bytes needed, state[2] = bytes seen, state[3] = lower | upper << 8 */
    uint32_t cp = d->state[0], need = d->state[1], seen = d->state[2];
    uint32_t lower = d->state[3] & 0xFF, upper = d->state[3] >> 8;
    if (!lower) { lower = 0x80; upper = 0xBF; }
    size_t i = 0;
    while (i < n) {
        if (need == 0) {
            size_t r = ascii_run(d, p + i, n - i, out);
            i += r;
            if (i >= n) break;
            unsigned b = p[i++];
            if (b < 0x80) put(d, out, b);
            else if (b >= 0xC2 && b <= 0xDF) { need = 1; cp = b & 0x1F; }
            else if (b >= 0xE0 && b <= 0xEF) { if (b == 0xE0) lower = 0xA0; if (b == 0xED) upper = 0x9F; need = 2; cp = b & 0xF; }
            else if (b >= 0xF0 && b <= 0xF4) { if (b == 0xF0) lower = 0x90; if (b == 0xF4) upper = 0x8F; need = 3; cp = b & 0x7; }
            else put(d, out, 0xFFFD);
            continue;
        }
        unsigned b = p[i];
        if (b < lower || b > upper) {
            cp = need = seen = 0;
            lower = 0x80; upper = 0xBF;
            put(d, out, 0xFFFD);
            continue;                                         /* the byte is processed again */
        }
        i++;
        lower = 0x80; upper = 0xBF;
        cp = (cp << 6) | (b & 0x3F);
        if (++seen != need) continue;
        put(d, out, cp);
        cp = need = seen = 0;
    }
    if (last && need) { put(d, out, 0xFFFD); cp = need = seen = 0; lower = 0x80; upper = 0xBF; }
    d->state[0] = cp; d->state[1] = need; d->state[2] = seen; d->state[3] = lower | (upper << 8);
    return true;
}

static bool w1252_dec(WebDecoder *d, const unsigned char *p, size_t n, WebBuf *out) {
    size_t i = 0;
    while (i < n) {
        i += ascii_run(d, p + i, n - i, out);
        if (i >= n) break;
        unsigned b = p[i++];
        if (b < 0x80) put(d, out, b);
        else if (b < 0xA0) put(d, out, w1252[b - 0x80]);
        else put(d, out, b);
    }
    return true;
}

static bool xud_dec(WebDecoder *d, const unsigned char *p, size_t n, WebBuf *out) {
    for (size_t i = 0; i < n; i++) put(d, out, p[i] < 0x80 ? p[i] : 0xF780 + p[i] - 0x80);
    return true;
}

static bool utf16_dec(WebDecoder *d, const unsigned char *p, size_t n, bool last, WebBuf *out, bool be) {
    /* state[0]: a pending lead byte + 1 (0 = none); state[1]: a pending lead surrogate (0 = none) */
    uint32_t lead_byte = d->state[0], lead_sur = d->state[1];
    for (size_t i = 0; i < n; i++) {
        if (!lead_byte) { lead_byte = (uint32_t)p[i] + 1; continue; }
        uint32_t u = be ? ((lead_byte - 1) << 8) | p[i] : ((uint32_t)p[i] << 8) | (lead_byte - 1);
        lead_byte = 0;
        if (lead_sur) {
            uint32_t ls = lead_sur;
            lead_sur = 0;
            if (u >= 0xDC00 && u <= 0xDFFF) { put(d, out, 0x10000 + ((ls - 0xD800) << 10) + (u - 0xDC00)); continue; }
            put(d, out, 0xFFFD);
        }
        if (u >= 0xD800 && u <= 0xDBFF) { lead_sur = u; continue; }
        if (u >= 0xDC00 && u <= 0xDFFF) { put(d, out, 0xFFFD); continue; }
        put(d, out, u);
    }
    if (last && (lead_byte || lead_sur)) { put(d, out, 0xFFFD); lead_byte = lead_sur = 0; }
    d->state[0] = lead_byte; d->state[1] = lead_sur;
    return true;
}

static uint32_t jis(const uint16_t *t, int count, uint32_t ptr) {
    return ptr < (uint32_t)count ? t[ptr] : 0;
}

static bool sjis_dec(WebDecoder *d, const unsigned char *p, size_t n, bool last, WebBuf *out) {
    uint32_t lead = d->state[0];
    size_t i = 0;
    while (i < n) {
        unsigned b = p[i];
        if (lead) {
            uint32_t l = lead, ptr = 0xFFFFFFFF;
            lead = 0;
            uint32_t off = b < 0x7F ? 0x40 : 0x41, loff = l < 0xA0 ? 0x81 : 0xC1;
            if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC)) ptr = (l - loff) * 188 + b - off;
            if (ptr >= 8836 && ptr <= 10715) { put(d, out, 0xE000 - 8836 + ptr); i++; continue; }
            uint32_t cp = ptr != 0xFFFFFFFF ? jis(web_jis0208, web_jis0208_count, ptr) : 0;
            if (cp) { put(d, out, cp); i++; continue; }
            put(d, out, 0xFFFD);
            if (b < 0x80) continue;                            /* an ASCII byte is processed again */
            i++;
            continue;
        }
        i++;
        if (b <= 0x80) put(d, out, b);
        else if (b >= 0xA1 && b <= 0xDF) put(d, out, 0xFF61 - 0xA1 + b);
        else if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) lead = b;
        else put(d, out, 0xFFFD);
    }
    if (last && lead) { put(d, out, 0xFFFD); lead = 0; }
    d->state[0] = lead;
    return true;
}

static bool eucjp_dec(WebDecoder *d, const unsigned char *p, size_t n, bool last, WebBuf *out) {
    uint32_t lead = d->state[0], j212 = d->state[1];
    size_t i = 0;
    while (i < n) {
        unsigned b = p[i];
        if (lead == 0x8E && b >= 0xA1 && b <= 0xDF) { lead = 0; put(d, out, 0xFF61 - 0xA1 + b); i++; continue; }
        if (lead == 0x8F && b >= 0xA1 && b <= 0xFE) { j212 = 1; lead = b; i++; continue; }
        if (lead) {
            uint32_t l = lead, cp = 0;
            lead = 0;
            if (l >= 0xA1 && l <= 0xFE && b >= 0xA1 && b <= 0xFE) {
                uint32_t ptr = (l - 0xA1) * 94 + b - 0xA1;
                cp = j212 ? jis(web_jis0212, web_jis0212_count, ptr) : jis(web_jis0208, web_jis0208_count, ptr);
            }
            j212 = 0;
            if (cp) { put(d, out, cp); i++; continue; }
            put(d, out, 0xFFFD);
            if (b < 0x80) continue;
            i++;
            continue;
        }
        i++;
        if (b < 0x80) put(d, out, b);
        else if (b == 0x8E || b == 0x8F || (b >= 0xA1 && b <= 0xFE)) lead = b;
        else put(d, out, 0xFFFD);
    }
    if (last && lead) { put(d, out, 0xFFFD); lead = 0; }
    d->state[0] = lead; d->state[1] = j212;
    return true;
}

/* The parse errors of "preprocessing the input stream": surrogates, noncharacters and controls other than ASCII white
 * space and NUL, reported for decoded (UTF-8/WTF-8) text. */
void enc_input_errors(const char *s, size_t n, void (*cb)(void *user, int code, size_t offset), void *user) {
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if ((c < 0x20 && c != 0 && c != '\t' && c != '\n' && c != '\f' && c != '\r') || c == 0x7F)
                cb(user, PE_CONTROL_CHARACTER_IN_INPUT_STREAM, i);
            i++;
            continue;
        }
        size_t len;
        uint32_t cp = utf8_decode(s + i, n - i, &len);
        if (cp >= 0xD800 && cp <= 0xDFFF) cb(user, PE_SURROGATE_IN_INPUT_STREAM, i);
        else if ((cp >= 0xFDD0 && cp <= 0xFDEF) || (cp & 0xFFFE) == 0xFFFE) cb(user, PE_NONCHARACTER_IN_INPUT_STREAM, i);
        else if (cp >= 0x80 && cp <= 0x9F) cb(user, PE_CONTROL_CHARACTER_IN_INPUT_STREAM, i);
        i += len;
    }
}

bool dec_run(WebDecoder *d, const unsigned char *p, size_t n, bool last, WebBuf *out) {
    switch (d->enc) {
    case ENC_UTF8: return utf8_dec(d, p, n, last, out);
    case ENC_WINDOWS_1252: return w1252_dec(d, p, n, out);
    case ENC_UTF16LE: return utf16_dec(d, p, n, last, out, false);
    case ENC_UTF16BE: return utf16_dec(d, p, n, last, out, true);
    case ENC_SHIFT_JIS: return sjis_dec(d, p, n, last, out);
    case ENC_EUC_JP: return eucjp_dec(d, p, n, last, out);
    case ENC_X_USER_DEFINED: return xud_dec(d, p, n, out);
    default: return w1252_dec(d, p, n, out);
    }
}
