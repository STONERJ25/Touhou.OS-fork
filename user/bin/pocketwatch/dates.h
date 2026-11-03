/* Calendar arithmetic for any year of the proleptic Gregorian calendar (no dependence on time_t). */
#ifndef PW_DATES_H
#define PW_DATES_H

#include <stdbool.h>

extern const char *const g_month_names[12];
extern const char *const g_weekday_names[7];       /* Sunday first */

bool date_is_leap(int y);
int  date_days_in_month(int y, int m);                             /* m = 1..12 */
long date_to_days(int y, int m, int d);                            /* days since 1970-01-01 */
void date_from_days(long days, int *y, int *m, int *d);
int  date_weekday(int y, int m, int d);                            /* 0 = Sunday .. 6 = Saturday */
int  date_day_of_year(int y, int m, int d);                        /* 1..366 */
int  date_iso_week(int y, int m, int d, int *iso_year);            /* 1..53, weeks start on Monday */
int  date_weeks_in_iso_year(int y);                                /* 52 or 53 */
void date_add_days(int *y, int *m, int *d, long delta);
void date_add_months(int *y, int *m, int delta);

#endif
