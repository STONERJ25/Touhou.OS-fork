/*
 * Sending signals for the Linux personality (docs/COMPAT.md): kill, tkill, tgkill, rt_sigqueueinfo, rt_tgsigqueueinfo.
 *
 * Every signal carries a siginfo that says who sent it: kill(2) makes SI_USER with the sender's process id and real user id, tkill(2) and tgkill(2) (which is
 * what raise(3) and pthread_kill(3) are) SI_TKILL, and sigqueue(3) is rt_sigqueueinfo with SI_QUEUE and a value the program chose. The siginfo of
 * rt_sigqueueinfo is the caller's own, passed on word for word -- except that a caller may not pretend to be the kernel or kill(2): a code of zero or more,
 * or SI_TKILL, is only accepted when the signal goes to the caller itself, as in Linux. The C library relies on this: its pthread_cancel handler (signal 32)
 * and its set-user-id broadcast (signal 33) ignore any signal whose si_code is not SI_TKILL or whose si_pid is not the process's own.
 *
 * Written from the kill(2), tkill(2), rt_sigqueueinfo(2), sigqueue(3) and signal(7) manual pages and POSIX.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "numbers.h"
#include "calls.h"
#include "irq.h"

static inline Process *me(void) { return current->proc; }

static bool valid_sig(int sig) { return sig >= 0 && sig <= SIGRTMAX_K; }

/* kill(pid, sig): pid is a 32-bit pid_t, so only the low half of the register counts (kill(-1) arrives as 0xFFFFFFFF or as all ones, depending on the caller). */
long lnx_kill(TrapFrame *tf) {
    int sig = (int)A1(tf);
    if (!valid_sig(sig)) return -EINVAL;
    return signal_kill(me(), (long)(int)A0(tf), sig);
}

struct FindThread {
    int tgid, tid;                      /* tgid 0: any process (tkill) */
    Task *found;
    Process *owner;
};

static void find_cb(Process *p, void *ctx) {
    struct FindThread *a = ctx;
    if (a->found || p->state == PROC_ZOMBIE || (a->tgid && p->pid != a->tgid)) return;
    for (Task *t = p->threads; t; t = t->thread_next)
        if (t->utid == a->tid) { a->found = t; a->owner = p; return; }
}

/* The common part of tkill, tgkill and rt_tgsigqueueinfo: find the thread, check the signal number and the permission (in that order, like Linux), send. The
 * lookup and the send are one critical section, so the thread cannot go away between them. */
static long send_to_thread(int tgid, int tid, int sig, const SigInfo *info) {
    struct FindThread a = { tgid, tid, NULL, NULL };
    u64 f = irq_save();
    process_for_each(find_cb, &a);
    long rc = 0;
    if (!a.found) rc = -ESRCH;
    else if (!valid_sig(sig)) rc = -EINVAL;
    else if (sig == SIGCANCEL && a.owner->abi != ABI_LINUX) rc = -EINVAL;     /* signal 31 is SIGSYS to us but pthread_cancel to a native thread: it is not ours to send */
    else if (!signal_permitted(me(), a.owner)) rc = -EPERM;
    else if (sig) rc = signal_send_thread_info(a.found, sig, info);
    irq_restore(f);
    return rc < 0 ? rc : 0;                             /* 1: merged into a signal that was pending already -- not an error */
}

static void sender_info(SigInfo *info, int sig, int code) {
    siginfo_clear(info, sig, code);
    siginfo_sender(info, me()->pid, me()->ruid);
}

/* tgkill(tgid, tid, sig): one thread of one process; the process id guards against the thread id having been reused. */
long lnx_tgkill(TrapFrame *tf) {
    int tgid = (int)A0(tf), tid = (int)A1(tf), sig = (int)A2(tf);
    if (tid <= 0 || tgid <= 0) return -EINVAL;
    SigInfo info;
    sender_info(&info, sig, SI_TKILL);
    return send_to_thread(tgid, tid, sig, &info);
}

/* tkill(tid, sig): the older form, with the thread id alone. */
long lnx_tkill(TrapFrame *tf) {
    int tid = (int)A0(tf), sig = (int)A1(tf);
    if (tid <= 0) return -EINVAL;
    SigInfo info;
    sender_info(&info, sig, SI_TKILL);
    return send_to_thread(0, tid, sig, &info);
}

/* rt_sigqueueinfo(tgid, sig, info): the process-directed form of sigqueue(3). */
long lnx_rt_sigqueueinfo(TrapFrame *tf) {
    int pid = (int)A0(tf), sig = (int)A1(tf);
    SigInfo info;
    if (copy_from_user(&info, (void *)A2(tf), sizeof info)) return -EFAULT;
    if ((info.si_code >= 0 || info.si_code == SI_TKILL) && current->utid != pid) return -EPERM;       /* no pretending to be the kernel or kill(2) */
    info.si_signo = sig;
    if (!valid_sig(sig)) return -EINVAL;
    Process *t = pid > 0 ? process_by_pid(pid) : NULL;
    if (!t || t->state == PROC_ZOMBIE) return -ESRCH;
    if (!signal_permitted(me(), t)) return -EPERM;
    long rc = sig ? signal_send_info(t, sig, &info) : 0;
    return rc < 0 ? rc : 0;                             /* 1: merged into a signal that was pending already -- not an error */
}

/* rt_tgsigqueueinfo(tgid, tid, sig, info): the same for one thread. */
long lnx_rt_tgsigqueueinfo(TrapFrame *tf) {
    int tgid = (int)A0(tf), tid = (int)A1(tf), sig = (int)A2(tf);
    SigInfo info;
    if (copy_from_user(&info, (void *)A3(tf), sizeof info)) return -EFAULT;
    if (tgid <= 0 || tid <= 0) return -EINVAL;
    if ((info.si_code >= 0 || info.si_code == SI_TKILL) && current->utid != tid) return -EPERM;
    info.si_signo = sig;
    return send_to_thread(tgid, tid, sig, &info);
}

/* wait4(pid, status, options, rusage): the native wait, plus a zeroed struct rusage for a caller that asks for one (the resource usage of a child is not kept; a
 * program that prints it, like time(1), shows zeros instead of whatever its buffer held). */
long lnx_wait4(TrapFrame *tf) {
    SyscallFn f = syscall_native(SYS_waitpid);
    long r = f ? f(tf) : -ENOSYS;
    if (r > 0 && A3(tf)) {
        u8 zero[144];
        memset(zero, 0, sizeof zero);
        if (copy_to_user((void *)A3(tf), zero, sizeof zero)) return -EFAULT;
    }
    return r;
}

/* waitid(idtype, id, infop, options, rusage): wait for a child by pid, by process group or for any, and say in a siginfo what happened to it (the same SIGCHLD
 * fields as a signal would carry, except the CPU times: waitid does not store those). Linux's values: P_ALL 0, P_PID 1, P_PGID 2; WNOHANG 1, WSTOPPED 2,
 * WEXITED 4, WCONTINUED 8, WNOWAIT 0x1000000. At least one of WEXITED, WSTOPPED and WCONTINUED must be asked for. */
long lnx_waitid(TrapFrame *tf) {
    int idtype = (int)A0(tf), id = (int)A1(tf), options = (int)A3(tf);
    if (options & ~(WAITID_NOHANG | WAITID_STOPPED | WAITID_EXITED | WAITID_CONTINUED | WAITID_NOWAIT)) return -EINVAL;
    if (!(options & (WAITID_STOPPED | WAITID_EXITED | WAITID_CONTINUED))) return -EINVAL;
    switch (idtype) {
    case WAITID_ALL: break;
    case WAITID_PID: if (id <= 0) return -EINVAL; break;
    case WAITID_PGID: if (id < 0) return -EINVAL; break;
    default: return -EINVAL;
    }
    WaitInfo w;
    long rc = process_waitid(idtype, id, options, &w);
    if (rc) return rc;
    if (A2(tf)) {
        SigInfo si;
        memset(&si, 0, sizeof si);
        if (w.pid) {
            siginfo_clear(&si, SIGCHLD, w.code);
            siginfo_sender(&si, w.pid, w.uid);
            siginfo_put_s32(&si, 8, w.status);
        }
        if (copy_to_user((void *)A2(tf), &si, 32)) return -EFAULT;          /* the six fields; si_utime and si_stime are not part of the answer */
    }
    if (A4(tf)) {
        u8 zero[144];
        memset(zero, 0, sizeof zero);
        if (copy_to_user((void *)A4(tf), zero, sizeof zero)) return -EFAULT;     /* struct rusage: not kept */
    }
    return 0;
}
