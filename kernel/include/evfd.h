/*
 * The event descriptors: epoll instances (kernel/fs/epoll.c), eventfd counters (kernel/fs/eventfd.c) and timerfd timers (kernel/fs/timerfd.c). They are ordinary
 * File objects with their own FileOps; the Linux system call layer (kernel/linux/epoll.c) creates them, installs the descriptor and converts the user structures.
 * All of them take part in poll, select and epoll through the readiness interface (include/pollq.h).
 */
#ifndef K_EVFD_H
#define K_EVFD_H

#include "vfs.h"
#include "pollq.h"

/* ---- epoll ---- */
#define EP_CTL_ADD 1
#define EP_CTL_DEL 2
#define EP_CTL_MOD 3
#define EP_ET      (1u << 31)           /* EPOLLET: report only changes */
#define EP_ONESHOT (1u << 30)           /* EPOLLONESHOT: report once, then wait for EPOLL_CTL_MOD */
#define EP_MAX_DEPTH 5                  /* how deep epoll sets may be nested */

struct EpEvent { u32 events; u64 data; };

long ep_create(File **out);                                                                  /* a new epoll instance (a File with read/write flags O_RDWR) */
bool ep_is(const File *f);
long ep_ctl(File *epf, int op, int fd, File *target, u32 events, u64 data);                  /* 0 or -errno: EEXIST, ENOENT, EINVAL, ELOOP, EPERM, ENOMEM */
long ep_wait(File *epf, struct EpEvent *out, int max, u64 deadline);                        /* events stored (>= 0; 0 at the deadline), or -EINTR */

/* ---- eventfd ---- */
#define EVFD_SEMAPHORE 1
long eventfd_create(u64 initval, int flags, File **out);                                      /* flags: EVFD_SEMAPHORE | O_NONBLOCK */

/* ---- timerfd ---- */
struct TfdSpec { s64 interval_ns; s64 value_ns; };                                           /* it_interval and it_value as nanoseconds */
long timerfd_make(int clock, int nonblock, File **out);                                      /* clock: 0 CLOCK_REALTIME, 1 CLOCK_MONOTONIC (7 CLOCK_BOOTTIME is the same) */
bool timerfd_is(const File *f);
long timerfd_set(File *f, bool absolute, const struct TfdSpec *nw, struct TfdSpec *old);     /* absolute: value_ns is a point on the timer's clock, else a duration */
long timerfd_get(File *f, struct TfdSpec *cur);                                              /* the time left (value_ns) and the interval */

#endif
