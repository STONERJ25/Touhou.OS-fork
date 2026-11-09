/*
 * The file-system and process calls that coreutils, a shell and the dynamic linker make beyond the first levels' set (docs/COMPAT.md, L2): statfs, fchdir, fchmod and
 * fchown and their *at forms, utimensat, linkat, the extended-attribute calls (answered "not supported"), advice and flushing hints, a few process queries and the fcntl commands
 * a shell uses. Written from the manual pages (section 2); the behaviour was checked against a real Linux through the differential tests of tests/case_linux_dyn.py.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "calls.h"
#include "numbers.h"
#include "netabi.h"

static inline Process *me(void) { return current->proc; }

/* ------------------------------------------------------------------ statfs */

/* struct statfs of x86-64 Linux (statfs(2); 120 bytes). */
struct lnx_statfs {
    s64 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    s32 f_fsid[2];
    s64 f_namelen, f_frsize, f_flags;
    s64 f_spare[4];
};
_Static_assert(sizeof(struct lnx_statfs) == 120 && __builtin_offsetof(struct lnx_statfs, f_fsid) == 56 && __builtin_offsetof(struct lnx_statfs, f_namelen) == 64,
               "struct statfs of x86-64 Linux");

#define LNX_ST_RDONLY 1

static long put_statfs(const struct gstatfs *g, u64 ubuf) {
    struct lnx_statfs l;
    memset(&l, 0, sizeof l);
    l.f_type = (s64)g->f_type;
    l.f_bsize = (s64)g->f_bsize;
    l.f_blocks = g->f_blocks;
    l.f_bfree = g->f_bfree;
    l.f_bavail = g->f_bavail;
    l.f_files = g->f_files;
    l.f_ffree = g->f_ffree;
    l.f_fsid[0] = (s32)g->f_fsid;
    l.f_namelen = (s64)g->f_namelen;
    l.f_frsize = (s64)g->f_bsize;
    l.f_flags = (g->f_flags & MNT_RDONLY) ? LNX_ST_RDONLY : 0;
    return copy_to_user((void *)ubuf, &l, sizeof l);
}

long lnx_statfs(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    struct gstatfs g;
    int n = strncpy_from_user(path, (const char *)A0(tf), PATH_MAX);
    long rc = n < 0 ? n : (n == 0 ? -ENOENT : vfs_statfs(path, me()->cwd, &me()->cred, &g));
    kfree(path);
    return rc ? rc : put_statfs(&g, A1(tf));
}

long lnx_fstatfs(TrapFrame *tf) {
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    struct gstatfs g;
    memset(&g, 0, sizeof g);
    g.f_bsize = PAGE_SIZE;
    g.f_namelen = GNAME_MAX;
    Mount *m = f->vn ? f->vn->mount : NULL;
    if (m) {
        g.f_fsid = m->id;
        g.f_flags = m->flags & MNT_RDONLY;
        if (m->type->statfs) m->type->statfs(m, &g);
    }
    file_put(f);
    return put_statfs(&g, A1(tf));
}

/* ------------------------------------------------------------------ working directory */

/* fchdir(fd): the directory the descriptor names becomes the working directory; its path (for getcwd) is the one the descriptor was opened by. */
long lnx_fchdir(TrapFrame *tf) {
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    long rc = 0;
    if (!f->vn || !S_ISDIR(f->vn->mode)) rc = -ENOTDIR;
    else if (vfs_access(f->vn, &me()->cred, X_OK)) rc = -EACCES;
    else {
        vnode_get(f->vn);
        process_set_cwd(me(), f->vn, f->path ? f->path : me()->cwd_path);
    }
    file_put(f);
    return rc;
}

/* ------------------------------------------------------------------ modes, owners, times */

static long chmod_vnode(VNode *v, u32 mode) {
    const Cred *c = &me()->cred;
    if (c->uid != 0 && c->uid != v->uid) return -EPERM;
    u32 nm = (v->mode & S_IFMT) | (mode & 07777);
    int rc = v->ops->chmod ? v->ops->chmod(v, nm) : 0;              /* a filesystem may refuse (FAT cannot store most modes) */
    if (rc == 0) { v->mode = nm; v->ctime = time_now_sec(); }
    return rc;
}

static long chown_vnode(VNode *v, u32 uid, u32 gid) {
    if (me()->cred.uid != 0) return -EPERM;
    u32 nu = uid != (u32)-1 ? uid : v->uid, ng = gid != (u32)-1 ? gid : v->gid;
    int rc = v->ops->chown ? v->ops->chown(v, nu, ng) : 0;
    if (rc == 0) { v->uid = nu; v->gid = ng; v->ctime = time_now_sec(); }
    return rc;
}

/* The file a descriptor names (fchmod, fchown, futimens): -EBADF when it is not open or has no vnode (a pipe). */
static long with_fd_vnode(int fd, long (*fn)(VNode *v, void *arg), void *arg) {
    File *f = fd_get_ref(me(), fd);
    if (!f) return -EBADF;
    long rc = f->vn ? fn(f->vn, arg) : -EINVAL;
    file_put(f);
    return rc;
}

/* The file (dirfd, path) names, followed through a final symlink unless nofollow; fn runs on it. */
static long with_path_vnode(int dirfd, u64 upath, bool nofollow, long (*fn)(VNode *v, void *arg), void *arg) {
    char *path;
    VNode *base, *v;
    File *hold;
    long rc = at_path(dirfd, upath, &path, &base, &hold);
    if (rc) return rc;
    rc = vfs_lookup(path, base, &me()->cred, nofollow ? LOOKUP_NOFOLLOW : 0, &v);
    if (rc == 0) {
        rc = fn(v, arg);
        vnode_put(v);
    }
    at_done(path, hold);
    return rc;
}

static long do_chmod(VNode *v, void *arg) { return chmod_vnode(v, *(u32 *)arg); }
static long do_chown(VNode *v, void *arg) { u32 *ids = arg; return chown_vnode(v, ids[0], ids[1]); }

long lnx_fchmod(TrapFrame *tf) { u32 mode = (u32)A1(tf); return with_fd_vnode((int)A0(tf), do_chmod, &mode); }
long lnx_fchmodat(TrapFrame *tf) { u32 mode = (u32)A2(tf); return with_path_vnode((int)A0(tf), A1(tf), false, do_chmod, &mode); }
long lnx_fchown(TrapFrame *tf) { u32 ids[2] = { (u32)A1(tf), (u32)A2(tf) }; return with_fd_vnode((int)A0(tf), do_chown, ids); }
long lnx_lchown(TrapFrame *tf) {
    u32 ids[2] = { (u32)A1(tf), (u32)A2(tf) };
    return with_path_vnode(AT_FDCWD, A0(tf), true, do_chown, ids);
}
long lnx_fchownat(TrapFrame *tf) {
    u32 ids[2] = { (u32)A2(tf), (u32)A3(tf) };
    int flags = (int)A4(tf);
    if (flags & ~(LNX_AT_SYMLINK_NOFOLLOW | LNX_AT_EMPTY_PATH)) return -EINVAL;
    if ((flags & LNX_AT_EMPTY_PATH) && A1(tf)) {                    /* fchownat(fd, "", ..., AT_EMPTY_PATH) is fchown(fd) */
        char c;
        if (copy_from_user(&c, (void *)A1(tf), 1)) return -EFAULT;
        if (!c) return with_fd_vnode((int)A0(tf), do_chown, ids);
    }
    return with_path_vnode((int)A0(tf), A1(tf), (flags & LNX_AT_SYMLINK_NOFOLLOW) != 0, do_chown, ids);
}

typedef struct { struct gtimespec ts[2]; bool have; } TimesArg;

#define LNX_UTIME_NOW  0x3fffffff
#define LNX_UTIME_OMIT 0x3ffffffe

/* The access and modification times: the caller must own the file (or be root); "now" for both only needs write permission, as in POSIX. */
static long do_times(VNode *v, void *arg) {
    const TimesArg *t = arg;
    u64 now = time_now_sec();
    const Cred *c = &me()->cred;
    bool want_now = !t->have || (t->ts[0].tv_nsec == LNX_UTIME_NOW && t->ts[1].tv_nsec == LNX_UTIME_NOW);
    long rc = 0;
    if (c->uid != 0 && c->uid != v->uid) rc = want_now ? vfs_access(v, c, W_OK) : -EPERM;
    if (rc) return rc;
    u64 old_atime = v->atime, old_mtime = v->mtime, old_ctime = v->ctime;
    if (!t->have) {
        v->atime = v->mtime = now;
    } else {
        for (int i = 0; i < 2; i++) {
            u64 *dst = i == 0 ? &v->atime : &v->mtime;
            if (t->ts[i].tv_nsec == LNX_UTIME_OMIT) continue;
            *dst = t->ts[i].tv_nsec == LNX_UTIME_NOW ? now : (t->ts[i].tv_sec < 0 ? 0 : (u64)t->ts[i].tv_sec);
        }
    }
    v->ctime = now;
    if (v->ops->settimes) {
        rc = v->ops->settimes(v, v->atime, v->mtime);
        if (rc) { v->atime = old_atime; v->mtime = old_mtime; v->ctime = old_ctime; }      /* it did not stick (a read-only volume) */
    }
    return rc;
}

/* utimensat(dirfd, path, times[2], flags); a null path means the descriptor itself (futimens). */
long lnx_utimensat(TrapFrame *tf) {
    int flags = (int)A3(tf);
    if (flags & ~LNX_AT_SYMLINK_NOFOLLOW) return -EINVAL;
    TimesArg t;
    memset(&t, 0, sizeof t);
    t.have = A2(tf) != 0;
    if (t.have) {
        if (copy_from_user(t.ts, (void *)A2(tf), sizeof t.ts)) return -EFAULT;
        for (int i = 0; i < 2; i++)
            if (t.ts[i].tv_nsec != LNX_UTIME_NOW && t.ts[i].tv_nsec != LNX_UTIME_OMIT && (t.ts[i].tv_nsec < 0 || t.ts[i].tv_nsec >= 1000000000)) return -EINVAL;
    }
    if (!A1(tf)) return with_fd_vnode((int)A0(tf), do_times, &t);
    return with_path_vnode((int)A0(tf), A1(tf), (flags & LNX_AT_SYMLINK_NOFOLLOW) != 0, do_times, &t);
}

/* linkat(olddirfd, old, newdirfd, new, flags): a hard link; both names must start from the same directory (the working directory in practice), as with renameat. */
long lnx_linkat(TrapFrame *tf) {
    if ((int)A4(tf) & ~(LNX_AT_SYMLINK_FOLLOW | LNX_AT_EMPTY_PATH)) return -EINVAL;
    char *a, *b;
    VNode *ba, *bb;
    File *ha, *hb;
    long rc = at_path((int)A0(tf), A1(tf), &a, &ba, &ha);
    if (rc) return rc;
    rc = at_path((int)A2(tf), A3(tf), &b, &bb, &hb);
    if (rc) { at_done(a, ha); return rc; }
    rc = (ba == bb || b[0] == '/') ? vfs_link(a, b, &me()->cred, ba) : -ENOSYS;
    at_done(a, ha);
    at_done(b, hb);
    return rc;
}

/* ------------------------------------------------------------------ /proc/PID/fd/N, /dev/fd/N, /dev/stdout */

/* Is abs one of the paths that name an open descriptor of a process: /proc/self/fd/N, /proc/PID/fd/N, /dev/fd/N, /dev/stdin, /dev/stdout, /dev/stderr? */
static bool fd_path(const char *abs, int *pid, int *fd) {
    const char *p = abs;
    *pid = 0;
    if (!strncmp(p, "/dev/", 5)) {
        p += 5;
        if (!strcmp(p, "stdin")) { *fd = 0; return true; }
        if (!strcmp(p, "stdout")) { *fd = 1; return true; }
        if (!strcmp(p, "stderr")) { *fd = 2; return true; }
        if (strncmp(p, "fd/", 3)) return false;
        p += 3;
    } else if (!strncmp(p, "/proc/", 6)) {
        p += 6;
        if (!strncmp(p, "self/fd/", 8)) p += 8;
        else if (!strncmp(p, "thread-self/fd/", 15)) p += 15;
        else if (*p >= '0' && *p <= '9') {
            int n = 0;
            while (*p >= '0' && *p <= '9' && n < 10000000) n = n * 10 + (*p++ - '0');
            if (strncmp(p, "/fd/", 4)) return false;
            *pid = n;
            p += 4;
        } else return false;
    } else return false;
    if (*p < '0' || *p > '9') return false;
    int n = 0;
    while (*p >= '0' && *p <= '9' && n < 1000000) n = n * 10 + (*p++ - '0');
    if (*p) return false;
    *fd = n;
    return true;
}

/* open() of such a path gives a new descriptor for the open file itself, whatever it is (a pipe, a terminal, a socket, a file whose name is gone). Reading the link and opening the
 * name does not do that: a pipe has no name. The new descriptor shares the file position with the old one (a real Linux makes a new open file for a regular file; for a pipe or a
 * terminal there is no difference). -ENOSYS means: not a path of that kind. */
long lnx_open_via_proc(const char *abs, int flags) {
    int pid, fd;
    if (!fd_path(abs, &pid, &fd)) return -ENOSYS;
    Process *self = me(), *t = pid ? process_by_pid(pid) : self;
    if (!t || t->state == PROC_ZOMBIE) return -ENOENT;
    if (t != self && self->cred.uid != 0 && t->cred.uid != self->cred.uid) return -EACCES;      /* /proc/PID/fd is for the owner and root */
    u64 fl = irq_save();
    File *f = fd >= 0 && fd < MAX_FD ? t->fds[fd] : NULL;
    if (f) file_get(f);
    irq_restore(fl);
    if (!f) return -ENOENT;
    long n = fd_install(self, f, 0, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);        /* the table takes over the reference */
    if (n < 0) file_put(f);
    return n;
}

/* ------------------------------------------------------------------ things that need no work, and things that are not supported */

/* The extended-attribute calls (getxattr ... flistxattr): this system has no attributes to give, which is what "not supported" says. ls, cp and mv ask and carry on. */
long lnx_noxattr(TrapFrame *tf) { (void)tf; return -ENOTSUP; }

/* posix_fadvise and msync are hints: the page cache and the write-back do not exist, so there is nothing to advise or flush. */
long lnx_hint(TrapFrame *tf) { (void)tf; return 0; }

long lnx_getpgrp(TrapFrame *tf) { (void)tf; return me()->pgid; }

long lnx_getsid(TrapFrame *tf) {
    if (A0(tf) > 0x7fffffffUL) return -ESRCH;
    Process *t = A0(tf) ? process_by_pid((int)A0(tf)) : me();
    return t && t->state != PROC_ZOMBIE ? t->sid : -ESRCH;
}

/* ------------------------------------------------------------------ fcntl */

#define LNX_F_GETLK  5
#define LNX_F_SETLK  6
#define LNX_F_SETLKW 7
#define LNX_F_UNLCK  2

/* fcntl(fd, cmd, arg): the native commands (duplicate, descriptor flags, status flags) and what a shell adds: a close-on-exec duplicate, and the advisory record locks, which
 * always succeed here (nothing else can compete for a lock on a system whose programs share no files across machines). */
long lnx_fcntl(TrapFrame *tf) {
    int fd = (int)A0(tf);
    switch ((int)A1(tf)) {
    case LNX_F_DUPFD_CLOEXEC: {
        File *f = fd_get_ref(me(), fd);
        if (!f) return -EBADF;
        long n = fd_install(me(), f, (int)A2(tf), FD_CLOEXEC);      /* the descriptor table takes over the reference */
        if (n < 0) file_put(f);
        return n;
    }
    case LNX_F_GETLK: {
        if (!fd_get(me(), fd)) return -EBADF;
        short unlocked = LNX_F_UNLCK;
        return copy_to_user((void *)A2(tf), &unlocked, sizeof unlocked);        /* l_type is the first member of struct flock */
    }
    case LNX_F_SETLK: case LNX_F_SETLKW:
        return fd_get(me(), fd) ? 0 : -EBADF;
    case LNX_F_GETPIPE_SZ: case LNX_F_SETPIPE_SZ: {                 /* a pipe holds 16 KiB and its size cannot change; anything else is not a pipe */
        File *f = fd_get(me(), fd);
        return f && f->vn && S_ISFIFO(f->vn->mode) ? 16384 : -EBADF;
    }
    default: {
        SyscallFn f = syscall_native(SYS_fcntl);
        return f ? f(tf) : -ENOSYS;
    }
    }
}
