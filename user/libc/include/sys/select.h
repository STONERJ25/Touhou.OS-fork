#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>

/* 1024 descriptors, as on Linux: the default soft RLIMIT_NOFILE (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"). */
#define FD_SETSIZE 1024
typedef struct { unsigned long bits[FD_SETSIZE / 64]; } fd_set;
#define FD_ZERO(s)      do { for (int __i = 0; __i < FD_SETSIZE / 64; __i++) (s)->bits[__i] = 0; } while (0)
#define FD_SET(fd, s)   ((s)->bits[(unsigned)(fd) / 64] |= 1UL << ((unsigned)(fd) % 64))
#define FD_CLR(fd, s)   ((s)->bits[(unsigned)(fd) / 64] &= ~(1UL << ((unsigned)(fd) % 64)))
#define FD_ISSET(fd, s) (((s)->bits[(unsigned)(fd) / 64] >> ((unsigned)(fd) % 64)) & 1UL)

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);

#endif
