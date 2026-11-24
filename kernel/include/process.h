/* Processes: address space, descriptors, credentials, signals, fork/exec/exit/wait. */
#ifndef K_PROCESS_H
#define K_PROCESS_H

#include "types.h"
#include "task.h"
#include "vfs.h"
#include "mm.h"
#include "abi.h"
#include "siginfo.h"
#include "syscall.h"

/* The descriptor table (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"): FD_INLINE slots live in the Process itself, a process that needs more gets a table
 * from the heap that doubles until it covers the descriptor asked for, never beyond FD_HARD_MAX (the highest hard RLIMIT_NOFILE, root included). Index the table only
 * below fd_slots(p); every loop over it runs to fd_slots(p). */
#define FD_INLINE       64
#define FD_SOFT_DEFAULT 1024        /* RLIMIT_NOFILE soft value of a new process (Linux's usual default) */
#define FD_HARD_MAX     4096        /* its hard value, and the most slots a table can have */
#define MAX_PROCS       256
#define USER_TOP        0x0000800000000000UL
#define USER_STACK_TOP  0x00007FFFFFFFF000UL
#define USER_STACK_MAX  (8UL * 1024 * 1024)
#define USER_STACK_INIT (64UL * 1024)
#define USER_MMAP_BASE  0x0000100000000000UL

#define VMA_R        PROT_READ
#define VMA_W        PROT_WRITE
#define VMA_X        PROT_EXEC
#define VMA_STACK    0x100
#define VMA_HEAP     0x200
#define VMA_IO       0x400          /* pages are not owned by this mapping and must not be freed */
#define VMA_WC       0x800          /* map write-combining (framebuffer) */
#define VMA_FILE     0x1000         /* filled from a file when it was mapped (a private copy): madvise(MADV_DONTNEED) must not zero it */

/* The file a mapping was made from, for /proc/<pid>/maps (docs/COMPAT.md, "A Linux-shaped /proc"). One object per mmap or loaded segment, shared by the pieces it is split into and
 * by the copies fork makes (refs), freed with the last of them. path is the path the file was opened by. */
typedef struct VmaFile {
    int  refs;
    u64  dev, ino;
    char path[];
} VmaFile;

typedef struct Vma {
    u64 start, end;
    u32 prot;                       /* PROT_* */
    u32 flags;                      /* VMA_* */
    struct Vma *next;
    VmaFile *file;                  /* NULL: anonymous memory; else the file this mapping shows and the offset in it of `start` */
    u64 file_off;
} Vma;

typedef struct AddrSpace {
    paddr_t pml4;
    Vma *vmas;
    u64 brk_start, brk;
    u64 mmap_next;
    Mutex lock;                     /* guards vmas and the page tables below pml4 against the process's other threads */
} AddrSpace;

typedef enum { PROC_RUNNING, PROC_ZOMBIE } ProcState;

/* Resource limits of one process (kernel/sys/rlimit.c, abi.h): soft and hard value of each RLIMIT_*. fork copies them, execve keeps them (they belong to the Process). */
typedef struct Rlimits {
    u64 cur[RLIM_NLIMITS];
    u64 max[RLIM_NLIMITS];
} Rlimits;

typedef struct Process {
    int pid, ppid, pgid, sid;
    struct Tty *ctty;               /* controlling terminal (what /dev/tty means); NULL = the console */
    char name[32];
    char cwd_path[256];
    VNode *cwd;
    Cred cred;                      /* effective user and group (what permission checks use) */
    u32 ruid, rgid;                 /* real user and group: who started the program (differs after a setuid exec) */
    u32 umask;
    ProcState state;
    int exit_status;                /* wait()-style status word */

    AddrSpace as;

    File **fds;                     /* fd_slots(p) entries: fd_small, or a heap table once more were needed (kernel/sys/process.c, fd_reserve) */
    u8 *fd_flags;
    int fd_cap;
    void **fd_retired;              /* heap tables fd_reserve replaced, kept until process_free (a preempted thread may still hold one) */
    File *fd_small[FD_INLINE];
    u8 fd_small_flags[FD_INLINE];

    Task *task;                     /* a live thread of the process (the oldest one); NULL once the process is a zombie */
    Task *threads;                  /* every live thread, linked through Task.thread_next (guarded by irq_save) */
    int nthreads;                   /* live threads */
    bool exiting;                   /* exit_group / fatal signal in progress: every thread must leave */
    Task *execing;                  /* the thread running execve: all others must leave, then it carries on alone */
    WaitQueue thread_wq;            /* execve sleeps here until the other threads are gone */
    struct Process *parent;
    struct Process *children;       /* singly linked through `sibling` */
    struct Process *sibling;
    struct Process *all_next;
    WaitQueue wait_wq;              /* the parent blocks here in waitpid */
    VNode *old_cwd[8];              /* directories left by chdir, released at exit: another thread may still be resolving a path against one */

    u64 sig_pending;                /* process-directed signals not yet taken by a thread (a set, see siginfo.h; per-thread masks live in Task) */
    struct gsigaction sigact[KNSIG];                    /* by signal number; native programs use 1..31, the Linux personality 1..64 */

    /* job control (docs/JOBCONTROL.md): a stopped process keeps all its threads parked at the edge of user mode */
    bool stopped;                   /* SIGSTOP / SIGTSTP / SIGTTIN / SIGTTOU took effect and no SIGCONT has followed */
    int  stop_sig;                  /* which signal stopped it (what WSTOPSIG reports) */
    bool stop_unreported;           /* a waitpid(WUNTRACED) in the parent has not seen this stop yet */
    bool continued_unreported;      /* a waitpid(WCONTINUED) in the parent has not seen the continue yet */
    WaitQueue stop_wq;              /* parked threads sleep here until SIGCONT (or until they must die) */
    int  nice;                      /* -20 (greedy) .. 19 (polite): scales the time slice of every thread */

    u64 start_ms;
    char cmdline[128];
    char exe[128];                  /* absolute path of the running program (/proc/<pid>/exe) */

    u64 arg_start, arg_end, env_start, env_end, start_stack;   /* where execve put the arguments, the environment and the initial stack pointer (the strings are read back through the page tables: /proc/<pid>/cmdline, environ) */
    u8 abi;                         /* system call personality: ABI_NATIVE or ABI_LINUX (docs/COMPAT.md); fork copies it, execve picks it from the file's markers (a file with none, or SYS_setabi's request: see exec.c) */
    u8 exec_abi;                    /* SYS_setabi: the personality of the next successful execve, plus one (0 = no request) */
    /* The Linux root (docs/COMPAT.md, kernel/linux/root.c): a Linux program started from a native one runs inside the tmpfs mounted on /linux, which is "/" for it */
    VNode *root;                    /* what "/" means to this process: NULL = the real root; else the root of the Linux tree (a reference; fork copies it, a native exec drops it) */
    bool exec_noroot;               /* SYS_setabi(ABI_LINUX | ABI_NOROOT): the next Linux program runs in the real root (a Linux binary kept at a native path) */
    u64 alarm_at;                   /* alarm(): the uptime in ms at which SIGALRM is sent (0 = no alarm); kept across execve, not inherited by fork */

    /* The Linux personality's signals and timers (docs/COMPAT.md; kernel/sys/signal.c, kernel/linux/timers.c). None of it is inherited by fork. */
    SigList sigl;                   /* the siginfo of the signals behind sig_pending */
    int  sigq_count;                /* entries held by the queues of the process and of its threads (SIGQ_PER_PROC at most) */
    u64  alarm_interval;            /* setitimer(ITIMER_REAL): ms between expirations after the first (0: once); alarm() clears it */
    u64  itimer_virt, itimer_virt_iv;   /* ITIMER_VIRTUAL: CPU ticks left, counting only user mode, and the reload value (0 = off) */
    u64  itimer_prof, itimer_prof_iv;   /* ITIMER_PROF: CPU ticks left, user and system together, and the reload value */
    struct KTimer *ktimers;         /* POSIX timers (timer_create); dropped at execve and at exit */
    int  ktimer_next_id;
    u64  dead_ticks, dead_utime;    /* cpu_ticks and utime_ticks of the threads that have ended */
    u64  child_ticks, child_utime;  /* the same for the children this process has waited for, and their waited-for children (times, getrusage) */
    Rlimits rlim;                   /* getrlimit / setrlimit: set to the defaults by process_alloc, copied by fork (kernel/sys/rlimit.c) */
} Process;

extern Process *g_init_proc;

/* lifecycle */
void     process_init(void);
Process *process_by_pid(int pid);
int      process_spawn_init(const char *path, char *const argv[]);      /* kernel: start PID 1 */
NORETURN void process_exit(int status);                                  /* status is a wait() word */
int      process_fork(TrapFrame *tf);                                    /* child pid or -errno */
int      process_fork_ex(TrapFrame *tf, u64 settid_addr, u64 cleartid_addr);   /* + the child's id stored in its memory / the word its exit clears (Linux clone) */
/* Returns 0 (tf holds the new user state), -errno, or EXEC_THREAD_LEAVES: another thread is tearing the process down (exit_group
 * or its own execve) and the caller must free what it holds and then call thread_leave(). */
#define EXEC_THREAD_LEAVES 1
int      process_exec(Process *p, const char *path, char *const argv[], char *const envp[], TrapFrame *tf);
long     process_wait(int pid, int *status, int options);

/* waitid(2) (the Linux personality): which child, which events (the Linux values of the option bits), and what happened */
#define WAITID_ALL        0
#define WAITID_PID        1
#define WAITID_PGID       2
#define WAITID_NOHANG     0x00000001
#define WAITID_STOPPED    0x00000002
#define WAITID_EXITED     0x00000004
#define WAITID_CONTINUED  0x00000008
#define WAITID_NOWAIT     0x01000000
typedef struct WaitInfo { int pid; u32 uid; int code; int status; s64 utime, stime; } WaitInfo;
long     process_waitid(int idtype, int id, int options, WaitInfo *out);       /* 0 (out->pid 0: WNOHANG found nothing), -ECHILD, -EINTR */

/* execve limits (docs/HARDENING.md): entries per vector, bytes per string (the NUL included), and the whole argument block, which
 * counts the strings and a pointer for each of them, as on Linux. Anything above fails with E2BIG. */
#define EXEC_MAX_ARGS   32768
#define EXEC_MAX_STRING (128 * 1024)
#define EXEC_MAX_TOTAL  (512 * 1024)
#define EXEC_FIXED      64                                                  /* argc, the two terminating NULLs, the auxiliary vector */
static inline size_t exec_footprint(size_t string_bytes, size_t nptrs) { return string_bytes + nptrs * sizeof(char *) + EXEC_FIXED; }
void     process_for_each(void (*fn)(Process *p, void *ctx), void *ctx);
extern int g_alarms_armed;                                               /* processes with an alarm pending: the scheduler tick looks only while there are any */
void     alarm_tick(u64 now);                                            /* sends SIGALRM to the processes whose alarm is due (called from the timer interrupt) */
int      process_count(void);

/* descriptors */
int      fd_install(Process *p, File *f, int minfd, int flags);        /* takes over the File reference */
File    *fd_get(Process *p, int fd);                                    /* borrowed reference */
File    *fd_get_ref(Process *p, int fd);                                /* owned reference (file_put it): safe against a close in another thread */
int      fd_close(Process *p, int fd);
int      fd_place(Process *p, int fd, File *f, int flags);              /* dup2: puts f (reference taken over) at fd, closing what was there; -EBADF beyond the soft limit */
int      fd_getflags(Process *p, int fd);                               /* descriptor flags read under the table lock, or -EBADF */
int      fd_setflags(Process *p, int fd, int set, int clear);           /* descriptor flags changed under the table lock: 0 or -EBADF */
int      fd_reserve(Process *p, int fd);                                /* grows the table so that fd is a slot: 0, or -ENOMEM / -EMFILE beyond FD_HARD_MAX */
static inline int fd_slots(const Process *p) { return p->fd_cap; }      /* the table's length: every loop over descriptors ends here */

/* user address space */
int      uvm_new(AddrSpace *as);
Vma     *vma_find(AddrSpace *as, u64 addr);
int      uvm_map(AddrSpace *as, u64 addr, u64 len, u32 prot, u32 flags);   /* demand-zero region */
void     uvm_set_file(AddrSpace *as, u64 addr, u64 len, const char *path, bool canonical, u64 dev, u64 ino, u64 off);   /* the mapping just made at addr shows this file, `off` into it (maps); a path that is not canonical yet is made so */
void     uvm_copy_file(Vma *to, const Vma *from);                          /* `to` shows the same file as `from`, at the matching offset when it starts later */
int      uvm_populate(AddrSpace *as, u64 addr, u64 len);                   /* allocate and map pages now */
int      uvm_unmap(AddrSpace *as, u64 addr, u64 len);
int      uvm_protect(AddrSpace *as, u64 addr, u64 len, u32 prot);
u64      uvm_find_free(AddrSpace *as, u64 len);
int      uvm_clone(AddrSpace *dst, const AddrSpace *src);
#define  UVM_COW_DEFAULT 1                                     /* copy-on-write fork unless "cow=off" (0: only with "cow=on"); docs/COW.md */
bool     uvm_cow_enabled(void);
u64      uvm_keep_cow(u64 newfl, u64 oldfl);                   /* new entry flags for a page that may still be marked copy-on-write */
int      uvm_cow_break(AddrSpace *as, u64 va, bool can_sleep); /* as->lock held: the page at va becomes private (0, -ENOMEM) */
void     uvm_destroy(AddrSpace *as);
bool     uvm_fault(AddrSpace *as, TrapFrame *tf, u64 addr);             /* demand paging; true if handled */
int      uvm_write(AddrSpace *as, u64 uaddr, const void *src, size_t n);   /* through the direct map */
u64      pte_flags_for(u32 prot, u32 vflags);

/* user memory access */
bool user_range_ok(const void *p, size_t n);
int  copy_from_user(void *dst, const void *src, size_t n);
int  copy_to_user(void *dst, const void *src, size_t n);
int  strncpy_from_user(char *dst, const char *src, size_t max);        /* length, or -EFAULT / -ENAMETOOLONG */
long deadline_from_timespec(const void *uts, u64 *deadline);          /* user struct gtimespec (relative) -> uptime_ms() deadline, ~0 = never; -EFAULT, -EINVAL */
long strnlen_user(const char *s, size_t max);                           /* length (NUL not counted) if the NUL is within the first max bytes, else -ENAMETOOLONG; -EFAULT */
int  clear_user(void *dst, size_t n);

/* threads (kernel/sys/thread.c) */
int      process_alloc_id(void);                                        /* next pid / tid (one number space) */
NORETURN void thread_leave(void);                                       /* the calling thread ends; the last one out finishes the process */
bool     thread_must_die(Task *t);                                      /* a group exit or an exec elsewhere wants this thread gone */
bool     thread_kill_others(Process *p);                                /* execve: all other threads gone (false: this one must leave) */
int      thread_count(void);                                            /* user threads in the whole system */
void     thread_unlink(Process *p, Task *t);                            /* irq off; see task.c */
void     futex_wake_all_at(u64 uaddr);                                  /* wake every waiter on a user word of the current address space */

/* job control (kernel/sys/signal.c, kernel/drivers/tty.c) */
void process_stop(Process *p, int sig);                                 /* the stop took effect: parents hear about it; threads park on their way to user mode */
void process_continue(Process *p);
void job_park(Process *p);                                              /* sleep while the process is stopped (returns at SIGCONT, or when the thread must die) */
bool pgrp_orphaned(int pgid);                                           /* no member has a parent in another group of the same session */
void pgrp_check_orphaned(int pgid);                                     /* orphaned and holds a stopped member: SIGHUP, then SIGCONT */
void pgrp_terminal_granted(int pgid);                                   /* the group became a terminal's foreground group: members stopped by SIGTTIN/SIGTTOU go on */
bool signal_is_stop(int sig);
int  tty_job_check(struct Tty *t, int kind);                            /* kernel/drivers/tty.c: 0, -EIO or -EINTR; may stop the caller */
#define JC_READ  0
#define JC_WRITE 1
#define JC_CTL   2

/* signals */
void signal_send(Process *p, int sig);                                  /* process-directed: any thread that does not block it */
void signal_send_thread(Task *t, int sig);                              /* thread-directed (tgkill, raise) */
void signal_kill_init(Process *init);                                   /* root's kill -9 1: the one signal init does not shrug off (see signal.c) */
void signal_force(Process *p, int sig);                                 /* synchronous faults: cannot be ignored or blocked */
void signal_pgrp(int pgid, int sig);                                    /* no permission checks: for the kernel's own use (the terminal's ^C) */
bool signal_permitted(const Process *from, const Process *to);
int  signal_group_from(const Process *from, int pgid, int sig, bool everyone);   /* kill(-pgid) / kill(-1) for a user */
bool pgrp_in_session(int pgid, int sid);                                /* some live process of the group belongs to the session */
void signal_deliver(TrapFrame *tf);                                     /* on the way back to user mode */
bool signal_pending_now(Process *p);                                    /* the calling thread has a deliverable signal (or must die) */
bool signal_cancel_pending(Task *t);                                    /* pthread_cancel is waiting for this thread to reach a cancellation point */
long sys_sigreturn_impl(TrapFrame *tf);

/* signals with a siginfo: the Linux personality queues them (kernel/sys/signal.c). A signal sent with no info is the kernel's own (SI_KERNEL). */
#define SIGQ_PER_PROC 128                                               /* queued signals one process may hold (sigqueue fails with EAGAIN beyond that) */
int  signal_send_info(Process *p, int sig, const SigInfo *info);        /* 0, or -EAGAIN when a queued (real-time) signal found no room */
int  signal_send_thread_info(Task *t, int sig, const SigInfo *info);
int  signal_send_from(const Process *from, Process *to, int sig);       /* kill(2): the sender's pid and user id go into the siginfo when `to` can read one */
long signal_kill(Process *self, long pid, int sig);                     /* kill(2) of either personality: one process, a group, everybody; 0 or -errno */
int  signal_group_info(const Process *from, int pgid, int sig, bool everyone, const SigInfo *info);   /* like signal_group_from, with a siginfo (NULL: from kill(2)) */
void signal_discard(Process *p, u64 mask);                              /* drop these pending signals, in the process and in every thread */
void signal_action_changed(Process *p, int sig);                        /* a signal whose action became "ignore" loses what is pending */
long signal_wait(u64 set, u64 deadline, SigInfo *info);                 /* sigtimedwait: the signal taken (and its siginfo), -EAGAIN at the deadline, -EINTR */
void signal_thread_exit(Task *t);                                       /* the thread's queued signals are dropped */
void signal_process_exit(Process *p);
void signal_exec_reset(Process *p);                                     /* execve: handlers back to default, nothing pending, POSIX timers gone */
void signal_child_info(SigInfo *info, const Process *child, int code, int status);   /* the SIGCHLD siginfo for a child event (code CLD_*, status as in siginfo) */
void process_cpu_ticks(const Process *p, u64 *user, u64 *total);        /* CPU time of all the threads, living and dead, in timer ticks */
void ktimers_cpu_tick(Process *p, bool user);                           /* the scheduler tick ran this process: setitimer(ITIMER_VIRTUAL, ITIMER_PROF) */
void ktimers_tick(u64 now);                                             /* POSIX timers that are due (called from the timer interrupt while any is armed) */
void ktimers_process_exit(Process *p);                                  /* exit and execve: the POSIX timers are dropped */
void ktimer_signal_taken(Process *p, SigInfo *info);                    /* a timer's signal is being delivered: its siginfo gets the overrun count */
void ktimer_signal_dropped(Process *p, const SigInfo *info);            /* a timer's signal was thrown away: the timer may queue the next one */
void signal_drop_timer(Process *p, int timerid);                        /* timer_delete: the timer's queued signal never arrives (irq off) */
extern int g_ktimers_armed;

/* resource limits (kernel/sys/rlimit.c; docs/HARDENING.md) */
void rlimit_defaults(Rlimits *r);                                       /* a new process starts from these (init, and what fork then overwrites with the parent's) */
long rlimit_get(const Process *caller, int pid, int res, struct grlimit *out);              /* pid 0 = the caller; 0 or -errno */
long rlimit_set(Process *caller, int pid, int res, const struct grlimit *nw);               /* the POSIX rules; 0 or -errno */
int  rlimit_nofile(const Process *p);                                   /* descriptors the process may have open now: its soft limit, at most FD_HARD_MAX */
bool rlimit_nproc_exceeded(const Process *p);                           /* fork would take the real user above RLIMIT_NPROC (root is exempt) */
int  rlimit_vm_check(const AddrSpace *as, u64 len, u32 prot, u32 vflags);                   /* -ENOMEM when mapping len more bytes of the caller's own space breaks RLIMIT_AS or RLIMIT_DATA */
bool rlimit_stack_ok(const AddrSpace *as, u64 addr);                    /* the stack may grow down to this address (RLIMIT_STACK of the caller) */

/* syscall plumbing */
typedef long (*SyscallFn)(TrapFrame *tf);
void syscall_register(int nr, SyscallFn fn);        /* subsystems add their own calls from an INITCALL */
#ifndef A0
#define A0(tf) ((tf)->rdi)
#define A1(tf) ((tf)->rsi)
#define A2(tf) ((tf)->rdx)
#define A3(tf) ((tf)->r10)
#define A4(tf) ((tf)->r8)
#define A5(tf) ((tf)->r9)
#endif
void syscall_init(void);
void syscall_dispatch(TrapFrame *tf);
void enter_user(TrapFrame *tf);                                         /* never returns */
void fork_child_start(void);
Task *task_create_fork(const char *name, const TrapFrame *tf, Process *proc);
/* Like task_create_fork, but the new thread also gets its start-up state (TLS, tid word, signal mask, tid) before it can run,
 * and is linked into proc->threads. A forked child's first thread and every pthread come through here. */
Task *task_create_thread(const char *name, const TrapFrame *tf, Process *proc, const ThreadInit *ti);
Task *task_create_user(const char *name, void (*entry)(void *), void *arg, Process *proc);
extern u64 g_syscall_kstack;

/* helpers */
void path_normalize(const char *base, const char *path, char *out, size_t n);
void path_canonical(VNode *root, const char *in, char *out, size_t n);   /* `in` (absolute) with every symbolic link along it replaced by its target, read in the directory root means by "/" (NULL: the caller's own); what a mapping and /proc/<pid>/exe show */

#endif
