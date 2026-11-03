/*
 * OpenType-CFF outlines: the 'CFF ' table (Adobe Technical Note 5176) and its Type 2 charstrings (Technical Note 5177).
 *
 * Only what drawing needs is read: the header, the INDEX structures, the Top DICT (CharStrings, Private, and for CID-keyed fonts
 * FDArray and FDSelect), and each Private DICT's local subroutines. Names, encodings and charsets are not needed because OpenType
 * addresses glyphs by index through 'cmap'. Hints (stems, hint masks) are parsed to find their length and then ignored, as is the
 * glyph width (the advance comes from 'hmtx'). Not supported: the deprecated four-argument `endchar` that composes an accented
 * character from two others (seac), and CFF2.
 *
 * Cubic Bezier curves are kept as cubics in the outline; the rasteriser flattens them. Limits keep hostile charstrings in check:
 * call depth 10 (the specification's), a stack of 96 operands, an operator budget per glyph, and the outline size limit.
 */
#include "ttfint.h"

#define CS_DEPTH      10
#define CS_STACK      96
#define CS_MAX_STEPS  (1u << 20)

/* ---------------------------------------------------------------- INDEX structures */

static uint32_t rd_off(Span s, size_t pos, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v = (v << 8) | sp_u8(s, pos + (size_t)i);
    return v;
}

/* Checks an INDEX at `off` and reports its shape and where it ends. */
static int index_parse(Span cf, uint32_t off, TtfCffSubrs *ix, uint32_t *end) {
    if (!sp_has(cf, off, 2)) return -1;
    uint32_t n = sp_u16(cf, off);
    ix->off = off;
    ix->count = n;
    ix->offsize = 0;
    if (n == 0) { *end = off + 2; return 0; }
    uint32_t os = sp_u8(cf, (size_t)off + 2);
    if (os < 1 || os > 4) return -1;
    size_t arr = (size_t)off + 3;
    if (!sp_has(cf, arr, (size_t)(n + 1) * os)) return -1;
    uint32_t last = rd_off(cf, arr + (size_t)n * os, (int)os);
    if (last < 1) return -1;
    size_t data_end = arr + (size_t)(n + 1) * os - 1 + last;       /* offsets are 1-based from the byte before the data */
    if (data_end > cf.n) return -1;
    ix->offsize = (uint8_t)os;
    *end = (uint32_t)data_end;
    return 0;
}

/* Element i of an INDEX that index_parse accepted. */
static int index_get(Span cf, const TtfCffSubrs *ix, uint32_t i, Span *out) {
    if (i >= ix->count || ix->offsize == 0) return -1;
    size_t os = ix->offsize, arr = (size_t)ix->off + 3;
    size_t base = arr + (size_t)(ix->count + 1) * os - 1;
    uint32_t a = rd_off(cf, arr + (size_t)i * os, (int)os), b = rd_off(cf, arr + (size_t)(i + 1) * os, (int)os);
    if (a < 1 || b < a || base + b > cf.n) return -1;
    *out = sp_make(cf.p + base + a, b - a);
    return 0;
}

/* ---------------------------------------------------------------- DICTs */

typedef int (*DictCb)(void *ctx, uint32_t op, const int32_t *a, int n);

static int dict_walk(Span d, DictCb cb, void *ctx) {
    int32_t a[48];
    int n = 0;
    size_t i = 0;
    while (i < d.n) {
        uint32_t b0 = d.p[i++];
        if (b0 <= 21) {
            uint32_t op = b0;
            if (b0 == 12) {
                if (i >= d.n) return -1;
                op = 0x0C00u | d.p[i++];
            }
            if (cb(ctx, op, a, n)) return -1;
            n = 0;
            continue;
        }
        if (n >= 48) return -1;
        if (b0 == 28) {
            if (!sp_has(d, i, 2)) return -1;
            a[n++] = sp_i16(d, i);
            i += 2;
        } else if (b0 == 29) {
            if (!sp_has(d, i, 4)) return -1;
            a[n++] = sp_i32(d, i);
            i += 4;
        } else if (b0 == 30) {                                      /* a real number: nibbles up to an 0xF; the values we need are integers */
            while (i < d.n) {
                uint32_t b = d.p[i++];
                if ((b & 0xF) == 0xF || (b >> 4) == 0xF) break;
            }
            a[n++] = 0;
        } else if (b0 >= 32 && b0 <= 246) {
            a[n++] = (int32_t)b0 - 139;
        } else if (b0 >= 247 && b0 <= 250) {
            if (i >= d.n) return -1;
            a[n++] = ((int32_t)b0 - 247) * 256 + d.p[i++] + 108;
        } else if (b0 >= 251 && b0 <= 254) {
            if (i >= d.n) return -1;
            a[n++] = -((int32_t)b0 - 251) * 256 - d.p[i++] - 108;
        } else {
            return -1;                                              /* reserved operator bytes */
        }
    }
    return 0;
}

typedef struct TopInfo {
    int32_t charstrings, fdarray, fdselect, priv_size, priv_off;
    int ros;
} TopInfo;

static int top_cb(void *ctx, uint32_t op, const int32_t *a, int n) {
    TopInfo *t = (TopInfo *)ctx;
    switch (op) {
    case 17: if (n >= 1) t->charstrings = a[n - 1]; break;
    case 18: if (n >= 2) { t->priv_size = a[n - 2]; t->priv_off = a[n - 1]; } break;
    case 0x0C1E: t->ros = 1; break;                                /* 12 30: ROS, the sign of a CID-keyed font */
    case 0x0C24: if (n >= 1) t->fdarray = a[n - 1]; break;          /* 12 36 */
    case 0x0C25: if (n >= 1) t->fdselect = a[n - 1]; break;         /* 12 37 */
    default: break;
    }
    return 0;
}

static int priv_cb(void *ctx, uint32_t op, const int32_t *a, int n) {
    if (op == 19 && n >= 1) *(int32_t *)ctx = a[n - 1];            /* Subrs: offset relative to the Private DICT */
    return 0;
}

/* Reads a Private DICT at (off, size) and fills `out` with its local subroutines (none: count 0). */
static int read_private(Span cf, int32_t size, int32_t off, TtfCffSubrs *out) {
    memset(out, 0, sizeof *out);
    if (size <= 0 || off <= 0) return 0;
    if (!sp_has(cf, (size_t)off, (size_t)size)) return -1;
    int32_t subrs = 0;
    if (dict_walk(sp_make(cf.p + off, (size_t)size), priv_cb, &subrs)) return -1;
    if (subrs <= 0) return 0;
    uint32_t end;
    if ((uint32_t)off + (uint32_t)subrs < (uint32_t)off) return -1;
    if (index_parse(cf, (uint32_t)off + (uint32_t)subrs, out, &end)) { memset(out, 0, sizeof *out); return -1; }
    return 0;
}

typedef struct FdInfo { int32_t priv_size, priv_off; } FdInfo;

static int fd_cb(void *ctx, uint32_t op, const int32_t *a, int n) {
    FdInfo *fd = (FdInfo *)ctx;
    if (op == 18 && n >= 2) { fd->priv_size = a[n - 2]; fd->priv_off = a[n - 1]; }
    return 0;
}

int ttfi_cff_open(TtfFont *f) {
    Span cf = sp_table(f, f->cff);
    if (cf.n < 4 || sp_u8(cf, 0) != 1) return TTF_E_UNSUPPORTED;     /* major version 1 only */
    uint32_t pos = sp_u8(cf, 2), end;
    TtfCffSubrs ix;
    if (index_parse(cf, pos, &ix, &end)) return TTF_E_CORRUPT;       /* Name INDEX */
    pos = end;
    if (index_parse(cf, pos, &ix, &end) || ix.count < 1) return TTF_E_CORRUPT;      /* Top DICT INDEX */
    Span top;
    if (index_get(cf, &ix, 0, &top)) return TTF_E_CORRUPT;
    pos = end;
    if (index_parse(cf, pos, &ix, &end)) return TTF_E_CORRUPT;       /* String INDEX */
    pos = end;
    if (index_parse(cf, pos, &f->cff_gsubrs, &end)) return TTF_E_CORRUPT;      /* Global Subr INDEX */

    TopInfo t;
    memset(&t, 0, sizeof t);
    if (dict_walk(top, top_cb, &t) || t.charstrings <= 0) return TTF_E_CORRUPT;
    TtfCffSubrs cs;
    if (index_parse(cf, (uint32_t)t.charstrings, &cs, &end) || cs.count < 1) return TTF_E_CORRUPT;
    f->cff_charstrings_off = cs.off;
    f->cff_charstrings_count = cs.count;
    f->cff_charstrings_offsize = cs.offsize;

    if (t.ros) {
        TtfCffSubrs fda;
        if (t.fdarray <= 0 || t.fdselect <= 0) return TTF_E_CORRUPT;
        if (index_parse(cf, (uint32_t)t.fdarray, &fda, &end)) return TTF_E_CORRUPT;
        int nfd = fda.count > TTF_MAX_FDS ? TTF_MAX_FDS : (int)fda.count;
        for (int i = 0; i < nfd; i++) {
            Span fdict;
            FdInfo fi = { 0, 0 };
            if (index_get(cf, &fda, (uint32_t)i, &fdict) || dict_walk(fdict, fd_cb, &fi)) return TTF_E_CORRUPT;
            if (read_private(cf, fi.priv_size, fi.priv_off, &f->cff_fd[i])) return TTF_E_CORRUPT;
        }
        int fmt = (int)sp_u8(cf, (size_t)t.fdselect);
        if (fmt == 0) {
            if (!sp_has(cf, (size_t)t.fdselect + 1, cs.count)) return TTF_E_CORRUPT;
        } else if (fmt == 3) {
            if (!sp_has(cf, (size_t)t.fdselect + 3, 3u * sp_u16(cf, (size_t)t.fdselect + 1) + 2)) return TTF_E_CORRUPT;
        } else {
            return TTF_E_CORRUPT;
        }
        f->cff_cid = 1;
        f->cff_nfd = nfd;
        f->cff_fdselect_off = (uint32_t)t.fdselect;
        f->cff_fdselect_format = fmt;
    } else {
        if (read_private(cf, t.priv_size, t.priv_off, &f->cff_fd[0])) return TTF_E_CORRUPT;
        f->cff_nfd = 1;
    }
    return TTF_OK;
}

/* Which font dictionary holds glyph `gid` (CID-keyed fonts); 0 for plain CFF. */
static int fd_for_glyph(const TtfFont *f, Span cf, uint32_t gid) {
    if (!f->cff_cid) return 0;
    size_t o = f->cff_fdselect_off;
    if (f->cff_fdselect_format == 0) return (int)sp_u8(cf, o + 1 + gid);
    uint32_t nr = sp_u16(cf, o + 1), lo = 0, hi = nr;
    while (lo < hi) {                                               /* the last range that starts at or before gid */
        uint32_t mid = lo + (hi - lo) / 2;
        if (sp_u16(cf, o + 3 + 3u * mid) <= gid) lo = mid + 1; else hi = mid;
    }
    return lo ? (int)sp_u8(cf, o + 3 + 3u * (lo - 1) + 2) : 0;
}

/* ---------------------------------------------------------------- the charstring interpreter */

typedef struct Cs {
    Span cf;
    TtfOutline *o;
    const TtfCffSubrs *gsubrs, *lsubrs;
    float st[CS_STACK];
    int sp;
    float x, y;
    int open;                                                       /* a contour has been started and not yet closed */
    int nhints;
    int width_done;                                                 /* the first stack-clearing operator has been seen */
    uint32_t steps;
    float trans[32];                                                /* the transient array of put/get */
} Cs;

typedef struct Frame { Span code; size_t pos; } Frame;

static int push(Cs *c, float v) {
    if (c->sp >= CS_STACK) return TTF_E_GLYPH;
    c->st[c->sp++] = v;
    return 0;
}

static int subr_bias(uint32_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

/* Arithmetic operators can build any float; converting an out-of-range or NaN float to int is undefined, so clamp first. */
static int safe_int(float v) {
    if (!(v > -1073741824.0f)) return v == v ? -1073741824 : 0;
    if (v > 1073741824.0f) return 1073741824;
    return (int)v;
}

static int finish_contour(Cs *c) {
    if (!c->open) return 0;
    c->open = 0;
    return ttfi_path_close(c->o);
}

/* A moveto only moves the current point. The contour's first point is written when the first line or curve is drawn, so a moveto that
 * nothing follows (some fonts end a glyph with one) leaves no stray point to stretch the glyph's bounding box. */
static int move_to(Cs *c, float dx, float dy) {
    int rc = finish_contour(c);
    if (rc) return rc;
    c->x += dx;
    c->y += dy;
    return 0;
}

static int ensure_open(Cs *c) {
    if (c->open) return 0;
    c->open = 1;
    return ttfi_path_move(c->o, c->x, c->y);
}

static int line_by(Cs *c, float dx, float dy) {
    int rc = ensure_open(c);
    if (rc) return rc;
    c->x += dx;
    c->y += dy;
    return ttfi_path_line(c->o, c->x, c->y);
}

static int curve_by(Cs *c, float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
    int rc = ensure_open(c);
    if (rc) return rc;
    float x1 = c->x + dx1, y1 = c->y + dy1, x2 = x1 + dx2, y2 = y1 + dy2, x3 = x2 + dx3, y3 = y2 + dy3;
    c->x = x3;
    c->y = y3;
    return ttfi_path_cubic(c->o, x1, y1, x2, y2, x3, y3);
}

/* The first operator that clears the stack may carry the glyph's width as an extra first operand: drop it. `extra` says whether the
 * operand count shows that it is there. */
static void take_width(Cs *c, int extra) {
    if (!c->width_done) {
        c->width_done = 1;
        if (extra && c->sp > 0) {
            for (int i = 1; i < c->sp; i++) c->st[i - 1] = c->st[i];
            c->sp--;
        }
    }
}

static float newton_sqrt(float v) {
    if (!(v > 0.0f)) return 0.0f;
    float x = v > 1.0f ? v : 1.0f;
    for (int i = 0; i < 40; i++) x = 0.5f * (x + v / x);
    return x;
}

static int op_hv_curves(Cs *c, int horizontal) {
    int n = c->sp, i = 0;
    if (n < 4) return TTF_E_GLYPH;
    const float *a = c->st;
    while (n - i >= 4) {
        float last = (n - i == 5) ? a[i + 4] : 0.0f;               /* the single extra operand belongs to the final curve */
        int rc;
        if (horizontal) rc = curve_by(c, a[i], 0, a[i + 1], a[i + 2], last, a[i + 3]);
        else rc = curve_by(c, 0, a[i], a[i + 1], a[i + 2], a[i + 3], last);
        if (rc) return rc;
        horizontal = !horizontal;
        i += 4;
    }
    return 0;
}

static int run(Cs *c, Span top) {
    Frame fr[CS_DEPTH + 1];
    int d = 0;
    fr[0].code = top;
    fr[0].pos = 0;
    int rc;
    for (;;) {
        Frame *F = &fr[d];
        if (F->pos >= F->code.n) {                                  /* ran off the end of a subroutine (or the glyph) without a return */
            if (d == 0) return finish_contour(c);
            d--;
            continue;
        }
        if (++c->steps > CS_MAX_STEPS) return TTF_E_TOOBIG;
        uint32_t b0 = F->code.p[F->pos++];

        if (b0 >= 32 || b0 == 28) {                                 /* operands */
            float v;
            if (b0 == 28) {
                if (!sp_has(F->code, F->pos, 2)) return TTF_E_GLYPH;
                v = (float)sp_i16(F->code, F->pos);
                F->pos += 2;
            } else if (b0 <= 246) {
                v = (float)((int)b0 - 139);
            } else if (b0 <= 250) {
                if (F->pos >= F->code.n) return TTF_E_GLYPH;
                v = (float)(((int)b0 - 247) * 256 + F->code.p[F->pos++] + 108);
            } else if (b0 <= 254) {
                if (F->pos >= F->code.n) return TTF_E_GLYPH;
                v = (float)(-((int)b0 - 251) * 256 - F->code.p[F->pos++] - 108);
            } else {
                if (!sp_has(F->code, F->pos, 4)) return TTF_E_GLYPH;
                v = (float)sp_i32(F->code, F->pos) / 65536.0f;
                F->pos += 4;
            }
            rc = push(c, v);
            if (rc) return rc;
            continue;
        }

        float *s = c->st;
        int n = c->sp;
        switch (b0) {
        case 1: case 3: case 18: case 23:                           /* hstem vstem hstemhm vstemhm */
            take_width(c, n & 1);
            c->nhints += c->sp / 2;
            c->sp = 0;
            break;
        case 19: case 20: {                                         /* hintmask cntrmask: operands are implied vstem hints, then mask bytes */
            take_width(c, n & 1);
            c->nhints += c->sp / 2;
            c->sp = 0;
            size_t mask = (size_t)(c->nhints + 7) / 8;
            if (mask > F->code.n - F->pos) return TTF_E_GLYPH;
            F->pos += mask;
            break;
        }
        case 21:                                                    /* rmoveto */
            take_width(c, n > 2);
            if (c->sp < 2) return TTF_E_GLYPH;
            rc = move_to(c, c->st[c->sp - 2], c->st[c->sp - 1]);
            c->sp = 0;
            if (rc) return rc;
            break;
        case 22:                                                    /* hmoveto */
            take_width(c, n > 1);
            if (c->sp < 1) return TTF_E_GLYPH;
            rc = move_to(c, c->st[c->sp - 1], 0);
            c->sp = 0;
            if (rc) return rc;
            break;
        case 4:                                                     /* vmoveto */
            take_width(c, n > 1);
            if (c->sp < 1) return TTF_E_GLYPH;
            rc = move_to(c, 0, c->st[c->sp - 1]);
            c->sp = 0;
            if (rc) return rc;
            break;
        case 14:                                                    /* endchar (the four-argument seac form is not composed) */
            take_width(c, n == 1 || n == 5);
            c->sp = 0;
            return finish_contour(c);
        case 5:                                                     /* rlineto */
            c->width_done = 1;
            for (int i = 0; i + 1 < n; i += 2) { rc = line_by(c, s[i], s[i + 1]); if (rc) return rc; }
            c->sp = 0;
            break;
        case 6: case 7: {                                           /* hlineto vlineto: alternate, starting horizontal or vertical */
            c->width_done = 1;
            int horiz = b0 == 6;
            for (int i = 0; i < n; i++) {
                rc = horiz ? line_by(c, s[i], 0) : line_by(c, 0, s[i]);
                if (rc) return rc;
                horiz = !horiz;
            }
            c->sp = 0;
            break;
        }
        case 8:                                                     /* rrcurveto */
            c->width_done = 1;
            for (int i = 0; i + 5 < n; i += 6) { rc = curve_by(c, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]); if (rc) return rc; }
            c->sp = 0;
            break;
        case 24: {                                                  /* rcurveline: curves, then one line */
            c->width_done = 1;
            int i = 0;
            for (; i + 8 <= n; i += 6) { rc = curve_by(c, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]); if (rc) return rc; }
            if (i + 2 <= n) { rc = line_by(c, s[i], s[i + 1]); if (rc) return rc; }
            c->sp = 0;
            break;
        }
        case 25: {                                                  /* rlinecurve: lines, then one curve */
            c->width_done = 1;
            int i = 0;
            for (; i + 8 <= n; i += 2) { rc = line_by(c, s[i], s[i + 1]); if (rc) return rc; }
            if (i + 6 <= n) { rc = curve_by(c, s[i], s[i + 1], s[i + 2], s[i + 3], s[i + 4], s[i + 5]); if (rc) return rc; }
            c->sp = 0;
            break;
        }
        case 26: {                                                  /* vvcurveto: dx1? {dya dxb dyb dyc}+ */
            c->width_done = 1;
            int i = 0;
            float dx1 = 0;
            if (n & 1) dx1 = s[i++];
            for (; i + 4 <= n; i += 4) { rc = curve_by(c, dx1, s[i], s[i + 1], s[i + 2], 0, s[i + 3]); dx1 = 0; if (rc) return rc; }
            c->sp = 0;
            break;
        }
        case 27: {                                                  /* hhcurveto: dy1? {dxa dxb dyb dxc}+ */
            c->width_done = 1;
            int i = 0;
            float dy1 = 0;
            if (n & 1) dy1 = s[i++];
            for (; i + 4 <= n; i += 4) { rc = curve_by(c, s[i], dy1, s[i + 1], s[i + 2], s[i + 3], 0); dy1 = 0; if (rc) return rc; }
            c->sp = 0;
            break;
        }
        case 30: case 31:                                           /* vhcurveto hvcurveto */
            c->width_done = 1;
            rc = op_hv_curves(c, b0 == 31);
            c->sp = 0;
            if (rc) return rc;
            break;
        case 10: case 29: {                                         /* callsubr callgsubr */
            if (c->sp < 1) return TTF_E_GLYPH;
            const TtfCffSubrs *ix = b0 == 10 ? c->lsubrs : c->gsubrs;
            int idx = safe_int(c->st[--c->sp]) + subr_bias(ix->count);
            Span code;
            if (idx < 0 || index_get(c->cf, ix, (uint32_t)idx, &code)) return TTF_E_GLYPH;
            if (d >= CS_DEPTH) return TTF_E_TOOBIG;
            d++;
            fr[d].code = code;
            fr[d].pos = 0;
            break;
        }
        case 11:                                                    /* return */
            if (d == 0) return finish_contour(c);
            d--;
            break;
        case 12: {                                                  /* two-byte operators */
            if (F->pos >= F->code.n) return TTF_E_GLYPH;
            uint32_t b1 = F->code.p[F->pos++];
            float a, b;
            switch (b1) {
            case 0: break;                                          /* dotsection (obsolete) */
            case 3: case 4: case 10: case 11: case 12: case 15: case 24: {      /* binary operators */
                if (n < 2) return TTF_E_GLYPH;
                b = s[--c->sp];
                a = s[--c->sp];
                float v = 0;
                switch (b1) {
                case 3: v = (a != 0 && b != 0); break;
                case 4: v = (a != 0 || b != 0); break;
                case 10: v = a + b; break;
                case 11: v = a - b; break;
                case 12: v = b != 0 ? a / b : 0; break;
                case 15: v = (a == b); break;
                default: v = a * b; break;
                }
                push(c, v);
                break;
            }
            case 5: case 9: case 14: case 26: {                     /* unary operators */
                if (n < 1) return TTF_E_GLYPH;
                a = s[c->sp - 1];
                s[c->sp - 1] = b1 == 5 ? (a == 0) : b1 == 9 ? (a < 0 ? -a : a) : b1 == 14 ? -a : newton_sqrt(a);
                break;
            }
            case 18:                                                /* drop */
                if (n < 1) return TTF_E_GLYPH;
                c->sp--;
                break;
            case 20: {                                              /* put: value index */
                if (n < 2) return TTF_E_GLYPH;
                int idx = safe_int(s[c->sp - 1]);
                a = s[c->sp - 2];
                c->sp -= 2;
                if (idx >= 0 && idx < 32) c->trans[idx] = a;
                break;
            }
            case 21: {                                              /* get */
                if (n < 1) return TTF_E_GLYPH;
                int idx = safe_int(s[c->sp - 1]);
                s[c->sp - 1] = idx >= 0 && idx < 32 ? c->trans[idx] : 0;
                break;
            }
            case 22: {                                              /* ifelse: s1 s2 v1 v2 -> s1 if v1 <= v2 else s2 */
                if (n < 4) return TTF_E_GLYPH;
                float s1 = s[c->sp - 4], s2 = s[c->sp - 3], v1 = s[c->sp - 2], v2 = s[c->sp - 1];
                c->sp -= 4;
                push(c, v1 <= v2 ? s1 : s2);
                break;
            }
            case 23:                                                /* random: deterministic, there is no entropy to draw here */
                rc = push(c, 0.5f);
                if (rc) return rc;
                break;
            case 27:                                                /* dup */
                if (n < 1) return TTF_E_GLYPH;
                rc = push(c, s[c->sp - 1]);
                if (rc) return rc;
                break;
            case 28:                                                /* exch */
                if (n < 2) return TTF_E_GLYPH;
                a = s[c->sp - 1]; s[c->sp - 1] = s[c->sp - 2]; s[c->sp - 2] = a;
                break;
            case 29: {                                              /* index: a negative index means the top element */
                if (n < 1) return TTF_E_GLYPH;
                int idx = safe_int(s[c->sp - 1]);
                if (idx < 0) idx = 0;
                if (idx >= c->sp - 1) return TTF_E_GLYPH;
                s[c->sp - 1] = s[c->sp - 2 - idx];
                break;
            }
            case 30: {                                              /* roll: N J */
                if (n < 2) return TTF_E_GLYPH;
                int cnt = safe_int(s[c->sp - 2]), j = safe_int(s[c->sp - 1]);
                c->sp -= 2;
                if (cnt < 0 || cnt > c->sp) return TTF_E_GLYPH;
                if (cnt > 1) {
                    float tmp[CS_STACK];
                    float *base = c->st + c->sp - cnt;
                    int sh = ((j % cnt) + cnt) % cnt;
                    for (int i = 0; i < cnt; i++) tmp[(i + sh) % cnt] = base[i];
                    for (int i = 0; i < cnt; i++) base[i] = tmp[i];
                }
                break;
            }
            case 34:                                                /* hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6 */
                c->width_done = 1;
                if (n < 7) return TTF_E_GLYPH;
                rc = curve_by(c, s[0], 0, s[1], s[2], s[3], 0);
                if (!rc) rc = curve_by(c, s[4], 0, s[5], -s[2], s[6], 0);
                c->sp = 0;
                if (rc) return rc;
                break;
            case 35:                                                /* flex: twelve deltas and a flex depth */
                c->width_done = 1;
                if (n < 12) return TTF_E_GLYPH;
                rc = curve_by(c, s[0], s[1], s[2], s[3], s[4], s[5]);
                if (!rc) rc = curve_by(c, s[6], s[7], s[8], s[9], s[10], s[11]);
                c->sp = 0;
                if (rc) return rc;
                break;
            case 36:                                                /* hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6; ends at the starting height */
                c->width_done = 1;
                if (n < 9) return TTF_E_GLYPH;
                rc = curve_by(c, s[0], s[1], s[2], s[3], s[4], 0);
                if (!rc) rc = curve_by(c, s[5], 0, s[6], s[7], s[8], -(s[1] + s[3] + s[7]));
                c->sp = 0;
                if (rc) return rc;
                break;
            case 37: {                                              /* flex1: five deltas pairs and d6, which is x or y by the larger drift */
                c->width_done = 1;
                if (n < 11) return TTF_E_GLYPH;
                float dx = s[0] + s[2] + s[4] + s[6] + s[8], dy = s[1] + s[3] + s[5] + s[7] + s[9];
                float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                rc = curve_by(c, s[0], s[1], s[2], s[3], s[4], s[5]);
                if (!rc) {
                    if (adx > ady) rc = curve_by(c, s[6], s[7], s[8], s[9], s[10], -dy);
                    else rc = curve_by(c, s[6], s[7], s[8], s[9], -dx, s[10]);
                }
                c->sp = 0;
                if (rc) return rc;
                break;
            }
            default:
                return TTF_E_GLYPH;
            }
            break;
        }
        default:
            return TTF_E_GLYPH;                                     /* reserved operators */
        }
    }
}

int ttfi_cff_outline(TtfRaster *r, const TtfFont *f, uint32_t glyph) {
    Span cf = sp_table(f, f->cff);
    TtfCffSubrs cs;
    memset(&cs, 0, sizeof cs);
    cs.off = f->cff_charstrings_off;
    cs.count = f->cff_charstrings_count;
    cs.offsize = f->cff_charstrings_offsize;
    Span code;
    if (index_get(cf, &cs, glyph, &code)) return TTF_E_GLYPH;
    int fd = fd_for_glyph(f, cf, glyph);
    if (fd < 0 || fd >= f->cff_nfd) return TTF_E_GLYPH;
    Cs c;                                                           /* about 700 bytes: fine on the stack */
    memset(&c, 0, sizeof c);
    c.cf = cf;
    c.o = &r->outline;
    c.gsubrs = &f->cff_gsubrs;
    c.lsubrs = &f->cff_fd[fd];
    return run(&c, code);
}
