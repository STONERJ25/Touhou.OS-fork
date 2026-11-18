#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/types.h>
#include <abi.h>

/* PRIO_PROCESS, PRIO_PGRP, PRIO_USER (abi.h). Nice values run from -20 (greedy) to 19 (polite); the default is 0. */
int getpriority(int which, id_t who);       /* the nice value, or -1 with errno set (clear errno first: -1 is also a nice value) */
int setpriority(int which, id_t who, int prio);

/* Resource limits (docs/HARDENING.md): RLIMIT_* and RLIM_INFINITY are in abi.h, with the numbers of Linux. A limit has a soft value, which the kernel enforces, and a
 * hard value, the ceiling for the soft one. Raise the soft limit up to the hard one as you like; lower the hard one and it stays lowered (only root raises it). */
typedef unsigned long rlim_t;
struct rlimit {
    rlim_t rlim_cur;                        /* soft */
    rlim_t rlim_max;                        /* hard */
};
int getrlimit(int resource, struct rlimit *rlim);                                           /* 0, or -1 with errno: EINVAL for a resource that does not exist */
int setrlimit(int resource, const struct rlimit *rlim);                                     /* EINVAL: soft above hard; EPERM: the hard limit would go up and the caller is not root */
int prlimit(pid_t pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit);   /* both at once, for pid (0 = this process): the same user's processes, or any as root */

#endif
