#include "dates.h"

const char *const g_month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December",
};
const char *const g_weekday_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

bool date_is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int date_days_in_month(int y, int m) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 30;
    return dm[m - 1] + (m == 2 && date_is_leap(y));
}

/* Howard Hinnant's days_from_civil: exact for every year, negative or not */
long date_to_days(int y, int m, int d) {
    long yy = y - (m <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    long yoe = yy - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void date_from_days(long z, int *y, int *m, int *d) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long dd = doy - (153 * mp + 2) / 5 + 1;
    long mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

int date_weekday(int y, int m, int d) {
    long days = date_to_days(y, m, d);
    int w = (int)((days + 4) % 7);                                 /* 1970-01-01 was a Thursday */
    return w < 0 ? w + 7 : w;
}

int date_day_of_year(int y, int m, int d) { return (int)(date_to_days(y, m, d) - date_to_days(y, 1, 1)) + 1; }

static int p_of(int y) { return ((y + y / 4 - y / 100 + y / 400) % 7 + 7) % 7; }

int date_weeks_in_iso_year(int y) { return 52 + (p_of(y) == 4 || p_of(y - 1) == 3 ? 1 : 0); }

int date_iso_week(int y, int m, int d, int *iso_year) {
    int wd = date_weekday(y, m, d);
    int isowd = wd == 0 ? 7 : wd;                                  /* Monday = 1 .. Sunday = 7 */
    int doy = date_day_of_year(y, m, d);
    int week = (doy - isowd + 10) / 7;
    int iy = y;
    if (week < 1) { iy = y - 1; week = date_weeks_in_iso_year(iy); }
    else if (week > date_weeks_in_iso_year(y)) { iy = y + 1; week = 1; }
    if (iso_year) *iso_year = iy;
    return week;
}

void date_add_days(int *y, int *m, int *d, long delta) { date_from_days(date_to_days(*y, *m, *d) + delta, y, m, d); }

void date_add_months(int *y, int *m, int delta) {
    int total = *y * 12 + (*m - 1) + delta;
    int ny = total / 12, nm = total % 12;
    if (nm < 0) { nm += 12; ny--; }
    *y = ny;
    *m = nm + 1;
}
