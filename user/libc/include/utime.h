#ifndef _UTIME_H
#define _UTIME_H
#include <sys/types.h>
#include <sys/time.h>

struct utimbuf { time_t actime, modtime; };

int utime(const char *path, const struct utimbuf *times);       /* NULL: now */
int utimes(const char *path, const struct timeval times[2]);    /* NULL: now */
int lutimes(const char *path, const struct timeval times[2]);   /* like utimes, but a final symlink is not followed */
#endif
