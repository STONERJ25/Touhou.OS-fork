#ifndef _FCNTL_H
#define _FCNTL_H
#include <sys/types.h>
#include <abi.h>
int open(const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);
#endif
