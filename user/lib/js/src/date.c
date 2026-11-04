/*
 * date.c: Date (ECMA-262 21.4) with Annex B (getYear, setYear, toGMTString).
 *
 * A Date holds a time value: milliseconds since 1970-01-01T00:00:00Z as a double, NaN for an invalid date, magnitude
 * at most 8.64e15.  The calendar arithmetic (Day, TimeWithinDay, YearFromTime, MakeDay, ...) follows 21.4.1 exactly,
 * in doubles, so out-of-range values behave as specified.  The time zone comes from the host: JSHostHooks.tz_offset_min
 * gives the offset from UTC at a UTC instant (the default is UTC), and the optional tz_name its abbreviation; the clock
 * is JSHostHooks.now_ms.
 *
 * Date.parse understands the ECMAScript date-time string format (21.4.1.32) and whatever toString, toUTCString and
 * toISOString produce, plus the usual legacy spellings ("Nov 3, 2026 10:00", "11/3/2026", "3 November 2026 10:00 PM
 * GMT+0100") which the specification leaves to the implementation.
 */
#include "jsint.h"

#define MS_PER_DAY 86400000.0
#define MS_PER_HOUR 3600000.0
#define MS_PER_MIN 60000.0
#define MAX_TIME 8.64e15

static inline bool finite_d(double d) { return d == d && d != KZ_INFINITY && d != -KZ_INFINITY; }
static inline double nan_d(void) { return jv_dbl(KZ_NAN_BITS); }
static inline double floor_d(double x) { return kz_math_floor(x); }
static inline double trunc_d(double x) { return kz_math_trunc(x); }

/* ================================================================== 21.4.1 calendar arithmetic */

static double day_of(double t) { return floor_d(t / MS_PER_DAY); }

static double time_within_day(double t) {
    double r = kz_math_fmod(t, MS_PER_DAY);
    return r < 0 ? r + MS_PER_DAY : r;
}

static double days_from_year(double y) {
    return 365.0 * (y - 1970) + floor_d((y - 1969) / 4) - floor_d((y - 1901) / 100) + floor_d((y - 1601) / 400);
}

static bool is_leap(double y) {
    return kz_math_fmod(y, 4) == 0 && (kz_math_fmod(y, 100) != 0 || kz_math_fmod(y, 400) == 0);
}

static double year_from_time(double t) {
    double y = floor_d(t / (MS_PER_DAY * 365.2425)) + 1970;
    while (days_from_year(y) * MS_PER_DAY > t) y--;
    while (days_from_year(y + 1) * MS_PER_DAY <= t) y++;
    return y;
}

static const int cum_days[2][13] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

typedef struct Civil {
    double year;
    int month, day;         /* month 0..11, day 1..31 */
    int hour, min, sec, ms;
    int wday;               /* 0 = Sunday */
} Civil;

static void civil_from_time(double t, Civil *c) {
    double d = day_of(t);
    double y = year_from_time(t);
    int dwy = (int)(d - days_from_year(y));
    int lp = is_leap(y);
    int m = 0;
    while (m < 11 && dwy >= cum_days[lp][m + 1]) m++;
    c->year = y;
    c->month = m;
    c->day = dwy - cum_days[lp][m] + 1;
    int tid = (int)time_within_day(t);
    c->hour = tid / 3600000;
    c->min = tid / 60000 % 60;
    c->sec = tid / 1000 % 60;
    c->ms = tid % 1000;
    int wd = (int)kz_math_fmod(d + 4, 7);
    c->wday = wd < 0 ? wd + 7 : wd;
}

/* MakeTime (21.4.1.27) */
static double make_time(double h, double m, double s, double ms) {
    if (!finite_d(h) || !finite_d(m) || !finite_d(s) || !finite_d(ms)) return nan_d();
    return trunc_d(h) * MS_PER_HOUR + trunc_d(m) * MS_PER_MIN + trunc_d(s) * 1000.0 + trunc_d(ms);
}

/* MakeDay (21.4.1.28) */
static double make_day(double year, double month, double date) {
    if (!finite_d(year) || !finite_d(month) || !finite_d(date)) return nan_d();
    double y = trunc_d(year), m = trunc_d(month), dt = trunc_d(date);
    double ym = y + floor_d(m / 12);
    if (!finite_d(ym) || ym > 400000 || ym < -400000) return nan_d();
    int mn = (int)kz_math_fmod(m, 12);
    if (mn < 0) mn += 12;
    double days = days_from_year(ym) + cum_days[is_leap(ym)][mn];
    return days + dt - 1;
}

/* MakeDate (21.4.1.29) */
static double make_date(double day, double time) {
    if (!finite_d(day) || !finite_d(time)) return nan_d();
    double tv = day * MS_PER_DAY + time;
    return finite_d(tv) ? tv : nan_d();
}

/* TimeClip (21.4.1.31) */
static double time_clip(double t) {
    if (!finite_d(t) || t > MAX_TIME || t < -MAX_TIME) return nan_d();
    t = trunc_d(t);
    return t == 0 ? 0.0 : t;
}

/* ================================================================== host time */

double kz_date_now(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    double t = rt->hooks.now_ms ? rt->hooks.now_ms(rt->hooks.opaque) : 0.0;
    t = time_clip(floor_d(t));
    return t == t ? t : 0.0;                    /* a broken clock reads as the epoch rather than as an invalid date */
}

static double offset_ms_at(JSContext *ctx, double utc) {
    JSRuntime *rt = ctx->rt;
    if (!rt->hooks.tz_offset_min || !finite_d(utc)) return 0;
    return (double)rt->hooks.tz_offset_min(rt->hooks.opaque, utc) * MS_PER_MIN;
}

/*
 * LocalTZA (21.4.1.25): the offset to add to a UTC time value to get local time (is_utc), or to subtract from a local
 * time value to get UTC.  For a local time that is skipped or repeated by a zone transition the offset before the
 * transition is used, as the specification says.
 */
double kz_local_tza(JSContext *ctx, double t, bool is_utc) {
    if (is_utc) return offset_ms_at(ctx, t);
    if (!finite_d(t)) return 0;
    double a = offset_ms_at(ctx, t - MS_PER_DAY - offset_ms_at(ctx, t - MS_PER_DAY));
    double b = offset_ms_at(ctx, t + MS_PER_DAY - offset_ms_at(ctx, t + MS_PER_DAY));
    if (a == b) return a;
    bool ok_a = offset_ms_at(ctx, t - a) == a, ok_b = offset_ms_at(ctx, t - b) == b;
    if (ok_b && !ok_a) return b;
    return a;                                   /* consistent with the old offset (or both, or neither): before the transition */
}

static double local_time(JSContext *ctx, double t) { return t + kz_local_tza(ctx, t, true); }
static double utc_time(JSContext *ctx, double t) { return finite_d(t) ? t - kz_local_tza(ctx, t, false) : nan_d(); }

/* ================================================================== formatting */

static const char *const day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const month_full[12] = { "january", "february", "march", "april", "may", "june", "july", "august",
                                            "september", "october", "november", "december" };
static const char *const day_full[7] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };

typedef struct Out { char *p; size_t n, cap; } Out;

static void o_str(Out *o, const char *s) { while (*s && o->n + 1 < o->cap) o->p[o->n++] = *s++; o->p[o->n] = 0; }
static void o_num(Out *o, long long v, int width) {
    char tmp[24];
    int k = 0;
    bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    do { tmp[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (k < width) tmp[k++] = '0';
    if (neg) tmp[k++] = '-';
    while (k && o->n + 1 < o->cap) o->p[o->n++] = tmp[--k];
    o->p[o->n] = 0;
}
static void o_ch(Out *o, char c) { if (o->n + 1 < o->cap) { o->p[o->n++] = c; o->p[o->n] = 0; } }

/* "Tue Nov 03 2026" with the year padded to four digits (a minus sign for negative years) */
static void fmt_date(Out *o, const Civil *c) {
    o_str(o, day_names[c->wday]);
    o_ch(o, ' ');
    o_str(o, month_names[c->month]);
    o_ch(o, ' ');
    o_num(o, c->day, 2);
    o_ch(o, ' ');
    o_num(o, (long long)c->year, 4);
}

/* "15:00:57 GMT-0500 (EST)" */
static void fmt_time(JSContext *ctx, Out *o, const Civil *c, double t_utc) {
    o_num(o, c->hour, 2); o_ch(o, ':'); o_num(o, c->min, 2); o_ch(o, ':'); o_num(o, c->sec, 2);
    o_str(o, " GMT");
    double off = offset_ms_at(ctx, t_utc) / MS_PER_MIN;
    o_ch(o, off < 0 ? '-' : '+');
    long long ao = (long long)(off < 0 ? -off : off);
    o_num(o, ao / 60, 2);
    o_num(o, ao % 60, 2);
    JSRuntime *rt = ctx->rt;
    if (rt->hooks.tz_name) {
        const char *nm = rt->hooks.tz_name(rt->hooks.opaque, t_utc);
        if (nm && *nm) { o_str(o, " ("); o_str(o, nm); o_ch(o, ')'); }
    }
}

enum { FMT_STRING, FMT_DATE, FMT_TIME, FMT_UTC, FMT_ISO, FMT_LOCALE, FMT_LOCALE_DATE, FMT_LOCALE_TIME };

static JSValue format_time_value(JSContext *ctx, double t, int kind) {
    char buf[128];
    Out o = { buf, 0, sizeof buf };
    buf[0] = 0;
    if (t != t) return kz_str_from_cstr(ctx, "Invalid Date");
    Civil c, u;
    double lt = local_time(ctx, t);
    civil_from_time(lt, &c);
    civil_from_time(t, &u);
    switch (kind) {
    case FMT_STRING: fmt_date(&o, &c); o_ch(&o, ' '); fmt_time(ctx, &o, &c, t); break;
    case FMT_DATE: fmt_date(&o, &c); break;
    case FMT_TIME: fmt_time(ctx, &o, &c, t); break;
    case FMT_UTC:
        o_str(&o, day_names[u.wday]); o_str(&o, ", "); o_num(&o, u.day, 2); o_ch(&o, ' '); o_str(&o, month_names[u.month]);
        o_ch(&o, ' '); o_num(&o, (long long)u.year, 4); o_ch(&o, ' ');
        o_num(&o, u.hour, 2); o_ch(&o, ':'); o_num(&o, u.min, 2); o_ch(&o, ':'); o_num(&o, u.sec, 2); o_str(&o, " GMT");
        break;
    case FMT_ISO:
        if (u.year >= 0 && u.year <= 9999) o_num(&o, (long long)u.year, 4);
        else { o_ch(&o, u.year < 0 ? '-' : '+'); o_num(&o, (long long)(u.year < 0 ? -u.year : u.year), 6); }
        o_ch(&o, '-'); o_num(&o, u.month + 1, 2); o_ch(&o, '-'); o_num(&o, u.day, 2);
        o_ch(&o, 'T'); o_num(&o, u.hour, 2); o_ch(&o, ':'); o_num(&o, u.min, 2); o_ch(&o, ':'); o_num(&o, u.sec, 2);
        o_ch(&o, '.'); o_num(&o, u.ms, 3); o_ch(&o, 'Z');
        break;
    default: {      /* the en-US forms: 11/3/2026, 3:00:57 PM */
        if (kind != FMT_LOCALE_TIME) {
            o_num(&o, c.month + 1, 1); o_ch(&o, '/'); o_num(&o, c.day, 1); o_ch(&o, '/'); o_num(&o, (long long)c.year, 1);
        }
        if (kind == FMT_LOCALE) o_str(&o, ", ");
        if (kind != FMT_LOCALE_DATE) {
            int h12 = c.hour % 12 ? c.hour % 12 : 12;
            o_num(&o, h12, 1); o_ch(&o, ':'); o_num(&o, c.min, 2); o_ch(&o, ':'); o_num(&o, c.sec, 2);
            o_str(&o, c.hour < 12 ? " AM" : " PM");
        }
        break;
    }
    }
    return kz_str_from_cstr(ctx, buf);
}

/* ================================================================== parsing */

#define PARSE_FAIL 0
#define PARSE_ISO 1

static int dig(char c) { return c >= '0' && c <= '9' ? c - '0' : -1; }

/* ES 21.4.1.32 Date Time String Format.  Returns 0 when the text is not shaped like it (try the legacy parser),
   1 with *out set (possibly NaN for impossible values). */
static int parse_iso(JSContext *ctx, const char *s, double *out) {
    const char *p = s;
    int sign = 1;
    double year;
    int nd;
    if (*p == '+' || *p == '-') {
        sign = *p == '-' ? -1 : 1;
        p++;
        year = 0;
        for (nd = 0; nd < 6; nd++) { int d = dig(p[nd]); if (d < 0) return 0; year = year * 10 + d; }
        p += 6;
        if (sign < 0 && year == 0) { *out = nan_d(); return 1; }       /* -000000 is not a year */
        year *= sign;
    } else {
        year = 0;
        for (nd = 0; nd < 4; nd++) { int d = dig(p[nd]); if (d < 0) return 0; year = year * 10 + d; }
        p += 4;
    }
    int month = 1, day = 1, hh = 0, mm = 0, ss = 0;
    double ms = 0;
    bool has_time = false, has_tz = false;
    double tz_min = 0;
    if (*p == '-') {
        if (dig(p[1]) < 0 || dig(p[2]) < 0) return 0;
        month = dig(p[1]) * 10 + dig(p[2]);
        p += 3;
        if (*p == '-') {
            if (dig(p[1]) < 0 || dig(p[2]) < 0) return 0;
            day = dig(p[1]) * 10 + dig(p[2]);
            p += 3;
        }
    }
    if (*p == 'T' || *p == 't' || (*p == ' ' && dig(p[1]) >= 0 && dig(p[2]) >= 0 && p[3] == ':')) {
        if (dig(p[1]) < 0 || dig(p[2]) < 0 || p[3] != ':' || dig(p[4]) < 0 || dig(p[5]) < 0) return 0;
        has_time = true;
        hh = dig(p[1]) * 10 + dig(p[2]);
        mm = dig(p[4]) * 10 + dig(p[5]);
        p += 6;
        if (*p == ':') {
            if (dig(p[1]) < 0 || dig(p[2]) < 0) return 0;
            ss = dig(p[1]) * 10 + dig(p[2]);
            p += 3;
            if (*p == '.' || *p == ',') {
                p++;
                if (dig(*p) < 0) return 0;
                double scale = 100;
                while (dig(*p) >= 0) { ms += dig(*p) * scale; scale /= 10; p++; }
                ms = floor_d(ms);
            }
        }
        if (*p == 'Z' || *p == 'z') { has_tz = true; p++; }
        else if (*p == '+' || *p == '-') {
            int sg = *p == '-' ? -1 : 1;
            if (dig(p[1]) < 0 || dig(p[2]) < 0) return 0;
            int th = dig(p[1]) * 10 + dig(p[2]), tm = 0;
            p += 3;
            if (*p == ':') p++;
            if (dig(p[0]) >= 0 && dig(p[1]) >= 0) { tm = dig(p[0]) * 10 + dig(p[1]); p += 2; }
            has_tz = true;
            tz_min = sg * (th * 60 + tm);
            if (th > 23 || tm > 59) { *out = nan_d(); return 1; }
        }
    } else if (*p == 'Z' && p[1] == 0 && false) {
        p++;
    }
    if (*p != 0) return 0;
    /* values out of range make the string invalid */
    bool leap = is_leap(year);
    if (month < 1 || month > 12 || day < 1 || day > cum_days[leap][month] - cum_days[leap][month - 1] ||
        hh > 24 || mm > 59 || ss > 59 || (hh == 24 && (mm || ss || ms))) {
        *out = nan_d();
        return 1;
    }
    double dayv = make_day(year, month - 1, day);
    double tv = make_date(dayv, make_time(hh, mm, ss, ms));
    if (has_tz) tv -= tz_min * MS_PER_MIN;
    else if (has_time) tv = utc_time(ctx, tv);      /* a date-time without an offset is local time */
    *out = time_clip(tv);
    return 1;
}

static int word_match(const char *w, size_t n, const char *const *full, int count) {
    if (n < 3) return -1;
    for (int i = 0; i < count; i++) {
        size_t fl = strlen(full[i]);
        if (n <= fl && !strncmp(w, full[i], n)) return i;
    }
    return -1;
}

static const struct { const char *name; int minutes; } zone_names[] = {
    { "utc", 0 }, { "gmt", 0 }, { "ut", 0 }, { "z", 0 }, { "est", -300 }, { "edt", -240 }, { "cst", -360 },
    { "cdt", -300 }, { "mst", -420 }, { "mdt", -360 }, { "pst", -480 }, { "pdt", -420 },
};

/* the spellings the specification leaves to the implementation */
static double parse_legacy(JSContext *ctx, const char *s) {
    const char *p = s;
    int nums[6], nn = 0;
    bool numfmt[6];                     /* true when the number had four or more digits */
    int month = -1, hh = -1, mm = 0, ss = 0, pm = -1;
    double ms = 0;
    bool has_tz = false;
    double tz_min = 0;
    bool slash_seen = false;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') { p++; continue; }
        if (c == '(') {
            int depth = 0;
            while (*p) { if (*p == '(') depth++; else if (*p == ')' && --depth == 0) { p++; break; } p++; }
            continue;
        }
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            char w[16];
            size_t n = 0;
            while (((unsigned char)*p | 0x20) >= 'a' && ((unsigned char)*p | 0x20) <= 'z' && n < sizeof w - 1) w[n++] = (char)(*p++ | 0x20);
            while (*p == '.') p++;
            w[n] = 0;
            if (!strcmp(w, "am")) { pm = 0; continue; }
            if (!strcmp(w, "pm")) { pm = 1; continue; }
            if (!strcmp(w, "t") && nn > 0) continue;
            int mi = word_match(w, n, month_full, 12);
            if (mi >= 0) { month = mi; continue; }
            if (word_match(w, n, day_full, 7) >= 0) continue;
            bool known = false;
            for (size_t i = 0; i < KZ_COUNTOF(zone_names); i++)
                if (!strcmp(w, zone_names[i].name)) { has_tz = true; tz_min = zone_names[i].minutes; known = true; break; }
            if (!known) return nan_d();
            continue;
        }
        if ((c == '+' || c == '-') && (has_tz || hh >= 0) && dig(p[1]) >= 0) {
            int sg = c == '-' ? -1 : 1;
            p++;
            int v = 0, nd2 = 0;
            while (dig(*p) >= 0 && nd2 < 4) { v = v * 10 + dig(*p); p++; nd2++; }
            int th, tm;
            if (*p == ':' ) { p++; th = v; tm = 0; while (dig(*p) >= 0) { tm = tm * 10 + dig(*p); p++; } }
            else if (nd2 <= 2) { th = v; tm = 0; }
            else { th = v / 100; tm = v % 100; }
            has_tz = true;
            tz_min = sg * (th * 60 + tm);
            continue;
        }
        if (c == '/' || c == '-' || c == '.') { if (c == '/') slash_seen = true; p++; continue; }
        if (dig((char)c) >= 0) {
            long v = 0;
            int nd2 = 0;
            while (dig(*p) >= 0) { if (v < 100000000) v = v * 10 + dig(*p); p++; nd2++; }
            if (*p == ':' && nd2 <= 2) {
                hh = (int)v;
                p++;
                mm = 0;
                while (dig(*p) >= 0) { mm = mm * 10 + dig(*p); p++; }
                if (*p == ':') {
                    p++;
                    ss = 0;
                    while (dig(*p) >= 0) { ss = ss * 10 + dig(*p); p++; }
                    if (*p == '.') {
                        p++;
                        double scale = 100;
                        while (dig(*p) >= 0) { ms += dig(*p) * scale; scale /= 10; p++; }
                        ms = floor_d(ms);
                    }
                }
                continue;
            }
            if (nn >= 6) return nan_d();
            nums[nn] = (int)v;
            numfmt[nn] = nd2 >= 3 || v > 31;
            nn++;
            continue;
        }
        return nan_d();
    }
    int year, day;
    if (month >= 0) {                                   /* "Nov 3 2026" or "3 Nov 2026" */
        if (nn == 0) return nan_d();
        if (nn == 1) { if (numfmt[0]) { year = nums[0]; day = 1; } else return nan_d(); }
        else if (numfmt[0] && !numfmt[1]) { year = nums[0]; day = nums[1]; }
        else { day = nums[0]; year = nums[1]; }
    } else {
        if (nn < 2) return nan_d();
        if (numfmt[0]) { year = nums[0]; month = nums[1] - 1; day = nn > 2 ? nums[2] : 1; }                 /* 2026/11/03 */
        else if (slash_seen || nn == 3) { month = nums[0] - 1; day = nums[1]; year = nn > 2 ? nums[2] : 2001; }   /* 11/3/2026 */
        else return nan_d();
    }
    if (year >= 0 && year < 50 && !numfmt[nn > 0 ? nn - 1 : 0]) year += 2000;
    else if (year >= 50 && year < 100) year += 1900;
    if (month < 0 || month > 11 || day < 1 || day > 31 || (hh >= 0 && (hh > 24 || mm > 59 || ss > 59))) return nan_d();
    if (hh < 0) hh = 0;
    if (pm == 1 && hh < 12) hh += 12;
    else if (pm == 0 && hh == 12) hh = 0;
    double tv = make_date(make_day(year, month, day), make_time(hh, mm, ss, ms));
    if (has_tz) tv -= tz_min * MS_PER_MIN;
    else tv = utc_time(ctx, tv);
    return time_clip(tv);
}

static double parse_date_string(JSContext *ctx, JSString *str) {
    char buf[160];
    uint32_t n = kz_str_len(str);
    if (n >= sizeof buf) return nan_d();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = kz_str_at(str, i);
        buf[i] = c < 128 && c ? (char)c : '?';
    }
    buf[n] = 0;
    char *b = buf;
    while (*b == ' ') b++;
    double v;
    if (parse_iso(ctx, b, &v)) return v;
    return parse_legacy(ctx, b);
}

/* ================================================================== the constructor and statics */

static bool is_date(JSValue v) { return kz_is_class(v, KZ_CLASS_DATE); }

static int this_time(JSContext *ctx, JSValue v, double *t) {
    if (!is_date(v)) { kz_throw_type(ctx, "this is not a Date object."); return -1; }
    *t = JV_OBJ(v)->u.date;
    return 0;
}

static JSValue date_ctor(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (rt->new_target == JS_UNDEFINED) return format_time_value(ctx, kz_date_now(ctx), FMT_STRING);
    double tv;
    KZ_ROOTS(ctx, r, 2);
    if (argc == 0) tv = kz_date_now(ctx);
    else if (argc == 1) {
        if (is_date(argv[0])) tv = JV_OBJ(argv[0])->u.date;
        else {
            r[0] = kz_to_primitive(ctx, argv[0], KZ_HINT_DEFAULT);
            if (r[0] == JS_EXCEPTION) return r[0];
            if (JV_IS_STRING(r[0])) {
                JSString *s = kz_str_flat(ctx, JV_STR(r[0]));
                if (!s) return JS_EXCEPTION;
                tv = parse_date_string(ctx, s);
            } else {
                double d;
                if (kz_to_number(ctx, r[0], &d)) return JS_EXCEPTION;
                tv = time_clip(d);
            }
        }
    } else {
        double f[7] = { nan_d(), 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 7 && i < argc; i++)
            if (kz_to_number(ctx, argv[i], &f[i])) return JS_EXCEPTION;
        double yr = f[0];
        if (yr == yr) {
            double yi = trunc_d(yr);
            if (yi >= 0 && yi <= 99) yr = 1900 + yi;
        }
        tv = time_clip(utc_time(ctx, make_date(make_day(yr, f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
    }
    r[0] = kz_ordinary_create_from_ctor(ctx, rt->new_target, KZ_I_DATE_PROTO, KZ_CLASS_DATE);
    if (r[0] == JS_EXCEPTION) return r[0];
    JV_OBJ(r[0])->u.date = tv;
    return r[0];
}

static JSValue date_now_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return jv_number(kz_date_now(ctx));
}

static JSValue date_parse_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)argc; (void)magic;
    KZ_ROOT(ctx, s);
    s = kz_to_string(ctx, argv[0]);
    if (s == JS_EXCEPTION) return s;
    JSString *f = kz_str_flat(ctx, JV_STR(s));
    if (!f) return JS_EXCEPTION;
    return jv_number(parse_date_string(ctx, f));
}

static JSValue date_utc_fn(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)this_val; (void)magic;
    double f[7] = { nan_d(), 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < 7 && i < argc; i++)
        if (kz_to_number(ctx, argv[i], &f[i])) return JS_EXCEPTION;
    double yr = f[0];
    if (yr == yr) {
        double yi = trunc_d(yr);
        if (yi >= 0 && yi <= 99) yr = 1900 + yi;
    }
    return jv_number(time_clip(make_date(make_day(yr, f[1], f[2]), make_time(f[3], f[4], f[5], f[6]))));
}

/* ================================================================== getters and setters */

enum { F_MS, F_SEC, F_MIN, F_HOUR, F_DATE, F_MONTH, F_YEAR, F_DAY };
#define UTCBIT 0x100

static JSValue date_get(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    if (t != t) return KZ_NAN_BITS;
    if (!(magic & UTCBIT)) t = local_time(ctx, t);
    Civil c;
    civil_from_time(t, &c);
    switch (magic & 0xFF) {
    case F_MS: return JV_MKINT(c.ms);
    case F_SEC: return JV_MKINT(c.sec);
    case F_MIN: return JV_MKINT(c.min);
    case F_HOUR: return JV_MKINT(c.hour);
    case F_DATE: return JV_MKINT(c.day);
    case F_MONTH: return JV_MKINT(c.month);
    case F_DAY: return JV_MKINT(c.wday);
    default: return jv_number(c.year);
    }
}

static JSValue date_get_time(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    return jv_number(t);
}

static JSValue date_get_tz_offset(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    if (t != t) return KZ_NAN_BITS;
    return jv_number((t - local_time(ctx, t)) / MS_PER_MIN);
}

/* Annex B getYear */
static JSValue date_get_year(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    if (t != t) return KZ_NAN_BITS;
    Civil c;
    civil_from_time(local_time(ctx, t), &c);
    return jv_number(c.year - 1900);
}

static JSValue date_set_time(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    double t, d;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    if (kz_to_number(ctx, argv[0], &d)) return JS_EXCEPTION;
    double v = time_clip(d);
    JV_OBJ(this_val)->u.date = v;
    return jv_number(v);
}

/* the setters: the first field they set, and how many arguments they take */
static const struct { uint8_t first, max; } setter_shape[] = {
    [F_MS] = { F_MS, 1 }, [F_SEC] = { F_SEC, 2 }, [F_MIN] = { F_MIN, 3 }, [F_HOUR] = { F_HOUR, 4 },
    [F_DATE] = { F_DATE, 1 }, [F_MONTH] = { F_MONTH, 2 }, [F_YEAR] = { F_YEAR, 3 },
};

static JSValue date_set(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    int field = magic & 0xFF;
    bool utc = (magic & UTCBIT) != 0;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    int max = setter_shape[field].max;
    int na = argc < 1 ? 1 : argc > max ? max : argc;
    double a[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < na; i++)
        if (kz_to_number(ctx, argv[i], &a[i])) return JS_EXCEPTION;
    if (t != t) {
        if (field != F_YEAR) return KZ_NAN_BITS;
        t = 0;
    } else if (!utc) t = local_time(ctx, t);
    Civil c;
    civil_from_time(t, &c);
    double y = c.year, mo = c.month, d = c.day, h = c.hour, mi = c.min, s = c.sec, ms = c.ms;
    switch (field) {
    case F_MS: ms = a[0]; break;
    case F_SEC: s = a[0]; if (na > 1) ms = a[1]; break;
    case F_MIN: mi = a[0]; if (na > 1) s = a[1]; if (na > 2) ms = a[2]; break;
    case F_HOUR: h = a[0]; if (na > 1) mi = a[1]; if (na > 2) s = a[2]; if (na > 3) ms = a[3]; break;
    case F_DATE: d = a[0]; break;
    case F_MONTH: mo = a[0]; if (na > 1) d = a[1]; break;
    default: y = a[0]; if (na > 1) mo = a[1]; if (na > 2) d = a[2]; break;
    }
    double nd = make_date(make_day(y, mo, d), make_time(h, mi, s, ms));
    double v = time_clip(utc ? nd : utc_time(ctx, nd));
    JV_OBJ(this_val)->u.date = v;
    return jv_number(v);
}

/* Annex B setYear */
static JSValue date_set_year(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    double t, y;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    if (kz_to_number(ctx, argv[0], &y)) return JS_EXCEPTION;
    if (y != y) { JV_OBJ(this_val)->u.date = nan_d(); return KZ_NAN_BITS; }
    double yi = trunc_d(y);
    double yyyy = (yi >= 0 && yi <= 99) ? yi + 1900 : y;
    t = t != t ? 0 : local_time(ctx, t);
    Civil c;
    civil_from_time(t, &c);
    double nd = make_date(make_day(yyyy, c.month, c.day), time_within_day(t));
    double v = time_clip(utc_time(ctx, nd));
    JV_OBJ(this_val)->u.date = v;
    return jv_number(v);
}

/* ================================================================== conversions to strings */

static JSValue date_to_string(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    return format_time_value(ctx, t, magic);
}

static JSValue date_to_iso(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    double t;
    if (this_time(ctx, this_val, &t)) return JS_EXCEPTION;
    if (t != t) return kz_throw_range(ctx, "Invalid time value");
    return format_time_value(ctx, t, FMT_ISO);
}

static JSValue date_to_json(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)argv; (void)magic;
    KZ_ROOTS(ctx, r, 2);
    r[0] = kz_to_object(ctx, this_val);
    if (r[0] == JS_EXCEPTION) return r[0];
    r[1] = kz_to_primitive(ctx, r[0], KZ_HINT_NUMBER);
    if (r[1] == JS_EXCEPTION) return r[1];
    if (JV_IS_NUMBER(r[1])) {
        double d = jv_num(r[1]);
        if (!finite_d(d)) return JS_NULL;
    }
    return kz_invoke(ctx, r[0], ctx->rt->atom[KZ_ATOM_toISOString], 0, NULL);
}

/* Date.prototype[@@toPrimitive](hint) */
static JSValue date_to_primitive(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic) {
    (void)argc; (void)magic;
    JSRuntime *rt = ctx->rt;
    if (!JV_IS_OBJECT(this_val)) return kz_throw_type(ctx, "Date.prototype[Symbol.toPrimitive] called on a non-object");
    int hint;
    if (JV_IS_STRING(argv[0])) {
        JSString *h = kz_str_flat(ctx, JV_STR(argv[0]));
        if (!h) return JS_EXCEPTION;
        if (kz_str_eq_ascii(h, "string") || kz_str_eq_ascii(h, "default")) hint = KZ_HINT_STRING;
        else if (kz_str_eq_ascii(h, "number")) hint = KZ_HINT_NUMBER;
        else return kz_throw_type(ctx, "Invalid hint: %s", "value");
    } else return kz_throw_type(ctx, "Invalid hint");
    JSValue names[2] = { rt->atom[KZ_ATOM_toString], rt->atom[KZ_ATOM_valueOf] };
    if (hint == KZ_HINT_NUMBER) { JSValue t = names[0]; names[0] = names[1]; names[1] = t; }
    KZ_ROOTS(ctx, r, 2);
    for (int i = 0; i < 2; i++) {
        r[0] = kz_get(ctx, this_val, names[i], this_val);
        if (r[0] == JS_EXCEPTION) return r[0];
        if (kz_is_callable(r[0])) {
            r[1] = kz_call(ctx, r[0], this_val, 0, NULL);
            if (r[1] == JS_EXCEPTION) return r[1];
            if (!JV_IS_OBJECT(r[1])) return r[1];
        }
    }
    return kz_throw_type(ctx, "Cannot convert object to primitive value");
}

/* ================================================================== setup */

static const KzFuncDef date_funcs[] = {
    KZ_FN("now", date_now_fn, 0),
    KZ_FN("parse", date_parse_fn, 1),
    KZ_FN("UTC", date_utc_fn, 7),
};

#define G(name, field, utc) KZ_FN_MAGIC(name, date_get, 0, (field) | (utc))
#define S(name, field, utc, len) KZ_FN_MAGIC_ARGS(name, date_set, len, setter_nargs[field], (field) | (utc))

static const uint8_t setter_nargs[] = { [F_MS] = 1, [F_SEC] = 2, [F_MIN] = 3, [F_HOUR] = 4, [F_DATE] = 1, [F_MONTH] = 2, [F_YEAR] = 3 };

static const KzFuncDef date_proto_funcs[] = {
    G("getDate", F_DATE, 0), G("getDay", F_DAY, 0), G("getFullYear", F_YEAR, 0), G("getHours", F_HOUR, 0),
    G("getMilliseconds", F_MS, 0), G("getMinutes", F_MIN, 0), G("getMonth", F_MONTH, 0), G("getSeconds", F_SEC, 0),
    KZ_FN("getTime", date_get_time, 0),
    KZ_FN("getTimezoneOffset", date_get_tz_offset, 0),
    G("getUTCDate", F_DATE, UTCBIT), G("getUTCDay", F_DAY, UTCBIT), G("getUTCFullYear", F_YEAR, UTCBIT),
    G("getUTCHours", F_HOUR, UTCBIT), G("getUTCMilliseconds", F_MS, UTCBIT), G("getUTCMinutes", F_MIN, UTCBIT),
    G("getUTCMonth", F_MONTH, UTCBIT), G("getUTCSeconds", F_SEC, UTCBIT),
    KZ_FN("getYear", date_get_year, 0),
    S("setDate", F_DATE, 0, 1), S("setFullYear", F_YEAR, 0, 3), S("setHours", F_HOUR, 0, 4),
    S("setMilliseconds", F_MS, 0, 1), S("setMinutes", F_MIN, 0, 3), S("setMonth", F_MONTH, 0, 2), S("setSeconds", F_SEC, 0, 2),
    KZ_FN("setTime", date_set_time, 1),
    S("setUTCDate", F_DATE, UTCBIT, 1), S("setUTCFullYear", F_YEAR, UTCBIT, 3), S("setUTCHours", F_HOUR, UTCBIT, 4),
    S("setUTCMilliseconds", F_MS, UTCBIT, 1), S("setUTCMinutes", F_MIN, UTCBIT, 3), S("setUTCMonth", F_MONTH, UTCBIT, 2),
    S("setUTCSeconds", F_SEC, UTCBIT, 2),
    KZ_FN("setYear", date_set_year, 1),
    KZ_FN_MAGIC("toDateString", date_to_string, 0, FMT_DATE),
    KZ_FN("toISOString", date_to_iso, 0),
    KZ_FN("toJSON", date_to_json, 1),
    KZ_FN_MAGIC("toLocaleDateString", date_to_string, 0, FMT_LOCALE_DATE),
    KZ_FN_MAGIC("toLocaleString", date_to_string, 0, FMT_LOCALE),
    KZ_FN_MAGIC("toLocaleTimeString", date_to_string, 0, FMT_LOCALE_TIME),
    KZ_FN_MAGIC("toString", date_to_string, 0, FMT_STRING),
    KZ_FN_MAGIC("toTimeString", date_to_string, 0, FMT_TIME),
    KZ_FN_MAGIC("toUTCString", date_to_string, 0, FMT_UTC),
    KZ_FN("valueOf", date_get_time, 0),
};

int kz_init_date(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    KZ_ROOTS(ctx, r, 3);
    r[0] = kz_new_object(ctx);                  /* %Date.prototype% is an ordinary object */
    if (r[0] == JS_EXCEPTION) return -1;
    ctx->intrinsic[KZ_I_DATE_PROTO] = r[0];
    r[1] = kz_make_ctor(ctx, date_ctor, "Date", 7, 0, KZ_I_DATE_PROTO, KZ_I_DATE);
    if (r[1] == JS_EXCEPTION) return -1;
    if (kz_define_functions(ctx, r[1], date_funcs, KZ_COUNTOF(date_funcs))) return -1;
    if (kz_define_functions(ctx, r[0], date_proto_funcs, KZ_COUNTOF(date_proto_funcs))) return -1;
    /* toGMTString is the same function object as toUTCString (B.2.3.3); [Symbol.toPrimitive] is not writable */
    JSValue k = kz_atom(ctx, "toUTCString");
    if (k == JS_EXCEPTION) return -1;
    r[2] = kz_get(ctx, r[0], k, r[0]);
    if (r[2] == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[0], kz_atom(ctx, "toGMTString"), r[2], KZ_PROP_W | KZ_PROP_C)) return -1;
    r[2] = kz_new_native_c(ctx, date_to_primitive, "[Symbol.toPrimitive]", 1, 0);
    if (r[2] == JS_EXCEPTION) return -1;
    if (kz_define_value(ctx, r[0], rt->sym[KZ_SYM_toPrimitive], r[2], KZ_PROP_C)) return -1;
    if (kz_define_value_atom(ctx, ctx->global, KZ_ATOM_Date, r[1], KZ_PROP_W | KZ_PROP_C)) return -1;
    return 0;
}
