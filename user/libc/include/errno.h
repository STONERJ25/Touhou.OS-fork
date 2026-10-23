#ifndef _ERRNO_H
#define _ERRNO_H

#include <abi.h>

/* errno is per thread: the word lives in the thread's control block, which the FS base points at (see pthread.h). */
static inline int *__errno_location(void) {
    char *self;
    __asm__ ("movq %%fs:0, %0" : "=r"(self));
    return (int *)(self + 8);
}
#define errno (*__errno_location())

char *strerror(int err);

#endif
