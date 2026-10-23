/* System call table and implementations. Handlers read their arguments straight from the TrapFrame. */
#include "process.h"
#include "kernel.h"
#include "cpu.h"
#include "arch.h"
#include "ktime.h"
#include "random.h"
#include "tty.h"
#include "irq.h"
#include "cpuinfo.h"
#include "fb.h"

extern void syscall_entry(void);
void machine_poweroff(void);
void machine_reboot(void);

typedef long (*SyscallFn)(TrapFrame *tf);

#define A0(tf) ((tf)->rdi)
#define A1(tf) ((tf)->rsi)
#define A2(tf) ((tf)->rdx)
#define A3(tf) ((tf)->r10)
#define A4(tf) ((tf)->r8)
#define A5(tf) ((tf)->r9)

#define IO_CHUNK 65536

static inline Process *me(void) { return current->proc; }

/* Copies a user path into a kernel buffer. */
static int get_path(u64 uptr, char *buf) {
    int n = strncpy_from_user(buf, (const char *)uptr, PATH_MAX);
    return n < 0 ? n : (n == 0 ? -ENOENT : 0);
}

/* ------------------------------------------------------------------ process control */

static long sys_exit(TrapFrame *tf) { process_exit((int)((A0(tf) & 0xFF) << 8)); }

static long sys_fork(TrapFrame *tf) { return process_fork(tf); }

static long sys_getpid(TrapFrame *tf) { (void)tf; return me()->pid; }
static long sys_getppid(TrapFrame *tf) { (void)tf; return me()->ppid; }
static long sys_getuid(TrapFrame *tf) { (void)tf; return me()->ruid; }
static long sys_getgid(TrapFrame *tf) { (void)tf; return me()->rgid; }

/* root (effective) sets real and effective ids at once; anyone else may only re-assert one of their own ids */
static long sys_setuid(TrapFrame *tf) {
    Process *p = me();
    u32 u = (u32)A0(tf);
    if (p->cred.uid == 0) { p->cred.uid = p->ruid = u; return 0; }
    if (u == p->ruid || u == p->cred.uid) { p->cred.uid = u; return 0; }
    return -EPERM;
}

static long sys_setgid(TrapFrame *tf) {
    Process *p = me();
    u32 g = (u32)A0(tf);
    if (p->cred.uid == 0) { p->cred.gid = p->rgid = g; return 0; }
    if (g == p->rgid || g == p->cred.gid) { p->cred.gid = g; return 0; }
    return -EPERM;
}

static long sys_umask(TrapFrame *tf) {
    u32 old = me()->umask;
    me()->umask = (u32)A0(tf) & 0777;
    return old;
}

static long sys_execve(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    long rc = get_path(A0(tf), path);
    char **kargv = NULL, **kenvp = NULL;
    int argc = 0, envc = 0;
    size_t total = 0;

    for (int pass = 0; !rc && pass < 2; pass++) {
        const u64 *uvec = (const u64 *)(pass ? A2(tf) : A1(tf));
        int cnt = 0;
        char **vec = kzalloc(sizeof(char *) * 257);
        if (!vec) { rc = -ENOMEM; break; }
        for (; uvec; cnt++) {
            u64 sp;
            if (cnt >= 256) { rc = -E2BIG; break; }
            if (copy_from_user(&sp, uvec + cnt, sizeof sp)) { rc = -EFAULT; break; }
            if (!sp) break;
            vec[cnt] = kmalloc(1024);
            if (!vec[cnt]) { rc = -ENOMEM; break; }
            int n = strncpy_from_user(vec[cnt], (const char *)sp, 1024);
            if (n < 0) { rc = n == -ENAMETOOLONG ? -E2BIG : n; cnt++; break; }
            total += (size_t)n + 1;
            if (total > 128 * 1024) { rc = -E2BIG; cnt++; break; }
        }
        if (pass) { kenvp = vec; envc = cnt; } else { kargv = vec; argc = cnt; }
    }

    if (!rc) {
        TrapFrame ntf;
        rc = process_exec(me(), path, kargv, kenvp, &ntf);
        if (rc == 0) *tf = ntf;
    }
    for (int i = 0; kargv && i < 257 && (i < argc || kargv[i]); i++) kfree(kargv[i]);
    for (int i = 0; kenvp && i < 257 && (i < envc || kenvp[i]); i++) kfree(kenvp[i]);
    kfree(kargv);
    kfree(kenvp);
    kfree(path);
    return rc == 0 ? 0 : rc;
}

static long sys_waitpid(TrapFrame *tf) {
    int status = 0;
    long r = process_wait((int)A0(tf), &status, (int)A2(tf));
    if (r > 0 && A1(tf) && copy_to_user((void *)A1(tf), &status, sizeof status)) return -EFAULT;
    return r;
}

static long sys_kill(TrapFrame *tf) {
    long pid = (long)A0(tf);
    int sig = (int)A1(tf);
    if (sig < 0 || sig >= NSIG || sig == SIGCANCEL) return -EINVAL;      /* SIGCANCEL is thread-directed only (tgkill) */
    Process *self = me();
    if (pid > 0) {
        Process *t = process_by_pid((int)pid);
        if (!t || t->state == PROC_ZOMBIE) return -ESRCH;
        if (self->cred.uid != 0 && self->cred.uid != t->cred.uid) return -EPERM;
        if (sig) signal_send(t, sig);
        return 0;
    }
    int pg = pid == 0 ? self->pgid : (int)-pid;
    if (sig) signal_pgrp(pg, sig);
    return 0;
}

static long sys_setpgid(TrapFrame *tf) {
    int pid = (int)A0(tf), pg = (int)A1(tf);
    Process *t = pid ? process_by_pid(pid) : me();
    if (!t) return -ESRCH;
    if (t != me() && t->ppid != me()->pid) return -EPERM;
    int old_pgid = t->pgid;
    t->pgid = pg ? pg : t->pid;
    if (old_pgid != t->pgid) pgrp_check_orphaned(old_pgid);        /* leaving may have orphaned the old group (job control) */
    return 0;
}

static long sys_getpgid(TrapFrame *tf) {
    Process *t = A0(tf) ? process_by_pid((int)A0(tf)) : me();
    return t ? t->pgid : -ESRCH;
}

static long sys_setsid(TrapFrame *tf) {
    (void)tf;
    int old_pgid = me()->pgid;
    me()->sid = me()->pgid = me()->pid;
    if (old_pgid != me()->pgid) pgrp_check_orphaned(old_pgid);
    me()->ctty = NULL;                      /* a new session starts without a controlling terminal */
    return me()->sid;
}

static long sys_tcsetpgrp(TrapFrame *tf) {
    tty_console()->fg_pgid = (int)A0(tf);   /* legacy: libgap now uses ioctl(TIOCSPGRP) on the descriptor */
    pgrp_terminal_granted(tty_console()->fg_pgid);
    return 0;
}

static long sys_yield(TrapFrame *tf) { (void)tf; task_yield(); return 0; }

static long sys_pause(TrapFrame *tf) {
    (void)tf;
    u64 f = irq_save();
    while (!signal_pending_now(me())) {
        current->intr = true;
        current->state = TASK_SLEEPING;
        current->wake_ms = ~0ULL;
        schedule();
        current->intr = false;
    }
    irq_restore(f);
    return -EINTR;
}

static long sys_nanosleep(TrapFrame *tf) {
    struct gtimespec ts;
    if (copy_from_user(&ts, (void *)A0(tf), sizeof ts)) return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
    u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000 + ((ts.tv_nsec % 1000000) ? 1 : 0);
    u64 end = uptime_ms() + ms;
    u64 f = irq_save();
    while (uptime_ms() < end) {
        if (signal_pending_now(me())) { irq_restore(f); return -EINTR; }
        current->intr = true;
        current->wake_ms = end;
        current->state = TASK_SLEEPING;
        schedule();
        current->intr = false;
    }
    irq_restore(f);
    return 0;
}

static long sys_clock_gettime(TrapFrame *tf) {
    struct gtimespec ts;
    if (A0(tf) == CLOCK_MONOTONIC) {
        u64 ms = uptime_ms();
        ts.tv_sec = (s64)(ms / 1000);
        ts.tv_nsec = (s64)(ms % 1000) * 1000000;
    } else {
        u64 ms = time_now_ms();
        ts.tv_sec = (s64)(ms / 1000);
        ts.tv_nsec = (s64)(ms % 1000) * 1000000;
    }
    return copy_to_user((void *)A1(tf), &ts, sizeof ts);
}

static long sys_gettime_ms(TrapFrame *tf) { (void)tf; return (long)time_now_ms(); }

static long sys_setname(TrapFrame *tf) {
    char name[32];
    int n = strncpy_from_user(name, (const char *)A0(tf), sizeof name);
    if (n < 0 && n != -ENAMETOOLONG) return n;
    name[sizeof name - 1] = 0;
    strlcpy(me()->name, name, sizeof me()->name);
    return 0;
}

static long sys_debuglog(TrapFrame *tf) {
    char line[200];
    int n = strncpy_from_user(line, (const char *)A0(tf), sizeof line);
    if (n < 0 && n != -ENAMETOOLONG) return n;
    line[sizeof line - 1] = 0;
    klog(me()->name, "%s", line);
    return 0;
}

/* ------------------------------------------------------------------ files */

static long sys_open(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    long rc = get_path(A0(tf), path);
    if (rc == 0) {
        Process *p = me();
        int flags = (int)A1(tf);
        File *f;
        rc = vfs_open(path, flags, (u32)A2(tf) & ~p->umask, &p->cred, p->cwd, &f);
        if (rc == 0) {
            char abs[256];
            path_normalize(p->cwd_path, path, abs, sizeof abs);        /* /proc/<pid>/fd/N shows this */
            file_set_path(f, abs);
            rc = fd_install(p, f, 0, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
            if (rc < 0) file_put(f);
        }
    }
    kfree(path);
    return rc;
}

static long sys_close(TrapFrame *tf) { return fd_close(me(), (int)A0(tf)); }

static long sys_read(TrapFrame *tf) {
    File *f = fd_get_ref(me(), (int)A0(tf));          /* a reference, taken under the table lock: another thread may close the fd */
    if (!f) return -EBADF;
    size_t n = A2(tf), total = 0;
    u8 *ubuf = (u8 *)A1(tf);
    if (!user_range_ok(ubuf, n)) { file_put(f); return -EFAULT; }
    size_t chunk = MIN(n, (size_t)IO_CHUNK);
    u8 *kbuf = kmalloc(chunk ? chunk : 1);
    if (!kbuf) { file_put(f); return -ENOMEM; }
    long rc = 0;
    while (total < n) {
        size_t want = MIN(n - total, chunk);
        ssize_t r = vfs_read(f, kbuf, want);
        if (r < 0) { rc = total ? (long)total : r; break; }
        if (r == 0) { rc = (long)total; break; }
        if (copy_to_user(ubuf + total, kbuf, (size_t)r)) { rc = total ? (long)total : -EFAULT; break; }
        total += (size_t)r;
        rc = (long)total;
        if ((size_t)r < want) break;
    }
    file_put(f);
    kfree(kbuf);
    return rc;
}

static long sys_write(TrapFrame *tf) {
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    size_t n = A2(tf), total = 0;
    const u8 *ubuf = (const u8 *)A1(tf);
    if (!user_range_ok(ubuf, n)) { file_put(f); return -EFAULT; }
    size_t chunk = MIN(n, (size_t)IO_CHUNK);
    u8 *kbuf = kmalloc(chunk ? chunk : 1);
    if (!kbuf) { file_put(f); return -ENOMEM; }
    long rc = 0;
    while (total < n) {
        size_t want = MIN(n - total, chunk);
        if (copy_from_user(kbuf, ubuf + total, want)) { rc = total ? (long)total : -EFAULT; break; }
        ssize_t w = vfs_write(f, kbuf, want);
        if (w < 0) { rc = total ? (long)total : w; break; }
        total += (size_t)w;
        rc = (long)total;
        if ((size_t)w < want) break;
    }
    file_put(f);
    kfree(kbuf);
    return rc;
}

static long sys_lseek(TrapFrame *tf) {
    File *f = fd_get(me(), (int)A0(tf));
    return f ? vfs_lseek(f, (s64)A1(tf), (int)A2(tf)) : -EBADF;
}

static void file_stat(File *f, struct gstat *st) {
    if (f->vn) vnode_stat(f->vn, st);
    else { memset(st, 0, sizeof *st); st->st_mode = S_IFIFO | 0600; }
}

static long do_stat(TrapFrame *tf, int flags) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    struct gstat st;
    long rc = get_path(A0(tf), path);
    if (rc == 0) rc = vfs_stat(path, me()->cwd, &me()->cred, flags, &st);
    kfree(path);
    if (rc) return rc;
    return copy_to_user((void *)A1(tf), &st, sizeof st);
}

static long sys_stat(TrapFrame *tf) { return do_stat(tf, 0); }
static long sys_lstat(TrapFrame *tf) { return do_stat(tf, LOOKUP_NOFOLLOW); }

static long sys_fstat(TrapFrame *tf) {
    File *f = fd_get(me(), (int)A0(tf));
    if (!f) return -EBADF;
    struct gstat st;
    file_stat(f, &st);
    return copy_to_user((void *)A1(tf), &st, sizeof st);
}

static long sys_readdir(TrapFrame *tf) {
    File *f = fd_get(me(), (int)A0(tf));
    if (!f) return -EBADF;
    size_t count = A2(tf), done = 0;
    struct gdirent de;
    while (done < count) {
        int rc = vfs_readdir(f, &de);
        if (rc < 0) return done ? (long)done : rc;
        if (rc == 0) break;
        if (copy_to_user((struct gdirent *)A1(tf) + done, &de, sizeof de)) return -EFAULT;
        done++;
    }
    return (long)done;
}

#define PATH_CALL(name, expr)                                  \
    static long name(TrapFrame *tf) {                          \
        char *path = kmalloc(PATH_MAX);                        \
        if (!path) return -ENOMEM;                             \
        long rc = get_path(A0(tf), path);                      \
        if (rc == 0) rc = (expr);                              \
        kfree(path);                                           \
        return rc;                                             \
    }

PATH_CALL(sys_mkdir, vfs_mkdir(path, (u32)A1(tf) & ~me()->umask, &me()->cred, me()->cwd))
PATH_CALL(sys_unlink, vfs_unlink(path, &me()->cred, me()->cwd))
PATH_CALL(sys_rmdir, vfs_rmdir(path, &me()->cred, me()->cwd))
PATH_CALL(sys_truncate, vfs_truncate(path, A1(tf), &me()->cred, me()->cwd))
PATH_CALL(sys_chmod, vfs_chmod(path, (u32)A1(tf), &me()->cred, me()->cwd))
PATH_CALL(sys_chown, vfs_chown(path, (u32)A1(tf), (u32)A2(tf), &me()->cred, me()->cwd))

static long sys_access(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    long rc = get_path(A0(tf), path);
    if (rc == 0) {
        VNode *v;
        rc = vfs_lookup(path, me()->cwd, &me()->cred, 0, &v);
        if (rc == 0) {
            rc = (A1(tf) & 7) ? vfs_access(v, &me()->cred, (int)(A1(tf) & 7)) : 0;
            vnode_put(v);
        }
    }
    kfree(path);
    return rc;
}

static long sys_rename(TrapFrame *tf) {
    char *a = kmalloc(PATH_MAX), *b = kmalloc(PATH_MAX);
    long rc = -ENOMEM;
    if (a && b) {
        rc = get_path(A0(tf), a);
        if (!rc) rc = get_path(A1(tf), b);
        if (!rc) rc = vfs_rename(a, b, &me()->cred, me()->cwd);
    }
    kfree(a);
    kfree(b);
    return rc;
}

static long sys_link(TrapFrame *tf) {
    char *a = kmalloc(PATH_MAX), *b = kmalloc(PATH_MAX);
    long rc = -ENOMEM;
    if (a && b) {
        rc = get_path(A0(tf), a);
        if (!rc) rc = get_path(A1(tf), b);
        if (!rc) rc = vfs_link(a, b, &me()->cred, me()->cwd);
    }
    kfree(a);
    kfree(b);
    return rc;
}

static long sys_symlink(TrapFrame *tf) {
    char *a = kmalloc(PATH_MAX), *b = kmalloc(PATH_MAX);
    long rc = -ENOMEM;
    if (a && b) {
        rc = get_path(A0(tf), a);
        if (!rc) rc = get_path(A1(tf), b);
        if (!rc) rc = vfs_symlink(a, b, &me()->cred, me()->cwd);
    }
    kfree(a);
    kfree(b);
    return rc;
}

static long sys_readlink(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX), *out = kmalloc(PATH_MAX);
    long rc = -ENOMEM;
    if (path && out) {
        rc = get_path(A0(tf), path);
        if (!rc) {
            ssize_t n = vfs_readlink(path, out, MIN(A2(tf), (u64)PATH_MAX), me()->cwd, &me()->cred);
            rc = n < 0 ? n : (copy_to_user((void *)A1(tf), out, (size_t)n) ? -EFAULT : n);
        }
    }
    kfree(path);
    kfree(out);
    return rc;
}

static long sys_chdir(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    long rc = get_path(A0(tf), path);
    if (rc == 0) {
        Process *p = me();
        VNode *v;
        rc = vfs_lookup(path, p->cwd, &p->cred, LOOKUP_DIR, &v);
        if (rc == 0 && (rc = vfs_access(v, &p->cred, X_OK)) != 0) vnode_put(v);
        if (rc == 0) {
            /* Another thread may be resolving a relative path against the old directory right now, so a threaded process keeps
             * the old reference in a small ring and drops the oldest one instead (everything is released at exit). */
            VNode *old = p->cwd;
            if (old && p->nthreads > 1) {
                u64 f = irq_save();
                VNode *drop = p->old_cwd[0];
                for (unsigned i = 0; i + 1 < ARRAY_SIZE(p->old_cwd); i++) p->old_cwd[i] = p->old_cwd[i + 1];
                p->old_cwd[ARRAY_SIZE(p->old_cwd) - 1] = old;
                irq_restore(f);
                old = drop;
            }
            if (old) vnode_put(old);
            p->cwd = v;
            char norm[256];
            path_normalize(p->cwd_path, path, norm, sizeof norm);
            strlcpy(p->cwd_path, norm, sizeof p->cwd_path);
        }
    }
    kfree(path);
    return rc;
}

static long sys_getcwd(TrapFrame *tf) {
    const char *c = me()->cwd_path;
    size_t len = strlen(c) + 1;
    if (len > A1(tf)) return -ERANGE;
    return copy_to_user((void *)A0(tf), c, len) ? -EFAULT : (long)len;
}

static long sys_dup(TrapFrame *tf) {
    File *f = fd_get(me(), (int)A0(tf));
    if (!f) return -EBADF;
    file_get(f);
    long fd = fd_install(me(), f, 0, 0);
    if (fd < 0) file_put(f);
    return fd;
}

static long sys_dup2(TrapFrame *tf) {
    int oldfd = (int)A0(tf), newfd = (int)A1(tf);
    File *f = fd_get(me(), oldfd);
    if (!f) return -EBADF;
    if (newfd < 0 || newfd >= MAX_FD) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (me()->fds[newfd]) fd_close(me(), newfd);
    file_get(f);
    me()->fds[newfd] = f;
    me()->fd_flags[newfd] = 0;
    return newfd;
}

static long sys_pipe(TrapFrame *tf) {
    File *r, *w;
    int rc = vfs_pipe(&r, &w);
    if (rc) return rc;
    int fds[2];
    fds[0] = fd_install(me(), r, 0, 0);
    fds[1] = fds[0] >= 0 ? fd_install(me(), w, 0, 0) : -EMFILE;
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) fd_close(me(), fds[0]); else file_put(r);
        file_put(w);
        return -EMFILE;
    }
    if (copy_to_user((void *)A0(tf), fds, sizeof fds)) {
        fd_close(me(), fds[0]);
        fd_close(me(), fds[1]);
        return -EFAULT;
    }
    return 0;
}

static long sys_fcntl(TrapFrame *tf) {
    Process *p = me();
    int fd = (int)A0(tf);
    File *f = fd_get(p, fd);
    if (!f) return -EBADF;
    switch (A1(tf)) {
    case F_DUPFD: {
        file_get(f);
        long n = fd_install(p, f, (int)A2(tf), 0);
        if (n < 0) file_put(f);
        return n;
    }
    case F_GETFD: return p->fd_flags[fd];
    case F_SETFD: p->fd_flags[fd] = (u8)A2(tf); return 0;
    case F_GETFL: return f->flags;
    case F_SETFL: f->flags = (f->flags & O_ACCMODE) | ((int)A2(tf) & (O_APPEND | O_NONBLOCK)); return 0;
    default: return -EINVAL;
    }
}

static long sys_ftruncate(TrapFrame *tf) {
    File *f = fd_get(me(), (int)A0(tf));
    return f ? vfs_ftruncate(f, A1(tf)) : -EBADF;
}

static long sys_ioctl(TrapFrame *tf) {
    File *f = fd_get(me(), (int)A0(tf));
    if (!f) return -EBADF;
    u8 buf[256];
    memset(buf, 0, sizeof buf);
    if (A2(tf) && user_range_ok((void *)A2(tf), sizeof buf) == false) return -EFAULT;
    if (A2(tf)) copy_from_user(buf, (void *)A2(tf), sizeof buf);       /* best effort: short mappings just read less */
    long rc = vfs_ioctl(f, A1(tf), buf);
    if (rc >= 0 && A2(tf)) copy_to_user((void *)A2(tf), buf, sizeof buf);
    return rc;
}

static long sys_poll(TrapFrame *tf) {
    u64 nfds = A1(tf);
    if (nfds > 64) return -EINVAL;
    struct gpollfd pfd[64];
    if (copy_from_user(pfd, (void *)A0(tf), nfds * sizeof(struct gpollfd))) return -EFAULT;
    long timeout = (long)A2(tf);
    u64 deadline = timeout < 0 ? ~0ULL : uptime_ms() + (u64)timeout;
    for (;;) {
        int ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            pfd[i].revents = 0;
            File *f = fd_get(me(), pfd[i].fd);
            if (!f) { pfd[i].revents = POLLNVAL; ready++; continue; }
            int m = vfs_poll(f) & (pfd[i].events | POLLERR | POLLHUP);
            pfd[i].revents = (short)m;
            if (m) ready++;
        }
        if (ready || uptime_ms() >= deadline) {
            copy_to_user((void *)A0(tf), pfd, nfds * sizeof(struct gpollfd));
            return ready;
        }
        if (signal_pending_now(me())) return -EINTR;
        task_sleep_ms(2);
    }
}

/* ------------------------------------------------------------------ memory */

static long do_brk(TrapFrame *tf) {
    AddrSpace *as = &me()->as;
    u64 want = A0(tf);
    if (want == 0 || want < as->brk_start || want > as->brk_start + (1UL << 30)) return (long)as->brk;
    u64 nb = ALIGN_UP(want, PAGE_SIZE), ob = ALIGN_UP(as->brk, PAGE_SIZE);
    if (nb > ob) {
        if (uvm_map(as, ob, nb - ob, PROT_READ | PROT_WRITE, VMA_HEAP)) return (long)as->brk;
    } else if (nb < ob) {
        uvm_unmap(as, nb, ob - nb);
    }
    as->brk = want;
    return (long)want;
}

static long do_mmap(TrapFrame *tf) {
    AddrSpace *as = &me()->as;
    u64 addr = A0(tf), len = A1(tf);
    u32 prot = (u32)A2(tf);
    int flags = (int)A3(tf), fd = (int)A4(tf);
    u64 off = A5(tf);
    if (!len) return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1)) return -EINVAL;
        uvm_unmap(as, addr, len);
    } else {
        addr = uvm_find_free(as, len);
        if (!addr) return -ENOMEM;
    }

    if (flags & MAP_ANONYMOUS) {
        int rc = uvm_map(as, addr, len, prot, 0);
        return rc ? rc : (long)addr;
    }

    File *f = fd_get(me(), fd);
    if (!f || !f->vn) return -EBADF;
    VNode *v = f->vn;
    /* Device memory (framebuffer) and shared buffers (tmpfs files mapped MAP_SHARED, e.g. window contents). RAM pages are
     * reference counted and mapped write-back; anything else (MMIO, framebuffer) is write-combining. */
    if (v->ops && v->ops->mmap_phys && ((flags & MAP_SHARED) || !S_ISREG(v->mode))) {
        if (off & (PAGE_SIZE - 1)) return -EINVAL;
        int rc = uvm_map(as, addr, len, prot, VMA_IO);
        if (rc) return rc;
        Vma *vma = vma_find(as, addr);
        for (u64 o = 0; o < len; o += PAGE_SIZE) {
            paddr_t pa;
            if (v->ops->mmap_phys(v, off + o, &pa)) { uvm_unmap(as, addr, len); return -EINVAL; }
            bool ram = pmm_page_managed(pa);
            if (ram) pmm_get_page(pa);
            else if (vma) vma->flags |= VMA_WC;
            if (vmm_map(as->pml4, addr + o, pa, pte_flags_for(prot, ram ? 0 : VMA_WC))) {
                if (ram) pmm_free_page(pa);
                uvm_unmap(as, addr, len);
                return -ENOMEM;
            }
        }
        return (long)addr;
    }
    if (!S_ISREG(v->mode)) return -ENODEV;
    int rc = uvm_map(as, addr, len, prot | PROT_WRITE, 0);   /* private copy: fill now, then drop write if not requested */
    if (!rc) rc = uvm_populate(as, addr, len);
    if (rc) return rc;
    u64 save = f->off;
    for (u64 o = 0; o < len; o += PAGE_SIZE) {
        paddr_t pa = vmm_translate(as->pml4, addr + o, NULL);
        f->off = off + o;
        vfs_read(f, phys_to_virt(pa), PAGE_SIZE);
    }
    f->off = save;
    if (!(prot & PROT_WRITE)) uvm_protect(as, addr, len, prot);
    return (long)addr;
}

static long do_munmap(TrapFrame *tf) {
    u64 addr = A0(tf), len = ALIGN_UP(A1(tf), PAGE_SIZE);
    if (addr & (PAGE_SIZE - 1) || !len) return -EINVAL;
    return uvm_unmap(&me()->as, addr, len);
}

static long do_mprotect(TrapFrame *tf) {
    u64 addr = A0(tf), len = ALIGN_UP(A1(tf), PAGE_SIZE);
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    return uvm_protect(&me()->as, addr, len, (u32)A2(tf));
}

/* The mappings of a process are shared by all its threads: every call that changes them (and fork's copy, and the page
 * fault handler) holds the address space lock, a sleeping mutex, for its whole duration. */
#define MM_LOCKED(name, impl)                                  \
    static long name(TrapFrame *tf) {                          \
        AddrSpace *as = &me()->as;                             \
        mutex_lock(&as->lock);                                 \
        long r = impl(tf);                                     \
        mutex_unlock(&as->lock);                               \
        return r;                                              \
    }

MM_LOCKED(sys_brk, do_brk)
MM_LOCKED(sys_mmap, do_mmap)
MM_LOCKED(sys_munmap, do_munmap)
MM_LOCKED(sys_mprotect, do_mprotect)

/* ------------------------------------------------------------------ system */

static long sys_uname(TrapFrame *tf) {
    struct guname u;
    memset(&u, 0, sizeof u);
    strlcpy(u.sysname, "Gensokyo", sizeof u.sysname);
    strlcpy(u.nodename, "gensokyo", sizeof u.nodename);
    strlcpy(u.release, KERNEL_VERSION, sizeof u.release);
    strlcpy(u.version, "TouhouOS fan build", sizeof u.version);
    strlcpy(u.machine, "x86_64", sizeof u.machine);
    strlcpy(u.codename, KERNEL_CODENAME, sizeof u.codename);
    return copy_to_user((void *)A0(tf), &u, sizeof u);
}

static long sys_sysinfo(TrapFrame *tf) {
    struct gsysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime_ms = uptime_ms();
    si.total_ram = pmm_total_pages() * PAGE_SIZE;
    si.free_ram = pmm_free_pages_count() * PAGE_SIZE;
    si.heap_used = heap_bytes_in_use();
    si.nprocs = (u32)process_count();
    si.ntasks = (u32)task_count();
    si.ticks = timer_ticks();
    for (int i = 0; i < 16; i++) si.irqs += irq_count(i);
    si.fb_width = g_fb.width;
    si.fb_height = g_fb.height;
    strlcpy(si.cpu_brand, g_cpu.brand, sizeof si.cpu_brand);
    return copy_to_user((void *)A0(tf), &si, sizeof si);
}

static long sys_reboot(TrapFrame *tf) {
    /* root, or anyone sitting at the machine: members of the shrine group may close it up (the desktop's Shut down button) */
    if (me()->cred.uid != 0 && !cred_in_group(&me()->cred, GID_SHRINE)) return -EPERM;
    if (A0(tf) == REBOOT_CHECK) return 0;
    vfs_sync_all();                                                         /* never leave the shrine with unwritten records */
    if ((A0(tf) & 0xFF00) == 0x100) qemu_exit((int)(A0(tf) & 0xFF));      /* test harness: leave QEMU with a status */
    klog("Gensokyo", "%s requested by '%s'", A0(tf) == REBOOT_POWEROFF ? "poweroff" : "restart", me()->name);
    if (A0(tf) == REBOOT_POWEROFF) machine_poweroff();
    machine_reboot();
}

static long sys_mount(TrapFrame *tf) {
    if (me()->cred.uid != 0) return -EPERM;
    char src[64], dst[PATH_MAX / 4], type[16], opts[96] = "";
    int n = strncpy_from_user(src, (const char *)A0(tf), sizeof src);
    if (n < 0) return n;
    n = strncpy_from_user(dst, (const char *)A1(tf), sizeof dst);
    if (n < 0) return n;
    n = strncpy_from_user(type, (const char *)A2(tf), sizeof type);
    if (n < 0) return n;
    u32 flags = (u32)A3(tf);
    if (flags & MNT_OPTS) {                                 /* the fifth argument (r8) is an option string */
        n = strncpy_from_user(opts, (const char *)A4(tf), sizeof opts);
        if (n < 0) return n;
        flags &= ~(u32)MNT_OPTS;
    }
    return vfs_mount_opts(type, src, dst, flags, opts);
}

static long sys_umount(TrapFrame *tf) {
    if (me()->cred.uid != 0) return -EPERM;
    char dst[PATH_MAX / 4];
    int n = strncpy_from_user(dst, (const char *)A0(tf), sizeof dst);
    return n < 0 ? n : vfs_umount(dst);
}

static long sys_sync(TrapFrame *tf) {
    (void)tf;
    vfs_sync_all();
    return 0;
}

static long sys_statfs(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    struct gstatfs st;
    long rc = get_path(A0(tf), path);
    if (rc == 0) rc = vfs_statfs(path, me()->cwd, &me()->cred, &st);
    kfree(path);
    if (rc) return rc;
    return copy_to_user((void *)A1(tf), &st, sizeof st);
}

static long sys_getrandom(TrapFrame *tf) {
    size_t n = MIN(A1(tf), (u64)4096);
    u8 buf[256];
    size_t done = 0;
    while (done < n) {
        size_t c = MIN(n - done, sizeof buf);
        random_fill(buf, c);
        if (copy_to_user((u8 *)A0(tf) + done, buf, c)) return -EFAULT;
        done += c;
    }
    return (long)n;
}

/* ------------------------------------------------------------------ signals */

static long sys_sigaction(TrapFrame *tf) {
    int sig = (int)A0(tf);
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
    Process *p = me();
    if (A2(tf) && copy_to_user((void *)A2(tf), &p->sigact[sig], sizeof(struct gsigaction))) return -EFAULT;
    if (A1(tf)) {
        struct gsigaction a;
        if (copy_from_user(&a, (void *)A1(tf), sizeof a)) return -EFAULT;
        p->sigact[sig] = a;
        if (a.handler == SIG_IGN) p->sig_pending &= ~(1U << sig);
    }
    return 0;
}

static long sys_sigprocmask(TrapFrame *tf) {            /* the mask belongs to the calling thread */
    u32 old = current->sig_blocked;
    u32 set = (u32)A1(tf) & ~((1U << SIGKILL) | (1U << SIGSTOP));
    switch (A0(tf)) {
    case SIG_BLOCK: current->sig_blocked |= set; break;
    case SIG_UNBLOCK: current->sig_blocked &= ~set; break;
    case SIG_SETMASK: current->sig_blocked = set; break;
    default: return -EINVAL;
    }
    if (A2(tf) && copy_to_user((void *)A2(tf), &old, sizeof old)) return -EFAULT;
    return 0;
}

static long sys_sigreturn(TrapFrame *tf) { return sys_sigreturn_impl(tf); }

/* ------------------------------------------------------------------ dispatch */

static SyscallFn g_table[SYS_nsyscalls] = {
    [SYS_exit] = sys_exit, [SYS_read] = sys_read, [SYS_write] = sys_write, [SYS_open] = sys_open,
    [SYS_close] = sys_close, [SYS_lseek] = sys_lseek, [SYS_stat] = sys_stat, [SYS_fstat] = sys_fstat,
    [SYS_lstat] = sys_lstat, [SYS_readdir] = sys_readdir, [SYS_mkdir] = sys_mkdir, [SYS_unlink] = sys_unlink,
    [SYS_rmdir] = sys_rmdir, [SYS_rename] = sys_rename, [SYS_chdir] = sys_chdir, [SYS_getcwd] = sys_getcwd,
    [SYS_dup] = sys_dup, [SYS_dup2] = sys_dup2, [SYS_pipe] = sys_pipe, [SYS_fork] = sys_fork,
    [SYS_execve] = sys_execve, [SYS_waitpid] = sys_waitpid, [SYS_getpid] = sys_getpid, [SYS_getppid] = sys_getppid,
    [SYS_kill] = sys_kill, [SYS_brk] = sys_brk, [SYS_mmap] = sys_mmap, [SYS_munmap] = sys_munmap,
    [SYS_yield] = sys_yield, [SYS_nanosleep] = sys_nanosleep, [SYS_clock_gettime] = sys_clock_gettime,
    [SYS_ioctl] = sys_ioctl, [SYS_uname] = sys_uname, [SYS_sysinfo] = sys_sysinfo, [SYS_reboot] = sys_reboot,
    [SYS_mount] = sys_mount, [SYS_umount] = sys_umount, [SYS_sync] = sys_sync, [SYS_ftruncate] = sys_ftruncate,
    [SYS_getrandom] = sys_getrandom, [SYS_symlink] = sys_symlink, [SYS_link] = sys_link, [SYS_readlink] = sys_readlink,
    [SYS_chmod] = sys_chmod, [SYS_chown] = sys_chown, [SYS_getuid] = sys_getuid, [SYS_getgid] = sys_getgid,
    [SYS_setuid] = sys_setuid, [SYS_setgid] = sys_setgid, [SYS_umask] = sys_umask, [SYS_fcntl] = sys_fcntl,
    [SYS_poll] = sys_poll, [SYS_setpgid] = sys_setpgid, [SYS_getpgid] = sys_getpgid, [SYS_sigaction] = sys_sigaction,
    [SYS_sigreturn] = sys_sigreturn, [SYS_sigprocmask] = sys_sigprocmask, [SYS_truncate] = sys_truncate,
    [SYS_access] = sys_access, [SYS_setsid] = sys_setsid, [SYS_mprotect] = sys_mprotect, [SYS_pause] = sys_pause,
    [SYS_setname] = sys_setname, [SYS_debuglog] = sys_debuglog, [SYS_gettime_ms] = sys_gettime_ms,
    [SYS_tcsetpgrp] = sys_tcsetpgrp, [SYS_statfs] = sys_statfs,
};

void syscall_register(int nr, SyscallFn fn) {
    if (nr >= 0 && nr < SYS_nsyscalls) g_table[nr] = fn;
}

void syscall_dispatch(TrapFrame *tf) {
    sti();
    u64 nr = tf->rax;
    long ret = -ENOSYS;
    if (nr < SYS_nsyscalls && g_table[nr]) ret = g_table[nr](tf);
    if (ret == -EINTR && current->proc && signal_cancel_pending(current)) ret = -ECANCELED;      /* pthread_cancel: see signal.c */
    if (nr != SYS_sigreturn) tf->rax = (u64)ret;
    task_on_trap_return(tf);                /* reschedule if due, deliver signals */
    cli();
}

void syscall_init(void) {
    wrmsr(MSR_STAR, ((u64)0x10 << 48) | ((u64)KERNEL_CS << 32));
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    wrmsr(MSR_SFMASK, 0x47700);             /* clear IF, DF, TF, AC, NT, IOPL on entry */
}
