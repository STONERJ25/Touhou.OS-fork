/* getpriority, setpriority, nice: scheduling priorities (see docs/JOBCONTROL.md). */
#include "syscall_priv.h"
#include <sys/resource.h>
#include <unistd.h>

int getpriority(int which, id_t who) {
    long r = __sc2(SYS_getpriority, which, (long)who);
    if (r < 0) { errno = (int)-r; return -1; }
    errno = 0;
    return 20 - (int)r;                     /* the kernel returns 20 - nice so that the result is never negative */
}

int setpriority(int which, id_t who, int prio) {
    return (int)__ret(__sc3(SYS_setpriority, which, (long)who, prio));
}

int nice(int incr) {
    errno = 0;
    int cur = getpriority(PRIO_PROCESS, 0);
    if (cur == -1 && errno) return -1;
    int want = cur + incr;
    if (want > NICE_MAX) want = NICE_MAX;
    if (want < NICE_MIN) want = NICE_MIN;
    if (setpriority(PRIO_PROCESS, 0, want) < 0) {
        if (errno == EACCES) errno = EPERM;     /* the traditional answer to a refused nice(-n) */
        return -1;
    }
    return want;
}
