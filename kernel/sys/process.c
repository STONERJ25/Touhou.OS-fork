/* Process table, descriptors, fork, exit, wait, and the fault hooks that tie user mode into the trap path. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "tty.h"
#include "ktime.h"
#include "arch.h"
#include "irq.h"

extern bool (*g_user_fault_hook)(TrapFrame *tf, u64 cr2);
extern bool (*g_pagefault_hook)(TrapFrame *tf, u64 cr2);
extern char __uaccess_start[], __uaccess_end[];
void __uaccess_fault(void);

static Process *g_procs;
static int g_next_pid = 1;
static int g_nprocs;
Process *g_init_proc;

/* ------------------------------------------------------------------ table */

static Process *process_alloc(void) {
    if (g_nprocs >= MAX_PROCS) return NULL;
    Process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    wq_init(&p->wait_wq);
    wq_init(&p->thread_wq);
    p->umask = 022;
    p->start_ms = uptime_ms();
    u64 f = irq_save();
    p->pid = g_next_pid++;
    p->all_next = g_procs;
    g_procs = p;
    g_nprocs++;
    irq_restore(f);
    return p;
}

static void process_free(Process *p) {
    u64 f = irq_save();
    for (Process **pp = &g_procs; *pp; pp = &(*pp)->all_next) {
        if (*pp == p) { *pp = p->all_next; break; }
    }
    g_nprocs--;
    irq_restore(f);
    kfree(p);
}

/* Process ids and thread ids come from one counter, so a thread id never equals the pid of another process. */
int process_alloc_id(void) {
    u64 f = irq_save();
    int id = g_next_pid++;
    irq_restore(f);
    return id;
}

Process *process_by_pid(int pid) {
    Process *found = NULL;
    u64 f = irq_save();
    for (Process *p = g_procs; p; p = p->all_next)
        if (p->pid == pid) { found = p; break; }
    irq_restore(f);
    return found;
}

void process_for_each(void (*fn)(Process *p, void *ctx), void *ctx) {
    u64 f = irq_save();
    for (Process *p = g_procs; p; p = p->all_next) fn(p, ctx);
    irq_restore(f);
}

int process_count(void) { return g_nprocs; }

/* ------------------------------------------------------------------ descriptors */

/* The descriptor table is shared by every thread: scans and updates run with interrupts disabled (its lock). */
int fd_install(Process *p, File *f, int minfd, int flags) {
    u64 fl = irq_save();
    for (int i = minfd < 0 ? 0 : minfd; i < MAX_FD; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            p->fd_flags[i] = (u8)flags;
            irq_restore(fl);
            return i;
        }
    }
    irq_restore(fl);
    return -EMFILE;
}

File *fd_get(Process *p, int fd) {
    if (fd < 0 || fd >= MAX_FD) return NULL;
    return p->fds[fd];
}

/* Like fd_get, but takes a reference under the table lock: safe against another thread closing the descriptor. */
File *fd_get_ref(Process *p, int fd) {
    if (fd < 0 || fd >= MAX_FD) return NULL;
    u64 fl = irq_save();
    File *f = p->fds[fd];
    if (f) file_get(f);
    irq_restore(fl);
    return f;
}

int fd_close(Process *p, int fd) {
    if (fd < 0 || fd >= MAX_FD) return -EBADF;
    u64 fl = irq_save();
    File *f = p->fds[fd];
    p->fds[fd] = NULL;
    p->fd_flags[fd] = 0;
    irq_restore(fl);
    if (!f) return -EBADF;
    file_put(f);
    return 0;
}

/* ------------------------------------------------------------------ paths */

/* Lexically joins base + path and removes "." and ".." components. */
void path_normalize(const char *base, const char *path, char *out, size_t n) {
    char tmp[PATH_MAX];
    if (path[0] == '/') strlcpy(tmp, path, sizeof tmp);
    else snprintf(tmp, sizeof tmp, "%s/%s", base, path);
    char *comps[128];
    int nc = 0;
    for (char *s = tmp; *s; ) {
        while (*s == '/') s++;
        if (!*s) break;
        char *start = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (!strcmp(start, ".")) continue;
        if (!strcmp(start, "..")) { if (nc) nc--; continue; }
        if (nc < 128) comps[nc++] = start;
    }
    out[0] = 0;
    if (!nc) { strlcpy(out, "/", n); return; }
    for (int i = 0; i < nc; i++) {
        strlcat(out, "/", n);
        strlcat(out, comps[i], n);
    }
}

/* ------------------------------------------------------------------ fork / exit / wait */

int process_fork(TrapFrame *tf) {
    Process *par = current->proc;
    Process *c = process_alloc();
    if (!c) return -EAGAIN;

    c->ppid = par->pid;
    c->pgid = par->pgid;
    c->sid = par->sid;
    c->ctty = par->ctty;
    c->cred = par->cred;
    c->ruid = par->ruid;
    c->rgid = par->rgid;
    c->umask = par->umask;
    strlcpy(c->name, par->name, sizeof c->name);
    strlcpy(c->cwd_path, par->cwd_path, sizeof c->cwd_path);
    strlcpy(c->cmdline, par->cmdline, sizeof c->cmdline);
    c->cwd = par->cwd;
    if (c->cwd) vnode_get(c->cwd);
    memcpy(c->sigact, par->sigact, sizeof c->sigact);
    u64 fl = irq_save();                            /* the table is shared with the parent's other threads */
    for (int i = 0; i < MAX_FD; i++) {
        if (par->fds[i]) {
            file_get(par->fds[i]);
            c->fds[i] = par->fds[i];
            c->fd_flags[i] = par->fd_flags[i];
        }
    }
    irq_restore(fl);

    int rc = uvm_new(&c->as);
    if (!rc) {
        mutex_lock(&par->as.lock);                  /* a sibling thread must not change the mappings while they are copied */
        rc = uvm_clone(&c->as, &par->as);
        mutex_unlock(&par->as.lock);
    }
    if (rc) {
        for (int i = 0; i < MAX_FD; i++) if (c->fds[i]) file_put(c->fds[i]);
        if (c->cwd) vnode_put(c->cwd);
        uvm_destroy(&c->as);
        process_free(c);
        return rc;
    }

    u64 f = irq_save();
    c->parent = par;
    c->sibling = par->children;
    par->children = c;
    irq_restore(f);

    /* Only the calling thread survives in the child: it keeps its TLS base and signal mask, and becomes the child's first thread. */
    ThreadInit ti = { .utid = c->pid, .fs_base = current->fs_base, .clear_tid = 0, .sig_blocked = current->sig_blocked };
    c->task = task_create_thread(c->name, tf, c, &ti);
    if (!c->task) {                                 /* unwind: the child never ran */
        u64 g = irq_save();
        par->children = c->sibling;
        irq_restore(g);
        for (int i = 0; i < MAX_FD; i++) if (c->fds[i]) file_put(c->fds[i]);
        if (c->cwd) vnode_put(c->cwd);
        uvm_destroy(&c->as);
        process_free(c);
        return -ENOMEM;
    }
    return c->pid;
}

/* The last thread out: releases everything the process owned and turns it into a zombie for the parent. */
static NORETURN void process_finalize(Process *p, int status) {
    if (p == g_init_proc) panic("init (pid 1) exited with status %#x: the shrine has no maiden", status);

    for (int i = 0; i < MAX_FD; i++) if (p->fds[i]) fd_close(p, i);
    if (p->cwd) { vnode_put(p->cwd); p->cwd = NULL; }
    for (unsigned i = 0; i < ARRAY_SIZE(p->old_cwd); i++) if (p->old_cwd[i]) { vnode_put(p->old_cwd[i]); p->old_cwd[i] = NULL; }

    /* Leave the dying address space before freeing it. */
    vmm_switch(vmm_kernel_pml4());
    current->pml4 = vmm_kernel_pml4();
    uvm_destroy(&p->as);

    u64 f = irq_save();
    bool wake_init = false;
    for (Process *c = p->children; c; ) {
        Process *next = c->sibling;
        c->parent = g_init_proc;
        c->ppid = g_init_proc->pid;
        c->sibling = g_init_proc->children;
        g_init_proc->children = c;
        if (c->state == PROC_ZOMBIE) wake_init = true;
        c = next;
    }
    p->children = NULL;
    p->exit_status = status;
    p->state = PROC_ZOMBIE;
    p->task = NULL;                                 /* the Task is about to be reaped; zombies keep only their status */
    Process *parent = p->parent;
    irq_restore(f);

    if (wake_init) wq_wake_all(&g_init_proc->wait_wq);
    if (parent) {
        signal_send(parent, SIGCHLD);
        wq_wake_all(&parent->wait_wq);
    }
    task_exit(status);
}

/* One thread leaves. Its tid word is zeroed and woken (that is how pthread_join learns it is gone), it unlinks itself, and
 * the last thread out finishes the process: with the status exit_group chose, or 0 when the last thread simply ran out. */
NORETURN void thread_leave(void) {
    Task *t = current;
    Process *p = t->proc;
    if (!p) task_exit(0);
    if (t->clear_tid) {
        u64 addr = t->clear_tid;
        t->clear_tid = 0;
        clear_user((void *)addr, sizeof(u32));
        futex_wake_all_at(addr);
    }
    u64 f = irq_save();
    thread_unlink(p, t);
    bool last = p->nthreads == 0;
    int status = p->exiting ? p->exit_status : 0;
    irq_restore(f);
    wq_wake_all(&p->thread_wq);                     /* an execve may be waiting for the others to go */
    if (last) process_finalize(p, status);
    task_exit(0);
}

/* exit_group: the status is a wait() word. Every other thread is woken and leaves at its next trip toward user mode. */
NORETURN void process_exit(int status) {
    Task *self = current;
    Process *p = self->proc;
    if (!p) task_exit(status);
    u64 f = irq_save();
    if (!p->exiting) {
        p->exiting = true;
        p->exit_status = status;
        for (Task *t = p->threads; t; t = t->thread_next)
            if (t != self) task_interrupt(t);
    }
    irq_restore(f);
    thread_leave();
}

long process_wait(int pid, int *status, int options) {
    Process *p = current->proc;
    u64 f = irq_save();
    for (;;) {
        bool have_child = false;
        for (Process *c = p->children; c; c = c->sibling) {
            if (pid > 0 && c->pid != pid) continue;
            if (pid == 0 && c->pgid != p->pgid) continue;
            if (pid < -1 && c->pgid != -pid) continue;
            have_child = true;
            if (c->state != PROC_ZOMBIE) continue;
            int cpid = c->pid;
            if (status) *status = c->exit_status;
            for (Process **pp = &p->children; *pp; pp = &(*pp)->sibling)
                if (*pp == c) { *pp = c->sibling; break; }
            irq_restore(f);
            process_free(c);
            return cpid;
        }
        if (!have_child) { irq_restore(f); return -ECHILD; }
        if (options & WNOHANG) { irq_restore(f); return 0; }
        if (signal_pending_now(p)) { irq_restore(f); return -EINTR; }
        current->intr = true;
        wq_block(&p->wait_wq);
        current->intr = false;
    }
}

/* ------------------------------------------------------------------ spawning PID 1 */

static void init_entry(void *arg) {
    char **argv = arg;
    Process *p = current->proc;
    static char *envp[] = { "HOME=/root", "PATH=/bin:/sbin:/usr/bin", "TERM=xterm", "USER=root",
                            "HOSTNAME=gensokyo", NULL };
    TrapFrame tf;
    int rc = process_exec(p, argv[0], argv, envp, &tf);
    if (rc) {
        klog("Gensokyo", "cannot start %s (%d): the shrine is empty", argv[0], rc);
        process_exit(1 << 8);
    }
    enter_user(&tf);
}

int process_spawn_init(const char *path, char *const argv[]) {
    static char *init_argv[2];
    static char init_path[128];
    strlcpy(init_path, path, sizeof init_path);
    init_argv[0] = init_path;
    init_argv[1] = NULL;
    (void)argv;

    Process *p = process_alloc();
    if (!p) return -ENOMEM;
    p->ppid = 0;
    p->pgid = p->sid = p->pid;
    strlcpy(p->name, "init", sizeof p->name);
    strlcpy(p->cwd_path, "/", sizeof p->cwd_path);
    VNode *root = vfs_root();
    vnode_get(root);
    p->cwd = root;
    p->cred.uid = p->cred.gid = 0;
    g_init_proc = p;

    for (int i = 0; i < 3; i++) {
        File *f;
        int rc = vfs_open("/dev/console", i == 0 ? O_RDONLY : O_WRONLY, 0, NULL, NULL, &f);
        if (rc) { klog("Gensokyo", "no /dev/console for init (%d)", rc); return rc; }
        fd_install(p, f, i, 0);
    }
    p->task = task_create_user("init", init_entry, init_argv, p);
    return p->task ? 0 : -ENOMEM;
}

/* ------------------------------------------------------------------ hooks into the trap path */

static bool process_pagefault(TrapFrame *tf, u64 cr2) {
    if (cr2 >= USER_TOP) return false;
    Process *p = current->proc;
    if (p && uvm_fault(&p->as, tf, cr2)) return true;
    if (!(tf->cs & 3) && tf->rip >= (u64)__uaccess_start && tf->rip < (u64)__uaccess_end) {
        tf->rip = (u64)__uaccess_fault;                     /* the copy routine reports a short copy */
        return true;
    }
    return false;
}

static bool process_user_fault(TrapFrame *tf, u64 cr2) {
    Process *p = current->proc;
    if (!p) return false;
    int sig;
    const char *what;
    switch (tf->vector) {
    case 0: case 16: case 19: sig = SIGFPE;  what = "did arithmetic the Boundary does not allow"; break;
    case 6:                   sig = SIGILL;  what = "cast an unknown spell card (illegal instruction)"; break;
    case 3:                   sig = SIGTRAP; what = "hit a breakpoint"; break;
    case 17:                  sig = SIGBUS;  what = "misaligned a spell"; break;
    case 14:                  sig = SIGSEGV; what = "crossed the Boundary"; break;
    default:                  sig = SIGSEGV; what = "violated the Boundary"; break;
    }
    if (tf->vector == 14)
        klog("Eiki", "BOUNDARY VIOLATION: '%s' (pid %d) %s at %#lx (rip %#lx, err %#lx) -- sent to the Netherworld (signal %d)",
             p->name, p->pid, what, (unsigned long)cr2, (unsigned long)tf->rip, (unsigned long)tf->error, sig);
    else
        klog("Eiki", "incident: '%s' (pid %d) %s (vector %lu, rip %#lx) -- signal %d", p->name, p->pid, what,
             (unsigned long)tf->vector, (unsigned long)tf->rip, sig);
    signal_force(p, sig);
    return true;
}

static bool process_sigpending_hook(void) {
    Process *p = current->proc;
    return p && signal_pending_now(p);
}

static void tty_signal(int pgid, int sig) { signal_pgrp(pgid, sig); }

static void signal_self(int sig) {                  /* SIGPIPE and friends: aimed at the thread that caused them */
    if (current->proc) signal_send_thread(current, sig);
}

/* execve: ask every other thread to leave and wait until none is left. False if the process is already being torn down
 * (exit_group, or another thread's execve got there first): the caller must then leave too. */
bool thread_kill_others(Process *p) {
    u64 f = irq_save();
    if (p->exiting || (p->execing && p->execing != current)) { irq_restore(f); return false; }
    if (p->nthreads > 1) {
        p->execing = current;
        for (Task *t = p->threads; t; t = t->thread_next)
            if (t != current) task_interrupt(t);
        while (p->nthreads > 1) wq_block(&p->thread_wq);
        p->execing = NULL;
    }
    irq_restore(f);
    return true;
}

extern void (*g_signal_self_hook)(int sig);

void process_init(void) {
    g_pagefault_hook = process_pagefault;
    g_user_fault_hook = process_user_fault;
    g_signal_pending_hook = process_sigpending_hook;
    g_tty_signal_hook = tty_signal;
    g_signal_self_hook = signal_self;
    syscall_init();
    klog("Alice", "process manager online: puppets may now be strung up in user mode");
}
