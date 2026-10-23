#ifndef _POLL_H
#define _POLL_H
#include <syscall.h>
typedef unsigned long nfds_t;
#define pollfd gpollfd
int poll(struct gpollfd *fds, nfds_t n, int timeout_ms);
#endif
