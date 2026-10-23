/* Processes: address space, descriptors, credentials, signals, fork/exec/exit/wait. */
#ifndef K_PROCESS_H
#define K_PROCESS_H

#include "types.h"
#include "task.h"
#include "vfs.h"
#include "mm.h"
#include "abi.h"
#include "syscall.h"

#define MAX_FD          64
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

typedef struct Vma {
    u64 start, end;
    u32 prot;                       /* PROT_* */
    u32 flags;                      /* VMA_* */
    struct Vma *next;
} Vma;

typedef struct AddrSpace {
    paddr_t pml4;
    Vma *vmas;
    u64 brk_start, brk;
    u64 mmap_next;
} AddrSpace;

typedef enum { PROC_RUNNING, PROC_ZOMBIE } ProcState;

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

    File *fds[MAX_FD];
    u8 fd_flags[MAX_FD];

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

    u32 sig_pending;                /* process-directed signals not yet taken by a thread (per-thread masks live in Task) */
    struct gsigaction sigact[NSIG];

    u64 start_ms;
    char cmdline[128];
} Process;

extern Process *g_init_proc;

/* lifecycle */
void     process_init(void);
Process *process_by_pid(int pid);
int      process_spawn_init(const char *path, char *const argv[]);      /* kernel: start PID 1 */
NORETURN void process_exit(int status);                                  /* status is a wait() word */
int      process_fork(TrapFrame *tf);                                    /* child pid or -errno */
int      process_exec(Process *p, const char *path, char *const argv[], char *const envp[], TrapFrame *tf);
long     process_wait(int pid, int *status, int options);
void     process_for_each(void (*fn)(Process *p, void *ctx), void *ctx);
int      process_count(void);

/* descriptors */
int      fd_install(Process *p, File *f, int minfd, int flags);        /* takes over the File reference */
File    *fd_get(Process *p, int fd);                                    /* borrowed reference */
File    *fd_get_ref(Process *p, int fd);                                /* owned reference (file_put it): safe against a close in another thread */
int      fd_close(Process *p, int fd);

/* user address space */
int      uvm_new(AddrSpace *as);
Vma     *vma_find(AddrSpace *as, u64 addr);
int      uvm_map(AddrSpace *as, u64 addr, u64 len, u32 prot, u32 flags);   /* demand-zero region */
int      uvm_populate(AddrSpace *as, u64 addr, u64 len);                   /* allocate and map pages now */
int      uvm_unmap(AddrSpace *as, u64 addr, u64 len);
int      uvm_protect(AddrSpace *as, u64 addr, u64 len, u32 prot);
u64      uvm_find_free(AddrSpace *as, u64 len);
int      uvm_clone(AddrSpace *dst, const AddrSpace *src);
void     uvm_destroy(AddrSpace *as);
bool     uvm_fault(AddrSpace *as, TrapFrame *tf, u64 addr);             /* demand paging; true if handled */
int      uvm_write(AddrSpace *as, u64 uaddr, const void *src, size_t n);   /* through the direct map */
u64      pte_flags_for(u32 prot, u32 vflags);

/* user memory access */
bool user_range_ok(const void *p, size_t n);
int  copy_from_user(void *dst, const void *src, size_t n);
int  copy_to_user(void *dst, const void *src, size_t n);
int  strncpy_from_user(char *dst, const char *src, size_t max);        /* length, or -EFAULT / -ENAMETOOLONG */
int  clear_user(void *dst, size_t n);

/* threads (kernel/sys/thread.c) */
int      process_alloc_id(void);                                        /* next pid / tid (one number space) */
NORETURN void thread_leave(void);                                       /* the calling thread ends; the last one out finishes the process */
bool     thread_must_die(Task *t);                                      /* a group exit or an exec elsewhere wants this thread gone */
bool     thread_kill_others(Process *p);                                /* execve: all other threads gone (false: this one must leave) */
int      thread_count(void);                                            /* user threads in the whole system */
void     thread_unlink(Process *p, Task *t);                            /* irq off; see task.c */
void     futex_wake_all_at(u64 uaddr);                                  /* wake every waiter on a user word of the current address space */

/* signals */
void signal_send(Process *p, int sig);                                  /* process-directed: any thread that does not block it */
void signal_send_thread(Task *t, int sig);                              /* thread-directed (tgkill, raise) */
void signal_force(Process *p, int sig);                                 /* synchronous faults: cannot be ignored or blocked */
void signal_pgrp(int pgid, int sig);
void signal_deliver(TrapFrame *tf);                                     /* on the way back to user mode */
bool signal_pending_now(Process *p);                                    /* the calling thread has a deliverable signal (or must die) */
long sys_sigreturn_impl(TrapFrame *tf);

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

#endif
