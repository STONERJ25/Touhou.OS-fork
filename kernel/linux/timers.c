/*
 * Timers and clocks for the Linux personality (docs/COMPAT.md): setitimer and getitimer (ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF), POSIX timers (timer_create,
 * timer_settime, timer_gettime, timer_getoverrun, timer_delete), clock_gettime, clock_getres, nanosleep, clock_nanosleep, times and getrusage.
 *
 * Time here is the system tick, one millisecond (TIMER_HZ): every expiry is rounded up to a whole tick, a timer never fires early, and the clocks count whole
 * milliseconds. The tick drives everything. ITIMER_REAL is the process's alarm (alarm_at, the same field that alarm(2) uses, checked by alarm_tick in
 * kernel/sys/process.c) plus an interval. ITIMER_VIRTUAL and ITIMER_PROF count down while the scheduler charges ticks to the process (user ticks only, and user
 * plus system ticks): a tick is charged to whatever was running when it arrived, so these are samples, as the CPU times of a classic Unix were. POSIX timers
 * sit in a list that the tick looks at while any of them is armed (g_ktimers_armed); one that fires sends its signal with a siginfo of SI_TIMER and the timer's
 * id, value and overrun count, and while that signal is still waiting further expirations only add to the overrun count (timer_getoverrun and si_overrun), as
 * POSIX 2.4.4 asks. CLOCK_REALTIME timers with an absolute time are converted to the tick clock when they are armed and do not follow later steps of the clock.
 *
 * What differs from Linux, on purpose: the resolution (1 ms; Linux shows 1 ns for most clocks), timers on the CPU-time clocks (EINVAL), the dynamic clock
 * numbers of pthread_getcpuclockid (EINVAL), clock_nanosleep on a CPU-time clock (EINVAL), the alarm clocks (EPERM, as for a program without the capability).
 *
 * Written from the setitimer(2), timer_create(2), timer_settime(2), clock_gettime(2), clock_nanosleep(2), sigevent(7) and time(7) manual pages and POSIX; the
 * constants and structure layouts are checked against the C library's by tests/host/test_linux_signals_abi.py.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "ktime.h"
#include "irq.h"
#include "numbers.h"
#include "calls.h"

static inline Process *me(void) { return current->proc; }

/* ------------------------------------------------------------------ structures and time arithmetic */

struct lnx_timespec { s64 sec, nsec; };
struct lnx_timeval { s64 sec, usec; };
struct lnx_itimerspec { struct lnx_timespec interval, value; };
struct lnx_itimerval { struct lnx_timeval interval, value; };
struct lnx_sigevent {                                   /* sigevent(7): 64 bytes */
    u64 value;                                          /* sigev_value: an int or a pointer */
    s32 signo, notify;
    s32 tid;                                            /* sigev_notify_thread_id, the start of the union */
    u8  pad[44];
};
_Static_assert(sizeof(struct lnx_sigevent) == 64, "struct sigevent is 64 bytes");
_Static_assert(sizeof(struct lnx_itimerspec) == 32 && sizeof(struct lnx_itimerval) == 32, "itimerspec and itimerval are 32 bytes");

#define MS_MAX (~0ULL >> 4)                             /* far enough to be "never", and far from overflowing when a time is added */

static bool timespec_ok(const struct lnx_timespec *t) { return t->sec >= 0 && t->nsec >= 0 && t->nsec < 1000000000; }
static bool timeval_ok(const struct lnx_timeval *t) { return t->sec >= 0 && t->usec >= 0 && t->usec < 1000000; }

static u64 ms_of(s64 sec, s64 sub, u64 per_ms) {        /* a time rounded up to whole milliseconds: sub is nanoseconds (per_ms 1000000) or microseconds (1000) */
    if ((u64)sec > MS_MAX / 1000) return MS_MAX;
    return (u64)sec * 1000 + ((u64)sub + per_ms - 1) / per_ms;
}
static u64 ms_of_timespec(const struct lnx_timespec *t) { return ms_of(t->sec, t->nsec, 1000000); }
static u64 ms_of_timeval(const struct lnx_timeval *t) { return ms_of(t->sec, t->usec, 1000); }

static void timespec_of_ms(u64 ms, struct lnx_timespec *t) { t->sec = (s64)(ms / 1000); t->nsec = (s64)(ms % 1000) * 1000000; }
static void timeval_of_ms(u64 ms, struct lnx_timeval *t) { t->sec = (s64)(ms / 1000); t->usec = (s64)(ms % 1000) * 1000; }

/* The clocks of clock_gettime(2) */
#define CLK_REALTIME            0
#define CLK_MONOTONIC           1
#define CLK_PROCESS_CPUTIME_ID     2
#define CLK_THREAD_CPUTIME_ID      3
#define CLK_MONOTONIC_RAW       4
#define CLK_REALTIME_COARSE     5
#define CLK_MONOTONIC_COARSE    6
#define CLK_BOOTTIME            7
#define CLK_REALTIME_ALARM      8
#define CLK_BOOTTIME_ALARM      9
#define CLK_TAI                 11

/* The time of a clock in milliseconds; false when the number names no clock here. The wall clock and the tick clock are the two bases; the CPU clocks count the
 * ticks the scheduler charged. */
static bool clock_ms(int clock, u64 *ms) {
    u64 total;
    switch (clock) {
    case CLK_REALTIME: case CLK_REALTIME_COARSE: case CLK_REALTIME_ALARM: case CLK_TAI:
        *ms = time_now_ms();
        return true;
    case CLK_MONOTONIC: case CLK_MONOTONIC_RAW: case CLK_MONOTONIC_COARSE: case CLK_BOOTTIME: case CLK_BOOTTIME_ALARM:
        *ms = uptime_ms();
        return true;
    case CLK_PROCESS_CPUTIME_ID:
        process_cpu_ticks(me(), NULL, &total);
        *ms = total * 1000 / TIMER_HZ;
        return true;
    case CLK_THREAD_CPUTIME_ID:
        *ms = current->cpu_ticks * 1000 / TIMER_HZ;
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ times, getrusage */

struct lnx_tms { s64 utime, stime, cutime, cstime; };               /* struct tms: clock_t is a long, in 1/100 s */
struct lnx_rusage { struct lnx_timeval utime, stime; s64 rest[14]; };      /* struct rusage (144 bytes): only the two times are kept */
_Static_assert(sizeof(struct lnx_rusage) == 144, "struct rusage is 144 bytes");

static s64 hundredths(u64 ticks) { return (s64)(ticks * 100 / TIMER_HZ); }

/* times(buf): the CPU time of the process, user and system, and of the children it has waited for; the result is the time since boot in 1/100 s. */
long lnx_times(TrapFrame *tf) {
    Process *p = me();
    u64 user, total;
    process_cpu_ticks(p, &user, &total);
    if (A0(tf)) {
        struct lnx_tms t = { hundredths(user), hundredths(total - user), hundredths(p->child_utime), hundredths(p->child_ticks - p->child_utime) };
        if (copy_to_user((void *)A0(tf), &t, sizeof t)) return -EFAULT;
    }
    return (long)(uptime_ms() / 10);
}

#define LNX_RUSAGE_SELF      0
#define LNX_RUSAGE_CHILDREN  (-1)
#define LNX_RUSAGE_THREAD    1

/* getrusage(who, buf): user and system time of the process, of the children it has waited for, or of the calling thread. The other counters (memory, faults,
 * context switches) are not kept and read as zero. */
long lnx_getrusage(TrapFrame *tf) {
    u64 user, total;
    switch ((int)A0(tf)) {
    case LNX_RUSAGE_SELF:     process_cpu_ticks(me(), &user, &total); break;
    case LNX_RUSAGE_CHILDREN: user = me()->child_utime; total = me()->child_ticks; break;
    case LNX_RUSAGE_THREAD:   user = current->utime_ticks; total = current->cpu_ticks; break;
    default: return -EINVAL;
    }
    struct lnx_rusage ru;
    memset(&ru, 0, sizeof ru);
    timeval_of_ms(user * 1000 / TIMER_HZ, &ru.utime);
    timeval_of_ms((total - user) * 1000 / TIMER_HZ, &ru.stime);
    if (copy_to_user((void *)A1(tf), &ru, sizeof ru)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ sleeping */

/* nanosleep(req, rem) and clock_nanosleep(clock, flags, req, rem): sleep until a deadline in milliseconds of uptime. A signal that needs attention ends the sleep
 * with EINTR and, for a relative sleep, the time that was left is stored in rem. */
static long sleep_until(u64 end, u64 urem) {
    u64 f = irq_save();
    while (uptime_ms() < end) {
        if (signal_pending_now(me())) {
            irq_restore(f);
            if (urem) {
                u64 now = uptime_ms(), left = end > now ? end - now : 0;
                struct lnx_timespec r;
                timespec_of_ms(left, &r);
                if (copy_to_user((void *)urem, &r, sizeof r)) return -EFAULT;
            }
            return -EINTR;
        }
        current->intr = true;
        current->wake_ms = end;
        current->state = TASK_SLEEPING;
        schedule();
        current->intr = false;
    }
    irq_restore(f);
    return 0;
}

static long timespec_ms(u64 ureq, u64 *ms) {
    struct lnx_timespec ts;
    if (copy_from_user(&ts, (void *)ureq, sizeof ts)) return -EFAULT;
    if (!timespec_ok(&ts)) return -EINVAL;
    *ms = ms_of_timespec(&ts);
    return 0;
}

long lnx_nanosleep(TrapFrame *tf) {
    u64 ms;
    long rc = timespec_ms(A0(tf), &ms);
    return rc ? rc : sleep_until(uptime_ms() + ms, A1(tf));
}

#define LNX_TIMER_ABSTIME 1

/* clock_nanosleep: the clocks that run by themselves (real time, monotonic, boot time, TAI). What Linux refuses, it refuses with these numbers: the CPU-time
 * clocks of a thread and the raw and coarse clocks are not supported (95), the alarm clocks need a capability (EPERM), the rest are not clocks. */
long lnx_clock_nanosleep(TrapFrame *tf) {
    int clock = (int)A0(tf), flags = (int)A1(tf);
    switch (clock) {
    case CLK_REALTIME: case CLK_TAI: case CLK_MONOTONIC: case CLK_BOOTTIME: break;
    case CLK_THREAD_CPUTIME_ID: case CLK_MONOTONIC_RAW: case CLK_REALTIME_COARSE: case CLK_MONOTONIC_COARSE: return -ENOTSUP;
    case CLK_REALTIME_ALARM: case CLK_BOOTTIME_ALARM: return -EPERM;
    default: return -EINVAL;                            /* includes the process CPU clock: a sleep on it would last until some other thread used CPU time */
    }
    if (flags & ~LNX_TIMER_ABSTIME) return -EINVAL;
    u64 ms;
    long rc = timespec_ms(A2(tf), &ms);
    if (rc) return rc;
    if (!(flags & LNX_TIMER_ABSTIME)) return sleep_until(uptime_ms() + ms, A3(tf));
    u64 now;
    clock_ms(clock, &now);
    return ms <= now ? 0 : sleep_until(uptime_ms() + (ms - now), 0);
}

/* ------------------------------------------------------------------ clock_gettime, clock_getres */

long lnx_clock_gettime(TrapFrame *tf) {
    u64 ms;
    if (!clock_ms((int)A0(tf), &ms)) return -EINVAL;
    struct lnx_timespec ts;
    timespec_of_ms(ms, &ts);
    return copy_to_user((void *)A1(tf), &ts, sizeof ts);
}

/* clock_getres(clock, res): one tick, 1 ms, for every clock this system has; a null pointer only asks whether the clock exists. */
long lnx_clock_getres(TrapFrame *tf) {
    u64 ms;
    if (!clock_ms((int)A0(tf), &ms)) return -EINVAL;
    if (!A1(tf)) return 0;
    struct lnx_timespec ts;
    timespec_of_ms(1000 / TIMER_HZ, &ts);
    return copy_to_user((void *)A1(tf), &ts, sizeof ts);
}

/* ------------------------------------------------------------------ setitimer, getitimer */

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

/* The state of one interval timer as a value/interval pair in milliseconds. */
static void itimer_get(Process *p, int which, u64 *value, u64 *interval) {
    u64 now = uptime_ms();
    switch (which) {
    case ITIMER_REAL:
        *value = p->alarm_at ? (p->alarm_at > now ? p->alarm_at - now : 1) : 0;
        *interval = p->alarm_interval;
        break;
    case ITIMER_VIRTUAL:
        *value = p->itimer_virt * 1000 / TIMER_HZ;
        *interval = p->itimer_virt_iv * 1000 / TIMER_HZ;
        break;
    default:
        *value = p->itimer_prof * 1000 / TIMER_HZ;
        *interval = p->itimer_prof_iv * 1000 / TIMER_HZ;
        break;
    }
}

static void itimer_set(Process *p, int which, u64 value, u64 interval) {
    switch (which) {
    case ITIMER_REAL: {
        bool had = p->alarm_at != 0;
        p->alarm_at = value ? uptime_ms() + value : 0;
        p->alarm_interval = value ? interval : 0;               /* a disarmed timer keeps no interval */
        if (!had && value) g_alarms_armed++;
        else if (had && !value) g_alarms_armed--;
        break;
    }
    case ITIMER_VIRTUAL:
        p->itimer_virt = value * TIMER_HZ / 1000;
        p->itimer_virt_iv = value ? interval * TIMER_HZ / 1000 : 0;
        break;
    default:
        p->itimer_prof = value * TIMER_HZ / 1000;
        p->itimer_prof_iv = value ? interval * TIMER_HZ / 1000 : 0;
        break;
    }
}

long lnx_getitimer(TrapFrame *tf) {
    int which = (int)A0(tf);
    if (which < ITIMER_REAL || which > ITIMER_PROF) return -EINVAL;
    u64 v, i;
    u64 f = irq_save();
    itimer_get(me(), which, &v, &i);
    irq_restore(f);
    struct lnx_itimerval out;
    timeval_of_ms(v, &out.value);
    timeval_of_ms(i, &out.interval);
    return copy_to_user((void *)A1(tf), &out, sizeof out);
}

/* setitimer(which, new, old): the previous setting goes to old; a null new is the same as all zeros (the timer is stopped), as in Linux. */
long lnx_setitimer(TrapFrame *tf) {
    int which = (int)A0(tf);
    if (which < ITIMER_REAL || which > ITIMER_PROF) return -EINVAL;
    struct lnx_itimerval nv;
    memset(&nv, 0, sizeof nv);
    if (A1(tf) && copy_from_user(&nv, (void *)A1(tf), sizeof nv)) return -EFAULT;
    if (!timeval_ok(&nv.value) || !timeval_ok(&nv.interval)) return -EINVAL;
    u64 ov, oi;
    u64 f = irq_save();
    itimer_get(me(), which, &ov, &oi);
    itimer_set(me(), which, ms_of_timeval(&nv.value), ms_of_timeval(&nv.interval));
    irq_restore(f);
    if (A2(tf)) {
        struct lnx_itimerval old;
        timeval_of_ms(ov, &old.value);
        timeval_of_ms(oi, &old.interval);
        if (copy_to_user((void *)A2(tf), &old, sizeof old)) return -EFAULT;
    }
    return 0;
}

/* The scheduler charged a tick to the process: the CPU-time interval timers count down. Called from the timer interrupt (kernel/sched/task.c) while either
 * is armed. The signal is the kernel's own (SI_KERNEL); the timer reloads from its interval, or stops. */
void ktimers_cpu_tick(Process *p, bool user) {
    if (user && p->itimer_virt && --p->itimer_virt == 0) {
        p->itimer_virt = p->itimer_virt_iv;
        signal_send(p, SIGVTALRM);
    }
    if (p->itimer_prof && --p->itimer_prof == 0) {
        p->itimer_prof = p->itimer_prof_iv;
        signal_send(p, SIGPROF);
    }
}

/* ------------------------------------------------------------------ POSIX timers */

#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD    2                               /* the C library turns this into SIGEV_THREAD_ID aimed at a helper thread; the kernel treats it like SIGEV_SIGNAL */
#define SIGEV_THREAD_ID 4
#define TIMERS_PER_PROC 64

typedef struct KTimer {
    struct KTimer *next;                                /* the process's timers */
    struct KTimer *armed_next;                          /* the global list of the armed ones */
    Process *owner;
    int id;
    int clock;
    int notify, signo, tid;                             /* tid: SIGEV_THREAD_ID */
    u64 value;                                          /* sigev_value, passed back in si_value */
    bool armed, on_list;
    u64 expiry, interval;                               /* uptime ms; ms */
    int overrun, overrun_last;                          /* expirations that found the signal still waiting; the count the last delivered signal reported */
    bool queued;                                        /* the signal of the last expiry has not been taken yet */
} KTimer;

int g_ktimers_armed;
static KTimer *g_armed;

static KTimer *timer_find(Process *p, int id) {         /* irq off */
    for (KTimer *k = p->ktimers; k; k = k->next) if (k->id == id) return k;
    return NULL;
}

static void armed_unlink(KTimer *k) {                   /* irq off */
    if (!k->on_list) return;
    for (KTimer **pp = &g_armed; *pp; pp = &(*pp)->armed_next)
        if (*pp == k) { *pp = k->armed_next; break; }
    k->on_list = false;
    g_ktimers_armed--;
}

static void armed_link(KTimer *k) {                     /* irq off */
    if (k->on_list) return;
    k->armed_next = g_armed;
    g_armed = k;
    k->on_list = true;
    g_ktimers_armed++;
}

/* The expiry of one timer, in the timer interrupt: re-arm or stop it, count a missed signal as an overrun, otherwise send the signal. */
static void timer_fire(KTimer *k, u64 now) {
    Process *p = k->owner;
    if (k->interval) {
        u64 missed = (now - k->expiry) / k->interval;
        k->expiry += (missed + 1) * k->interval;
        if (missed) k->overrun += (int)(missed > 1000000 ? 1000000 : missed);
    } else {
        k->armed = false;
        armed_unlink(k);
    }
    if (k->notify == SIGEV_NONE || p->state == PROC_ZOMBIE) return;
    if (k->queued) {
        if (k->overrun < 1000000) k->overrun++;
        return;
    }
    SigInfo si;
    siginfo_clear(&si, k->signo, SI_TIMER);
    siginfo_timer(&si, k->id, 0, k->value);
    int rc;
    if (k->notify == SIGEV_THREAD_ID) {
        Task *t = NULL;
        for (Task *x = p->threads; x; x = x->thread_next) if (x->utid == k->tid) { t = x; break; }
        if (!t) return;                                                  /* the thread is gone: the signal has nowhere to go */
        rc = signal_send_thread_info(t, k->signo, &si);
    } else {
        rc = signal_send_info(p, k->signo, &si);
    }
    k->queued = rc == 0;                                                 /* 0: an entry waits in a queue; otherwise the signal was thrown away or merged into one already pending */
}

/* Called from the scheduler tick while any timer is armed. */
void ktimers_tick(u64 now) {
    for (KTimer *k = g_armed, *next; k; k = next) {
        next = k->armed_next;                                           /* timer_fire may unlink k */
        if (now >= k->expiry) timer_fire(k, now);
    }
}

/* signal.c took the signal of a timer: the siginfo gets the overrun count of that moment, and the timer may queue its next one. */
void ktimer_signal_taken(Process *p, SigInfo *info) {
    KTimer *k = timer_find(p, siginfo_get_s32(info, 0));
    if (!k) return;
    siginfo_put_s32(info, 4, k->overrun);
    k->overrun_last = k->overrun;
    k->overrun = 0;
    k->queued = false;
}

/* signal.c threw away the signal of a timer (the action became "ignore", a standard signal merged into one already pending, the queue was purged): the timer may
 * queue its next one. */
void ktimer_signal_dropped(Process *p, const SigInfo *info) {
    KTimer *k = timer_find(p, siginfo_get_s32(info, 0));
    if (k) { k->queued = false; k->overrun = 0; }
}

/* The process is going away or has executed another program: its POSIX timers are dropped. */
void ktimers_process_exit(Process *p) {
    u64 f = irq_save();
    KTimer *list = p->ktimers;
    p->ktimers = NULL;
    for (KTimer *k = list; k; k = k->next) armed_unlink(k);
    irq_restore(f);
    while (list) {
        KTimer *k = list;
        list = k->next;
        kfree(k);
    }
}

static bool timer_clock_ok(int clock) {
    return clock == CLK_REALTIME || clock == CLK_MONOTONIC || clock == CLK_BOOTTIME || clock == CLK_TAI;
}

/* timer_create(clock, sevp, timerid): a null sevp is SIGEV_SIGNAL with SIGALRM and the timer's own id as the value. Linux's errors: ENOTSUP for the raw and coarse
 * clocks, EPERM for the alarm clocks, EINVAL for a signal number outside 1..64, an unknown notification, or a thread that is not in this process. */
long lnx_timer_create(TrapFrame *tf) {
    int clock = (int)A0(tf);
    switch (clock) {
    case CLK_MONOTONIC_RAW: case CLK_REALTIME_COARSE: case CLK_MONOTONIC_COARSE: return -ENOTSUP;
    case CLK_REALTIME_ALARM: case CLK_BOOTTIME_ALARM: return -EPERM;
    default:
        if (!timer_clock_ok(clock)) return -EINVAL;
        break;
    }
    Process *p = me();
    struct lnx_sigevent sev;
    memset(&sev, 0, sizeof sev);
    bool defaults = !A1(tf);
    if (!defaults && copy_from_user(&sev, (void *)A1(tf), sizeof sev)) return -EFAULT;
    if (!defaults) {
        if (sev.notify != SIGEV_SIGNAL && sev.notify != SIGEV_NONE && sev.notify != SIGEV_THREAD && sev.notify != SIGEV_THREAD_ID) return -EINVAL;
        if (sev.notify != SIGEV_NONE && (sev.signo <= 0 || sev.signo > SIGRTMAX_K)) return -EINVAL;
        if (sev.notify == SIGEV_THREAD_ID) {
            bool found = false;
            u64 f = irq_save();
            for (Task *t = p->threads; t; t = t->thread_next) if (t->utid == sev.tid) { found = true; break; }
            irq_restore(f);
            if (!found) return -EINVAL;
        }
    }
    int count = 0;
    for (KTimer *k = p->ktimers; k; k = k->next) count++;
    if (count >= TIMERS_PER_PROC) return -EAGAIN;
    KTimer *k = kzalloc(sizeof *k);
    if (!k) return -ENOMEM;
    k->owner = p;
    k->clock = clock;
    k->notify = defaults ? SIGEV_SIGNAL : sev.notify;
    k->signo = defaults ? SIGALRM : sev.signo;
    k->tid = sev.tid;
    u64 f = irq_save();
    k->id = p->ktimer_next_id++;
    k->value = defaults ? (u64)(u32)k->id : sev.value;
    k->next = p->ktimers;
    p->ktimers = k;
    irq_restore(f);
    int id = k->id;
    if (copy_to_user((void *)A2(tf), &id, sizeof id)) {
        u64 g = irq_save();
        for (KTimer **pp = &p->ktimers; *pp; pp = &(*pp)->next) if (*pp == k) { *pp = k->next; break; }
        irq_restore(g);
        kfree(k);
        return -EFAULT;
    }
    return 0;
}

/* The state of a timer as a value/interval pair, in ms. irq off. */
static void timer_state(const KTimer *k, u64 *value, u64 *interval) {
    u64 now = uptime_ms();
    *value = k->armed ? (k->expiry > now ? k->expiry - now : 1) : 0;
    *interval = k->armed ? k->interval : 0;
}

/* timer_settime(timerid, flags, new, old): TIMER_ABSTIME makes new.value a time on the timer's clock instead of a distance from now. Other flag bits are ignored,
 * as in Linux. A value of zero stops the timer. */
long lnx_timer_settime(TrapFrame *tf) {
    struct lnx_itimerspec ns;
    if (!A2(tf)) return -EINVAL;
    if (copy_from_user(&ns, (void *)A2(tf), sizeof ns)) return -EFAULT;
    if (!timespec_ok(&ns.value) || !timespec_ok(&ns.interval)) return -EINVAL;
    Process *p = me();
    u64 value = ms_of_timespec(&ns.value), interval = ms_of_timespec(&ns.interval);
    bool absolute = (A1(tf) & LNX_TIMER_ABSTIME) != 0;
    u64 f = irq_save();
    KTimer *k = timer_find(p, (int)A0(tf));
    if (!k) { irq_restore(f); return -EINVAL; }
    u64 ov, oi;
    timer_state(k, &ov, &oi);
    u64 first = value;                                                  /* ms from now until the first expiry */
    if (absolute && value) {
        u64 now;
        clock_ms(k->clock, &now);
        first = value > now ? value - now : 1;                          /* a time in the past fires at the next tick */
    }
    k->overrun = 0;
    if (value) {
        k->expiry = uptime_ms() + first;
        k->interval = interval;
        k->armed = true;
        armed_link(k);
    } else {
        k->armed = false;
        k->interval = 0;
        armed_unlink(k);
    }
    irq_restore(f);
    if (A3(tf)) {
        struct lnx_itimerspec old;
        timespec_of_ms(ov, &old.value);
        timespec_of_ms(oi, &old.interval);
        if (copy_to_user((void *)A3(tf), &old, sizeof old)) return -EFAULT;
    }
    return 0;
}

long lnx_timer_gettime(TrapFrame *tf) {
    u64 v, i;
    u64 f = irq_save();
    KTimer *k = timer_find(me(), (int)A0(tf));
    if (!k) { irq_restore(f); return -EINVAL; }
    timer_state(k, &v, &i);
    irq_restore(f);
    struct lnx_itimerspec out;
    timespec_of_ms(v, &out.value);
    timespec_of_ms(i, &out.interval);
    return copy_to_user((void *)A1(tf), &out, sizeof out);
}

/* timer_getoverrun: how many expirations the signal that was last taken stood for, minus one (the count its si_overrun showed). */
long lnx_timer_getoverrun(TrapFrame *tf) {
    u64 f = irq_save();
    KTimer *k = timer_find(me(), (int)A0(tf));
    long rc = k ? k->overrun_last : -EINVAL;
    irq_restore(f);
    return rc;
}

long lnx_timer_delete(TrapFrame *tf) {
    Process *p = me();
    int id = (int)A0(tf);
    u64 f = irq_save();
    KTimer *k = timer_find(p, id);
    if (k) {
        for (KTimer **pp = &p->ktimers; *pp; pp = &(*pp)->next) if (*pp == k) { *pp = k->next; break; }
        armed_unlink(k);
        signal_drop_timer(p, id);                                       /* its signal, if it is waiting, never arrives */
    }
    irq_restore(f);
    if (!k) return -EINVAL;
    kfree(k);
    return 0;
}
