#ifndef K_RANDOM_H
#define K_RANDOM_H

#include "types.h"

void random_init(void);
u64  random_u64(void);
void random_mix(u64 v);                 /* stir in entropy (interrupt timing, input events) */
void random_fill(void *buf, size_t n);
bool random_ready(void);                /* at least 128 bits of estimated entropy went into the generator (see /proc/random) */

#endif
