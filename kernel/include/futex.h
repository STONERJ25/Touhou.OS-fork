/* Futex wait queues keyed by physical address (kernel/sys/futex.c). The system call is built on these. */
#ifndef K_FUTEX_H
#define K_FUTEX_H

#include "types.h"

long futex_wait_key(u64 key, u32 val, u64 deadline);       /* sleep while the u32 at physical address key equals val; 0 woken, -EAGAIN, -ETIMEDOUT, -EINTR */
int  futex_wake_key(u64 key, int n);                       /* wake up to n sleepers (oldest first); returns how many */
int  futex_requeue_key(u64 key1, int nwake, u64 key2, int nrequeue);
int  futex_waiters_on(u64 key);
int  futex_selftest(void);                                 /* kernel self test; returns the number of failures */

#endif
