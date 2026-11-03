#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <time.h>
struct timeval { time_t tv_sec; suseconds_t tv_usec; };
int gettimeofday(struct timeval *tv, void *tz);
#endif
