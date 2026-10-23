/* Calendar time (UTC only: Gensokyo keeps its own time zone). */
#include <time.h>
#include <string.h>
#include <stdio.h>

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
    return out;
}

struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
struct tm *localtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm) {
    long y = tm->tm_year + 1900 + tm->tm_mon / 12;
    int mon = tm->tm_mon % 12;
    if (mon < 0) { mon += 12; y--; }
    long days = days_from_civil(y, (unsigned)mon + 1, 1) + tm->tm_mday - 1;
    return days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
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
        case 'Z': OUT("UTC"); break;
        case 'z': OUT("+0000"); break;
        case 'F': snprintf(tmp, sizeof tmp, "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); OUT(tmp); break;
        case 'T': snprintf(tmp, sizeof tmp, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); OUT(tmp); break;
        case 'R': snprintf(tmp, sizeof tmp, "%02d:%02d", tm->tm_hour, tm->tm_min); OUT(tmp); break;
        case 'D': snprintf(tmp, sizeof tmp, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); OUT(tmp); break;
        case 'c': snprintf(tmp, sizeof tmp, "%.3s %.3s %2d %02d:%02d:%02d %d", wday_names[tm->tm_wday % 7],
                           mon_names[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
                  OUT(tmp); break;
        case 's': snprintf(tmp, sizeof tmp, "%ld", (long)mktime((struct tm *)tm)); OUT(tmp); break;
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

char *ctime(const time_t *t) { return asctime(gmtime(t)); }
