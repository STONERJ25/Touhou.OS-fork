/*
 * CSS property values: what each longhand accepts (CSS 2.1, CSS Values 4, Color 4, Backgrounds 3, Fonts 4, Flexbox 1,
 * Grid 1, Lists 3, Generated Content), parsed into specified values (SVal) that style.c computes, and the shorthands and
 * aliases (logical properties of horizontal left-to-right text, -webkit-/-moz-/-ms- prefixes) expanded into longhands.
 *
 * Errors follow CSS: an invalid value drops the whole declaration. A value with var() is kept as text and parsed when
 * the variables are known (style.c). Lengths may be calc(), min(), max(), clamp() expressions (CssCalc trees).
 * Quirks mode accepts unitless numbers as pixels for the properties the quirks standard lists.
 */
#include "css.h"

#include <math.h>

/* ================================================================================================ keyword / property tables */

static const char *const kw_names[K_COUNT] = {
#define K(id, s) s,
#include "csskw.h"
#undef K
};

static const char *const prop_names[CP_COUNT] = {
#define P(id, s, inh) s,
#include "cssprops.h"
#undef P
};

const uint8_t css_prop_inherited[CP_COUNT] = {
#define P(id, s, inh) inh,
#include "cssprops.h"
#undef P
};

int css_keyword(const char *s, size_t n) {
    if (!n || n > 40) return -1;
    for (int i = 1; i < K_COUNT; i++)
        if (strlen(kw_names[i]) == n && strncasecmp(kw_names[i], s, n) == 0) return i;
    return -1;
}

const char *css_keyword_name(int k) { return k > 0 && k < K_COUNT ? kw_names[k] : ""; }

int css_property(const char *s, size_t n) {
    for (int i = 0; i < CP_COUNT; i++)
        if (strlen(prop_names[i]) == n && strncasecmp(prop_names[i], s, n) == 0) return i;
    return -1;
}

const char *css_property_name(int p) { return p >= 0 && p < CP_COUNT ? prop_names[p] : ""; }

/* ================================================================================================ token cursor */

typedef struct Cur {
    const CssTokens *t;
    uint32_t i, end;
    WebDoc *d;
} Cur;

static void skip_ws(Cur *c) {
    while (c->i < c->end && c->t->v[c->i].type == CT_WS) c->i++;
}

static const CssTok *peek(Cur *c) {
    skip_ws(c);
    return c->i < c->end ? &c->t->v[c->i] : NULL;
}

static bool at_end(Cur *c) { return peek(c) == NULL; }

static const CssTok *take(Cur *c) {
    const CssTok *k = peek(c);
    if (!k) return NULL;
    /* a function or block is one component value: skip to after its closer */
    if (k->type == CT_FUNCTION || k->type == CT_LPAREN || k->type == CT_LBRACK || k->type == CT_LBRACE)
        c->i = k->close < c->end ? k->close + 1 : c->end;
    else c->i++;
    return k;
}

static const char *kval(const Cur *c, const CssTok *k) { return tok_val(c->t, k); }

static int ident_kw(const Cur *c, const CssTok *k) {
    if (!k || k->type != CT_IDENT) return -1;
    return css_keyword(kval(c, k), k->vn);
}

static bool is_delim(const CssTok *k, int ch) { return k && k->type == CT_DELIM && k->delim == ch; }

/* the inside of a function token as a cursor */
static Cur inside(const Cur *c, const CssTok *f) {
    Cur s = *c;
    s.i = (uint32_t)(f - c->t->v) + 1;
    s.end = f->close < c->end ? f->close : c->end;
    return s;
}

static bool fname_is(const Cur *c, const CssTok *k, const char *name) {
    return k && k->type == CT_FUNCTION && k->vn == strlen(name) && strncasecmp(kval(c, k), name, k->vn) == 0;
}

static char *dupval(WebDoc *d, const char *s, size_t n) { return wa_strndup(&d->arena, s, n); }

/* ================================================================================================ colours */

static const struct { const char *name; uint32_t rgb; } named_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A}, {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0},
    {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B},
    {"darkgoldenrod", 0xB8860B}, {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00},
    {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F},
    {"darkslateblue", 0x483D8B}, {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0},
    {"forestgreen", 0x228B22}, {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C}, {"indigo", 0x4B0082},
    {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5},
    {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3}, {"lightgreen", 0x90EE90},
    {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA},
    {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500}, {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093},
    {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD},
    {"powderblue", 0xB0E0E6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xFF0000},
    {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072},
    {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D},
    {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB}, {"slateblue", 0x6A5ACD}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C},
    {"teal", 0x008080}, {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
    /* CSS system colours, with the values of a light scheme */
    {"canvas", 0xFFFFFF}, {"canvastext", 0x000000}, {"linktext", 0x0000EE}, {"visitedtext", 0x551A8B},
    {"activetext", 0xFF0000}, {"buttonface", 0xEFEFEF}, {"buttontext", 0x000000}, {"buttonborder", 0x767676},
    {"field", 0xFFFFFF}, {"fieldtext", 0x000000}, {"highlight", 0x3390FF}, {"highlighttext", 0xFFFFFF},
    {"selecteditem", 0x3390FF}, {"selecteditemtext", 0xFFFFFF}, {"mark", 0xFFFF00}, {"marktext", 0x000000},
    {"graytext", 0x808080}, {"accentcolor", 0x3367D6}, {"accentcolortext", 0xFFFFFF},
    {"window", 0xFFFFFF}, {"windowtext", 0x000000}, {"buttonshadow", 0x888888}, {"threedface", 0xEFEFEF},
};

bool css_named_color(const char *s, size_t n, WebColor *out) {
    for (size_t i = 0; i < sizeof named_colors / sizeof named_colors[0]; i++)
        if (strlen(named_colors[i].name) == n && strncasecmp(named_colors[i].name, s, n) == 0) {
            *out = 0xFF000000u | named_colors[i].rgb;
            return true;
        }
    return false;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool css_hex_color(const char *s, size_t n, WebColor *out) {
    int v[8];
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    for (size_t i = 0; i < n; i++) if ((v[i] = hexv(s[i])) < 0) return false;
    uint32_t r, g, b, a = 255;
    if (n <= 4) {
        r = (uint32_t)v[0] * 17; g = (uint32_t)v[1] * 17; b = (uint32_t)v[2] * 17;
        if (n == 4) a = (uint32_t)v[3] * 17;
    } else {
        r = (uint32_t)(v[0] * 16 + v[1]); g = (uint32_t)(v[2] * 16 + v[3]); b = (uint32_t)(v[4] * 16 + v[5]);
        if (n == 8) a = (uint32_t)(v[6] * 16 + v[7]);
    }
    *out = a << 24 | r << 16 | g << 8 | b;
    return true;
}

static uint32_t clamp255(double v) {
    if (!(v > 0)) return 0;                                  /* also NaN */
    if (v >= 255) return 255;
    return (uint32_t)(v + 0.5);
}

/* one channel of rgb(): number 0..255 or percentage; alpha: number 0..1 or percentage */
static bool channel(Cur *c, bool alpha, double *out) {
    const CssTok *k = take(c);
    if (!k) return false;
    if (k->type == CT_NUMBER) { *out = alpha ? k->num * 255 : k->num; return true; }
    if (k->type == CT_PERCENT) { *out = k->num * 2.55; return true; }
    if (k->type == CT_IDENT && tok_is_ident(c->t, k, "none")) { *out = alpha ? 255 : 0; return true; }
    return false;
}

static double hue2rgb(double m1, double m2, double h) {
    if (h < 0) h += 1;
    if (h > 1) h -= 1;
    if (h * 6 < 1) return m1 + (m2 - m1) * h * 6;
    if (h * 2 < 1) return m2;
    if (h * 3 < 2) return m1 + (m2 - m1) * (2.0 / 3 - h) * 6;
    return m1;
}

static bool angle_deg(const Cur *c, const CssTok *k, double *deg) {
    if (k->type == CT_NUMBER) { *deg = k->num; return true; }
    if (k->type != CT_DIMENSION) return false;
    const char *u = kval(c, k);
    if (k->vn == 3 && strncasecmp(u, "deg", 3) == 0) *deg = k->num;
    else if (k->vn == 3 && strncasecmp(u, "rad", 3) == 0) *deg = k->num * 180 / M_PI;
    else if (k->vn == 4 && strncasecmp(u, "grad", 4) == 0) *deg = k->num * 0.9;
    else if (k->vn == 4 && strncasecmp(u, "turn", 4) == 0) *deg = k->num * 360;
    else return false;
    return true;
}

/* rgb()/rgba()/hsl()/hsla()/hwb() in both the legacy comma syntax and the space syntax with "/ alpha" */
static bool color_function(Cur *c, const CssTok *f, WebColor *out) {
    Cur in = inside(c, f);
    const char *name = kval(c, f);
    size_t nn = f->vn;
    bool rgb = (nn == 3 && strncasecmp(name, "rgb", 3) == 0) || (nn == 4 && strncasecmp(name, "rgba", 4) == 0);
    bool hsl = (nn == 3 && strncasecmp(name, "hsl", 3) == 0) || (nn == 4 && strncasecmp(name, "hsla", 4) == 0);
    bool hwb = nn == 3 && strncasecmp(name, "hwb", 3) == 0;
    if (!rgb && !hsl && !hwb) return false;
    double v[4] = { 0, 0, 0, 255 };
    for (int i = 0; i < 4; i++) {
        const CssTok *k = peek(&in);
        if (!k) { if (i < 3) return false; break; }
        if (i > 0) {
            if (k->type == CT_COMMA) { take(&in); k = peek(&in); }
            else if (is_delim(k, '/')) { if (i != 3) return false; take(&in); k = peek(&in); }
            else if (i == 3) return false;
        }
        if (!k) return false;
        if (i == 0 && (hsl || hwb)) {
            take(&in);
            if (!angle_deg(c, k, &v[0])) {
                if (tok_is_ident(c->t, k, "none")) v[0] = 0;
                else return false;
            }
            continue;
        }
        if (i < 3 && (hsl || hwb)) {
            take(&in);
            if (k->type == CT_PERCENT || k->type == CT_NUMBER) v[i] = k->num;
            else if (tok_is_ident(c->t, k, "none")) v[i] = 0;
            else return false;
            continue;
        }
        if (!channel(&in, i == 3, &v[i])) return false;
    }
    if (!at_end(&in)) return false;
    uint32_t a = clamp255(v[3]);
    if (rgb) {
        *out = a << 24 | clamp255(v[0]) << 16 | clamp255(v[1]) << 8 | clamp255(v[2]);
        return true;
    }
    double h = fmod(v[0], 360);
    if (h < 0) h += 360;
    h /= 360;
    double r, g, b;
    if (hsl) {
        double s = v[1] / 100, l = v[2] / 100;
        if (s < 0) s = 0;
        if (s > 1) s = 1;
        if (l < 0) l = 0;
        if (l > 1) l = 1;
        double m2 = l <= 0.5 ? l * (s + 1) : l + s - l * s, m1 = l * 2 - m2;
        r = hue2rgb(m1, m2, h + 1.0 / 3);
        g = hue2rgb(m1, m2, h);
        b = hue2rgb(m1, m2, h - 1.0 / 3);
    } else {
        double w = v[1] / 100, bl = v[2] / 100;
        if (w + bl >= 1) { double gray = w / (w + bl); r = g = b = gray; }
        else {
            r = hue2rgb(0, 1, h + 1.0 / 3) * (1 - w - bl) + w;
            g = hue2rgb(0, 1, h) * (1 - w - bl) + w;
            b = hue2rgb(0, 1, h - 1.0 / 3) * (1 - w - bl) + w;
        }
    }
    *out = a << 24 | clamp255(r * 255) << 16 | clamp255(g * 255) << 8 | clamp255(b * 255);
    return true;
}

static bool parse_color_tok(Cur *c, WebColor *out) {
    const CssTok *k = peek(c);
    if (!k) return false;
    if (k->type == CT_HASH) {
        if (!css_hex_color(kval(c, k), k->vn, out)) return false;
        take(c);
        return true;
    }
    if (k->type == CT_IDENT) {
        const char *s = kval(c, k);
        if (k->vn == 11 && strncasecmp(s, "transparent", 11) == 0) { *out = 0; take(c); return true; }
        if (k->vn == 12 && strncasecmp(s, "currentcolor", 12) == 0) { *out = COLOR_CURRENT; take(c); return true; }
        if (css_named_color(s, k->vn, out)) { take(c); return true; }
        /* quirks mode: hex digits without the '#' ("ff0000") */
        if (c->d && c->d->quirks == 2 && css_hex_color(s, k->vn, out) && (k->vn == 3 || k->vn == 6)) { take(c); return true; }
        return false;
    }
    if (k->type == CT_FUNCTION) {
        if (color_function(c, k, out)) { take(c); return true; }
        return false;
    }
    if (c->d && c->d->quirks == 2 && (k->type == CT_NUMBER || k->type == CT_DIMENSION) && k->flags & CTF_INT) {
        /* quirks: "#" omitted before digits, e.g. color: 008000 */
        char buf[16];
        size_t n = 0;
        const char *src = c->t->src + k->pos;
        for (uint32_t i = k->pos; i < k->end && n < 15; i++) buf[n++] = src[i - k->pos];
        if ((n == 3 || n == 6) && css_hex_color(buf, n, out)) { take(c); return true; }
    }
    return false;
}

bool css_parse_color(const CssTokens *t, uint32_t *i, uint32_t end, WebColor *out) {
    Cur c = { t, *i, end, NULL };
    if (!parse_color_tok(&c, out)) return false;
    *i = c.i;
    return true;
}

/* ================================================================================================ lengths and calc() */

enum { CALC_NUM, CALC_LEN, CALC_ADD, CALC_SUB, CALC_MUL, CALC_DIV, CALC_MIN, CALC_MAX, CALC_CLAMP };
struct CssCalc {
    uint8_t op, unit;
    float v;
    CssCalc *a, *b, *c;
    CssCalc **args;
    int nargs;
};

static int unit_of(const char *u, size_t n) {
    static const struct { const char *s; int u; float scale; } units[] = {
        {"px", SU_PX, 1}, {"em", SU_EM, 1}, {"rem", SU_REM, 1}, {"ex", SU_EX, 1}, {"ch", SU_CH, 1}, {"vw", SU_VW, 1},
        {"vh", SU_VH, 1}, {"vmin", SU_VMIN, 1}, {"vmax", SU_VMAX, 1}, {"fr", SU_FR, 1}, {"lh", SU_LH, 1},
        {"svw", SU_VW, 1}, {"lvw", SU_VW, 1}, {"dvw", SU_VW, 1}, {"svh", SU_VH, 1}, {"lvh", SU_VH, 1}, {"dvh", SU_VH, 1},
        {"cqw", SU_VW, 1}, {"cqh", SU_VH, 1}, {"vi", SU_VW, 1}, {"vb", SU_VH, 1},
    };
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++)
        if (strlen(units[i].s) == n && strncasecmp(units[i].s, u, n) == 0) return units[i].u;
    return -1;
}

/* absolute units become px: 1in = 96px = 72pt = 6pc = 2.54cm = 25.4mm = 101.6Q */
static bool abs_unit(const char *u, size_t n, float *scale) {
    static const struct { const char *s; float k; } a[] = {
        {"in", 96}, {"pt", 96.0f / 72}, {"pc", 16}, {"cm", 96 / 2.54f}, {"mm", 96 / 25.4f}, {"q", 96 / 101.6f},
    };
    for (size_t i = 0; i < sizeof a / sizeof a[0]; i++)
        if (strlen(a[i].s) == n && strncasecmp(a[i].s, u, n) == 0) { *scale = a[i].k; return true; }
    return false;
}

/* A <length> or <percentage> token: SV_LEN with SU_* (absolute units converted to px). */
static bool length_token(const Cur *c, const CssTok *k, SVal *v, bool quirky) {
    if (k->type == CT_DIMENSION) {
        const char *u = kval(c, k);
        float scale;
        int su = unit_of(u, k->vn);
        if (su >= 0) { v->kind = SV_LEN; v->unit = (uint8_t)su; v->num = (float)k->num; return true; }
        if (abs_unit(u, k->vn, &scale)) { v->kind = SV_LEN; v->unit = SU_PX; v->num = (float)(k->num * scale); return true; }
        return false;
    }
    if (k->type == CT_PERCENT) { v->kind = SV_LEN; v->unit = SU_PCT; v->num = (float)k->num; return true; }
    if (k->type == CT_NUMBER && (k->num == 0 || quirky)) { v->kind = SV_LEN; v->unit = SU_PX; v->num = (float)k->num; return true; }
    return false;
}

static CssCalc *calc_node(WebDoc *d, int op) {
    CssCalc *n = wa_alloc(&d->arena, sizeof *n);
    if (n) n->op = (uint8_t)op;
    return n;
}

static CssCalc *calc_sum(Cur *c, int depth);

static CssCalc *calc_value(Cur *c, int depth) {
    const CssTok *k = peek(c);
    if (!k || depth > 32) return NULL;
    if (k->type == CT_NUMBER) {
        take(c);
        CssCalc *n = calc_node(c->d, CALC_NUM);
        if (n) n->v = (float)k->num;
        return n;
    }
    if (k->type == CT_DIMENSION || k->type == CT_PERCENT) {
        SVal v;
        memset(&v, 0, sizeof v);
        if (!length_token(c, k, &v, false)) return NULL;
        take(c);
        CssCalc *n = calc_node(c->d, CALC_LEN);
        if (n) { n->unit = v.unit; n->v = v.num; }
        return n;
    }
    if (k->type == CT_LPAREN || fname_is(c, k, "calc")) {
        take(c);
        Cur in = inside(c, k);
        CssCalc *r = calc_sum(&in, depth + 1);
        return r && at_end(&in) ? r : NULL;
    }
    if (fname_is(c, k, "min") || fname_is(c, k, "max") || fname_is(c, k, "clamp")) {
        take(c);
        Cur in = inside(c, k);
        CssCalc *n = calc_node(c->d, fname_is(c, k, "min") ? CALC_MIN : fname_is(c, k, "max") ? CALC_MAX : CALC_CLAMP);
        if (!n) return NULL;
        CssCalc *args[16];
        int na = 0;
        for (;;) {
            CssCalc *a = calc_sum(&in, depth + 1);
            if (!a || na == 16) return NULL;
            args[na++] = a;
            const CssTok *s = peek(&in);
            if (!s) break;
            if (s->type != CT_COMMA) return NULL;
            take(&in);
        }
        if (n->op == CALC_CLAMP && na != 3) return NULL;
        n->args = wa_alloc(&c->d->arena, sizeof(CssCalc *) * (size_t)na);
        if (!n->args) return NULL;
        memcpy(n->args, args, sizeof(CssCalc *) * (size_t)na);
        n->nargs = na;
        return n;
    }
    if (k->type == CT_IDENT) {                                /* the constants of CSS Values 4 */
        double v = 0;
        if (tok_is_ident(c->t, k, "pi")) v = M_PI;
        else if (tok_is_ident(c->t, k, "e")) v = M_E;
        else return NULL;
        take(c);
        CssCalc *n = calc_node(c->d, CALC_NUM);
        if (n) n->v = (float)v;
        return n;
    }
    return NULL;
}

static CssCalc *calc_product(Cur *c, int depth) {
    CssCalc *l = calc_value(c, depth);
    while (l) {
        const CssTok *k = peek(c);
        if (!is_delim(k, '*') && !is_delim(k, '/')) break;
        take(c);
        CssCalc *r = calc_value(c, depth);
        if (!r) return NULL;
        CssCalc *n = calc_node(c->d, is_delim(k, '*') ? CALC_MUL : CALC_DIV);
        if (!n) return NULL;
        n->a = l;
        n->b = r;
        l = n;
    }
    return l;
}

static CssCalc *calc_sum(Cur *c, int depth) {
    CssCalc *l = calc_product(c, depth);
    while (l) {
        /* + and - must have white space around them, so a number token like "-5px" is not an operator */
        const CssTok *k = peek(c);
        if (!is_delim(k, '+') && !is_delim(k, '-')) break;
        take(c);
        CssCalc *r = calc_product(c, depth);
        if (!r) return NULL;
        CssCalc *n = calc_node(c->d, is_delim(k, '+') ? CALC_ADD : CALC_SUB);
        if (!n) return NULL;
        n->a = l;
        n->b = r;
        l = n;
    }
    return l;
}

static bool calc_function(Cur *c, const CssTok *k) {
    return fname_is(c, k, "calc") || fname_is(c, k, "min") || fname_is(c, k, "max") || fname_is(c, k, "clamp") ||
           fname_is(c, k, "-webkit-calc") || fname_is(c, k, "-moz-calc");
}

/* calc() whose value is a plain number (no lengths): flex-grow: calc(1 + 1) */
static bool calc_number_only(const CssCalc *n, double *out);

/* length flags */
#define LF_NEG 1
#define LF_PCT 2
#define LF_AUTO 4
#define LF_NONE 8
#define LF_NORMAL 16
#define LF_INTRINSIC 32
#define LF_QUIRKY 64
#define LF_NUMBER 128         /* line-height: a plain number */
#define LF_CONTENT 256        /* flex-basis: content */

static bool parse_len(Cur *c, int flags, SVal *v) {
    memset(v, 0, sizeof *v);
    const CssTok *k = peek(c);
    if (!k) return false;
    if (k->type == CT_IDENT) {
        int kw = ident_kw(c, k);
        bool ok = (kw == K_AUTO && (flags & LF_AUTO)) || (kw == K_NONE && (flags & LF_NONE)) ||
                  (kw == K_NORMAL && (flags & LF_NORMAL)) || (kw == K_CONTENT && (flags & LF_CONTENT)) ||
                  ((kw == K_MIN_CONTENT || kw == K_MAX_CONTENT || kw == K_FIT_CONTENT) && (flags & LF_INTRINSIC));
        if (!ok && (flags & LF_INTRINSIC)) {                 /* prefixed sizing keywords */
            if (tok_is_ident(c->t, k, "-webkit-fill-available") || tok_is_ident(c->t, k, "-moz-available") ||
                tok_is_ident(c->t, k, "stretch")) { ok = true; kw = K_AUTO; }
            else if (tok_is_ident(c->t, k, "-webkit-fit-content") || tok_is_ident(c->t, k, "-moz-fit-content")) {
                ok = true;
                kw = K_FIT_CONTENT;
            } else if (tok_is_ident(c->t, k, "-webkit-min-content")) { ok = true; kw = K_MIN_CONTENT; }
            else if (tok_is_ident(c->t, k, "-webkit-max-content")) { ok = true; kw = K_MAX_CONTENT; }
        }
        if (!ok) return false;
        take(c);
        v->kind = SV_KW;
        v->kw = (uint16_t)kw;
        return true;
    }
    if (k->type == CT_FUNCTION && calc_function(c, k)) {
        take(c);
        Cur in = inside(c, k);
        CssCalc *e;
        if (fname_is(c, k, "calc") || fname_is(c, k, "-webkit-calc") || fname_is(c, k, "-moz-calc")) e = calc_sum(&in, 0);
        else { in = *c; in.i = (uint32_t)(k - c->t->v); in.end = k->close + 1; e = calc_value(&in, 0); }
        if (!e || (fname_is(c, k, "calc") && !at_end(&in))) return false;
        double num;
        if (calc_number_only(e, &num)) {
            if (flags & LF_NUMBER) { v->kind = SV_NUM; v->num = (float)num; return true; }
            if (num != 0) return false;
            v->kind = SV_LEN; v->unit = SU_PX; v->num = 0;
            return true;
        }
        v->kind = SV_CALC;
        v->u.calc = e;
        return true;
    }
    if (k->type == CT_NUMBER && (flags & LF_NUMBER)) {
        if (k->num < 0 && !(flags & LF_NEG)) return false;
        take(c);
        v->kind = SV_NUM;
        v->num = (float)k->num;
        return true;
    }
    if (k->type == CT_PERCENT && !(flags & LF_PCT)) return false;
    if (!length_token(c, k, v, (flags & LF_QUIRKY) && c->d && c->d->quirks == 2)) return false;
    if (v->unit == SU_FR) return false;
    if (v->num < 0 && !(flags & LF_NEG)) return false;
    take(c);
    return true;
}

static bool calc_number_only(const CssCalc *n, double *out) {
    switch (n->op) {
    case CALC_NUM: *out = n->v; return true;
    case CALC_LEN: return false;
    case CALC_ADD: case CALC_SUB: case CALC_MUL: case CALC_DIV: {
        double a, b;
        if (!calc_number_only(n->a, &a) || !calc_number_only(n->b, &b)) return false;
        *out = n->op == CALC_ADD ? a + b : n->op == CALC_SUB ? a - b : n->op == CALC_MUL ? a * b : (b != 0 ? a / b : 0);
        return true;
    }
    default: {
        double best = 0;
        for (int i = 0; i < n->nargs; i++) {
            double x;
            if (!calc_number_only(n->args[i], &x)) return false;
            if (i == 0 || (n->op == CALC_MIN ? x < best : x > best)) best = x;
        }
        if (n->op == CALC_CLAMP) {
            double lo, val, hi;
            calc_number_only(n->args[0], &lo);
            calc_number_only(n->args[1], &val);
            calc_number_only(n->args[2], &hi);
            best = val < lo ? lo : val > hi ? hi : val;
            if (best < lo) best = lo;
        }
        *out = best;
        return true;
    }
    }
}

/* Evaluation of a calc tree for style.c and layout: units resolved from an environment, percentages against a basis. */
static float unit_px(const CalcEnv *e, int unit, float v) {
    switch (unit) {
    case SU_EM: case SU_LH: return v * e->em;
    case SU_REM: return v * e->rem;
    case SU_EX: return v * e->ex;
    case SU_CH: return v * e->ch;
    case SU_VW: return v * e->vw / 100;
    case SU_VH: return v * e->vh / 100;
    case SU_VMIN: return v * (e->vw < e->vh ? e->vw : e->vh) / 100;
    case SU_VMAX: return v * (e->vw > e->vh ? e->vw : e->vh) / 100;
    default: return v;
    }
}

/* linear form px + pct; false when min()/max() mix percentages and lengths (then evaluate at layout time) */
bool calc_linear(const CssCalc *n, const CalcEnv *e, float *px, float *pct) {
    switch (n->op) {
    case CALC_NUM: *px = n->v; *pct = 0; return true;      /* a bare number in a length context: treated as px */
    case CALC_LEN:
        if (n->unit == SU_PCT) { *px = 0; *pct = n->v; }
        else { *px = unit_px(e, n->unit, n->v); *pct = 0; }
        return true;
    case CALC_ADD: case CALC_SUB: {
        float ax, ap, bx, bp;
        if (!calc_linear(n->a, e, &ax, &ap) || !calc_linear(n->b, e, &bx, &bp)) return false;
        float s = n->op == CALC_ADD ? 1 : -1;
        *px = ax + s * bx;
        *pct = ap + s * bp;
        return true;
    }
    case CALC_MUL: case CALC_DIV: {
        double k;
        float ax, ap;
        if (n->op == CALC_MUL && calc_number_only(n->a, &k)) {
            if (!calc_linear(n->b, e, &ax, &ap)) return false;
        } else if (calc_number_only(n->b, &k)) {
            if (!calc_linear(n->a, e, &ax, &ap)) return false;
            if (n->op == CALC_DIV) k = k != 0 ? 1 / k : 0;
        } else return false;
        *px = (float)(ax * k);
        *pct = (float)(ap * k);
        return true;
    }
    default: {
        /* min/max/clamp: linear only when no argument has a percentage part */
        float vals[16] = { 0 };
        for (int i = 0; i < n->nargs && i < 16; i++) {
            float p;
            if (!calc_linear(n->args[i], e, &vals[i], &p) || p != 0) return false;
        }
        float best = vals[0];
        if (n->op == CALC_CLAMP) best = vals[1] < vals[0] ? vals[0] : vals[1] > vals[2] ? vals[2] : vals[1];
        else for (int i = 1; i < n->nargs && i < 16; i++) if (n->op == CALC_MIN ? vals[i] < best : vals[i] > best) best = vals[i];
        if (n->op == CALC_CLAMP && best < vals[0]) best = vals[0];
        *px = best;
        *pct = 0;
        return true;
    }
    }
}

CssCalc *calc_resolve(WebArena *a, const CssCalc *n, const CalcEnv *e) {
    CssCalc *r = wa_alloc(a, sizeof *r);
    if (!r) return NULL;
    *r = *n;
    if (n->op == CALC_LEN && n->unit != SU_PCT) { r->unit = SU_PX; r->v = unit_px(e, n->unit, n->v); }
    if (n->a) r->a = calc_resolve(a, n->a, e);
    if (n->b) r->b = calc_resolve(a, n->b, e);
    if (n->nargs) {
        r->args = wa_alloc(a, sizeof(CssCalc *) * (size_t)n->nargs);
        if (!r->args) return NULL;
        for (int i = 0; i < n->nargs; i++) r->args[i] = calc_resolve(a, n->args[i], e);
    }
    return r;
}

float calc_eval(const CssCalc *n, const CalcEnv *e, float basis) {
    switch (n->op) {
    case CALC_NUM: return n->v;
    case CALC_LEN: return n->unit == SU_PCT ? n->v * basis / 100 : unit_px(e, n->unit, n->v);
    case CALC_ADD: return calc_eval(n->a, e, basis) + calc_eval(n->b, e, basis);
    case CALC_SUB: return calc_eval(n->a, e, basis) - calc_eval(n->b, e, basis);
    case CALC_MUL: return calc_eval(n->a, e, basis) * calc_eval(n->b, e, basis);
    case CALC_DIV: { float b = calc_eval(n->b, e, basis); return b != 0 ? calc_eval(n->a, e, basis) / b : 0; }
    default: {
        float best = calc_eval(n->args[0], e, basis);
        if (n->op == CALC_CLAMP) {
            float lo = best, v = calc_eval(n->args[1], e, basis), hi = calc_eval(n->args[2], e, basis);
            best = v < lo ? lo : v > hi ? hi : v;
            return best < lo ? lo : best;
        }
        for (int i = 1; i < n->nargs; i++) {
            float x = calc_eval(n->args[i], e, basis);
            if (n->op == CALC_MIN ? x < best : x > best) best = x;
        }
        return best;
    }
    }
}

/* ================================================================================================ value parsers */

static void set_kw(SVal *v, int kw) { memset(v, 0, sizeof *v); v->kind = SV_KW; v->kw = (uint16_t)kw; }

static bool one_kw(Cur *c, SVal *v, const int *allowed) {
    const CssTok *k = peek(c);
    int kw = ident_kw(c, k);
    if (kw < 0) return false;
    for (int i = 0; allowed[i]; i++)
        if (allowed[i] == kw) { take(c); set_kw(v, kw); return true; }
    return false;
}

#define KWS(...) ((const int[]){ __VA_ARGS__, 0 })

static bool parse_int(Cur *c, SVal *v, bool allow_neg) {
    const CssTok *k = peek(c);
    if (!k) return false;
    if (k->type == CT_FUNCTION && calc_function(c, k)) {
        SVal t;
        if (!parse_len(c, LF_NUMBER | LF_NEG, &t) || t.kind != SV_NUM) return false;
        memset(v, 0, sizeof *v);
        v->kind = SV_NUM;
        v->num = roundf(t.num);
        return allow_neg || v->num >= 0;
    }
    if (k->type != CT_NUMBER || !(k->flags & CTF_INT)) return false;
    if (k->num < 0 && !allow_neg) return false;
    take(c);
    memset(v, 0, sizeof *v);
    v->kind = SV_NUM;
    v->num = (float)k->num;
    return true;
}

static bool parse_number(Cur *c, SVal *v, bool allow_neg) {
    const CssTok *k = peek(c);
    if (!k) return false;
    if (k->type == CT_FUNCTION && calc_function(c, k)) {
        if (!parse_len(c, LF_NUMBER | LF_NEG, v) || v->kind != SV_NUM) return false;
        return allow_neg || v->num >= 0;
    }
    if (k->type != CT_NUMBER || (k->num < 0 && !allow_neg)) return false;
    take(c);
    memset(v, 0, sizeof *v);
    v->kind = SV_NUM;
    v->num = (float)k->num;
    return true;
}

static bool parse_url(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    if (!k) return false;
    if (k->type == CT_URL) {
        take(c);
        memset(v, 0, sizeof *v);
        v->kind = SV_URL;
        v->u.str = dupval(c->d, kval(c, k), k->vn);
        return v->u.str != NULL;
    }
    if (fname_is(c, k, "url") || fname_is(c, k, "src")) {
        Cur in = inside(c, k);
        const CssTok *s = take(&in);
        if (!s || s->type != CT_STRING || !at_end(&in)) return false;
        take(c);
        memset(v, 0, sizeof *v);
        v->kind = SV_URL;
        v->u.str = dupval(c->d, kval(c, s), s->vn);
        return v->u.str != NULL;
    }
    return false;
}

/* <image>: url() or a gradient (kept as its first and last colours: v0 = first, v1 = last; kw = 1 horizontal) */
typedef struct GradientVal { WebColor a, b; bool horizontal; } GradientVal;

static bool parse_gradient(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    if (!k || k->type != CT_FUNCTION) return false;
    const char *n = kval(c, k);
    bool lin = (k->vn >= 15 && strncasecmp(n + k->vn - 15, "linear-gradient", 15) == 0);
    bool other = strstr(n, "gradient") != NULL;
    if (!lin && !other) return false;
    Cur in = inside(c, k);
    take(c);
    GradientVal *g = wa_alloc(&c->d->arena, sizeof *g);
    if (!g) return false;
    bool first = true, got = false;
    while (!at_end(&in)) {
        const CssTok *t0 = peek(&in);
        if (first && t0->type == CT_IDENT && tok_is_ident(in.t, t0, "to")) {
            take(&in);
            const CssTok *dir = take(&in);
            if (dir && (tok_is_ident(in.t, dir, "right") || tok_is_ident(in.t, dir, "left"))) g->horizontal = true;
            while (!at_end(&in) && peek(&in)->type != CT_COMMA) take(&in);
            if (!at_end(&in)) take(&in);
            first = false;
            continue;
        }
        if (first && t0->type == CT_DIMENSION) {                  /* an angle */
            double deg;
            if (angle_deg(&in, t0, &deg)) {
                int a = ((int)deg % 360 + 360) % 360;
                g->horizontal = a == 90 || a == 270;
            }
            while (!at_end(&in) && peek(&in)->type != CT_COMMA) take(&in);
            if (!at_end(&in)) take(&in);
            first = false;
            continue;
        }
        first = false;
        WebColor col;
        if (parse_color_tok(&in, &col)) {
            if (!got) g->a = col;
            g->b = col;
            got = true;
        } else take(&in);
    }
    if (!got) return false;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->kw = 1;                                               /* gradient */
    v->u.ptr = g;
    return true;
}

static bool parse_image(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    if (!k) return false;
    if (ident_kw(c, k) == K_NONE) { take(c); set_kw(v, K_NONE); return true; }
    if (parse_url(c, v)) return true;
    if (parse_gradient(c, v)) return true;
    if (fname_is(c, k, "image-set") || fname_is(c, k, "-webkit-image-set")) {
        Cur in = inside(c, k);
        take(c);
        if (parse_url(&in, v)) return true;
        const CssTok *s = peek(&in);
        if (s && s->type == CT_STRING) {
            memset(v, 0, sizeof *v);
            v->kind = SV_URL;
            v->u.str = dupval(c->d, kval(c, s), s->vn);
            return true;
        }
        return false;
    }
    return false;
}

static bool parse_border_width(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    int kw = ident_kw(c, k);
    if (kw == K_THIN || kw == K_MEDIUM || kw == K_THICK) { take(c); set_kw(v, kw); return true; }
    return parse_len(c, LF_QUIRKY, v);
}

static const int border_styles[] = { K_NONE, K_HIDDEN, K_SOLID, K_DOTTED, K_DASHED, K_DOUBLE, K_GROOVE, K_RIDGE,
                                     K_INSET, K_OUTSET, 0 };

static bool parse_font_family(Cur *c, SVal *v) {
    const char *names[32];
    int n = 0;
    while (!at_end(c)) {
        const CssTok *k = peek(c);
        if (k->type == CT_STRING) {
            take(c);
            if (n < 32) names[n++] = dupval(c->d, kval(c, k), k->vn);
        } else if (k->type == CT_IDENT) {
            /* a sequence of identifiers joined by single spaces */
            WebBuf b;
            buf_init(&b, &c->d->arena);
            while (peek(c) && peek(c)->type == CT_IDENT) {
                const CssTok *w = take(c);
                if (b.n) buf_putc(&b, ' ');
                buf_put(&b, kval(c, w), w->vn);
            }
            if (n < 32) names[n++] = dupval(c->d, b.p ? b.p : "", b.n);
            buf_free(&b);
        } else return false;
        const CssTok *s = peek(c);
        if (!s) break;
        if (s->type != CT_COMMA) return false;
        take(c);
        if (at_end(c)) return false;
    }
    if (!n) return false;
    FontList *fl = wa_alloc(&c->d->arena, sizeof *fl);
    const char **arr = wa_alloc(&c->d->arena, sizeof(char *) * (size_t)n);
    if (!fl || !arr) return false;
    memcpy(arr, names, sizeof(char *) * (size_t)n);
    fl->names = arr;
    fl->n = n;
    uint32_t h = 0;
    for (int i = 0; i < n; i++) h = h * 31 + web_hash(arr[i], strlen(arr[i]));
    fl->hash = h;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = fl;
    return true;
}

static bool parse_font_size(Cur *c, SVal *v) {
    int kw = ident_kw(c, peek(c));
    static const int sizes[] = { K_XX_SMALL, K_X_SMALL, K_SMALL, K_MEDIUM, K_LARGE, K_X_LARGE, K_XX_LARGE, K_XXX_LARGE,
                                 K_SMALLER, K_LARGER, 0 };
    for (int i = 0; sizes[i]; i++)
        if (kw == sizes[i]) { take(c); set_kw(v, kw); return true; }
    return parse_len(c, LF_PCT | LF_QUIRKY, v);
}

static bool parse_font_weight(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    int kw = ident_kw(c, k);
    if (kw == K_NORMAL || kw == K_BOLD || kw == K_BOLDER || kw == K_LIGHTER) { take(c); set_kw(v, kw); return true; }
    if (k && k->type == CT_NUMBER && k->num >= 1 && k->num <= 1000) {
        take(c);
        memset(v, 0, sizeof *v);
        v->kind = SV_NUM;
        v->num = (float)k->num;
        return true;
    }
    return false;
}

static bool parse_line_height(Cur *c, SVal *v) {
    return parse_len(c, LF_NORMAL | LF_NUMBER | LF_PCT | LF_QUIRKY, v);
}

static int quote_kind(int kw) {
    return kw == K_OPEN_QUOTE ? CC_OPEN_QUOTE : kw == K_CLOSE_QUOTE ? CC_CLOSE_QUOTE :
           kw == K_NO_OPEN_QUOTE ? CC_NO_OPEN_QUOTE : kw == K_NO_CLOSE_QUOTE ? CC_NO_CLOSE_QUOTE : -1;
}

static bool parse_content(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    int kw = ident_kw(c, k);
    if (kw == K_NONE || kw == K_NORMAL) {
        Cur after = *c;
        take(&after);
        if (at_end(&after)) {
            *c = after;
            set_kw(v, kw);
            return true;
        }
    }
    ContentItem items[32];
    int n = 0;
    while (!at_end(c) && n < 32) {
        k = peek(c);
        ContentItem it;
        memset(&it, 0, sizeof it);
        if (k->type == CT_STRING) {
            take(c);
            it.kind = CC_STRING;
            it.s = dupval(c->d, kval(c, k), k->vn);
        } else if (k->type == CT_URL || fname_is(c, k, "url")) {
            SVal u;
            if (!parse_url(c, &u)) return false;
            it.kind = CC_URL;
            it.s = u.u.str;
        } else if (fname_is(c, k, "attr")) {
            Cur in = inside(c, k);
            const CssTok *a = take(&in);
            if (!a || a->type != CT_IDENT) return false;
            take(c);
            it.kind = CC_ATTR;
            it.name = atom_get_lower(&c->d->atoms, kval(c, a), a->vn);
        } else if (fname_is(c, k, "counter") || fname_is(c, k, "counters")) {
            bool plural = fname_is(c, k, "counters");
            Cur in = inside(c, k);
            const CssTok *a = take(&in);
            if (!a || a->type != CT_IDENT) return false;
            it.kind = plural ? CC_COUNTERS : CC_COUNTER;
            it.name = atom_get(&c->d->atoms, kval(c, a), a->vn);
            it.style = LT_DECIMAL;
            if (plural) {
                const CssTok *cm = take(&in), *s = take(&in);
                if (!cm || cm->type != CT_COMMA || !s || s->type != CT_STRING) return false;
                it.sep = dupval(c->d, kval(c, s), s->vn);
            }
            const CssTok *cm = peek(&in);
            if (cm && cm->type == CT_COMMA) {
                take(&in);
                const CssTok *st = take(&in);
                int skw = ident_kw(&in, st);
                static const struct { int kw, lt; } map[] = {
                    {K_DISC, LT_DISC}, {K_CIRCLE, LT_CIRCLE}, {K_SQUARE, LT_SQUARE}, {K_DECIMAL, LT_DECIMAL},
                    {K_DECIMAL_LEADING_ZERO, LT_DECIMAL_LZ}, {K_LOWER_ALPHA, LT_LOWER_ALPHA},
                    {K_LOWER_LATIN, LT_LOWER_ALPHA}, {K_UPPER_ALPHA, LT_UPPER_ALPHA}, {K_UPPER_LATIN, LT_UPPER_ALPHA},
                    {K_LOWER_ROMAN, LT_LOWER_ROMAN}, {K_UPPER_ROMAN, LT_UPPER_ROMAN}, {K_LOWER_GREEK, LT_LOWER_GREEK},
                    {K_NONE, LT_NONE},
                };
                for (size_t m = 0; m < sizeof map / sizeof map[0]; m++)
                    if (map[m].kw == skw) it.style = (uint8_t)map[m].lt;
            }
            take(c);
        } else if (is_delim(k, '/')) {                       /* alternative text: content: "x" / "alt" */
            break;
        } else if (k->type == CT_IDENT && quote_kind(ident_kw(c, k)) >= 0) {
            it.kind = (uint8_t)quote_kind(ident_kw(c, k));
            take(c);
        } else return false;
        items[n++] = it;
    }
    if (!n) return false;
    CssContent *cc = wa_alloc(&c->d->arena, sizeof *cc);
    ContentItem *arr = wa_alloc(&c->d->arena, sizeof(ContentItem) * (size_t)n);
    if (!cc || !arr) return false;
    memcpy(arr, items, sizeof(ContentItem) * (size_t)n);
    cc->v = arr;
    cc->n = n;
    while (!at_end(c)) take(c);                                /* the alternative text after '/' is ignored */
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = cc;
    return true;
}

static bool parse_counters(Cur *c, SVal *v, int dflt) {
    if (ident_kw(c, peek(c)) == K_NONE) { take(c); set_kw(v, K_NONE); return true; }
    CounterOp ops[16];
    int n = 0;
    while (!at_end(c)) {
        const CssTok *k = take(c);
        if (!k || k->type != CT_IDENT || n == 16) return false;
        ops[n].name = atom_get(&c->d->atoms, kval(c, k), k->vn);
        ops[n].value = dflt;
        const CssTok *num = peek(c);
        if (num && num->type == CT_NUMBER && (num->flags & CTF_INT)) { take(c); ops[n].value = (int)num->num; }
        n++;
    }
    if (!n) return false;
    CounterList *cl = wa_alloc(&c->d->arena, sizeof *cl);
    CounterOp *arr = wa_alloc(&c->d->arena, sizeof(CounterOp) * (size_t)n);
    if (!cl || !arr) return false;
    memcpy(arr, ops, sizeof(CounterOp) * (size_t)n);
    cl->v = arr;
    cl->n = n;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = cl;
    return true;
}

static bool parse_quotes(Cur *c, SVal *v) {
    int kw = ident_kw(c, peek(c));
    if (kw == K_NONE || kw == K_AUTO) { take(c); set_kw(v, kw); return true; }
    const char *q[16];
    int n = 0;
    while (!at_end(c)) {
        const CssTok *k = take(c);
        if (!k || k->type != CT_STRING || n == 16) return false;
        q[n++] = dupval(c->d, kval(c, k), k->vn);
    }
    if (!n || n % 2) return false;
    Quotes *qs = wa_alloc(&c->d->arena, sizeof *qs);
    const char **arr = wa_alloc(&c->d->arena, sizeof(char *) * (size_t)n);
    if (!qs || !arr) return false;
    memcpy(arr, q, sizeof(char *) * (size_t)n);
    qs->v = arr;
    qs->n = n;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = qs;
    return true;
}

static bool parse_list_type(Cur *c, SVal *v) {
    const CssTok *k = peek(c);
    if (k && k->type == CT_STRING) {
        take(c);
        memset(v, 0, sizeof *v);
        v->kind = SV_STR;
        v->u.str = dupval(c->d, kval(c, k), k->vn);
        return true;
    }
    int kw = ident_kw(c, k);
    static const int types[] = { K_NONE, K_DISC, K_CIRCLE, K_SQUARE, K_DECIMAL, K_DECIMAL_LEADING_ZERO, K_LOWER_ALPHA,
                                 K_UPPER_ALPHA, K_LOWER_LATIN, K_UPPER_LATIN, K_LOWER_ROMAN, K_UPPER_ROMAN, K_LOWER_GREEK,
                                 K_HIRAGANA, K_KATAKANA, K_CJK_DECIMAL, K_DISCLOSURE_OPEN, K_DISCLOSURE_CLOSED, 0 };
    for (int i = 0; types[i]; i++)
        if (kw == types[i]) { take(c); set_kw(v, kw); return true; }
    if (k && k->type == CT_IDENT) {                            /* an unknown counter style falls back to decimal */
        take(c);
        set_kw(v, K_DECIMAL);
        return true;
    }
    return false;
}

static bool parse_vertical_align(Cur *c, SVal *v) {
    static const int kws[] = { K_BASELINE, K_SUB, K_SUPER, K_TEXT_TOP, K_TEXT_BOTTOM, K_MIDDLE, K_TOP, K_BOTTOM, 0 };
    if (one_kw(c, v, kws)) return true;
    return parse_len(c, LF_PCT | LF_NEG, v);
}

static bool parse_display(Cur *c, SVal *v, SVal *v1) {
    const CssTok *k = peek(c);
    if (!k || k->type != CT_IDENT) return false;
    const char *s = kval(c, k);
    /* legacy prefixed values */
    if (k->vn >= 8 && strncasecmp(s, "-webkit-", 8) == 0) {
        const char *r = s + 8;
        size_t rn = k->vn - 8;
        int kw = -1;
        if ((rn == 3 && strncasecmp(r, "box", 3) == 0) || (rn == 4 && strncasecmp(r, "flex", 4) == 0)) kw = K_FLEX;
        else if ((rn == 10 && strncasecmp(r, "inline-box", 10) == 0) || (rn == 11 && strncasecmp(r, "inline-flex", 11) == 0)) kw = K_INLINE_FLEX;
        if (kw < 0) return false;
        take(c);
        set_kw(v, kw);
        return at_end(c);
    }
    int a = ident_kw(c, k);
    static const int single[] = { K_NONE, K_INLINE, K_BLOCK, K_INLINE_BLOCK, K_LIST_ITEM, K_FLEX, K_INLINE_FLEX, K_GRID,
        K_INLINE_GRID, K_TABLE, K_INLINE_TABLE, K_TABLE_ROW_GROUP, K_TABLE_HEADER_GROUP, K_TABLE_FOOTER_GROUP,
        K_TABLE_ROW, K_TABLE_CELL, K_TABLE_COLUMN_GROUP, K_TABLE_COLUMN, K_TABLE_CAPTION, K_CONTENTS, K_FLOW_ROOT,
        K_RUN_IN, K_RUBY, K_RUBY_TEXT, K_FLOW, 0 };
    bool ok = false;
    for (int i = 0; single[i]; i++) if (a == single[i]) ok = true;
    if (!ok) return false;
    take(c);
    set_kw(v, a);
    /* the two-value syntax: "block flex", "inline flow-root", "block flow list-item" */
    const CssTok *b = peek(c);
    if (b) {
        int bk = ident_kw(c, b);
        int outer = a, inner = bk;
        if (bk == K_INLINE || bk == K_BLOCK) { outer = bk; inner = a; }
        take(c);
        int r = -1;
        bool list = false;
        if (inner == K_LIST_ITEM || outer == K_LIST_ITEM) list = true;
        const CssTok *third = peek(c);
        if (third) { if (ident_kw(c, third) == K_LIST_ITEM) list = true; else return false; take(c); }
        if (outer == K_BLOCK) {
            if (inner == K_FLOW || inner == K_LIST_ITEM) r = list ? K_LIST_ITEM : K_BLOCK;
            else if (inner == K_FLOW_ROOT) r = K_FLOW_ROOT;
            else if (inner == K_FLEX) r = K_FLEX;
            else if (inner == K_GRID) r = K_GRID;
            else if (inner == K_TABLE) r = K_TABLE;
        } else if (outer == K_INLINE) {
            if (inner == K_FLOW) r = K_INLINE;
            else if (inner == K_FLOW_ROOT) r = K_INLINE_BLOCK;
            else if (inner == K_FLEX) r = K_INLINE_FLEX;
            else if (inner == K_GRID) r = K_INLINE_GRID;
            else if (inner == K_TABLE) r = K_INLINE_TABLE;
            else if (inner == K_LIST_ITEM) r = K_LIST_ITEM;
        } else if (outer == K_LIST_ITEM) {
            r = K_LIST_ITEM;
        }
        if (r < 0) return false;
        set_kw(v, r);
    }
    (void)v1;
    return true;
}

static bool parse_tracksize(Cur *c, TrackSize *ts, bool allow_fr, bool allow_intrinsic) {
    memset(ts, 0, sizeof *ts);
    const CssTok *k = peek(c);
    if (!k) return false;
    int kw = ident_kw(c, k);
    if (kw == K_AUTO) { take(c); ts->kind = TS_AUTO; return true; }
    if (kw == K_MIN_CONTENT && allow_intrinsic) { take(c); ts->kind = TS_MIN_CONTENT; return true; }
    if (kw == K_MAX_CONTENT && allow_intrinsic) { take(c); ts->kind = TS_MAX_CONTENT; return true; }
    if (k->type == CT_DIMENSION && allow_fr && k->vn == 2 && strncasecmp(kval(c, k), "fr", 2) == 0) {
        if (k->num < 0) return false;
        take(c);
        ts->kind = TS_FR;
        ts->v = (float)k->num;
        return true;
    }
    SVal v;
    if (!parse_len(c, LF_PCT, &v)) return false;
    if (v.kind == SV_CALC) {                                 /* evaluate calc() of lengths without percentages now */
        CalcEnv e = { 16, 16, 1024, 768, 8, 8 };
        float px, pct;
        if (!calc_linear(v.u.calc, &e, &px, &pct)) return false;
        ts->kind = TS_LEN;
        ts->unit = pct != 0 ? SU_PCT : SU_PX;
        ts->v = pct != 0 ? pct : px;
        return true;
    }
    if (v.kind != SV_LEN) return false;
    ts->kind = TS_LEN;
    ts->unit = v.unit;
    ts->v = v.num;
    return true;
}

static bool parse_track(Cur *c, Track *t) {
    memset(t, 0, sizeof *t);
    const CssTok *k = peek(c);
    if (!k) return false;
    if (fname_is(c, k, "minmax")) {
        Cur in = inside(c, k);
        if (!parse_tracksize(&in, &t->min, false, true)) return false;
        const CssTok *cm = take(&in);
        if (!cm || cm->type != CT_COMMA) return false;
        if (!parse_tracksize(&in, &t->max, true, true) || !at_end(&in)) return false;
        take(c);
        return true;
    }
    if (fname_is(c, k, "fit-content")) {
        Cur in = inside(c, k);
        TrackSize lim;
        if (!parse_tracksize(&in, &lim, false, false) || !at_end(&in)) return false;
        take(c);
        t->min.kind = TS_AUTO;
        t->max = lim;
        t->max.kind = TS_FIT_CONTENT;
        return true;
    }
    if (!parse_tracksize(c, &t->max, true, true)) return false;
    t->min = t->max;
    if (t->max.kind == TS_FR) t->min.kind = TS_AUTO;          /* "1fr" is minmax(auto, 1fr) */
    return true;
}

static int line_names(Cur *c, WebAtom *out, int max) {
    const CssTok *k = peek(c);
    if (!k || k->type != CT_LBRACK) return 0;
    Cur in = inside(c, k);
    in.i = (uint32_t)(k - c->t->v) + 1;
    in.end = k->close;
    take(c);
    int n = 0;
    while (!at_end(&in)) {
        const CssTok *w = take(&in);
        if (w && w->type == CT_IDENT && n < max) out[n++] = atom_get(&c->d->atoms, kval(c, w), w->vn);
    }
    return n;
}

static bool parse_tracklist(Cur *c, SVal *v, bool implicit) {
    if (!implicit && ident_kw(c, peek(c)) == K_NONE) { take(c); set_kw(v, K_NONE); return true; }
    Track tracks[256];
    int n = 0;
    TrackList *tl = wa_alloc(&c->d->arena, sizeof *tl);
    if (!tl) return false;
    tl->repeat_at = -1;
    WebAtom names[8];
    int nn;
    for (;;) {
        nn = implicit ? 0 : line_names(c, names, 8);
        if (at_end(c)) break;
        const CssTok *k = peek(c);
        if (!implicit && fname_is(c, k, "repeat")) {
            Cur in = inside(c, k);
            take(c);
            const CssTok *cnt = take(&in);
            int count = 0;
            bool autor = false;
            if (cnt && cnt->type == CT_NUMBER && (cnt->flags & CTF_INT) && cnt->num >= 1) count = cnt->num > 100 ? 100 : (int)cnt->num;
            else if (cnt && (tok_is_ident(c->t, cnt, "auto-fill") || tok_is_ident(c->t, cnt, "auto-fit"))) {
                autor = true;
                tl->auto_fit = tok_is_ident(c->t, cnt, "auto-fit");
                count = 1;
            } else return false;
            const CssTok *cm = take(&in);
            if (!cm || cm->type != CT_COMMA) return false;
            Track rep[16];
            int nr = 0;
            WebAtom rnames[8];
            while (!at_end(&in) && nr < 16) {
                line_names(&in, rnames, 8);                  /* names inside repeat() are not kept */
                if (at_end(&in)) break;
                if (!parse_track(&in, &rep[nr])) return false;
                nr++;
            }
            if (!nr) return false;
            if (autor) {
                if (tl->has_auto_repeat) return false;
                tl->has_auto_repeat = true;
                tl->repeat_at = n;
                tl->repeat_n = nr;
            }
            for (int r = 0; r < count; r++)
                for (int k3 = 0; k3 < nr && n < 256; k3++) tracks[n++] = rep[k3];
            continue;
        }
        if (n == 256 || !parse_track(c, &tracks[n])) return false;
        if (nn) {
            tracks[n].names = wa_alloc(&c->d->arena, sizeof(WebAtom) * (size_t)nn);
            if (tracks[n].names) { memcpy(tracks[n].names, names, sizeof(WebAtom) * (size_t)nn); tracks[n].nnames = nn; }
        }
        n++;
    }
    if (nn) {
        tl->end_names = wa_alloc(&c->d->arena, sizeof(WebAtom) * (size_t)nn);
        if (tl->end_names) { memcpy(tl->end_names, names, sizeof(WebAtom) * (size_t)nn); tl->nend = nn; }
    }
    if (!n) return false;
    tl->v = wa_alloc(&c->d->arena, sizeof(Track) * (size_t)n);
    if (!tl->v) return false;
    memcpy(tl->v, tracks, sizeof(Track) * (size_t)n);
    tl->n = n;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = tl;
    return true;
}

static bool parse_areas(Cur *c, SVal *v) {
    if (ident_kw(c, peek(c)) == K_NONE) { take(c); set_kw(v, K_NONE); return true; }
    WebAtom cells[1024];
    int rows = 0, cols = -1, n = 0;
    while (!at_end(c)) {
        const CssTok *k = take(c);
        if (!k || k->type != CT_STRING) return false;
        const char *s = kval(c, k);
        int cn = 0;
        for (uint32_t i = 0; i < k->vn;) {
            while (i < k->vn && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) i++;
            if (i >= k->vn) break;
            uint32_t st = i;
            if (s[i] == '.') { while (i < k->vn && s[i] == '.') i++; if (n < 1024) cells[n++] = AT_NONE; }
            else {
                while (i < k->vn && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '.') i++;
                if (n < 1024) cells[n++] = atom_get(&c->d->atoms, s + st, i - st);
            }
            cn++;
        }
        if (cols < 0) cols = cn;
        if (cn != cols || cn == 0) return false;
        rows++;
    }
    if (!rows || n != rows * cols) return false;
    GridAreas *ga = wa_alloc(&c->d->arena, sizeof *ga);
    WebAtom *arr = wa_alloc(&c->d->arena, sizeof(WebAtom) * (size_t)n);
    if (!ga || !arr) return false;
    memcpy(arr, cells, sizeof(WebAtom) * (size_t)n);
    ga->rows = rows;
    ga->cols = cols;
    ga->cells = arr;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = ga;
    return true;
}

/* <grid-line>: auto | <integer> [name] | span && [<integer> || name] | name */
static bool parse_grid_line(Cur *c, SVal *v) {
    GridLine gl;
    memset(&gl, 0, sizeof gl);
    if (ident_kw(c, peek(c)) == K_AUTO) {
        take(c);
        gl.autoval = true;
    } else {
        bool any = false;
        while (!at_end(c)) {
            const CssTok *k = peek(c);
            if (is_delim(k, '/')) break;
            if (k->type == CT_IDENT && tok_is_ident(c->t, k, "span")) { take(c); gl.span = 1; any = true; continue; }
            if (k->type == CT_NUMBER && (k->flags & CTF_INT) && k->num != 0) {
                take(c);
                gl.n = (int16_t)(k->num > 1000 ? 1000 : k->num < -1000 ? -1000 : k->num);
                any = true;
                continue;
            }
            if (k->type == CT_IDENT) { take(c); gl.name = atom_get(&c->d->atoms, kval(c, k), k->vn); any = true; continue; }
            return false;
        }
        if (!any) return false;
        if (gl.span && gl.n < 0) return false;
        if (gl.span && !gl.n && !gl.name) return false;
        if (gl.span && !gl.n) gl.n = 1;
    }
    GridLine *p = wa_alloc(&c->d->arena, sizeof *p);
    if (!p) return false;
    *p = gl;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = p;
    return true;
}

static bool parse_shadow(Cur *c, SVal *v) {
    if (ident_kw(c, peek(c)) == K_NONE) { take(c); set_kw(v, K_NONE); return true; }
    Shadow s;
    memset(&s, 0, sizeof s);
    s.color = COLOR_CURRENT;
    float lens[4];
    int nl = 0;
    bool color = false;
    while (!at_end(c)) {
        const CssTok *k = peek(c);
        if (k->type == CT_COMMA) {                            /* only the first shadow is kept */
            while (!at_end(c)) take(c);
            break;
        }
        if (ident_kw(c, k) == K_INSET) { take(c); s.inset = true; continue; }
        SVal l;
        Cur save = *c;
        if (nl < 4 && parse_len(c, LF_NEG, &l) && l.kind == SV_LEN && l.unit == SU_PX) { lens[nl++] = l.num; continue; }
        *c = save;
        if (nl < 4 && parse_len(c, LF_NEG, &l) && l.kind == SV_LEN) { lens[nl++] = l.num * (l.unit == SU_EM || l.unit == SU_REM ? 16 : 1); continue; }
        *c = save;
        if (!color && parse_color_tok(c, &s.color)) { color = true; continue; }
        return false;
    }
    if (nl < 2) return false;
    s.x = lens[0];
    s.y = lens[1];
    s.blur = nl > 2 ? lens[2] : 0;
    s.spread = nl > 3 ? lens[3] : 0;
    Shadow *p = wa_alloc(&c->d->arena, sizeof *p);
    if (!p) return false;
    *p = s;
    memset(v, 0, sizeof *v);
    v->kind = SV_PTR;
    v->u.ptr = p;
    return true;
}

static bool parse_bg_size(Cur *c, SVal *v0, SVal *v1) {
    int kw = ident_kw(c, peek(c));
    if (kw == K_COVER || kw == K_CONTAIN) { take(c); set_kw(v0, kw); set_kw(v1, kw); return true; }
    if (!parse_len(c, LF_PCT | LF_AUTO, v0)) return false;
    if (!at_end(c) && peek(c)->type != CT_COMMA) { if (!parse_len(c, LF_PCT | LF_AUTO, v1)) return false; }
    else set_kw(v1, K_AUTO);
    return true;
}

/* background-position: 1 to 4 values; v0 = x, v1 = y as lengths/percentages (keywords converted) */
static bool parse_position(Cur *c, SVal *x, SVal *y) {
    SVal vals[4];
    int kws[4], n = 0;
    while (n < 4 && !at_end(c)) {
        const CssTok *k = peek(c);
        if (k->type == CT_COMMA || is_delim(k, '/')) break;
        int kw = ident_kw(c, k);
        if (kw == K_LEFT || kw == K_RIGHT || kw == K_TOP || kw == K_BOTTOM || kw == K_CENTER) {
            take(c);
            kws[n] = kw;
            memset(&vals[n], 0, sizeof vals[n]);
            n++;
            continue;
        }
        if (!parse_len(c, LF_PCT | LF_NEG | LF_QUIRKY, &vals[n])) break;
        kws[n] = 0;
        n++;
    }
    if (!n) return false;
    SVal px, py;
    memset(&px, 0, sizeof px);
    memset(&py, 0, sizeof py);
    px.kind = py.kind = SV_LEN;
    px.unit = py.unit = SU_PCT;
    px.num = py.num = 50;
    #define KWPCT(kw) ((kw) == K_LEFT || (kw) == K_TOP ? 0.0f : (kw) == K_RIGHT || (kw) == K_BOTTOM ? 100.0f : 50.0f)
    if (n == 1) {
        if (kws[0] == K_TOP || kws[0] == K_BOTTOM) py.num = KWPCT(kws[0]);
        else if (kws[0]) px.num = KWPCT(kws[0]);
        else px = vals[0];
    } else if (n == 2) {
        bool swap = kws[0] == K_TOP || kws[0] == K_BOTTOM || kws[1] == K_LEFT || kws[1] == K_RIGHT;
        int xi = swap ? 1 : 0, yi = swap ? 0 : 1;
        if (kws[xi]) px.num = KWPCT(kws[xi]); else px = vals[xi];
        if (kws[yi]) py.num = KWPCT(kws[yi]); else py = vals[yi];
    } else {
        /* 3 or 4 values: keyword offset pairs ("right 10px bottom 20px") */
        for (int i = 0; i < n; i++) {
            if (!kws[i]) continue;
            bool hor = kws[i] == K_LEFT || kws[i] == K_RIGHT || (kws[i] == K_CENTER && i == 0);
            SVal *dst = hor ? &px : &py;
            dst->kind = SV_LEN;
            dst->unit = SU_PCT;
            dst->num = KWPCT(kws[i]);
            if (i + 1 < n && !kws[i + 1]) {
                SVal off = vals[i + 1];
                if (kws[i] == K_RIGHT || kws[i] == K_BOTTOM) {
                    if (off.kind == SV_LEN && off.unit == SU_PCT) { dst->num = 100 - off.num; }
                    else { *dst = off; dst->kw = 1; }       /* kw = 1: measured from the far edge */
                } else *dst = off;
            }
        }
    }
    #undef KWPCT
    *x = px;
    *y = py;
    return true;
}

static bool parse_radius(Cur *c, SVal *v) {
    if (!parse_len(c, LF_PCT | LF_QUIRKY, v)) return false;
    if (!at_end(c)) { SVal v2; if (!parse_len(c, LF_PCT, &v2)) return false; }
    return true;
}

static bool parse_aspect(Cur *c, SVal *v) {
    if (ident_kw(c, peek(c)) == K_AUTO) {
        take(c);
        if (at_end(c)) { set_kw(v, K_AUTO); return true; }
    }
    SVal a, b;
    if (!parse_number(c, &a, false)) return false;
    b.num = 1;
    if (is_delim(peek(c), '/')) { take(c); if (!parse_number(c, &b, false)) return false; }
    if (ident_kw(c, peek(c)) == K_AUTO) take(c);
    if (a.num <= 0 || b.num <= 0) { set_kw(v, K_AUTO); return true; }
    memset(v, 0, sizeof *v);
    v->kind = SV_NUM;
    v->num = a.num / b.num;
    return true;
}

static bool parse_deco_line(Cur *c, SVal *v) {
    int bits = 0;
    bool none = false;
    while (!at_end(c)) {
        int kw = ident_kw(c, peek(c));
        if (kw == K_NONE) none = true;
        else if (kw == K_UNDERLINE) bits |= DECO_UNDERLINE;
        else if (kw == K_OVERLINE) bits |= DECO_OVERLINE;
        else if (kw == K_LINE_THROUGH) bits |= DECO_LINE_THROUGH;
        else if (kw == K_BLINK) {}
        else return false;
        take(c);
    }
    if (none && bits) return false;
    memset(v, 0, sizeof *v);
    v->kind = SV_NUM;
    v->num = (float)bits;
    return true;
}

static bool parse_border_spacing(Cur *c, SVal *v0, SVal *v1) {
    if (!parse_len(c, LF_QUIRKY, v0)) return false;
    if (!at_end(c)) { if (!parse_len(c, LF_QUIRKY, v1)) return false; }
    else *v1 = *v0;
    return true;
}

static bool parse_align(Cur *c, SVal *v, int which) {
    /* which: 0 justify-content/align-content, 1 align-items/justify-items, 2 align-self/justify-self */
    int kws[3];
    int n = 0;
    while (!at_end(c) && n < 3) {
        int kw = ident_kw(c, peek(c));
        if (kw < 0) return false;
        kws[n++] = kw;
        take(c);
    }
    if (!n) return false;
    int kw = kws[n - 1];
    if (n >= 2 && (kws[0] == K_SAFE || kws[0] == K_UNSAFE || kws[0] == K_FIRST || kws[0] == K_LAST || kws[0] == K_LEGACY)) {
        if (kws[0] == K_LAST && kw == K_BASELINE) kw = K_LAST;
    } else if (n >= 2 && kws[1] == K_LEGACY) kw = kws[0];
    else if (n >= 2) return false;
    static const int ok[] = { K_AUTO, K_NORMAL, K_STRETCH, K_START, K_END, K_CENTER, K_FLEX_START, K_FLEX_END,
                              K_BASELINE, K_LAST, K_SPACE_BETWEEN, K_SPACE_AROUND, K_SPACE_EVENLY, K_LEFT, K_RIGHT,
                              K_SELF_START, K_SELF_END, 0 };
    for (int i = 0; ok[i]; i++)
        if (ok[i] == kw) {
            if (which != 0 && (kw == K_SPACE_BETWEEN || kw == K_SPACE_AROUND || kw == K_SPACE_EVENLY)) return false;
            if (which == 0 && (kw == K_SELF_START || kw == K_SELF_END)) return false;
            set_kw(v, kw);
            return true;
        }
    return false;
}

static bool parse_cursor(Cur *c, SVal *v) {
    /* [url() [x y]?,]* keyword: the images are skipped */
    while (!at_end(c)) {
        const CssTok *k = peek(c);
        if (k->type == CT_URL || fname_is(c, k, "url") || fname_is(c, k, "image-set")) {
            take(c);
            while (!at_end(c) && peek(c)->type == CT_NUMBER) take(c);
            if (!at_end(c) && peek(c)->type == CT_COMMA) take(c);
            continue;
        }
        break;
    }
    const CssTok *k = peek(c);
    if (!k || k->type != CT_IDENT) return false;
    take(c);
    int kw = ident_kw(c, k);
    if (kw < 0) kw = K_DEFAULT;                              /* unknown cursor names: the default arrow */
    set_kw(v, kw);
    return true;
}

/* ================================================================================================ longhands */

/* Parses a longhand's value from the cursor (which must be fully consumed). */
static bool parse_longhand(Cur *c, int p, SVal *v0, SVal *v1) {
    memset(v0, 0, sizeof *v0);
    memset(v1, 0, sizeof *v1);
    bool ok;
    switch (p) {
    case CP_COLOR: case CP_BACKGROUND_COLOR: case CP_BORDER_TOP_COLOR: case CP_BORDER_RIGHT_COLOR:
    case CP_BORDER_BOTTOM_COLOR: case CP_BORDER_LEFT_COLOR: case CP_OUTLINE_COLOR: case CP_TEXT_DECORATION_COLOR: {
        WebColor col;
        if (p == CP_OUTLINE_COLOR && ident_kw(c, peek(c)) == K_AUTO) { take(c); v0->kind = SV_COLOR; v0->u.color = COLOR_CURRENT; ok = true; break; }
        ok = parse_color_tok(c, &col);
        v0->kind = SV_COLOR;
        v0->u.color = col;
        break;
    }
    case CP_FONT_FAMILY: ok = parse_font_family(c, v0); break;
    case CP_FONT_SIZE: ok = parse_font_size(c, v0); break;
    case CP_FONT_WEIGHT: ok = parse_font_weight(c, v0); break;
    case CP_FONT_STYLE:
        ok = one_kw(c, v0, KWS(K_NORMAL, K_ITALIC, K_OBLIQUE));
        if (ok && v0->kw == K_OBLIQUE && !at_end(c)) take(c);   /* an angle after oblique */
        break;
    case CP_FONT_VARIANT: {
        ok = one_kw(c, v0, KWS(K_NORMAL, K_SMALL_CAPS, K_NONE));
        if (!ok && peek(c) && peek(c)->type == CT_IDENT) { take(c); set_kw(v0, K_NORMAL); ok = true; }
        while (ok && !at_end(c)) take(c);
        break;
    }
    case CP_LINE_HEIGHT: ok = parse_line_height(c, v0); break;
    case CP_TEXT_ALIGN:
        ok = one_kw(c, v0, KWS(K_START, K_END, K_LEFT, K_RIGHT, K_CENTER, K_JUSTIFY, K_MATCH_PARENT, K_WEBKIT_CENTER,
                               K_WEBKIT_LEFT, K_WEBKIT_RIGHT));
        if (!ok && peek(c) && peek(c)->type == CT_IDENT && tok_is_ident(c->t, peek(c), "-moz-center")) { take(c); set_kw(v0, K_WEBKIT_CENTER); ok = true; }
        break;
    case CP_TEXT_INDENT: ok = parse_len(c, LF_PCT | LF_NEG | LF_QUIRKY, v0); while (ok && !at_end(c) && peek(c)->type == CT_IDENT) take(c); break;
    case CP_TEXT_TRANSFORM: ok = one_kw(c, v0, KWS(K_NONE, K_UPPERCASE, K_LOWERCASE, K_CAPITALIZE, K_FULL_WIDTH)); break;
    case CP_LETTER_SPACING: case CP_WORD_SPACING: ok = parse_len(c, LF_NORMAL | LF_NEG | LF_QUIRKY | LF_PCT, v0); break;
    case CP_WHITE_SPACE: {
        ok = one_kw(c, v0, KWS(K_NORMAL, K_PRE, K_NOWRAP, K_PRE_WRAP, K_PRE_LINE, K_BREAK_SPACES));
        if (!ok) {                                           /* the newer longhand pair "collapse wrap" forms */
            int a = ident_kw(c, peek(c));
            if (a == K_COLLAPSE || a == K_PRESERVE || a == K_WRAP || a == K_NOWRAP) {
                int coll = K_COLLAPSE, wrap = K_WRAP;
                while (!at_end(c)) {
                    int k = ident_kw(c, take(c));
                    if (k == K_COLLAPSE || k == K_PRESERVE) coll = k;
                    else if (k == K_WRAP || k == K_NOWRAP) wrap = k;
                    else return false;
                }
                set_kw(v0, coll == K_PRESERVE ? (wrap == K_NOWRAP ? K_PRE : K_PRE_WRAP) : (wrap == K_NOWRAP ? K_NOWRAP : K_NORMAL));
                ok = true;
            }
        }
        break;
    }
    case CP_WORD_BREAK: ok = one_kw(c, v0, KWS(K_NORMAL, K_BREAK_ALL, K_KEEP_ALL, K_BREAK_WORD)); break;
    case CP_OVERFLOW_WRAP: ok = one_kw(c, v0, KWS(K_NORMAL, K_BREAK_WORD, K_ANYWHERE)); break;
    case CP_DIRECTION: ok = one_kw(c, v0, KWS(K_LTR, K_RTL)); break;
    case CP_LIST_STYLE_TYPE: ok = parse_list_type(c, v0); break;
    case CP_LIST_STYLE_POSITION: ok = one_kw(c, v0, KWS(K_INSIDE, K_OUTSIDE)); break;
    case CP_LIST_STYLE_IMAGE: case CP_BACKGROUND_IMAGE:
        ok = parse_image(c, v0);
        if (ok && p == CP_BACKGROUND_IMAGE) while (!at_end(c)) take(c);   /* further layers are ignored */
        break;
    case CP_VISIBILITY: ok = one_kw(c, v0, KWS(K_VISIBLE, K_HIDDEN, K_COLLAPSE)); break;
    case CP_CURSOR: ok = parse_cursor(c, v0); break;
    case CP_POINTER_EVENTS: {
        const CssTok *k = take(c);
        ok = k && k->type == CT_IDENT;
        if (ok) set_kw(v0, ident_kw(c, k) == K_NONE ? K_NONE : K_AUTO);
        break;
    }
    case CP_BORDER_COLLAPSE: ok = one_kw(c, v0, KWS(K_SEPARATE, K_COLLAPSE)); break;
    case CP_BORDER_SPACING: ok = parse_border_spacing(c, v0, v1); break;
    case CP_CAPTION_SIDE: ok = one_kw(c, v0, KWS(K_TOP, K_BOTTOM)); break;
    case CP_EMPTY_CELLS: ok = one_kw(c, v0, KWS(K_SHOW, K_HIDE)); break;
    case CP_QUOTES: ok = parse_quotes(c, v0); break;
    case CP_TAB_SIZE: ok = parse_number(c, v0, false) || parse_len(c, 0, v0); break;
    case CP_DISPLAY: ok = parse_display(c, v0, v1); break;
    case CP_POSITION:
        ok = one_kw(c, v0, KWS(K_STATIC, K_RELATIVE, K_ABSOLUTE, K_FIXED, K_STICKY));
        if (!ok && peek(c) && tok_is_ident(c->t, peek(c), "-webkit-sticky")) { take(c); set_kw(v0, K_STICKY); ok = true; }
        break;
    case CP_TOP: case CP_RIGHT: case CP_BOTTOM: case CP_LEFT:
    case CP_MARGIN_TOP: case CP_MARGIN_RIGHT: case CP_MARGIN_BOTTOM: case CP_MARGIN_LEFT:
        ok = parse_len(c, LF_PCT | LF_NEG | LF_AUTO | LF_QUIRKY, v0);
        break;
    case CP_FLOAT: ok = one_kw(c, v0, KWS(K_NONE, K_LEFT, K_RIGHT, K_INLINE_START, K_INLINE_END)); break;
    case CP_CLEAR: ok = one_kw(c, v0, KWS(K_NONE, K_LEFT, K_RIGHT, K_BOTH, K_INLINE_START, K_INLINE_END)); break;
    case CP_Z_INDEX: if (ident_kw(c, peek(c)) == K_AUTO) { take(c); set_kw(v0, K_AUTO); ok = true; } else ok = parse_int(c, v0, true); break;
    case CP_ORDER: ok = parse_int(c, v0, true); break;
    case CP_WIDTH: case CP_HEIGHT: case CP_MIN_WIDTH: case CP_MIN_HEIGHT:
        ok = parse_len(c, LF_PCT | LF_AUTO | LF_INTRINSIC | LF_QUIRKY, v0);
        break;
    case CP_MAX_WIDTH: case CP_MAX_HEIGHT: ok = parse_len(c, LF_PCT | LF_NONE | LF_INTRINSIC | LF_QUIRKY, v0); break;
    case CP_PADDING_TOP: case CP_PADDING_RIGHT: case CP_PADDING_BOTTOM: case CP_PADDING_LEFT:
        ok = parse_len(c, LF_PCT | LF_QUIRKY, v0);
        break;
    case CP_BORDER_TOP_WIDTH: case CP_BORDER_RIGHT_WIDTH: case CP_BORDER_BOTTOM_WIDTH: case CP_BORDER_LEFT_WIDTH:
    case CP_OUTLINE_WIDTH:
        ok = parse_border_width(c, v0);
        break;
    case CP_BORDER_TOP_STYLE: case CP_BORDER_RIGHT_STYLE: case CP_BORDER_BOTTOM_STYLE: case CP_BORDER_LEFT_STYLE:
        ok = one_kw(c, v0, border_styles);
        break;
    case CP_OUTLINE_STYLE: ok = one_kw(c, v0, border_styles) || one_kw(c, v0, KWS(K_AUTO)); break;
    case CP_OUTLINE_OFFSET: ok = parse_len(c, LF_NEG, v0); break;
    case CP_BORDER_TOP_LEFT_RADIUS: case CP_BORDER_TOP_RIGHT_RADIUS: case CP_BORDER_BOTTOM_RIGHT_RADIUS:
    case CP_BORDER_BOTTOM_LEFT_RADIUS:
        ok = parse_radius(c, v0);
        break;
    case CP_BOX_SIZING: ok = one_kw(c, v0, KWS(K_CONTENT_BOX, K_BORDER_BOX)); break;
    case CP_OVERFLOW_X: case CP_OVERFLOW_Y: {
        ok = one_kw(c, v0, KWS(K_VISIBLE, K_HIDDEN, K_SCROLL, K_AUTO, K_CLIP, K_OVERLAY));
        if (!ok && peek(c) && (tok_is_ident(c->t, peek(c), "-moz-hidden-unscrollable") || tok_is_ident(c->t, peek(c), "-webkit-paged-x"))) { take(c); set_kw(v0, K_HIDDEN); ok = true; }
        if (ok && v0->kw == K_OVERLAY) v0->kw = K_AUTO;
        break;
    }
    case CP_OPACITY: {
        const CssTok *k = peek(c);
        if (k && k->type == CT_PERCENT) { take(c); v0->kind = SV_NUM; v0->num = (float)(k->num / 100); ok = true; }
        else ok = parse_number(c, v0, true);
        break;
    }
    case CP_BACKGROUND_POSITION_X: case CP_BACKGROUND_POSITION_Y: {
        SVal x, y;
        ok = parse_position(c, &x, &y);
        *v0 = p == CP_BACKGROUND_POSITION_X ? x : y;
        while (ok && !at_end(c)) take(c);
        break;
    }
    case CP_BACKGROUND_SIZE: ok = parse_bg_size(c, v0, v1); while (ok && !at_end(c)) take(c); break;
    case CP_BACKGROUND_REPEAT: {
        ok = one_kw(c, v0, KWS(K_REPEAT, K_REPEAT_X, K_REPEAT_Y, K_NO_REPEAT, K_SPACE, K_ROUND));
        if (ok && !at_end(c) && peek(c)->type == CT_IDENT) {
            SVal b;
            if (one_kw(c, &b, KWS(K_REPEAT, K_NO_REPEAT, K_SPACE, K_ROUND))) {
                if (v0->kw == K_REPEAT && b.kw == K_NO_REPEAT) v0->kw = K_REPEAT_X;
                else if (v0->kw == K_NO_REPEAT && b.kw == K_REPEAT) v0->kw = K_REPEAT_Y;
                else if (v0->kw != b.kw) v0->kw = K_REPEAT;
            }
        }
        while (ok && !at_end(c)) take(c);
        break;
    }
    case CP_BACKGROUND_ATTACHMENT: ok = one_kw(c, v0, KWS(K_SCROLL, K_FIXED, K_LOCAL)); while (ok && !at_end(c)) take(c); break;
    case CP_BACKGROUND_CLIP: case CP_BACKGROUND_ORIGIN:
        ok = one_kw(c, v0, KWS(K_BORDER_BOX, K_PADDING_BOX, K_CONTENT_BOX, K_TEXT_KW));
        while (ok && !at_end(c)) take(c);
        break;
    case CP_TEXT_DECORATION_LINE: ok = parse_deco_line(c, v0); break;
    case CP_TEXT_DECORATION_STYLE: ok = one_kw(c, v0, KWS(K_SOLID, K_DOUBLE, K_DOTTED, K_DASHED, K_WAVY)); break;
    case CP_TEXT_OVERFLOW: ok = one_kw(c, v0, KWS(K_CLIP, K_ELLIPSIS)); if (!ok && peek(c) && peek(c)->type == CT_STRING) { take(c); set_kw(v0, K_ELLIPSIS); ok = true; } break;
    case CP_VERTICAL_ALIGN: ok = parse_vertical_align(c, v0); break;
    case CP_TABLE_LAYOUT: ok = one_kw(c, v0, KWS(K_AUTO, K_FIXED)); break;
    case CP_CONTENT: ok = parse_content(c, v0); break;
    case CP_COUNTER_RESET: case CP_COUNTER_SET: ok = parse_counters(c, v0, 0); break;
    case CP_COUNTER_INCREMENT: ok = parse_counters(c, v0, 1); break;
    case CP_FLEX_DIRECTION: ok = one_kw(c, v0, KWS(K_ROW, K_ROW_REVERSE, K_COLUMN, K_COLUMN_REVERSE)); break;
    case CP_FLEX_WRAP: ok = one_kw(c, v0, KWS(K_NOWRAP, K_WRAP, K_WRAP_REVERSE)); break;
    case CP_FLEX_GROW: case CP_FLEX_SHRINK: ok = parse_number(c, v0, false); break;
    case CP_FLEX_BASIS: ok = parse_len(c, LF_PCT | LF_AUTO | LF_CONTENT | LF_INTRINSIC, v0); break;
    case CP_JUSTIFY_CONTENT: case CP_ALIGN_CONTENT: ok = parse_align(c, v0, 0); break;
    case CP_ALIGN_ITEMS: case CP_JUSTIFY_ITEMS: ok = parse_align(c, v0, 1); break;
    case CP_ALIGN_SELF: case CP_JUSTIFY_SELF: ok = parse_align(c, v0, 2); break;
    case CP_ROW_GAP: case CP_COLUMN_GAP: ok = parse_len(c, LF_PCT | LF_NORMAL, v0); break;
    case CP_GRID_TEMPLATE_COLUMNS: case CP_GRID_TEMPLATE_ROWS: ok = parse_tracklist(c, v0, false); break;
    case CP_GRID_AUTO_COLUMNS: case CP_GRID_AUTO_ROWS: ok = parse_tracklist(c, v0, true); break;
    case CP_GRID_TEMPLATE_AREAS: ok = parse_areas(c, v0); break;
    case CP_GRID_AUTO_FLOW: {
        int bits = 0;
        ok = false;
        while (!at_end(c)) {
            int kw = ident_kw(c, take(c));
            if (kw == K_ROW) bits |= 1;
            else if (kw == K_COLUMN) bits |= 2;
            else if (kw == K_DENSE) bits |= 4;
            else return false;
            ok = true;
        }
        if ((bits & 3) == 3) return false;
        v0->kind = SV_NUM;
        v0->num = (float)bits;
        break;
    }
    case CP_GRID_COLUMN_START: case CP_GRID_COLUMN_END: case CP_GRID_ROW_START: case CP_GRID_ROW_END:
        ok = parse_grid_line(c, v0);
        break;
    case CP_ASPECT_RATIO: ok = parse_aspect(c, v0); break;
    case CP_OBJECT_FIT: ok = one_kw(c, v0, KWS(K_FILL, K_CONTAIN, K_COVER, K_NONE, K_SCALE_DOWN)); break;
    case CP_BOX_SHADOW: ok = parse_shadow(c, v0); break;
    default: ok = false; break;
    }
    return ok && at_end(c);
}

/* ================================================================================================ shorthands and aliases */

enum {
    SH_BASE = 0x4000,
    SH_MARGIN, SH_PADDING, SH_BORDER, SH_BORDER_TOP, SH_BORDER_RIGHT, SH_BORDER_BOTTOM, SH_BORDER_LEFT, SH_BORDER_WIDTH,
    SH_BORDER_STYLE, SH_BORDER_COLOR, SH_BORDER_RADIUS, SH_BACKGROUND, SH_BACKGROUND_POSITION, SH_FONT, SH_LIST_STYLE,
    SH_FLEX, SH_FLEX_FLOW, SH_GAP, SH_GRID_TEMPLATE, SH_GRID_AREA, SH_GRID_ROW, SH_GRID_COLUMN, SH_PLACE_ITEMS,
    SH_PLACE_CONTENT, SH_PLACE_SELF, SH_INSET, SH_OVERFLOW, SH_OUTLINE, SH_TEXT_DECORATION, SH_MARGIN_BLOCK,
    SH_MARGIN_INLINE, SH_PADDING_BLOCK, SH_PADDING_INLINE, SH_INSET_BLOCK, SH_INSET_INLINE, SH_BORDER_BLOCK,
    SH_BORDER_INLINE, SH_GRID, SH_ALL,
};

static const struct { const char *name; int sh; } shorthands[] = {
    {"margin", SH_MARGIN}, {"padding", SH_PADDING}, {"border", SH_BORDER}, {"border-top", SH_BORDER_TOP},
    {"border-right", SH_BORDER_RIGHT}, {"border-bottom", SH_BORDER_BOTTOM}, {"border-left", SH_BORDER_LEFT},
    {"border-width", SH_BORDER_WIDTH}, {"border-style", SH_BORDER_STYLE}, {"border-color", SH_BORDER_COLOR},
    {"border-radius", SH_BORDER_RADIUS}, {"background", SH_BACKGROUND}, {"background-position", SH_BACKGROUND_POSITION},
    {"font", SH_FONT}, {"list-style", SH_LIST_STYLE}, {"flex", SH_FLEX}, {"flex-flow", SH_FLEX_FLOW}, {"gap", SH_GAP},
    {"grid-gap", SH_GAP}, {"grid-template", SH_GRID_TEMPLATE}, {"grid-area", SH_GRID_AREA}, {"grid-row", SH_GRID_ROW},
    {"grid-column", SH_GRID_COLUMN}, {"place-items", SH_PLACE_ITEMS}, {"place-content", SH_PLACE_CONTENT},
    {"place-self", SH_PLACE_SELF}, {"inset", SH_INSET}, {"overflow", SH_OVERFLOW}, {"outline", SH_OUTLINE},
    {"text-decoration", SH_TEXT_DECORATION}, {"margin-block", SH_MARGIN_BLOCK}, {"margin-inline", SH_MARGIN_INLINE},
    {"padding-block", SH_PADDING_BLOCK}, {"padding-inline", SH_PADDING_INLINE}, {"inset-block", SH_INSET_BLOCK},
    {"inset-inline", SH_INSET_INLINE}, {"border-block", SH_BORDER_BLOCK}, {"border-inline", SH_BORDER_INLINE},
    {"grid", SH_GRID}, {"all", SH_ALL},
};

/* simple renamings: logical properties for horizontal left-to-right writing, legacy names */
static const struct { const char *name; int prop; } aliases[] = {
    {"word-wrap", CP_OVERFLOW_WRAP}, {"grid-row-gap", CP_ROW_GAP}, {"grid-column-gap", CP_COLUMN_GAP},
    {"margin-block-start", CP_MARGIN_TOP}, {"margin-block-end", CP_MARGIN_BOTTOM}, {"margin-inline-start", CP_MARGIN_LEFT},
    {"margin-inline-end", CP_MARGIN_RIGHT}, {"padding-block-start", CP_PADDING_TOP}, {"padding-block-end", CP_PADDING_BOTTOM},
    {"padding-inline-start", CP_PADDING_LEFT}, {"padding-inline-end", CP_PADDING_RIGHT}, {"inset-block-start", CP_TOP},
    {"inset-block-end", CP_BOTTOM}, {"inset-inline-start", CP_LEFT}, {"inset-inline-end", CP_RIGHT},
    {"inline-size", CP_WIDTH}, {"block-size", CP_HEIGHT}, {"min-inline-size", CP_MIN_WIDTH},
    {"min-block-size", CP_MIN_HEIGHT}, {"max-inline-size", CP_MAX_WIDTH}, {"max-block-size", CP_MAX_HEIGHT},
    {"border-block-start-width", CP_BORDER_TOP_WIDTH}, {"border-block-end-width", CP_BORDER_BOTTOM_WIDTH},
    {"border-inline-start-width", CP_BORDER_LEFT_WIDTH}, {"border-inline-end-width", CP_BORDER_RIGHT_WIDTH},
    {"border-block-start-style", CP_BORDER_TOP_STYLE}, {"border-block-end-style", CP_BORDER_BOTTOM_STYLE},
    {"border-inline-start-style", CP_BORDER_LEFT_STYLE}, {"border-inline-end-style", CP_BORDER_RIGHT_STYLE},
    {"border-block-start-color", CP_BORDER_TOP_COLOR}, {"border-block-end-color", CP_BORDER_BOTTOM_COLOR},
    {"border-inline-start-color", CP_BORDER_LEFT_COLOR}, {"border-inline-end-color", CP_BORDER_RIGHT_COLOR},
    {"border-start-start-radius", CP_BORDER_TOP_LEFT_RADIUS}, {"border-start-end-radius", CP_BORDER_TOP_RIGHT_RADIUS},
    {"border-end-start-radius", CP_BORDER_BOTTOM_LEFT_RADIUS}, {"border-end-end-radius", CP_BORDER_BOTTOM_RIGHT_RADIUS},
    {"text-decoration-thickness", -2}, {"text-underline-offset", -2}, {"text-rendering", -2},
};

static const struct { const char *name; int sh; } shorthand_aliases[] = {
    {"border-block-start", SH_BORDER_TOP}, {"border-block-end", SH_BORDER_BOTTOM},
    {"border-inline-start", SH_BORDER_LEFT}, {"border-inline-end", SH_BORDER_RIGHT},
};

int css_shorthand(const char *s, size_t n) {
    for (size_t i = 0; i < sizeof shorthands / sizeof shorthands[0]; i++)
        if (strlen(shorthands[i].name) == n && strncasecmp(shorthands[i].name, s, n) == 0) return shorthands[i].sh;
    for (size_t i = 0; i < sizeof shorthand_aliases / sizeof shorthand_aliases[0]; i++)
        if (strlen(shorthand_aliases[i].name) == n && strncasecmp(shorthand_aliases[i].name, s, n) == 0) return shorthand_aliases[i].sh;
    return -1;
}

/* resolves a property name (with vendor prefixes stripped and aliases applied): CP_*, SH_*, -2 known-but-ignored, -1 */
static int resolve_name(const char *s, size_t n) {
    for (int pass = 0; pass < 2; pass++) {
        int p = css_property(s, n);
        if (p >= 0) return p;
        int sh = css_shorthand(s, n);
        if (sh >= 0) return sh;
        for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
            if (strlen(aliases[i].name) == n && strncasecmp(aliases[i].name, s, n) == 0) return aliases[i].prop;
        if (pass == 0) {
            if (n > 8 && strncasecmp(s, "-webkit-", 8) == 0) { s += 8; n -= 8; continue; }
            if (n > 5 && strncasecmp(s, "-moz-", 5) == 0) { s += 5; n -= 5; continue; }
            if (n > 4 && strncasecmp(s, "-ms-", 4) == 0) { s += 4; n -= 4; continue; }
        }
        break;
    }
    return -1;
}

static void emit(CssDecls *out, WebArena *a, int prop, const SVal *v0, const SVal *v1, bool imp) {
    if (out->n == out->cap) {
        uint32_t nc = out->cap ? out->cap * 2 : 8;
        CssDecl *v = wa_realloc(a, out->v, sizeof(CssDecl) * out->cap, sizeof(CssDecl) * nc);
        if (!v) return;
        out->v = v;
        out->cap = nc;
    }
    CssDecl *d = &out->v[out->n++];
    memset(d, 0, sizeof *d);
    d->prop = (uint16_t)prop;
    d->important = imp;
    if (v0) d->v0 = *v0;
    if (v1) d->v1 = *v1;
}

static void emit_wide(CssDecls *out, WebArena *a, int prop, int wide, bool imp) {
    emit(out, a, prop, NULL, NULL, imp);
    if (out->n) out->v[out->n - 1].wide = (uint8_t)wide;
}

/* the longhands of a shorthand (for CSS-wide keywords and var()) */
static int sh_longhands(int sh, int *out) {
    int n = 0;
#define L(x) out[n++] = (x)
    switch (sh) {
    case SH_MARGIN: L(CP_MARGIN_TOP); L(CP_MARGIN_RIGHT); L(CP_MARGIN_BOTTOM); L(CP_MARGIN_LEFT); break;
    case SH_PADDING: L(CP_PADDING_TOP); L(CP_PADDING_RIGHT); L(CP_PADDING_BOTTOM); L(CP_PADDING_LEFT); break;
    case SH_BORDER:
        for (int s = 0; s < 4; s++) { L(CP_BORDER_TOP_WIDTH + s); L(CP_BORDER_TOP_STYLE + s); L(CP_BORDER_TOP_COLOR + s); }
        break;
    case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT: {
        int s = sh - SH_BORDER_TOP;
        L(CP_BORDER_TOP_WIDTH + s); L(CP_BORDER_TOP_STYLE + s); L(CP_BORDER_TOP_COLOR + s);
        break;
    }
    case SH_BORDER_WIDTH: for (int s = 0; s < 4; s++) L(CP_BORDER_TOP_WIDTH + s); break;
    case SH_BORDER_STYLE: for (int s = 0; s < 4; s++) L(CP_BORDER_TOP_STYLE + s); break;
    case SH_BORDER_COLOR: for (int s = 0; s < 4; s++) L(CP_BORDER_TOP_COLOR + s); break;
    case SH_BORDER_RADIUS: for (int s = 0; s < 4; s++) L(CP_BORDER_TOP_LEFT_RADIUS + s); break;
    case SH_BACKGROUND:
        L(CP_BACKGROUND_COLOR); L(CP_BACKGROUND_IMAGE); L(CP_BACKGROUND_POSITION_X); L(CP_BACKGROUND_POSITION_Y);
        L(CP_BACKGROUND_SIZE); L(CP_BACKGROUND_REPEAT); L(CP_BACKGROUND_ATTACHMENT); L(CP_BACKGROUND_ORIGIN);
        L(CP_BACKGROUND_CLIP);
        break;
    case SH_BACKGROUND_POSITION: L(CP_BACKGROUND_POSITION_X); L(CP_BACKGROUND_POSITION_Y); break;
    case SH_FONT: L(CP_FONT_STYLE); L(CP_FONT_VARIANT); L(CP_FONT_WEIGHT); L(CP_FONT_SIZE); L(CP_LINE_HEIGHT); L(CP_FONT_FAMILY); break;
    case SH_LIST_STYLE: L(CP_LIST_STYLE_TYPE); L(CP_LIST_STYLE_POSITION); L(CP_LIST_STYLE_IMAGE); break;
    case SH_FLEX: L(CP_FLEX_GROW); L(CP_FLEX_SHRINK); L(CP_FLEX_BASIS); break;
    case SH_FLEX_FLOW: L(CP_FLEX_DIRECTION); L(CP_FLEX_WRAP); break;
    case SH_GAP: L(CP_ROW_GAP); L(CP_COLUMN_GAP); break;
    case SH_GRID_TEMPLATE: case SH_GRID: L(CP_GRID_TEMPLATE_ROWS); L(CP_GRID_TEMPLATE_COLUMNS); L(CP_GRID_TEMPLATE_AREAS); break;
    case SH_GRID_AREA: L(CP_GRID_ROW_START); L(CP_GRID_COLUMN_START); L(CP_GRID_ROW_END); L(CP_GRID_COLUMN_END); break;
    case SH_GRID_ROW: L(CP_GRID_ROW_START); L(CP_GRID_ROW_END); break;
    case SH_GRID_COLUMN: L(CP_GRID_COLUMN_START); L(CP_GRID_COLUMN_END); break;
    case SH_PLACE_ITEMS: L(CP_ALIGN_ITEMS); L(CP_JUSTIFY_ITEMS); break;
    case SH_PLACE_CONTENT: L(CP_ALIGN_CONTENT); L(CP_JUSTIFY_CONTENT); break;
    case SH_PLACE_SELF: L(CP_ALIGN_SELF); L(CP_JUSTIFY_SELF); break;
    case SH_INSET: L(CP_TOP); L(CP_RIGHT); L(CP_BOTTOM); L(CP_LEFT); break;
    case SH_OVERFLOW: L(CP_OVERFLOW_X); L(CP_OVERFLOW_Y); break;
    case SH_OUTLINE: L(CP_OUTLINE_WIDTH); L(CP_OUTLINE_STYLE); L(CP_OUTLINE_COLOR); break;
    case SH_TEXT_DECORATION: L(CP_TEXT_DECORATION_LINE); L(CP_TEXT_DECORATION_STYLE); L(CP_TEXT_DECORATION_COLOR); break;
    case SH_MARGIN_BLOCK: L(CP_MARGIN_TOP); L(CP_MARGIN_BOTTOM); break;
    case SH_MARGIN_INLINE: L(CP_MARGIN_LEFT); L(CP_MARGIN_RIGHT); break;
    case SH_PADDING_BLOCK: L(CP_PADDING_TOP); L(CP_PADDING_BOTTOM); break;
    case SH_PADDING_INLINE: L(CP_PADDING_LEFT); L(CP_PADDING_RIGHT); break;
    case SH_INSET_BLOCK: L(CP_TOP); L(CP_BOTTOM); break;
    case SH_INSET_INLINE: L(CP_LEFT); L(CP_RIGHT); break;
    case SH_BORDER_BLOCK: for (int s = 0; s < 4; s += 2) { L(CP_BORDER_TOP_WIDTH + s); L(CP_BORDER_TOP_STYLE + s); L(CP_BORDER_TOP_COLOR + s); } break;
    case SH_BORDER_INLINE: for (int s = 1; s < 4; s += 2) { L(CP_BORDER_TOP_WIDTH + s); L(CP_BORDER_TOP_STYLE + s); L(CP_BORDER_TOP_COLOR + s); } break;
    case SH_ALL:
        for (int p = 0; p < CP_COUNT; p++) if (p != CP_DIRECTION) L(p);
        break;
    default: break;
    }
#undef L
    return n;
}

/* 1 to 4 values for top, right, bottom, left */
static bool four(Cur *c, const int props[4], bool imp, CssDecls *out, int flags, bool (*part)(Cur *, int, SVal *)) {
    SVal v[4];
    int n = 0;
    while (!at_end(c)) {
        if (n == 4 || !part(c, flags, &v[n])) return false;
        n++;
    }
    if (!n) return false;
    if (n == 1) v[1] = v[2] = v[3] = v[0];
    else if (n == 2) { v[2] = v[0]; v[3] = v[1]; }
    else if (n == 3) v[3] = v[1];
    for (int i = 0; i < 4; i++) emit(out, &c->d->arena, props[i], &v[i], NULL, imp);
    return true;
}

static bool part_len(Cur *c, int flags, SVal *v) { return parse_len(c, flags, v); }
static bool part_bwidth(Cur *c, int flags, SVal *v) { (void)flags; return parse_border_width(c, v); }
static bool part_bstyle(Cur *c, int flags, SVal *v) { (void)flags; return one_kw(c, v, border_styles); }
static bool part_color(Cur *c, int flags, SVal *v) {
    (void)flags;
    memset(v, 0, sizeof *v);
    v->kind = SV_COLOR;
    return parse_color_tok(c, &v->u.color);
}
static bool part_radius(Cur *c, int flags, SVal *v) {
    (void)flags;
    const CssTok *k = peek(c);
    if (is_delim(k, '/')) return false;
    return parse_len(c, LF_PCT | LF_QUIRKY, v);
}

/* border: <width> || <style> || <color>, for the given sides */
static bool border_sides(Cur *c, const int *sides, int nsides, bool imp, CssDecls *out) {
    SVal w, s, col;
    bool hw = false, hs = false, hc = false;
    while (!at_end(c)) {
        Cur save = *c;
        if (!hs && one_kw(c, &s, border_styles)) { hs = true; continue; }
        *c = save;
        if (!hw && parse_border_width(c, &w)) { hw = true; continue; }
        *c = save;
        memset(&col, 0, sizeof col);
        col.kind = SV_COLOR;
        if (!hc && parse_color_tok(c, &col.u.color)) { hc = true; continue; }
        return false;
    }
    if (!hw && !hs && !hc) return false;
    if (!hw) set_kw(&w, K_MEDIUM);
    if (!hs) set_kw(&s, K_NONE);
    if (!hc) { memset(&col, 0, sizeof col); col.kind = SV_COLOR; col.u.color = COLOR_CURRENT; }
    for (int i = 0; i < nsides; i++) {
        emit(out, &c->d->arena, CP_BORDER_TOP_WIDTH + sides[i], &w, NULL, imp);
        emit(out, &c->d->arena, CP_BORDER_TOP_STYLE + sides[i], &s, NULL, imp);
        emit(out, &c->d->arena, CP_BORDER_TOP_COLOR + sides[i], &col, NULL, imp);
    }
    return true;
}

static bool sh_font(Cur *c, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    const CssTok *k = peek(c);
    int kw = ident_kw(c, k);
    if (kw == K_CAPTION || kw == K_ICON || kw == K_MENU || kw == K_MESSAGE_BOX || kw == K_SMALL_CAPTION || kw == K_STATUS_BAR) {
        take(c);
        if (!at_end(c)) return false;
        /* system fonts: the UI's font, 13 px sans-serif */
        SVal st, var, wt, sz, lh, fam;
        set_kw(&st, K_NORMAL); set_kw(&var, K_NORMAL); set_kw(&wt, K_NORMAL); set_kw(&lh, K_NORMAL);
        memset(&sz, 0, sizeof sz); sz.kind = SV_LEN; sz.unit = SU_PX; sz.num = 13;
        static const char *const sys[] = { "system-ui", "sans-serif" };
        FontList *fl = wa_alloc(a, sizeof *fl);
        if (!fl) return false;
        fl->names = sys;
        fl->n = 2;
        memset(&fam, 0, sizeof fam);
        fam.kind = SV_PTR;
        fam.u.ptr = fl;
        emit(out, a, CP_FONT_STYLE, &st, NULL, imp); emit(out, a, CP_FONT_VARIANT, &var, NULL, imp);
        emit(out, a, CP_FONT_WEIGHT, &wt, NULL, imp); emit(out, a, CP_FONT_SIZE, &sz, NULL, imp);
        emit(out, a, CP_LINE_HEIGHT, &lh, NULL, imp); emit(out, a, CP_FONT_FAMILY, &fam, NULL, imp);
        return true;
    }
    SVal st, var, wt, sz, lh, fam;
    set_kw(&st, K_NORMAL); set_kw(&var, K_NORMAL); set_kw(&wt, K_NORMAL); set_kw(&lh, K_NORMAL);
    bool got_size = false;
    for (int guard = 0; guard < 5 && !at_end(c); guard++) {
        k = peek(c);
        kw = ident_kw(c, k);
        if (kw == K_NORMAL) { take(c); continue; }
        if (kw == K_ITALIC || kw == K_OBLIQUE) { take(c); set_kw(&st, kw); continue; }
        if (kw == K_SMALL_CAPS) { take(c); set_kw(&var, kw); continue; }
        if (kw == K_BOLD || kw == K_BOLDER || kw == K_LIGHTER) { take(c); set_kw(&wt, kw); continue; }
        if (k->type == CT_NUMBER && k->num >= 1 && k->num <= 1000 && (k->flags & CTF_INT)) {
            take(c);
            memset(&wt, 0, sizeof wt);
            wt.kind = SV_NUM;
            wt.num = (float)k->num;
            continue;
        }
        if (k->type == CT_IDENT && (tok_is_ident(c->t, k, "condensed") || tok_is_ident(c->t, k, "expanded") ||
                                    tok_is_ident(c->t, k, "semi-condensed") || tok_is_ident(c->t, k, "semi-expanded") ||
                                    tok_is_ident(c->t, k, "ultra-condensed") || tok_is_ident(c->t, k, "extra-condensed") ||
                                    tok_is_ident(c->t, k, "ultra-expanded") || tok_is_ident(c->t, k, "extra-expanded"))) {
            take(c);
            continue;
        }
        break;
    }
    if (!parse_font_size(c, &sz)) return false;
    got_size = true;
    if (is_delim(peek(c), '/')) {
        take(c);
        if (!parse_line_height(c, &lh)) return false;
    }
    if (!got_size || !parse_font_family(c, &fam) || !at_end(c)) return false;
    emit(out, a, CP_FONT_STYLE, &st, NULL, imp); emit(out, a, CP_FONT_VARIANT, &var, NULL, imp);
    emit(out, a, CP_FONT_WEIGHT, &wt, NULL, imp); emit(out, a, CP_FONT_SIZE, &sz, NULL, imp);
    emit(out, a, CP_LINE_HEIGHT, &lh, NULL, imp); emit(out, a, CP_FONT_FAMILY, &fam, NULL, imp);
    return true;
}

static bool sh_background(Cur *c, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    SVal col, img, px, py, sw, shh, rep, att, org, clip;
    memset(&col, 0, sizeof col); col.kind = SV_COLOR; col.u.color = 0;
    set_kw(&img, K_NONE);
    memset(&px, 0, sizeof px); px.kind = SV_LEN; px.unit = SU_PCT; px.num = 0;
    py = px;
    set_kw(&sw, K_AUTO); set_kw(&shh, K_AUTO);
    set_kw(&rep, K_REPEAT); set_kw(&att, K_SCROLL); set_kw(&org, K_PADDING_BOX); set_kw(&clip, K_BORDER_BOX);
    bool hcol = false, himg = false, hpos = false, hrep = false, hatt = false;
    int hbox = 0;
    int layer = 0;
    while (!at_end(c)) {
        const CssTok *k = peek(c);
        if (k->type == CT_COMMA) {
            /* a later layer: only its colour (the final layer may have one) is used; the image is the first layer's */
            take(c);
            layer++;
            continue;
        }
        Cur save = *c;
        SVal t0, t1;
        if (!himg && parse_image(c, &t0)) { if (layer == 0) img = t0; himg = layer == 0; continue; }
        *c = save;
        if (!hpos && parse_position(c, &t0, &t1)) {
            if (layer == 0) { px = t0; py = t1; hpos = true; }
            if (is_delim(peek(c), '/')) {
                take(c);
                SVal s0, s1;
                if (!parse_bg_size(c, &s0, &s1)) return false;
                if (layer == 0) { sw = s0; shh = s1; }
            }
            continue;
        }
        *c = save;
        if (!hrep && one_kw(c, &t0, KWS(K_REPEAT, K_REPEAT_X, K_REPEAT_Y, K_NO_REPEAT, K_SPACE, K_ROUND))) {
            SVal t2;
            if (!at_end(c) && one_kw(c, &t2, KWS(K_REPEAT, K_NO_REPEAT, K_SPACE, K_ROUND))) {
                if (t0.kw == K_REPEAT && t2.kw == K_NO_REPEAT) t0.kw = K_REPEAT_X;
                else if (t0.kw == K_NO_REPEAT && t2.kw == K_REPEAT) t0.kw = K_REPEAT_Y;
            }
            if (layer == 0) { rep = t0; hrep = true; }
            continue;
        }
        *c = save;
        if (!hatt && one_kw(c, &t0, KWS(K_SCROLL, K_FIXED, K_LOCAL))) { if (layer == 0) { att = t0; hatt = true; } continue; }
        *c = save;
        if (hbox < 2 && one_kw(c, &t0, KWS(K_BORDER_BOX, K_PADDING_BOX, K_CONTENT_BOX, K_TEXT_KW))) {
            if (layer == 0) { if (!hbox) { org = t0; clip = t0; } else clip = t0; }
            hbox++;
            continue;
        }
        *c = save;
        WebColor cc;
        if (!hcol && parse_color_tok(c, &cc)) { col.u.color = cc; hcol = true; continue; }
        return false;
    }
    emit(out, a, CP_BACKGROUND_COLOR, &col, NULL, imp);
    emit(out, a, CP_BACKGROUND_IMAGE, &img, NULL, imp);
    emit(out, a, CP_BACKGROUND_POSITION_X, &px, NULL, imp);
    emit(out, a, CP_BACKGROUND_POSITION_Y, &py, NULL, imp);
    emit(out, a, CP_BACKGROUND_SIZE, &sw, &shh, imp);
    emit(out, a, CP_BACKGROUND_REPEAT, &rep, NULL, imp);
    emit(out, a, CP_BACKGROUND_ATTACHMENT, &att, NULL, imp);
    emit(out, a, CP_BACKGROUND_ORIGIN, &org, NULL, imp);
    emit(out, a, CP_BACKGROUND_CLIP, &clip, NULL, imp);
    return true;
}

static bool sh_flex(Cur *c, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    SVal g, s, b;
    memset(&g, 0, sizeof g); g.kind = SV_NUM;
    s = g;
    int kw = ident_kw(c, peek(c));
    if (kw == K_NONE || kw == K_AUTO) {
        take(c);
        if (!at_end(c)) return false;
        g.num = kw == K_AUTO ? 1 : 0;
        s.num = kw == K_AUTO ? 1 : 0;
        set_kw(&b, K_AUTO);
    } else {
        bool hg = false, hb = false;
        g.num = 1;
        s.num = 1;
        memset(&b, 0, sizeof b); b.kind = SV_LEN; b.unit = SU_PCT; b.num = 0;
        while (!at_end(c)) {
            const CssTok *k = peek(c);
            if (!hg && k->type == CT_NUMBER) {
                if (k->num < 0) return false;
                take(c);
                g.num = (float)k->num;
                hg = true;
                const CssTok *k2 = peek(c);
                if (k2 && k2->type == CT_NUMBER) {
                    if (k2->num < 0) return false;
                    take(c);
                    s.num = (float)k2->num;
                }
                continue;
            }
            if (!hb && parse_len(c, LF_PCT | LF_AUTO | LF_CONTENT | LF_INTRINSIC, &b)) { hb = true; continue; }
            return false;
        }
        if (!hg && !hb) return false;
    }
    emit(out, a, CP_FLEX_GROW, &g, NULL, imp);
    emit(out, a, CP_FLEX_SHRINK, &s, NULL, imp);
    emit(out, a, CP_FLEX_BASIS, &b, NULL, imp);
    return true;
}

/* two values separated by '/' for grid-row/grid-column (the end defaults to the start's name, else auto) */
static bool grid_pair(Cur *c, int p1, int p2, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    Cur part = *c;
    uint32_t slash = c->end;
    for (uint32_t i = c->i; i < c->end; i++) if (is_delim(&c->t->v[i], '/')) { slash = i; break; }
    part.end = slash;
    SVal s, e;
    if (!parse_grid_line(&part, &s) || !at_end(&part)) return false;
    if (slash < c->end) {
        Cur p2c = *c;
        p2c.i = slash + 1;
        if (!parse_grid_line(&p2c, &e) || !at_end(&p2c)) return false;
    } else {
        const GridLine *gs = s.u.ptr;
        GridLine *ge = wa_alloc(a, sizeof *ge);
        if (!ge) return false;
        if (gs->name && !gs->n && !gs->span) ge->name = gs->name;
        else ge->autoval = true;
        memset(&e, 0, sizeof e);
        e.kind = SV_PTR;
        e.u.ptr = ge;
    }
    c->i = c->end;
    emit(out, a, p1, &s, NULL, imp);
    emit(out, a, p2, &e, NULL, imp);
    return true;
}

static bool sh_grid_area(Cur *c, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    uint32_t cuts[5];
    int nc = 0;
    cuts[nc++] = c->i;
    for (uint32_t i = c->i; i < c->end && nc < 4; i++) if (is_delim(&c->t->v[i], '/')) cuts[nc++] = i + 1;
    SVal v[4];
    for (int k = 0; k < nc; k++) {
        Cur p = *c;
        p.i = cuts[k];
        p.end = k + 1 < nc ? cuts[k + 1] - 1 : c->end;
        if (!parse_grid_line(&p, &v[k]) || !at_end(&p)) return false;
    }
    for (int k = nc; k < 4; k++) {
        const GridLine *src = (k == 3 && nc >= 2) ? v[1].u.ptr : v[k == 1 ? 0 : k - 2 >= 0 ? k - 2 : 0].u.ptr;
        GridLine *g = wa_alloc(a, sizeof *g);
        if (!g) return false;
        if (src->name && !src->n && !src->span) g->name = src->name;
        else g->autoval = true;
        memset(&v[k], 0, sizeof v[k]);
        v[k].kind = SV_PTR;
        v[k].u.ptr = g;
    }
    c->i = c->end;
    emit(out, a, CP_GRID_ROW_START, &v[0], NULL, imp);
    emit(out, a, CP_GRID_COLUMN_START, &v[1], NULL, imp);
    emit(out, a, CP_GRID_ROW_END, &v[2], NULL, imp);
    emit(out, a, CP_GRID_COLUMN_END, &v[3], NULL, imp);
    return true;
}

static bool sh_grid_template(Cur *c, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    if (ident_kw(c, peek(c)) == K_NONE) {
        take(c);
        if (!at_end(c)) return false;
        SVal n;
        set_kw(&n, K_NONE);
        emit(out, a, CP_GRID_TEMPLATE_ROWS, &n, NULL, imp);
        emit(out, a, CP_GRID_TEMPLATE_COLUMNS, &n, NULL, imp);
        emit(out, a, CP_GRID_TEMPLATE_AREAS, &n, NULL, imp);
        return true;
    }
    uint32_t slash = c->end;
    for (uint32_t i = c->i; i < c->end; i++) if (is_delim(&c->t->v[i], '/')) { slash = i; break; }
    Cur rows = *c;
    rows.end = slash;
    /* rows: either a track list, or area strings each optionally followed by a row size */
    bool strings = false;
    for (uint32_t i = rows.i; i < rows.end; i++) if (c->t->v[i].type == CT_STRING) strings = true;
    SVal rv, cv, av;
    set_kw(&av, K_NONE);
    if (strings) {
        /* "a a" 40px "b c" 1fr: the strings make grid-template-areas, the sizes after them the rows */
        Cur sc = rows;
        Track tr[64];
        int nt = 0;
        WebAtom cells[1024];
        int ncell = 0, nrows = 0, ncols = -1;
        while (!at_end(&sc)) {
            const CssTok *k = peek(&sc);
            if (k->type == CT_LBRACK) { take(&sc); continue; }
            if (k->type == CT_STRING) {
                take(&sc);
                const char *s = kval(&sc, k);
                int cn = 0;
                for (uint32_t i = 0; i < k->vn;) {
                    while (i < k->vn && s[i] == ' ') i++;
                    if (i >= k->vn) break;
                    uint32_t st = i;
                    if (s[i] == '.') { while (i < k->vn && s[i] == '.') i++; if (ncell < 1024) cells[ncell++] = AT_NONE; }
                    else { while (i < k->vn && s[i] != ' ' && s[i] != '.') i++; if (ncell < 1024) cells[ncell++] = atom_get(&c->d->atoms, s + st, i - st); }
                    cn++;
                }
                if (ncols < 0) ncols = cn;
                if (cn != ncols) return false;
                nrows++;
                if (nt < 64) {
                    memset(&tr[nt], 0, sizeof tr[nt]);
                    tr[nt].min.kind = tr[nt].max.kind = TS_AUTO;
                    nt++;
                }
                continue;
            }
            if (!nt || !parse_track(&sc, &tr[nt - 1])) return false;
        }
        if (!nrows || ncols <= 0) return false;
        GridAreas *ga = wa_alloc(a, sizeof *ga);
        WebAtom *arr = wa_alloc(a, sizeof(WebAtom) * (size_t)ncell);
        TrackList *tl = wa_alloc(a, sizeof *tl);
        Track *tv = wa_alloc(a, sizeof(Track) * (size_t)nt);
        if (!ga || !arr || !tl || !tv) return false;
        memcpy(arr, cells, sizeof(WebAtom) * (size_t)ncell);
        ga->rows = nrows; ga->cols = ncols; ga->cells = arr;
        memcpy(tv, tr, sizeof(Track) * (size_t)nt);
        tl->v = tv; tl->n = nt; tl->repeat_at = -1;
        memset(&av, 0, sizeof av); av.kind = SV_PTR; av.u.ptr = ga;
        memset(&rv, 0, sizeof rv); rv.kind = SV_PTR; rv.u.ptr = tl;
    } else {
        if (!parse_tracklist(&rows, &rv, false) || !at_end(&rows)) return false;
    }
    if (slash < c->end) {
        Cur cols = *c;
        cols.i = slash + 1;
        if (!parse_tracklist(&cols, &cv, false) || !at_end(&cols)) return false;
    } else if (strings) {
        set_kw(&cv, K_NONE);
    } else return false;
    c->i = c->end;
    emit(out, a, CP_GRID_TEMPLATE_ROWS, &rv, NULL, imp);
    emit(out, a, CP_GRID_TEMPLATE_COLUMNS, &cv, NULL, imp);
    emit(out, a, CP_GRID_TEMPLATE_AREAS, &av, NULL, imp);
    return true;
}

/* Parses a shorthand's value; false when invalid. */
static bool parse_shorthand(Cur *c, int sh, bool imp, CssDecls *out) {
    WebArena *a = &c->d->arena;
    static const int margins[4] = { CP_MARGIN_TOP, CP_MARGIN_RIGHT, CP_MARGIN_BOTTOM, CP_MARGIN_LEFT };
    static const int paddings[4] = { CP_PADDING_TOP, CP_PADDING_RIGHT, CP_PADDING_BOTTOM, CP_PADDING_LEFT };
    static const int insets[4] = { CP_TOP, CP_RIGHT, CP_BOTTOM, CP_LEFT };
    static const int bw[4] = { CP_BORDER_TOP_WIDTH, CP_BORDER_RIGHT_WIDTH, CP_BORDER_BOTTOM_WIDTH, CP_BORDER_LEFT_WIDTH };
    static const int bs[4] = { CP_BORDER_TOP_STYLE, CP_BORDER_RIGHT_STYLE, CP_BORDER_BOTTOM_STYLE, CP_BORDER_LEFT_STYLE };
    static const int bc[4] = { CP_BORDER_TOP_COLOR, CP_BORDER_RIGHT_COLOR, CP_BORDER_BOTTOM_COLOR, CP_BORDER_LEFT_COLOR };
    static const int br[4] = { CP_BORDER_TOP_LEFT_RADIUS, CP_BORDER_TOP_RIGHT_RADIUS, CP_BORDER_BOTTOM_RIGHT_RADIUS,
                               CP_BORDER_BOTTOM_LEFT_RADIUS };
    switch (sh) {
    case SH_MARGIN: return four(c, margins, imp, out, LF_PCT | LF_NEG | LF_AUTO | LF_QUIRKY, part_len);
    case SH_PADDING: return four(c, paddings, imp, out, LF_PCT | LF_QUIRKY, part_len);
    case SH_INSET: return four(c, insets, imp, out, LF_PCT | LF_NEG | LF_AUTO, part_len);
    case SH_BORDER_WIDTH: return four(c, bw, imp, out, 0, part_bwidth);
    case SH_BORDER_STYLE: return four(c, bs, imp, out, 0, part_bstyle);
    case SH_BORDER_COLOR: return four(c, bc, imp, out, 0, part_color);
    case SH_BORDER_RADIUS: {
        Cur first = *c;
        uint32_t slash = c->end;
        for (uint32_t i = c->i; i < c->end; i++) if (is_delim(&c->t->v[i], '/')) { slash = i; break; }
        first.end = slash;
        if (!four(&first, br, imp, out, 0, part_radius)) return false;
        c->i = c->end;                                       /* the vertical radii after '/' are ignored */
        return true;
    }
    case SH_BORDER: { static const int all[4] = { 0, 1, 2, 3 }; return border_sides(c, all, 4, imp, out); }
    case SH_BORDER_TOP: case SH_BORDER_RIGHT: case SH_BORDER_BOTTOM: case SH_BORDER_LEFT: {
        int side = sh - SH_BORDER_TOP;
        return border_sides(c, &side, 1, imp, out);
    }
    case SH_BORDER_BLOCK: { static const int s[2] = { 0, 2 }; return border_sides(c, s, 2, imp, out); }
    case SH_BORDER_INLINE: { static const int s[2] = { 1, 3 }; return border_sides(c, s, 2, imp, out); }
    case SH_BACKGROUND: return sh_background(c, imp, out);
    case SH_BACKGROUND_POSITION: {
        SVal x, y;
        if (!parse_position(c, &x, &y)) return false;
        while (!at_end(c)) take(c);
        emit(out, a, CP_BACKGROUND_POSITION_X, &x, NULL, imp);
        emit(out, a, CP_BACKGROUND_POSITION_Y, &y, NULL, imp);
        return true;
    }
    case SH_FONT: return sh_font(c, imp, out);
    case SH_LIST_STYLE: {
        SVal type, pos, img;
        bool ht = false, hp = false, hi = false;
        int nones = 0;
        while (!at_end(c)) {
            Cur save = *c;
            if (ident_kw(c, peek(c)) == K_NONE) { take(c); nones++; continue; }
            if (!hp && one_kw(c, &pos, KWS(K_INSIDE, K_OUTSIDE))) { hp = true; continue; }
            *c = save;
            if (!hi && parse_image(c, &img)) { hi = true; continue; }
            *c = save;
            if (!ht && parse_list_type(c, &type)) { ht = true; continue; }
            return false;
        }
        if (nones > 2) return false;
        if (nones) {
            if (!ht) { set_kw(&type, K_NONE); ht = true; nones--; }
            if (nones && !hi) { set_kw(&img, K_NONE); hi = true; nones--; }
            if (nones) return false;
        }
        if (!ht) set_kw(&type, K_DISC);
        if (!hp) set_kw(&pos, K_OUTSIDE);
        if (!hi) set_kw(&img, K_NONE);
        emit(out, a, CP_LIST_STYLE_TYPE, &type, NULL, imp);
        emit(out, a, CP_LIST_STYLE_POSITION, &pos, NULL, imp);
        emit(out, a, CP_LIST_STYLE_IMAGE, &img, NULL, imp);
        return true;
    }
    case SH_FLEX: return sh_flex(c, imp, out);
    case SH_FLEX_FLOW: {
        SVal dir, wrap;
        bool hd = false, hw = false;
        while (!at_end(c)) {
            if (!hd && one_kw(c, &dir, KWS(K_ROW, K_ROW_REVERSE, K_COLUMN, K_COLUMN_REVERSE))) { hd = true; continue; }
            if (!hw && one_kw(c, &wrap, KWS(K_NOWRAP, K_WRAP, K_WRAP_REVERSE))) { hw = true; continue; }
            return false;
        }
        if (!hd && !hw) return false;
        if (!hd) set_kw(&dir, K_ROW);
        if (!hw) set_kw(&wrap, K_NOWRAP);
        emit(out, a, CP_FLEX_DIRECTION, &dir, NULL, imp);
        emit(out, a, CP_FLEX_WRAP, &wrap, NULL, imp);
        return true;
    }
    case SH_GAP: {
        SVal r, col;
        if (!parse_len(c, LF_PCT | LF_NORMAL, &r)) return false;
        if (!at_end(c)) { if (!parse_len(c, LF_PCT | LF_NORMAL, &col)) return false; }
        else col = r;
        emit(out, a, CP_ROW_GAP, &r, NULL, imp);
        emit(out, a, CP_COLUMN_GAP, &col, NULL, imp);
        return at_end(c);
    }
    case SH_GRID_TEMPLATE: case SH_GRID: return sh_grid_template(c, imp, out);
    case SH_GRID_AREA: return sh_grid_area(c, imp, out);
    case SH_GRID_ROW: return grid_pair(c, CP_GRID_ROW_START, CP_GRID_ROW_END, imp, out);
    case SH_GRID_COLUMN: return grid_pair(c, CP_GRID_COLUMN_START, CP_GRID_COLUMN_END, imp, out);
    case SH_PLACE_ITEMS: case SH_PLACE_CONTENT: case SH_PLACE_SELF: {
        /* <align> [<justify>]; a value of two words ("first baseline", "safe center") counts as one */
        int which = sh == SH_PLACE_CONTENT ? 0 : sh == SH_PLACE_ITEMS ? 1 : 2;
        int pa = sh == SH_PLACE_CONTENT ? CP_ALIGN_CONTENT : sh == SH_PLACE_ITEMS ? CP_ALIGN_ITEMS : CP_ALIGN_SELF;
        int pj = sh == SH_PLACE_CONTENT ? CP_JUSTIFY_CONTENT : sh == SH_PLACE_ITEMS ? CP_JUSTIFY_ITEMS : CP_JUSTIFY_SELF;
        uint32_t idx[4];
        int n = 0;
        for (uint32_t i = c->i; i < c->end && n < 4; i++) if (c->t->v[i].type != CT_WS) idx[n++] = i;
        if (n == 0 || n > 4) return false;
        int split = n;
        if (n >= 2) {
            int k0 = ident_kw(c, &c->t->v[idx[0]]);
            bool prefix = k0 == K_SAFE || k0 == K_UNSAFE || k0 == K_FIRST || k0 == K_LAST;
            split = prefix ? 2 : 1;
            if (split > n) return false;
        }
        Cur first = *c, second = *c;
        first.end = idx[split - 1] + 1;
        second.i = split < n ? idx[split] : c->end;
        SVal va, vj;
        if (!parse_align(&first, &va, which)) return false;
        if (split < n) { if (!parse_align(&second, &vj, which)) return false; }
        else vj = va;
        c->i = c->end;
        emit(out, a, pa, &va, NULL, imp);
        emit(out, a, pj, &vj, NULL, imp);
        return true;
    }
    case SH_OVERFLOW: {
        static const int kws[] = { K_VISIBLE, K_HIDDEN, K_SCROLL, K_AUTO, K_CLIP, K_OVERLAY, 0 };
        SVal x, y;
        if (!one_kw(c, &x, kws)) return false;
        if (!at_end(c)) { if (!one_kw(c, &y, kws)) return false; }
        else y = x;
        if (x.kw == K_OVERLAY) x.kw = K_AUTO;
        if (y.kw == K_OVERLAY) y.kw = K_AUTO;
        emit(out, a, CP_OVERFLOW_X, &x, NULL, imp);
        emit(out, a, CP_OVERFLOW_Y, &y, NULL, imp);
        return at_end(c);
    }
    case SH_OUTLINE: {
        SVal w, s, col;
        bool hw = false, hs = false, hc = false;
        while (!at_end(c)) {
            Cur save = *c;
            if (!hs && (one_kw(c, &s, border_styles) || one_kw(c, &s, KWS(K_AUTO)))) { hs = true; continue; }
            *c = save;
            if (!hw && parse_border_width(c, &w)) { hw = true; continue; }
            *c = save;
            memset(&col, 0, sizeof col);
            col.kind = SV_COLOR;
            if (!hc && parse_color_tok(c, &col.u.color)) { hc = true; continue; }
            return false;
        }
        if (!hw) set_kw(&w, K_MEDIUM);
        if (!hs) set_kw(&s, K_NONE);
        if (!hc) { memset(&col, 0, sizeof col); col.kind = SV_COLOR; col.u.color = COLOR_CURRENT; }
        emit(out, a, CP_OUTLINE_WIDTH, &w, NULL, imp);
        emit(out, a, CP_OUTLINE_STYLE, &s, NULL, imp);
        emit(out, a, CP_OUTLINE_COLOR, &col, NULL, imp);
        return true;
    }
    case SH_TEXT_DECORATION: {
        int bits = 0;
        bool hl = false, hs = false, hc = false, none = false;
        SVal st, col;
        set_kw(&st, K_SOLID);
        memset(&col, 0, sizeof col);
        col.kind = SV_COLOR;
        col.u.color = COLOR_CURRENT;
        while (!at_end(c)) {
            int kw = ident_kw(c, peek(c));
            if (kw == K_NONE) { take(c); none = true; hl = true; continue; }
            if (kw == K_UNDERLINE) { take(c); bits |= DECO_UNDERLINE; hl = true; continue; }
            if (kw == K_OVERLINE) { take(c); bits |= DECO_OVERLINE; hl = true; continue; }
            if (kw == K_LINE_THROUGH) { take(c); bits |= DECO_LINE_THROUGH; hl = true; continue; }
            if (kw == K_BLINK) { take(c); hl = true; continue; }
            if (!hs && one_kw(c, &st, KWS(K_SOLID, K_DOUBLE, K_DOTTED, K_DASHED, K_WAVY))) { hs = true; continue; }
            if (!hc && parse_color_tok(c, &col.u.color)) { hc = true; continue; }
            SVal th;
            if (parse_len(c, LF_PCT | LF_AUTO, &th)) continue;  /* thickness: ignored */
            return false;
        }
        if (none && bits) return false;
        if (!hl && !hs && !hc) return false;
        SVal line;
        memset(&line, 0, sizeof line);
        line.kind = SV_NUM;
        line.num = (float)bits;
        emit(out, a, CP_TEXT_DECORATION_LINE, &line, NULL, imp);
        emit(out, a, CP_TEXT_DECORATION_STYLE, &st, NULL, imp);
        emit(out, a, CP_TEXT_DECORATION_COLOR, &col, NULL, imp);
        return true;
    }
    case SH_MARGIN_BLOCK: case SH_MARGIN_INLINE: case SH_PADDING_BLOCK: case SH_PADDING_INLINE:
    case SH_INSET_BLOCK: case SH_INSET_INLINE: {
        int lh[4];
        sh_longhands(sh, lh);
        int flags = (sh == SH_PADDING_BLOCK || sh == SH_PADDING_INLINE) ? LF_PCT : LF_PCT | LF_NEG | LF_AUTO;
        SVal v1, v2;
        if (!parse_len(c, flags, &v1)) return false;
        if (!at_end(c)) { if (!parse_len(c, flags, &v2)) return false; }
        else v2 = v1;
        emit(out, a, lh[0], &v1, NULL, imp);
        emit(out, a, lh[1], &v2, NULL, imp);
        return at_end(c);
    }
    default:
        return false;
    }
}

/* ================================================================================================ declarations */

static bool has_var(const CssTokens *t, uint32_t s, uint32_t e) {
    for (uint32_t i = s; i < e; i++) {
        const CssTok *k = &t->v[i];
        if (k->type == CT_FUNCTION && k->vn == 3 && strncasecmp(tok_val(t, k), "var", 3) == 0) return true;
        if (k->type == CT_FUNCTION && k->vn == 3 && strncasecmp(tok_val(t, k), "env", 3) == 0) return true;
    }
    return false;
}

/* Parses the value tokens [s, e) of property `pid` (CP_* or SH_*), appending declarations. */
static bool parse_value(WebDoc *d, int pid, const CssTokens *t, uint32_t s, uint32_t e, bool imp, CssDecls *out) {
    Cur c = { t, s, e, d };
    const CssTok *k = peek(&c);
    if (!k) return false;
    /* CSS-wide keywords */
    int kw = ident_kw(&c, k);
    if (kw == K_INHERIT || kw == K_INITIAL || kw == K_UNSET || kw == K_REVERT || kw == K_REVERT_LAYER) {
        take(&c);
        if (!at_end(&c)) return false;
        if (kw == K_REVERT_LAYER) kw = K_REVERT;
        if (pid >= SH_BASE) {
            int lh[CP_COUNT];
            int n = sh_longhands(pid, lh);
            for (int i = 0; i < n; i++) emit_wide(out, &d->arena, lh[i], kw, imp);
        } else emit_wide(out, &d->arena, pid, kw, imp);
        return true;
    }
    if (has_var(t, s, e)) {
        const char *src = t->src + t->v[s].pos;
        size_t len = t->v[e - 1].end - t->v[s].pos;
        char *raw = wa_strndup(&d->arena, src, len);
        if (!raw) return false;
        int lh[CP_COUNT];
        int n = pid >= SH_BASE ? sh_longhands(pid, lh) : (lh[0] = pid, 1);
        for (int i = 0; i < n; i++) {
            emit(out, &d->arena, lh[i], NULL, NULL, imp);
            CssDecl *dd = &out->v[out->n - 1];
            dd->pending = 1;
            dd->via = (uint16_t)pid;
            dd->v0.kind = SV_STR;
            dd->v0.u.str = raw;
        }
        return true;
    }
    if (pid >= SH_BASE) {
        uint32_t before = out->n;
        if (pid == SH_ALL) return false;                     /* "all" only takes CSS-wide keywords */
        if (!parse_shorthand(&c, pid, imp, out)) { out->n = before; return false; }
        return true;
    }
    SVal v0, v1;
    if (!parse_longhand(&c, pid, &v0, &v1)) return false;
    emit(out, &d->arena, pid, &v0, &v1, imp);
    return true;
}

bool css_parse_value_text(WebDoc *d, int pid, const char *s, size_t n, CssDecls *out, bool important) {
    CssTokens t;
    WebArena *a = &d->arena;
    if (!css_tokenize(a, s, n, &t)) return false;
    bool ok = t.n > 0 && parse_value(d, pid, &t, 0, t.n, important, out);
    css_tokens_free(a, &t);
    return ok;
}

/* "consume a list of declarations" (CSS Syntax 3, 5.4.5): name ':' value [!important] ';', with error recovery. Nested
 * rules (CSS nesting) are skipped. */
void css_parse_decls(WebDoc *d, const CssTokens *t, uint32_t start, uint32_t end, CssDecls *out) {
    uint32_t i = start;
    while (i < end) {
        const CssTok *k = &t->v[i];
        if (k->type == CT_WS || k->type == CT_SEMI) { i++; continue; }
        if (k->type == CT_AT) {                              /* an at-rule inside a block: skip it */
            while (i < end && t->v[i].type != CT_SEMI && t->v[i].type != CT_LBRACE) i++;
            if (i < end && t->v[i].type == CT_LBRACE) i = t->v[i].close + 1;
            continue;
        }
        if (k->type != CT_IDENT) {
            /* not a declaration: a nested rule or junk; skip to the next ';' (or past a {} block) */
            while (i < end && t->v[i].type != CT_SEMI) {
                if (t->v[i].type == CT_LBRACE || t->v[i].type == CT_FUNCTION || t->v[i].type == CT_LPAREN || t->v[i].type == CT_LBRACK) {
                    bool brace = t->v[i].type == CT_LBRACE;
                    i = t->v[i].close + 1;
                    if (brace) break;
                } else i++;
            }
            continue;
        }
        uint32_t name = i++;
        while (i < end && t->v[i].type == CT_WS) i++;
        if (i >= end || t->v[i].type != CT_COLON) {
            while (i < end && t->v[i].type != CT_SEMI) {
                if (t->v[i].type == CT_LBRACE) { i = t->v[i].close + 1; break; }
                if (t->v[i].type == CT_FUNCTION || t->v[i].type == CT_LPAREN || t->v[i].type == CT_LBRACK) i = t->v[i].close + 1;
                else i++;
            }
            continue;
        }
        i++;
        uint32_t vs = i;
        while (i < end && t->v[i].type != CT_SEMI) {
            if (t->v[i].type == CT_FUNCTION || t->v[i].type == CT_LPAREN || t->v[i].type == CT_LBRACK || t->v[i].type == CT_LBRACE)
                i = t->v[i].close < end ? t->v[i].close + 1 : end;
            else i++;
        }
        uint32_t ve = i;
        /* trailing white space and !important */
        while (ve > vs && t->v[ve - 1].type == CT_WS) ve--;
        bool imp = false;
        if (ve > vs && tok_is_ident(t, &t->v[ve - 1], "important")) {
            uint32_t j = ve - 1;
            while (j > vs && t->v[j - 1].type == CT_WS) j--;
            if (j > vs && t->v[j - 1].type == CT_DELIM && t->v[j - 1].delim == '!') {
                imp = true;
                ve = j - 1;
                while (ve > vs && t->v[ve - 1].type == CT_WS) ve--;
            }
        }
        while (vs < ve && t->v[vs].type == CT_WS) vs++;
        const char *nm = tok_val(t, &t->v[name]);
        uint32_t nn = t->v[name].vn;
        if (nn > 2 && nm[0] == '-' && nm[1] == '-') {        /* a custom property: its value is kept as text */
            emit(out, &d->arena, CP_CUSTOM, NULL, NULL, imp);
            CssDecl *dd = &out->v[out->n - 1];
            dd->name = atom_get(&d->atoms, nm, nn);
            const char *src = ve > vs ? t->src + t->v[vs].pos : "";
            size_t len = ve > vs ? t->v[ve - 1].end - t->v[vs].pos : 0;
            dd->v0.kind = SV_STR;
            dd->v0.u.str = wa_strndup(&d->arena, src, len);
            continue;
        }
        if (ve <= vs) continue;
        int pid = resolve_name(nm, nn);
        if (pid < 0) continue;                               /* unknown or ignored (-2) */
        parse_value(d, pid, t, vs, ve, imp, out);
    }
}

CssDecls *css_parse_style_attr(WebDoc *d, const char *s, size_t n) {
    CssDecls *out = wa_alloc(&d->arena, sizeof *out);
    if (!out) return NULL;
    CssTokens t;
    if (!css_tokenize(&d->arena, s, n, &t)) return out;
    css_parse_decls(d, &t, 0, t.n, out);
    css_tokens_free(&d->arena, &t);
    return out;
}
