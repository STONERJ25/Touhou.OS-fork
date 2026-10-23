#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <sys/types.h>
#include <abi.h>
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
#endif
