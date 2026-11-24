#ifndef K_KTIME_ELAPSED_H
#define K_KTIME_ELAPSED_H

/* Elapsed time between two readings of the uptime clock, without the trap of the unsigned subtraction.
 *
 * uptime_ms() (and net_now(), which is the same number) never goes backwards: it is the tick count. But the two numbers of a `now - stamp` are often read at different
 * moments by different code: `now` at the top of a loop, the stamp by whatever ran since then (the receive path inside the same poll, another thread that was scheduled in
 * between, an interrupt handler). Then the stamp is the newer one, `now - stamp` is 2^64-1 instead of "minus one", and every "older than N ms" test fires at once. That is
 * how the gateway's ARP entry was forgotten the moment it was learned (docs/NETWORK.md, "The first-connection stall"); docs/RELIABILITY.md, "Elapsed time and unsigned
 * subtraction", lists every such subtraction in the kernel and says which could go wrong.
 *
 * A stamp from the future means "just now": the elapsed time is 0, and what is left of a deadline that has passed is 0. Pure functions on 64-bit numbers (no kernel
 * header needed: tests/host/test_ktime_elapsed.py compiles this file on the host). These are for the 64-bit millisecond clock, which does not wrap in any machine's life;
 * the counters that do wrap (the ACPI PM timer in drivers/timer.c, TCP sequence numbers) keep their own masked or signed arithmetic. */

#include <stdint.h>

/* Milliseconds from `stamp` to `now`; 0 when the stamp is the newer of the two. */
static inline uint64_t ktime_elapsed(uint64_t now, uint64_t stamp) { return now > stamp ? now - stamp : 0; }

/* Milliseconds from `now` to `deadline`; 0 when the deadline has passed. */
static inline uint64_t ktime_left(uint64_t deadline, uint64_t now) { return deadline > now ? deadline - now : 0; }

/* Has more than `ms` passed since `stamp`? Never true for a stamp newer than `now`. */
static inline int ktime_older_than(uint64_t now, uint64_t stamp, uint64_t ms) { return ktime_elapsed(now, stamp) > ms; }

/* Has at least `ms` passed since `stamp`? (`ms` = 0: always.) */
static inline int ktime_at_least(uint64_t now, uint64_t stamp, uint64_t ms) { return ktime_elapsed(now, stamp) >= ms; }

#endif
