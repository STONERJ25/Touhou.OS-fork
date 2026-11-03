/* utime / utimes / lutimes on top of the SYS_utimes system call. */
#include "syscall_priv.h"
#include <utime.h>
#include <stddef.h>

static int set_times(const char *path, const struct timeval *tv, int flags) {
    struct gtimespec ts[2];
    if (tv) {
        for (int i = 0; i < 2; i++) { ts[i].tv_sec = tv[i].tv_sec; ts[i].tv_nsec = (int64_t)tv[i].tv_usec * 1000; }
    }
    return (int)__ret(__sc3(SYS_utimes, (long)path, tv ? (long)ts : 0, flags));
}

int utimes(const char *path, const struct timeval times[2]) { return set_times(path, times, 0); }
int lutimes(const char *path, const struct timeval times[2]) { return set_times(path, times, 1); }

int utime(const char *path, const struct utimbuf *t) {
    if (!t) return set_times(path, NULL, 0);
    struct timeval tv[2] = { { t->actime, 0 }, { t->modtime, 0 } };
    return set_times(path, tv, 0);
}
