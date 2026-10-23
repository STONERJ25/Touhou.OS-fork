/*
 * Calendar time with POSIX time zones.
 *
 * The zone comes from $TZ, else the first line of /etc/timezone, else UTC. The format is the POSIX one:
 *     NAME offset [DSTNAME [offset] [,start[/time],end[/time]]]
 * with the offset as hours (and optional :minutes) WEST of Greenwich, and rules like M3.2.0 (month 3, second Sunday).
 * Examples:  UTC    JST-9    EST5EDT,M3.2.0,M11.1.0    CET-1CEST,M3.5.0,M10.5.0/3
 * With a name and no rule after the DST name, the US rules (second Sunday of March to first Sunday of November,
 * 02:00 local) are used.
 */
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static const char *const wday_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mon_names[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                         "September", "October", "November", "December" };

static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/* ---------------------------------------------------------------- zones */

typedef struct { int month, week, wday, secs; } Rule;          /* Mm.w.d/time; week 5 means "the last" */

typedef struct {
    char std[12], dst[12];
    long std_off, dst_off;                                      /* seconds EAST of UTC */
    int has_dst;
    Rule start, end;
} Zone;

static Zone g_zone;
static int g_zone_ready;
static char g_zone_src[96];

char *tzname[2] = { "UTC", "UTC" };
long timezone = 0;
int daylight = 0;

static int parse_name(const char **p, char *out) {
    const char *s = *p;
    int n = 0;
    if (*s == '<') {                                            /* <+0530> style names */
        s++;
        while (*s && *s != '>' && n < 11) out[n++] = *s++;
        if (*s == '>') s++;
    } else {
        while (((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) && n < 11) out[n++] = *s++;
    }
    out[n] = 0;
    *p = s;
    return n >= 3 || (n > 0 && s != *p);
}

/* [+-]hh[:mm[:ss]] in seconds */
static int parse_hms(const char **p, long *out) {
    const char *s = *p;
    int sign = 1;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }
    if (*s < '0' || *s > '9') return 0;
    long v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        long n = 0;
        while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
        v[i] = n;
        if (*s != ':' || i == 2) break;
        s++;
    }
    *out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    *p = s;
    return 1;
}

static int parse_rule(const char **p, Rule *r) {
    const char *s = *p;
    r->secs = 7200;
    if (*s == ',') s++;
    if (*s != 'M') return 0;                                    /* Julian-day rules are not supported */
    s++;
    r->month = (int)strtol(s, (char **)&s, 10);
    if (*s++ != '.') return 0;
    r->week = (int)strtol(s, (char **)&s, 10);
    if (*s++ != '.') return 0;
    r->wday = (int)strtol(s, (char **)&s, 10);
    if (*s == '/') {
        s++;
        long t;
        if (!parse_hms(&s, &t)) return 0;
        r->secs = (int)t;
    }
    *p = s;
    return r->month >= 1 && r->month <= 12 && r->week >= 1 && r->week <= 5 && r->wday >= 0 && r->wday <= 6;
}

static void zone_utc(void) {
    memset(&g_zone, 0, sizeof g_zone);
    strcpy(g_zone.std, "UTC");
    strcpy(g_zone.dst, "UTC");
}

static void zone_parse(const char *tz) {
    zone_utc();
    const char *s = tz;
    long off;
    if (!parse_name(&s, g_zone.std) || !parse_hms(&s, &off)) { zone_utc(); return; }
    g_zone.std_off = -off;
    if (*s && *s != ',') {
        if (parse_name(&s, g_zone.dst)) {
            g_zone.has_dst = 1;
            g_zone.dst_off = g_zone.std_off + 3600;
            if (*s && *s != ',' && parse_hms(&s, &off)) g_zone.dst_off = -off;
            /* default rules: the United States */
            g_zone.start = (Rule){ 3, 2, 0, 7200 };
            g_zone.end = (Rule){ 11, 1, 0, 7200 };
            if (*s == ',') {
                if (!parse_rule(&s, &g_zone.start) || *s != ',' || !parse_rule(&s, &g_zone.end)) { zone_utc(); return; }
            }
        }
    }
}

static void zone_load(void) {
    if (g_zone_ready) return;
    g_zone_ready = 1;
    const char *tz = getenv("TZ");
    char buf[96];
    if (!tz || !*tz) {
        int fd = open("/etc/timezone", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) {
                buf[n] = 0;
                buf[strcspn(buf, "\r\n")] = 0;
                tz = buf;
            }
        }
    }
    if (tz && *tz) {
        snprintf(g_zone_src, sizeof g_zone_src, "%s", tz);
        zone_parse(g_zone_src);
    } else {
        zone_utc();
    }
    tzname[0] = g_zone.std;
    tzname[1] = g_zone.dst;
    timezone = -g_zone.std_off;
    daylight = g_zone.has_dst;
}

void tzset(void) {
    g_zone_ready = 0;
    zone_load();
}

/* The UTC instant at which `r` takes effect in `year`, given the local offset in force just before it. */
static long rule_instant(long year, const Rule *r, long offset_before) {
    long first = days_from_civil(year, (unsigned)r->month, 1);
    int first_wday = (int)((first + 4) % 7);
    if (first_wday < 0) first_wday += 7;
    int day = 1 + (r->wday - first_wday + 7) % 7 + (r->week - 1) * 7;
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = mdays[r->month - 1] + (r->month == 2 && is_leap(year));
    while (day > dim) day -= 7;                                 /* week 5: the last one in the month */
    return (first + day - 1) * 86400L + r->secs - offset_before;
}

static int year_of(long t) {
    struct tm tm;
    time_t tt = t;
    gmtime_r(&tt, &tm);
    return tm.tm_year + 1900;
}

/* Local offset (seconds east) in force at the UTC instant t; *isdst tells whether it is the summer one. */
static long zone_offset(long t, int *isdst) {
    zone_load();
    *isdst = 0;
    if (!g_zone.has_dst) return g_zone.std_off;
    long y = year_of(t + g_zone.std_off);
    long s = rule_instant(y, &g_zone.start, g_zone.std_off);
    long e = rule_instant(y, &g_zone.end, g_zone.dst_off);
    int in_dst = s < e ? (t >= s && t < e) : !(t >= e && t < s);       /* southern hemisphere: the summer wraps the new year */
    *isdst = in_dst;
    return in_dst ? g_zone.dst_off : g_zone.std_off;
}

/* ---------------------------------------------------------------- conversion */

struct tm *gmtime_r(const time_t *tp, struct tm *out) {
    long t = *tp;
    long days = t / 86400;
    long rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) out->tm_wday += 7;
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    out->tm_year = (int)(y + (m <= 2) - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_yday = (int)(days - days_from_civil(y + (m <= 2), 1, 1));
    out->tm_isdst = 0;
    out->tm_gmtoff = 0;
    out->tm_zone = "UTC";
    return out;
}

struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }

struct tm *localtime_r(const time_t *tp, struct tm *out) {
    int isdst;
    long off = zone_offset(*tp, &isdst);
    time_t shifted = *tp + off;
    gmtime_r(&shifted, out);
    out->tm_isdst = isdst;
    out->tm_gmtoff = off;
    out->tm_zone = isdst ? g_zone.dst : g_zone.std;
    return out;
}

struct tm *localtime(const time_t *t) { static struct tm tm; return localtime_r(t, &tm); }

/* The calendar fields taken as UTC. */
time_t timegm(struct tm *tm) {
    long y = tm->tm_year + 1900 + tm->tm_mon / 12;
    int mon = tm->tm_mon % 12;
    if (mon < 0) { mon += 12; y--; }
    long days = days_from_civil(y, (unsigned)mon + 1, 1) + tm->tm_mday - 1;
    return days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
}

/* The calendar fields taken as local time (in the zone above); normalises *tm like the standard says. */
time_t mktime(struct tm *tm) {
    long naive = timegm(tm);
    int isdst;
    long off = zone_offset(naive - g_zone.std_off, &isdst);         /* a first guess: the offset around that moment */
    long t = naive - off;
    long off2 = zone_offset(t, &isdst);                             /* one refinement settles all but the ambiguous hour */
    if (off2 != off) t = naive - off2;
    time_t tt = t;
    localtime_r(&tt, tm);
    return t;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

size_t strftime(char *buf, size_t n, const char *fmt, const struct tm *tm) {
    size_t pos = 0;
    char tmp[64];
#define OUT(s) do { const char *_s = (s); while (*_s && pos + 1 < n) buf[pos++] = *_s++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { if (pos + 1 < n) buf[pos++] = *fmt; continue; }
        fmt++;
        switch (*fmt) {
        case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); OUT(tmp); break;
        case 'y': snprintf(tmp, sizeof tmp, "%02d", tm->tm_year % 100); OUT(tmp); break;
        case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); OUT(tmp); break;
        case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); OUT(tmp); break;
        case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); OUT(tmp); break;
        case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); OUT(tmp); break;
        case 'I': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); OUT(tmp); break;
        case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); OUT(tmp); break;
        case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); OUT(tmp); break;
        case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); OUT(tmp); break;
        case 'p': OUT(tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'a': snprintf(tmp, sizeof tmp, "%.3s", wday_names[tm->tm_wday % 7]); OUT(tmp); break;
        case 'A': OUT(wday_names[tm->tm_wday % 7]); break;
        case 'b': case 'h': snprintf(tmp, sizeof tmp, "%.3s", mon_names[tm->tm_mon % 12]); OUT(tmp); break;
        case 'B': OUT(mon_names[tm->tm_mon % 12]); break;
        case 'Z': OUT(tm->tm_zone ? tm->tm_zone : "UTC"); break;
        case 'z': {
            long off = tm->tm_gmtoff;
            snprintf(tmp, sizeof tmp, "%c%02ld%02ld", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 3600, (off < 0 ? -off : off) % 3600 / 60);
            OUT(tmp);
            break;
        }
        case 'F': snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); OUT(tmp); break;
        case 'T': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); OUT(tmp); break;
        case 'R': snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); OUT(tmp); break;
        case 'D': snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); OUT(tmp); break;
        case 'c': snprintf(tmp, sizeof tmp, "%.3s %.3s %2d %02d:%02d:%02d %d", wday_names[tm->tm_wday % 7],
                           mon_names[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
                  OUT(tmp); break;
        case 's': {
            struct tm copy = *tm;
            snprintf(tmp, sizeof tmp, "%ld", (long)mktime(&copy));
            OUT(tmp);
            break;
        }
        case '%': OUT("%"); break;
        default: break;
        }
    }
    if (n) buf[pos < n ? pos : n - 1] = 0;
    return pos;
#undef OUT
}

char *asctime(const struct tm *tm) {
    static char buf[32];
    strftime(buf, sizeof buf, "%a %b %e %H:%M:%S %Y\n", tm);
    return buf;
}

char *ctime(const time_t *t) { return asctime(localtime(t)); }
