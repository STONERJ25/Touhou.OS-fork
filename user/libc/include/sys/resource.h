#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/types.h>
#include <abi.h>

/* PRIO_PROCESS, PRIO_PGRP, PRIO_USER (abi.h). Nice values run from -20 (greedy) to 19 (polite); the default is 0. */
int getpriority(int which, id_t who);       /* the nice value, or -1 with errno set (clear errno first: -1 is also a nice value) */
int setpriority(int which, id_t who, int prio);

#endif
