#ifndef _TIME_H
#define _TIME_H
#include <sys/types.h>
#include <abi.h>
#define CLOCKS_PER_SEC 1000000
struct timespec { time_t tv_sec; long tv_nsec; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;                 /* seconds east of UTC */
    const char *tm_zone;            /* abbreviation of the zone: "UTC", "JST", "EDT" */
};
time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t a, time_t b);
int clock_gettime(int clk, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *tm);       /* the fields are local time */
time_t timegm(struct tm *tm);       /* the fields are UTC */
size_t strftime(char *buf, size_t n, const char *fmt, const struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);

/* Time zone: $TZ, else the first line of /etc/timezone, else UTC. POSIX syntax: EST5EDT,M3.2.0,M11.1.0  JST-9  UTC */
void tzset(void);
extern char *tzname[2];
extern long timezone;               /* seconds WEST of UTC, standard time */
extern int daylight;
#endif
