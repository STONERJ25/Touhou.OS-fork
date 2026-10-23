#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

#include <time.h>

/* POSIX semaphores, unnamed only. value is a futex word; waiters counts sleepers so sem_post knows whether to wake. */
typedef struct { volatile int __value; volatile int __waiters; } sem_t;
#define SEM_VALUE_MAX 0x7FFFFFFF

int sem_init(sem_t *s, int pshared, unsigned value);
int sem_destroy(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abstime);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *value);

#endif
