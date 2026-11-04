/*
 * The "ask the system about itself" calls a C library and ordinary tools make: statx (what current coreutils use for stat and ls), sysinfo, the processor mask,
 * prctl's process name, uname, the page size and the clock tick, gmtime/strftime (pure library code, to see the library itself works), getrandom with flags.
 * Prints only what the manual pages promise, so a real Linux prints the same (tools/linux-oracle.py); tests/case_linux.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    struct statx sx;
    int r = statx(AT_FDCWD, "/", AT_STATX_SYNC_AS_STAT, STATX_BASIC_STATS, &sx);
    printf("statx of /: %d\n", r);
    printf("mask has the basic fields: %d\n", (sx.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS);
    printf("/ is a directory: %d, nlink positive: %d\n", S_ISDIR(sx.stx_mode) ? 1 : 0, sx.stx_nlink > 0);
    errno = 0;
    r = statx(AT_FDCWD, "/no/such/path", 0, STATX_BASIC_STATS, &sx);
    printf("statx of a missing path: %d, errno %d\n", r, errno);
    errno = 0;
    r = statx(AT_FDCWD, "/", 0x40000000, STATX_BASIC_STATS, &sx);
    printf("statx with unknown flags: %d, errno %d\n", r, errno);
    r = statx(1, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &sx);
    printf("statx of stdout by descriptor: %d\n", r);

    struct sysinfo si;
    r = sysinfo(&si);
    printf("sysinfo: %d, memory known: %d, free <= total: %d, unit positive: %d, processes: %d, uptime positive: %d\n", r, si.totalram > 0,
           si.freeram <= si.totalram, si.mem_unit >= 1, si.procs >= 1, si.uptime > 0);

    cpu_set_t set;
    CPU_ZERO(&set);
    r = sched_getaffinity(0, sizeof set, &set);
    printf("sched_getaffinity: %d, at least one processor: %d\n", r, CPU_COUNT(&set) >= 1);
    printf("online processors: %d\n", sysconf(_SC_NPROCESSORS_ONLN) >= 1);

    prctl(PR_SET_NAME, "lnx-sys-test", 0, 0, 0);
    char name[32];
    memset(name, 0, sizeof name);
    r = prctl(PR_GET_NAME, name, 0, 0, 0);
    printf("prctl name: %d, %s\n", r, name);
    errno = 0;
    r = prctl(0x7777, 0, 0, 0, 0);
    printf("prctl with an unknown option: %d, errno %d\n", r, errno);

    struct utsname u;
    uname(&u);
    printf("uname: %s %s\n", u.sysname, u.machine);
    printf("page size %ld, clock tick %ld\n", sysconf(_SC_PAGESIZE), sysconf(_SC_CLK_TCK));
    printf("open files limit positive: %d\n", sysconf(_SC_OPEN_MAX) > 0);

    time_t t = 86400 * 365 + 3661;
    char buf[64];
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %a", &tmv);
    printf("gmtime: %s\n", buf);

    unsigned char rnd[8];
    errno = 0;
    ssize_t g = getrandom(rnd, sizeof rnd, GRND_NONBLOCK);
    printf("getrandom: %zd\n", g);
    errno = 0;
    g = getrandom(rnd, sizeof rnd, 0x1000);
    printf("getrandom with unknown flags: %zd, errno %d\n", g, errno);
    return 0;
}
