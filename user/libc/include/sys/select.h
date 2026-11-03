#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>

#define FD_SETSIZE 64
typedef struct { unsigned long bits; } fd_set;
#define FD_ZERO(s)      ((s)->bits = 0)
#define FD_SET(fd, s)   ((s)->bits |= 1UL << (fd))
#define FD_CLR(fd, s)   ((s)->bits &= ~(1UL << (fd)))
#define FD_ISSET(fd, s) (((s)->bits >> (fd)) & 1UL)

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout);

#endif
