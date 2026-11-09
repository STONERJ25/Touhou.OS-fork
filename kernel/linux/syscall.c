/*
 * The Linux system call table (docs/COMPAT.md). A process in the ABI_LINUX personality puts a Linux x86-64 call number in rax and the arguments in rdi, rsi, rdx,
 * r10, r8, r9 (the same registers as the native calls); the result is the return value or a negative errno. Native errno values, open flags, mmap flags,
 * signal numbers up to 31 and the ioctl request numbers of the terminal are already the same as Linux's, so a call that means exactly what a native call
 * means is routed to it (NATIVE below); every call whose numbers, structures or semantics differ has a handler of its own (OWN).
 *
 * Written from the manual pages (section 2) and the psABI, checked against a real Linux by differential tests (tests/case_linux.py, tests/host/test_linux_abi.py).
 * Not implemented yet: 32-bit programs, inotify, io_uring, signalfd and the other calls that docs/COMPAT.md lists as missing. A call with no entry fails with
 * ENOSYS and is named once in the kernel log ("not implemented"); "linux.trace" on the kernel command line logs every call.
 */
#include "linux.h"
#include "kernel.h"
#include "cpu.h"
#include "cmdline.h"
#include "initcall.h"
#include "printk.h"
#include "ktime.h"
#include "irq.h"
#include "numbers.h"
#include "calls.h"

typedef struct LinuxCall {
    SyscallFn fn;               /* a handler of its own, or NULL */
    s16       native;           /* else the native call with the same meaning, plus one (0: none) */
} LinuxCall;

#define OWN(f)      { .fn = (f) }
#define NATIVE(n)   { .native = (s16)((n) + 1) }

#define A0(tf) ((tf)->rdi)
#define A1(tf) ((tf)->rsi)
#define A2(tf) ((tf)->rdx)
#define A3(tf) ((tf)->r10)
#define A4(tf) ((tf)->r8)
#define A5(tf) ((tf)->r9)

#define LINUX_RELEASE "6.1.0-gensokyo"
#define TAG "Outside World"                     /* the log tag of the layer that speaks for programs from outside Gensokyo */

static inline Process *me(void) { return current->proc; }

/* ------------------------------------------------------------------ process and thread bookkeeping */

/* arch_prctl(2): only the FS base is supported (it is where the C library keeps the thread control block). */
static long lnx_arch_prctl(TrapFrame *tf) {
    u64 arg = A1(tf);
    switch (A0(tf)) {
    case LNX_ARCH_SET_FS:
        if (arg >= USER_TOP) return -EPERM;
        task_set_fs_base(current, arg);
        return 0;
    case LNX_ARCH_GET_FS:
        return copy_to_user((void *)arg, &current->fs_base, sizeof(u64));
    default:
        return -EINVAL;
    }
}

/* set_tid_address(2): the word at the address is zeroed and woken (futex) when this thread exits, which is what the native threads already do with clear_tid. */
static long lnx_set_tid_address(TrapFrame *tf) {
    current->clear_tid = A0(tf);
    return current->utid;
}

/* The robust futex list and restartable sequences are optional kernel services: a C library copes when they answer ENOSYS, and says nothing. */
static long lnx_unsupported(TrapFrame *tf) { (void)tf; return -ENOSYS; }

/* ------------------------------------------------------------------ resource limits */

struct lnx_rlimit { u64 cur, max; };

static bool rlimit_of(int res, struct lnx_rlimit *r) {
    r->cur = r->max = LNX_RLIM_INFINITY;
    switch (res) {
    case LNX_RLIMIT_STACK:  r->cur = USER_STACK_MAX; break;
    case LNX_RLIMIT_CORE:   r->cur = 0; break;
    case LNX_RLIMIT_NOFILE: r->cur = r->max = MAX_FD; break;
    case LNX_RLIMIT_NPROC:  r->cur = r->max = MAX_PROCS; break;
    case LNX_RLIMIT_MEMLOCK: r->cur = r->max = 64 * 1024; break;
    case LNX_RLIMIT_SIGPENDING: r->cur = r->max = SIGQ_PER_PROC; break;         /* queued signals per process (kernel/sys/signal.c) */
    case LNX_RLIMIT_NICE: case LNX_RLIMIT_RTPRIO: r->cur = r->max = 0; break;
    default: return res >= 0 && res < LNX_RLIM_NLIMITS;
    }
    return true;
}

/* prlimit64(pid, resource, new, old): the limits are reported as this system has them (the stack, the descriptor table) and a new value is accepted and not enforced. */
static long lnx_prlimit64(TrapFrame *tf) {
    int pid = (int)A0(tf);
    struct lnx_rlimit cur, nw;
    if (pid != 0 && pid != me()->pid) return -ESRCH;
    if (!rlimit_of((int)A1(tf), &cur)) return -EINVAL;
    if (A3(tf) && copy_to_user((void *)A3(tf), &cur, sizeof cur)) return -EFAULT;
    if (A2(tf) && copy_from_user(&nw, (void *)A2(tf), sizeof nw)) return -EFAULT;
    if (A2(tf) && nw.cur > nw.max) return -EINVAL;
    return 0;
}

static long lnx_getrlimit(TrapFrame *tf) {
    struct lnx_rlimit cur;
    if (!rlimit_of((int)A0(tf), &cur)) return -EINVAL;
    return copy_to_user((void *)A1(tf), &cur, sizeof cur);
}

static long lnx_setrlimit(TrapFrame *tf) {
    struct lnx_rlimit cur, nw;
    if (!rlimit_of((int)A0(tf), &cur)) return -EINVAL;
    if (copy_from_user(&nw, (void *)A1(tf), sizeof nw)) return -EFAULT;
    return nw.cur > nw.max ? -EINVAL : 0;
}

/* ------------------------------------------------------------------ system information */

/* struct sysinfo of x86-64 Linux (sysinfo(2); 112 bytes). Sizes are in units of mem_unit (1 byte here); there is no swap and no load average. */
struct lnx_sysinfo {
    s64 uptime;
    u64 loads[3];
    u64 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    u16 procs, pad;
    u32 pad2;
    u64 totalhigh, freehigh;
    u32 mem_unit, pad3;
};
_Static_assert(sizeof(struct lnx_sysinfo) == 112 && __builtin_offsetof(struct lnx_sysinfo, procs) == 80 && __builtin_offsetof(struct lnx_sysinfo, mem_unit) == 104,
               "struct sysinfo of x86-64 Linux");

static long lnx_sysinfo(TrapFrame *tf) {
    struct lnx_sysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime = (s64)(uptime_ms() / 1000);
    si.totalram = pmm_total_pages() * PAGE_SIZE;
    si.freeram = pmm_free_pages_count() * PAGE_SIZE;
    si.procs = (u16)process_count();
    si.mem_unit = 1;
    return copy_to_user((void *)A0(tf), &si, sizeof si);
}

/* getrandom(buf, n, flags): the native call fills the buffer; the flags (GRND_NONBLOCK, GRND_RANDOM, GRND_INSECURE) change nothing here but unknown ones are an error. */
static long lnx_getrandom(TrapFrame *tf) {
    if ((u32)A2(tf) & ~7u) return -EINVAL;
    SyscallFn f = syscall_native(SYS_getrandom);
    return f ? f(tf) : -ENOSYS;
}

/* sched_getaffinity(pid, size, mask): the kernel runs one processor, so the mask is CPU 0; the length returned is the number of bytes stored. */
static long lnx_sched_getaffinity(TrapFrame *tf) {
    if (A1(tf) < 8 || (A1(tf) & 7)) return -EINVAL;
    u64 mask = 1;
    if (copy_to_user((void *)A2(tf), &mask, sizeof mask)) return -EFAULT;
    return 8;
}

/* prctl(2): the process name (what ps shows) and a few switches programs set and never look at again; anything else is EINVAL. */
#define LNX_PR_SET_PDEATHSIG 1
#define LNX_PR_GET_PDEATHSIG 2
#define LNX_PR_GET_DUMPABLE  3
#define LNX_PR_SET_DUMPABLE  4
#define LNX_PR_SET_NAME      15
#define LNX_PR_GET_NAME      16
#define LNX_PR_SET_NO_NEW_PRIVS 38
#define LNX_PR_GET_NO_NEW_PRIVS 39

static long lnx_prctl(TrapFrame *tf) {
    switch (A0(tf)) {
    case LNX_PR_SET_NAME: {
        char name[16];
        size_t i = 0;
        for (; i < 15; i++) {
            if (copy_from_user(&name[i], (const char *)A1(tf) + i, 1)) return -EFAULT;
            if (!name[i]) break;
        }
        name[i] = 0;
        strlcpy(me()->name, name, sizeof me()->name);
        strlcpy(current->name, name, sizeof current->name);
        return 0;
    }
    case LNX_PR_GET_NAME: {
        char name[16];
        memset(name, 0, sizeof name);
        strlcpy(name, me()->name, sizeof name);
        return copy_to_user((void *)A1(tf), name, sizeof name);
    }
    case LNX_PR_GET_PDEATHSIG: {
        int none = 0;
        return copy_to_user((void *)A1(tf), &none, sizeof none);
    }
    case LNX_PR_SET_PDEATHSIG: case LNX_PR_SET_DUMPABLE: case LNX_PR_SET_NO_NEW_PRIVS:
        return 0;
    case LNX_PR_GET_DUMPABLE:
        return 1;
    case LNX_PR_GET_NO_NEW_PRIVS:
        return 0;
    default:
        return -EINVAL;
    }
}

/* ------------------------------------------------------------------ uname */

struct lnx_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

static long lnx_uname(TrapFrame *tf) {
    struct lnx_utsname u;
    memset(&u, 0, sizeof u);
    strlcpy(u.sysname, "Linux", sizeof u.sysname);
    strlcpy(u.nodename, "gensokyo", sizeof u.nodename);
    strlcpy(u.release, LINUX_RELEASE, sizeof u.release);
    strlcpy(u.version, "#1 TouhouOS Linux personality", sizeof u.version);
    strlcpy(u.machine, "x86_64", sizeof u.machine);
    strlcpy(u.domainname, "(none)", sizeof u.domainname);
    return copy_to_user((void *)A0(tf), &u, sizeof u);
}

/* ------------------------------------------------------------------ stat */

/* struct stat of x86-64 Linux (stat(2); 144 bytes). The native structure has other field order and only whole seconds. */
struct lnx_stat {
    u64 st_dev, st_ino, st_nlink;
    u32 st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    s64 st_size, st_blksize, st_blocks;
    s64 st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    s64 unused[3];
};
_Static_assert(sizeof(struct lnx_stat) == 144, "struct stat of x86-64 Linux is 144 bytes");
_Static_assert(__builtin_offsetof(struct lnx_stat, st_rdev) == 40 && __builtin_offsetof(struct lnx_stat, st_size) == 48 &&
               __builtin_offsetof(struct lnx_stat, st_atime) == 72 && __builtin_offsetof(struct lnx_stat, st_mtime) == 88 &&
               __builtin_offsetof(struct lnx_stat, st_ctime) == 104, "stat(2) field offsets");

static long put_stat(const struct gstat *g, u64 ubuf) {
    struct lnx_stat l;
    memset(&l, 0, sizeof l);
    l.st_dev = g->st_dev;
    l.st_ino = g->st_ino;
    l.st_nlink = g->st_nlink;
    l.st_mode = g->st_mode;
    l.st_uid = g->st_uid;
    l.st_gid = g->st_gid;
    l.st_rdev = g->st_rdev;
    l.st_size = g->st_size;
    l.st_blksize = (s64)g->st_blksize;
    l.st_blocks = (s64)g->st_blocks;
    l.st_atime = g->st_atime;
    l.st_mtime = g->st_mtime;
    l.st_ctime = g->st_ctime;
    return copy_to_user((void *)ubuf, &l, sizeof l);
}

static void stat_of_file(File *f, struct gstat *st) {
    if (f->vn) vnode_stat(f->vn, st);
    else { memset(st, 0, sizeof *st); st->st_mode = S_IFIFO | 0600; }           /* a pipe or a socket has no vnode */
}

/* The directory an *at() call starts from: the working directory for AT_FDCWD (and for any absolute path, whatever dirfd says), else an open directory. */
static long at_base(int dirfd, const char *path, VNode **base, File **hold) {
    *hold = NULL;
    *base = me()->cwd;
    if (path[0] == '/' || dirfd == AT_FDCWD) return 0;
    File *f = fd_get_ref(me(), dirfd);
    if (!f) return -EBADF;
    if (!f->vn || !S_ISDIR(f->vn->mode)) { file_put(f); return -ENOTDIR; }
    *base = f->vn;
    *hold = f;
    return 0;
}

/* The common part of newfstatat and statx: finds the file named by (dirfd, path, flags) and fills *st (an empty path with AT_EMPTY_PATH means dirfd itself). */
static long stat_at(int dirfd, u64 upath, int flags, struct gstat *st) {
    if (flags & ~(LNX_AT_SYMLINK_NOFOLLOW | LNX_AT_EMPTY_PATH | LNX_AT_NO_AUTOMOUNT)) return -EINVAL;
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    long rc;
    int n = strncpy_from_user(path, (const char *)upath, PATH_MAX);
    if (n < 0) {
        rc = n;
    } else if (n == 0) {
        if (!(flags & LNX_AT_EMPTY_PATH)) {
            rc = -ENOENT;
        } else if (dirfd == AT_FDCWD) {
            vnode_stat(me()->cwd, st);
            rc = 0;
        } else {
            File *f = fd_get_ref(me(), dirfd);
            if (!f) { rc = -EBADF; }
            else { stat_of_file(f, st); file_put(f); rc = 0; }
        }
    } else {
        VNode *base;
        File *hold;
        rc = at_base(dirfd, path, &base, &hold);
        if (!rc) rc = vfs_stat(path, base, &me()->cred, (flags & LNX_AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0, st);
        if (hold) file_put(hold);
    }
    kfree(path);
    return rc;
}

static long do_fstatat(int dirfd, u64 upath, u64 ubuf, int flags) {
    struct gstat st;
    long rc = stat_at(dirfd, upath, flags, &st);
    return rc ? rc : put_stat(&st, ubuf);
}

/* struct statx (statx(2); 256 bytes): the extended stat that current C libraries and coreutils use for stat, ls and friends. */
struct lnx_statx_ts { s64 tv_sec; u32 tv_nsec; s32 pad; };
struct lnx_statx {
    u32 stx_mask, stx_blksize;
    u64 stx_attributes;
    u32 stx_nlink, stx_uid, stx_gid;
    u16 stx_mode, pad1;
    u64 stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct lnx_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    u32 stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    u64 stx_mnt_id;
    u32 stx_dio_mem_align, stx_dio_offset_align;
    u64 spare[12];
};
_Static_assert(sizeof(struct lnx_statx) == 256, "struct statx is 256 bytes");

#define LNX_STATX_BASIC_STATS 0x7ffu            /* type, mode, nlink, uid, gid, atime, mtime, ctime, ino, size, blocks */
#define LNX_AT_STATX_SYNC_TYPE 0x6000           /* how fresh the answer must be: irrelevant to a local filesystem */

/* statx(dirfd, path, flags, mask, buf): the basic fields only, with whole-second times; the birth time and the attributes are not kept. */
static long lnx_statx(TrapFrame *tf) {
    struct gstat st;
    long rc = stat_at((int)A0(tf), A1(tf), (int)A2(tf) & ~LNX_AT_STATX_SYNC_TYPE, &st);
    if (rc) return rc;
    struct lnx_statx x;
    memset(&x, 0, sizeof x);
    x.stx_mask = LNX_STATX_BASIC_STATS;
    x.stx_blksize = (u32)st.st_blksize;
    x.stx_nlink = st.st_nlink;
    x.stx_uid = st.st_uid;
    x.stx_gid = st.st_gid;
    x.stx_mode = (u16)st.st_mode;
    x.stx_ino = st.st_ino;
    x.stx_size = (u64)st.st_size;
    x.stx_blocks = st.st_blocks;
    x.stx_atime.tv_sec = st.st_atime;
    x.stx_mtime.tv_sec = st.st_mtime;
    x.stx_ctime.tv_sec = st.st_ctime;
    x.stx_rdev_major = (u32)((st.st_rdev >> 8) & 0xfff);
    x.stx_rdev_minor = (u32)(st.st_rdev & 0xff);
    x.stx_dev_major = (u32)((st.st_dev >> 8) & 0xfff);
    x.stx_dev_minor = (u32)(st.st_dev & 0xff);
    return copy_to_user((void *)A4(tf), &x, sizeof x);
}

static long lnx_newfstatat(TrapFrame *tf) { return do_fstatat((int)A0(tf), A1(tf), A2(tf), (int)A3(tf)); }
static long lnx_stat(TrapFrame *tf) { return do_fstatat(AT_FDCWD, A0(tf), A1(tf), 0); }
static long lnx_lstat(TrapFrame *tf) { return do_fstatat(AT_FDCWD, A0(tf), A1(tf), LNX_AT_SYMLINK_NOFOLLOW); }

static long lnx_fstat(TrapFrame *tf) {
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    struct gstat st;
    stat_of_file(f, &st);
    file_put(f);
    return put_stat(&st, A1(tf));
}

/* readlinkat(dirfd, path, buf, size): the target is not NUL-terminated and is cut to size; the result is the length stored. */
static long lnx_readlinkat(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX), *out = kmalloc(PATH_MAX);
    long rc = -ENOMEM;
    if (path && out) {
        int n = strncpy_from_user(path, (const char *)A1(tf), PATH_MAX);
        VNode *base;
        File *hold = NULL;
        rc = n < 0 ? n : (n == 0 ? -ENOENT : at_base((int)A0(tf), path, &base, &hold));
        if (!rc) {
            ssize_t got = vfs_readlink(path, out, MIN(A3(tf), (u64)PATH_MAX), base, &me()->cred);
            rc = got < 0 ? got : (copy_to_user((void *)A2(tf), out, (size_t)got) ? -EFAULT : got);
        }
        if (hold) file_put(hold);
    }
    kfree(path);
    kfree(out);
    return rc;
}

/* ------------------------------------------------------------------ the rest of the *at() family */

/* Copies the path argument of an *at() call and finds the directory it starts from. On success *path is a buffer the caller frees with kfree, and *hold is
 * NULL or a reference the caller releases with file_put once *base is no longer needed. An empty path is ENOENT. */
long at_path(int dirfd, u64 upath, char **path, VNode **base, File **hold) {
    *path = kmalloc(PATH_MAX);
    if (!*path) return -ENOMEM;
    int n = strncpy_from_user(*path, (const char *)upath, PATH_MAX);
    long rc = n < 0 ? n : (n == 0 ? -ENOENT : at_base(dirfd, *path, base, hold));
    if (rc) { kfree(*path); *path = NULL; }
    return rc;
}

void at_done(char *path, File *hold) {
    kfree(path);
    if (hold) file_put(hold);
}

/* openat(dirfd, path, flags, mode): open(2) relative to a directory descriptor. */
static long lnx_openat(TrapFrame *tf) {
    char *path;
    VNode *base;
    File *hold, *f;
    long rc = at_path((int)A0(tf), A1(tf), &path, &base, &hold);
    if (rc) return rc;
    Process *p = me();
    int flags = (int)A2(tf);
    if (path[0] == '/' && (!strncmp(path, "/proc/", 6) || !strncmp(path, "/dev/", 5))) {         /* /dev/stdout, /proc/self/fd/N, /dev/fd/N: the open file itself */
        char abs0[256];
        path_normalize("/", path, abs0, sizeof abs0);
        long m = lnx_open_via_proc(abs0, flags);
        if (m != -ENOSYS) { at_done(path, hold); return m; }
    }
    rc = vfs_open(path, flags, (u32)A3(tf) & ~p->umask, &p->cred, base, &f);
    if (rc == 0) {
        char abs[256];
        path_normalize(hold && hold->path ? hold->path : p->cwd_path, path, abs, sizeof abs);        /* /proc/<pid>/fd/N shows this */
        file_set_path(f, abs);
        rc = fd_install(p, f, 0, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
        if (rc < 0) file_put(f);
    }
    at_done(path, hold);
    return rc;
}

/* open(path, flags, mode) is openat(AT_FDCWD, ...): one code path, so that the paths of an open file (/dev/stdout, /proc/self/fd/N) behave the same for old and new C libraries. */
static long lnx_open(TrapFrame *tf) {
    TrapFrame t = *tf;
    t.rdi = (u64)(long)AT_FDCWD;
    t.rsi = A0(tf);
    t.rdx = A1(tf);
    t.r10 = A2(tf);
    return lnx_openat(&t);
}

/* creat(path, mode) is open(path, O_CREAT | O_WRONLY | O_TRUNC, mode). */
static long lnx_creat(TrapFrame *tf) {
    TrapFrame t = *tf;
    t.rdi = (u64)(long)AT_FDCWD;
    t.rsi = A0(tf);
    t.rdx = O_CREAT | O_WRONLY | O_TRUNC;
    t.r10 = A1(tf);
    return lnx_openat(&t);
}

static long lnx_mkdirat(TrapFrame *tf) {
    char *path;
    VNode *base;
    File *hold;
    long rc = at_path((int)A0(tf), A1(tf), &path, &base, &hold);
    if (rc) return rc;
    rc = vfs_mkdir(path, (u32)A2(tf) & 07777 & ~me()->umask, &me()->cred, base);
    at_done(path, hold);
    return rc;
}

/* unlinkat(dirfd, path, flags): unlink(2), or rmdir(2) with AT_REMOVEDIR. */
static long lnx_unlinkat(TrapFrame *tf) {
    int flags = (int)A2(tf);
    if (flags & ~LNX_AT_REMOVEDIR) return -EINVAL;
    char *path;
    VNode *base;
    File *hold;
    long rc = at_path((int)A0(tf), A1(tf), &path, &base, &hold);
    if (rc) return rc;
    rc = (flags & LNX_AT_REMOVEDIR) ? vfs_rmdir(path, &me()->cred, base) : vfs_unlink(path, &me()->cred, base);
    at_done(path, hold);
    return rc;
}

/* renameat(olddirfd, old, newdirfd, new): both names must start from the same directory (the working directory in practice); renameat2 only with no flags. */
static long do_renameat(int olddirfd, u64 uold, int newdirfd, u64 unew) {
    char *a, *b;
    VNode *ba, *bb;
    File *ha, *hb;
    long rc = at_path(olddirfd, uold, &a, &ba, &ha);
    if (rc) return rc;
    rc = at_path(newdirfd, unew, &b, &bb, &hb);
    if (rc) { at_done(a, ha); return rc; }
    rc = (ba == bb || b[0] == '/') ? vfs_rename(a, b, &me()->cred, ba) : -ENOSYS;
    at_done(a, ha);
    at_done(b, hb);
    return rc;
}

static long lnx_renameat(TrapFrame *tf) { return do_renameat((int)A0(tf), A1(tf), (int)A2(tf), A3(tf)); }
static long lnx_renameat2(TrapFrame *tf) { return A4(tf) ? -EINVAL : do_renameat((int)A0(tf), A1(tf), (int)A2(tf), A3(tf)); }

static long lnx_symlinkat(TrapFrame *tf) {
    char *target = kmalloc(PATH_MAX);
    char *path;
    VNode *base;
    File *hold;
    if (!target) return -ENOMEM;
    int n = strncpy_from_user(target, (const char *)A0(tf), PATH_MAX);
    long rc = n < 0 ? n : (n == 0 ? -ENOENT : at_path((int)A1(tf), A2(tf), &path, &base, &hold));
    if (!rc) {
        rc = vfs_symlink(target, path, &me()->cred, base);
        at_done(path, hold);
    }
    kfree(target);
    return rc;
}

/* faccessat(dirfd, path, mode, flags): access(2) relative to a directory. The permission test uses the effective ids (AT_EACCESS is the only behaviour here). */
static long do_faccessat(int dirfd, u64 upath, int mode, int flags) {
    if ((mode & ~7) || (flags & ~(LNX_AT_EACCESS | LNX_AT_SYMLINK_NOFOLLOW))) return -EINVAL;
    char *path;
    VNode *base, *v;
    File *hold;
    long rc = at_path(dirfd, upath, &path, &base, &hold);
    if (rc) return rc;
    rc = vfs_lookup(path, base, &me()->cred, (flags & LNX_AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW : 0, &v);
    if (rc == 0) {
        rc = mode ? vfs_access(v, &me()->cred, mode) : 0;
        vnode_put(v);
    }
    at_done(path, hold);
    return rc;
}

static long lnx_faccessat(TrapFrame *tf) { return do_faccessat((int)A0(tf), A1(tf), (int)A2(tf), 0); }
static long lnx_faccessat2(TrapFrame *tf) { return do_faccessat((int)A0(tf), A1(tf), (int)A2(tf), (int)A3(tf)); }

/* pread64 and pwrite64: the ordinary read or write at an explicit offset that leaves the file position alone. Only files that have positions. */
static long do_pio(TrapFrame *tf, int native_nr) {
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    if (!f->vn || !S_ISREG(f->vn->mode)) { file_put(f); return -ESPIPE; }
    if ((s64)A3(tf) < 0) { file_put(f); return -EINVAL; }
    SyscallFn one = syscall_native(native_nr);
    u64 saved = f->off;
    f->off = A3(tf);
    long r = one ? one(tf) : -ENOSYS;
    f->off = saved;
    file_put(f);
    return r;
}

static long lnx_pread64(TrapFrame *tf) { return do_pio(tf, SYS_read); }
static long lnx_pwrite64(TrapFrame *tf) { return do_pio(tf, SYS_write); }

/* ------------------------------------------------------------------ directories */

/* struct linux_dirent64 (getdents(2)): the name follows the 19-byte header, and every record is padded to a multiple of 8. */
struct lnx_dirent64 {
    u64  d_ino;
    s64  d_off;             /* the position of the next entry: a cookie for seekdir */
    u16  d_reclen;
    u8   d_type;
    char d_name[];
};
_Static_assert(__builtin_offsetof(struct lnx_dirent64, d_name) == 19, "linux_dirent64 header");

/* getdents64(fd, buf, count): as many whole records as fit, 0 at the end of the directory; EINVAL when not even one fits. */
static long lnx_getdents64(TrapFrame *tf) {
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    u64 count = MIN(A2(tf), (u64)65536);
    u8 *kbuf = kmalloc(count ? count : 1);
    if (!kbuf) { file_put(f); return -ENOMEM; }
    u64 pos = 0;
    long err = 0;
    struct gdirent de;
    for (;;) {
        u64 before = f->off;
        int r = vfs_readdir(f, &de);
        if (r < 0) { err = r; break; }
        if (r == 0) break;
        u64 reclen = ALIGN_UP(sizeof(struct lnx_dirent64) + de.d_namlen + 1, 8);
        if (pos + reclen > count) {                 /* does not fit: it stays for the next call */
            f->off = before;
            if (!pos) err = -EINVAL;
            break;
        }
        struct lnx_dirent64 *d = (struct lnx_dirent64 *)(kbuf + pos);
        memset(d, 0, reclen);
        d->d_ino = de.d_ino;
        d->d_off = (s64)f->off;
        d->d_reclen = (u16)reclen;
        d->d_type = (u8)de.d_type;
        memcpy(d->d_name, de.d_name, de.d_namlen);
        pos += reclen;
    }
    long rc = (err && !pos) ? err : (pos && copy_to_user((void *)A1(tf), kbuf, pos)) ? -EFAULT : (long)pos;
    kfree(kbuf);
    file_put(f);
    return rc;
}

/* ------------------------------------------------------------------ processes and descriptors */

/* clone(flags, stack, parent_tid, child_tid, tls): the forms a C library's fork() and vfork-style spawning use. The child gets a copy of the address space (a
 * CLONE_VM child shares it on a real Linux; here it does not, which only matters to a program that passes data back through memory before the exec).
 * Threads (CLONE_THREAD) are not implemented yet. */
static long clone_thread(TrapFrame *tf, u64 flags) {
    const u64 base = LNX_CLONE_VM | LNX_CLONE_FS | LNX_CLONE_FILES | LNX_CLONE_SIGHAND | LNX_CLONE_THREAD | LNX_CLONE_SYSVSEM;      /* what pthread_create passes */
    const u64 optional = LNX_CLONE_SETTLS | LNX_CLONE_PARENT_SETTID | LNX_CLONE_CHILD_CLEARTID | LNX_CLONE_CHILD_SETTID;
    if ((flags & base) != base) return -EINVAL;
    if (flags & ~(base | optional)) return -ENOSYS;
    if (!A1(tf)) return -EINVAL;                    /* a thread runs on a stack of its own */
    Process *p = me();
    if (p->nthreads >= THREADS_PER_PROC_MAX || thread_count() >= THREADS_TOTAL_MAX) return -EAGAIN;
    int utid = process_alloc_id();
    if ((flags & LNX_CLONE_PARENT_SETTID) && A2(tf) && copy_to_user((void *)A2(tf), &utid, sizeof utid)) return -EFAULT;
    if ((flags & LNX_CLONE_CHILD_SETTID) && A3(tf) && copy_to_user((void *)A3(tf), &utid, sizeof utid)) return -EFAULT;     /* the memory is shared: the child sees it at once */
    TrapFrame child = *tf;
    child.rsp = A1(tf);
    ThreadInit ti = { .utid = utid, .fs_base = (flags & LNX_CLONE_SETTLS) ? A4(tf) : current->fs_base,
                      .clear_tid = (flags & LNX_CLONE_CHILD_CLEARTID) ? A3(tf) : 0, .sig_blocked = current->sig_blocked };
    Task *t = task_create_thread(p->name, &child, p, &ti);
    return t ? utid : -EAGAIN;
}

static long lnx_clone(TrapFrame *tf) {
    u64 flags = A0(tf);
    if (flags & LNX_CLONE_THREAD) return clone_thread(tf, flags);
    if ((flags & LNX_CSIGNAL) != SIGCHLD) return -ENOSYS;
    if (flags & ~(u64)(LNX_CSIGNAL | LNX_CLONE_VM | LNX_CLONE_VFORK | LNX_CLONE_PARENT_SETTID | LNX_CLONE_CHILD_SETTID | LNX_CLONE_CHILD_CLEARTID)) return -ENOSYS;
    TrapFrame child = *tf;
    if (A1(tf)) child.rsp = A1(tf);
    long pid = process_fork_ex(&child, (flags & LNX_CLONE_CHILD_SETTID) ? A3(tf) : 0, (flags & LNX_CLONE_CHILD_CLEARTID) ? A3(tf) : 0);
    if (pid > 0 && (flags & LNX_CLONE_PARENT_SETTID) && A2(tf)) {
        u32 v = (u32)pid;
        copy_to_user((void *)A2(tf), &v, sizeof v);
    }
    return pid;
}

/* exit(status) ends the calling thread; the last thread out ends the process with that status. exit_group ends them all. */
static long lnx_exit(TrapFrame *tf) {
    SyscallFn f = syscall_native(me()->nthreads > 1 ? SYS_thread_exit : SYS_exit);
    return f ? f(tf) : -ENOSYS;
}

/* futex(uaddr, op, val, timeout, uaddr2, val3): the operations a C library's locks, condition variables and thread joins use. The private and clock flags are
 * folded away (every futex here is looked up by physical address); WAIT takes a relative timeout, WAIT_BITSET an absolute one on the monotonic or the real-time clock. */
static long futex_timeout(u64 utimeout, bool absolute, bool realtime, u64 *deadline) {
    *deadline = ~0ULL;
    if (!utimeout) return 0;
    struct { s64 sec, nsec; } ts;
    if (copy_from_user(&ts, (void *)utimeout, sizeof ts)) return -EFAULT;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000) return -EINVAL;
    u64 ms = (u64)ts.sec * 1000 + (u64)ts.nsec / 1000000 + ((ts.nsec % 1000000) ? 1 : 0);
    if (!absolute) { *deadline = uptime_ms() + ms; return 0; }
    TrapFrame none;
    memset(&none, 0, sizeof none);
    SyscallFn wall = syscall_native(SYS_gettime_ms);
    u64 now = realtime ? (u64)(wall ? wall(&none) : 0) : uptime_ms();
    if (ms <= now) return -ETIMEDOUT;
    *deadline = uptime_ms() + (ms - now);
    return 0;
}

static long lnx_futex(TrapFrame *tf) {
    u64 uaddr = A0(tf), deadline;
    int op = (int)A1(tf), cmd = op & LNX_FUTEX_CMD_MASK;
    bool realtime = (op & LNX_FUTEX_CLOCK_REALTIME) != 0;
    long rc;
    switch (cmd) {
    case LNX_FUTEX_WAIT:
        rc = futex_timeout(A3(tf), false, realtime, &deadline);
        return rc ? rc : futex_wait_deadline(uaddr, (u32)A2(tf), deadline);
    case LNX_FUTEX_WAIT_BITSET:
        if (!(u32)A5(tf)) return -EINVAL;
        rc = futex_timeout(A3(tf), true, realtime, &deadline);
        return rc ? rc : futex_wait_deadline(uaddr, (u32)A2(tf), deadline);
    case LNX_FUTEX_WAKE:
        return futex_wake_addr(uaddr, (int)A2(tf));
    case LNX_FUTEX_WAKE_BITSET:
        return (u32)A5(tf) ? futex_wake_addr(uaddr, (int)A2(tf)) : -EINVAL;
    case LNX_FUTEX_REQUEUE:
        return futex_requeue_addr(uaddr, (int)A2(tf), A3(tf), A4(tf), false, 0);
    case LNX_FUTEX_CMP_REQUEUE:
        return futex_requeue_addr(uaddr, (int)A2(tf), A3(tf), A4(tf), true, (u32)A5(tf));
    default:
        return -ENOSYS;
    }
}

/* pipe2(fds, flags): pipe plus O_CLOEXEC and O_NONBLOCK on both ends. */
static long lnx_pipe2(TrapFrame *tf) {
    int flags = (int)A1(tf);
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    SyscallFn mk = syscall_native(SYS_pipe);
    long rc = mk ? mk(tf) : -ENOSYS;
    int fds[2];
    if (rc || !flags) return rc;
    if (copy_from_user(fds, (void *)A0(tf), sizeof fds)) return -EFAULT;
    for (int i = 0; i < 2; i++) {
        File *f = fd_get(me(), fds[i]);
        if (!f) continue;
        if (flags & O_NONBLOCK) f->flags |= O_NONBLOCK;
        if (flags & O_CLOEXEC) me()->fd_flags[fds[i]] |= FD_CLOEXEC;
    }
    return 0;
}

/* dup3(old, new, flags): dup2 that refuses old == new and can set O_CLOEXEC. */
static long lnx_dup3(TrapFrame *tf) {
    if ((int)A0(tf) == (int)A1(tf) || ((int)A2(tf) & ~O_CLOEXEC)) return -EINVAL;
    SyscallFn d2 = syscall_native(SYS_dup2);
    long r = d2 ? d2(tf) : -ENOSYS;
    if (r >= 0 && ((int)A2(tf) & O_CLOEXEC)) me()->fd_flags[r] |= FD_CLOEXEC;
    return r;
}

/* Services that need no work here: flushing (the filesystems write through), advisory locks, memory advice. */
static long lnx_zero(TrapFrame *tf) { (void)tf; return 0; }

/* time(tloc) and gettimeofday(tv, tz) from the wall clock in milliseconds. */
static long wall_ms(TrapFrame *tf) {
    SyscallFn ms = syscall_native(SYS_gettime_ms);
    return ms ? ms(tf) : 0;
}

static long lnx_time(TrapFrame *tf) {
    long t = wall_ms(tf) / 1000;
    if (A0(tf) && copy_to_user((void *)A0(tf), &t, sizeof t)) return -EFAULT;
    return t;
}

static long lnx_gettimeofday(TrapFrame *tf) {
    long ms = wall_ms(tf);
    struct { s64 sec, usec; } tv = { ms / 1000, (ms % 1000) * 1000 };
    struct { int minuteswest, dsttime; } tz = { 0, 0 };
    if (A0(tf) && copy_to_user((void *)A0(tf), &tv, sizeof tv)) return -EFAULT;
    if (A1(tf) && copy_to_user((void *)A1(tf), &tz, sizeof tz)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ vectored I/O and small adaptations of native calls */

struct lnx_iovec { u64 base, len; };

/* readv(2) and writev(2): each element is an ordinary read or write; a short transfer or an error ends the sequence (the transfer as a whole is not atomic). */
static long do_iov(TrapFrame *tf, int native_nr) {
    u64 cnt = A2(tf);
    SyscallFn one = syscall_native(native_nr);
    if (!one || cnt > 1024) return -EINVAL;
    long total = 0;
    for (u64 i = 0; i < cnt; i++) {
        struct lnx_iovec v;
        if (copy_from_user(&v, (const struct lnx_iovec *)A1(tf) + i, sizeof v)) return total ? total : -EFAULT;
        if (v.len == 0) continue;
        TrapFrame t = *tf;
        t.rsi = v.base;
        t.rdx = v.len;
        long r = one(&t);
        if (r < 0) return total ? total : r;
        total += r;
        if ((u64)r < v.len) break;
    }
    return total;
}

static long lnx_readv(TrapFrame *tf) { return do_iov(tf, SYS_read); }
static long lnx_writev(TrapFrame *tf) { return do_iov(tf, SYS_write); }

/* nanosleep, clock_nanosleep, clock_gettime, clock_getres, the interval timers and the POSIX timers are in timers.c; poll, ppoll, select, pselect6 and epoll are in epoll.c. */

/* ------------------------------------------------------------------ the table */

static const LinuxCall g_calls[LINUX_NSYSCALLS] = {
    [LNX_read] = NATIVE(SYS_read), [LNX_write] = NATIVE(SYS_write), [LNX_open] = OWN(lnx_open), [LNX_close] = NATIVE(SYS_close),
    [LNX_stat] = OWN(lnx_stat), [LNX_fstat] = OWN(lnx_fstat), [LNX_lstat] = OWN(lnx_lstat), [LNX_poll] = OWN(lnx_poll),
    [LNX_lseek] = NATIVE(SYS_lseek), [LNX_mmap] = NATIVE(SYS_mmap), [LNX_mprotect] = NATIVE(SYS_mprotect), [LNX_munmap] = NATIVE(SYS_munmap),
    [LNX_brk] = NATIVE(SYS_brk), [LNX_ioctl] = OWN(lnx_ioctl), [LNX_readv] = OWN(lnx_readv), [LNX_writev] = OWN(lnx_writev),
    [LNX_access] = NATIVE(SYS_access), [LNX_pipe] = NATIVE(SYS_pipe), [LNX_sched_yield] = NATIVE(SYS_yield),
    [LNX_dup] = NATIVE(SYS_dup), [LNX_dup2] = NATIVE(SYS_dup2), [LNX_nanosleep] = OWN(lnx_nanosleep), [LNX_clock_nanosleep] = OWN(lnx_clock_nanosleep), [LNX_getpid] = NATIVE(SYS_getpid),
    [LNX_fork] = NATIVE(SYS_fork), [LNX_vfork] = NATIVE(SYS_fork), [LNX_execve] = NATIVE(SYS_execve), [LNX_exit] = OWN(lnx_exit),
    [LNX_uname] = OWN(lnx_uname), [LNX_fcntl] = OWN(lnx_fcntl),
    [LNX_truncate] = NATIVE(SYS_truncate), [LNX_ftruncate] = NATIVE(SYS_ftruncate), [LNX_chdir] = NATIVE(SYS_chdir),
    [LNX_rename] = NATIVE(SYS_rename), [LNX_mkdir] = NATIVE(SYS_mkdir), [LNX_rmdir] = NATIVE(SYS_rmdir), [LNX_link] = NATIVE(SYS_link),
    [LNX_unlink] = NATIVE(SYS_unlink), [LNX_symlink] = NATIVE(SYS_symlink), [LNX_readlink] = NATIVE(SYS_readlink), [LNX_chmod] = NATIVE(SYS_chmod),
    [LNX_chown] = NATIVE(SYS_chown), [LNX_umask] = NATIVE(SYS_umask), [LNX_getrlimit] = OWN(lnx_getrlimit),
    [LNX_getuid] = NATIVE(SYS_getuid), [LNX_getgid] = NATIVE(SYS_getgid), [LNX_setuid] = NATIVE(SYS_setuid), [LNX_setgid] = NATIVE(SYS_setgid),
    [LNX_geteuid] = NATIVE(SYS_geteuid), [LNX_getegid] = NATIVE(SYS_getegid), [LNX_setpgid] = NATIVE(SYS_setpgid), [LNX_getppid] = NATIVE(SYS_getppid),
    [LNX_setsid] = NATIVE(SYS_setsid), [LNX_getpgid] = NATIVE(SYS_getpgid),
    [LNX_arch_prctl] = OWN(lnx_arch_prctl), [LNX_setrlimit] = OWN(lnx_setrlimit), [LNX_gettid] = NATIVE(SYS_gettid),
    [LNX_set_tid_address] = OWN(lnx_set_tid_address), [LNX_clock_gettime] = OWN(lnx_clock_gettime), [LNX_exit_group] = NATIVE(SYS_exit_group),
    [LNX_newfstatat] = OWN(lnx_newfstatat), [LNX_readlinkat] = OWN(lnx_readlinkat),
    [LNX_set_robust_list] = OWN(lnx_unsupported), [LNX_get_robust_list] = OWN(lnx_unsupported), [LNX_rseq] = OWN(lnx_unsupported),
    [LNX_prlimit64] = OWN(lnx_prlimit64), [LNX_getrandom] = OWN(lnx_getrandom),
    [LNX_pread64] = OWN(lnx_pread64), [LNX_pwrite64] = OWN(lnx_pwrite64), [LNX_madvise] = OWN(lnx_madvise), [LNX_mremap] = OWN(lnx_mremap), [LNX_clone] = OWN(lnx_clone),
    [LNX_flock] = OWN(lnx_zero), [LNX_fsync] = OWN(lnx_zero), [LNX_fdatasync] = OWN(lnx_zero), [LNX_getcwd] = NATIVE(SYS_getcwd),
    [LNX_creat] = OWN(lnx_creat), [LNX_gettimeofday] = OWN(lnx_gettimeofday), [LNX_sync] = NATIVE(SYS_sync), [LNX_time] = OWN(lnx_time),
    [LNX_openat] = OWN(lnx_openat), [LNX_mkdirat] = OWN(lnx_mkdirat), [LNX_unlinkat] = OWN(lnx_unlinkat), [LNX_renameat] = OWN(lnx_renameat),
    [LNX_symlinkat] = OWN(lnx_symlinkat), [LNX_faccessat] = OWN(lnx_faccessat), [LNX_dup3] = OWN(lnx_dup3), [LNX_pipe2] = OWN(lnx_pipe2),
    [LNX_renameat2] = OWN(lnx_renameat2), [LNX_faccessat2] = OWN(lnx_faccessat2), [LNX_getdents64] = OWN(lnx_getdents64),
    [LNX_statx] = OWN(lnx_statx), [LNX_sysinfo] = OWN(lnx_sysinfo), [LNX_prctl] = OWN(lnx_prctl),
    [LNX_sched_getaffinity] = OWN(lnx_sched_getaffinity), [LNX_sched_setaffinity] = OWN(lnx_zero),
    [LNX_rt_sigaction] = OWN(lnx_rt_sigaction), [LNX_rt_sigprocmask] = OWN(lnx_rt_sigprocmask), [LNX_rt_sigreturn] = OWN(lnx_rt_sigreturn),
    [LNX_pause] = NATIVE(SYS_pause), [LNX_alarm] = NATIVE(SYS_alarm), [LNX_rt_sigpending] = OWN(lnx_rt_sigpending),
    [LNX_rt_sigtimedwait] = OWN(lnx_rt_sigtimedwait), [LNX_rt_sigsuspend] = OWN(lnx_rt_sigsuspend), [LNX_sigaltstack] = OWN(lnx_sigaltstack),
    [LNX_futex] = OWN(lnx_futex), [LNX_clone3] = OWN(lnx_unsupported),
    /* L2 (linux-loader): dynamic programs, coreutils and a shell; the handlers are in fsops.c */
    [LNX_msync] = OWN(lnx_hint), [LNX_fadvise64] = OWN(lnx_hint), [LNX_fchdir] = OWN(lnx_fchdir), [LNX_fchmod] = OWN(lnx_fchmod), [LNX_fchown] = OWN(lnx_fchown),
    [LNX_lchown] = OWN(lnx_lchown), [LNX_getpgrp] = OWN(lnx_getpgrp), [LNX_getsid] = OWN(lnx_getsid),
    [LNX_statfs] = OWN(lnx_statfs), [LNX_fstatfs] = OWN(lnx_fstatfs), [LNX_fchownat] = OWN(lnx_fchownat), [LNX_linkat] = OWN(lnx_linkat),
    [LNX_fchmodat] = OWN(lnx_fchmodat), [LNX_utimensat] = OWN(lnx_utimensat),
    [LNX_mincore] = OWN(lnx_unsupported), [LNX_copy_file_range] = OWN(lnx_unsupported),      /* grep and cp ask, get ENOSYS and carry on (cp falls back to read and write): not worth a log line */
    [LNX_setxattr] = OWN(lnx_noxattr), [LNX_lsetxattr] = OWN(lnx_noxattr), [LNX_fsetxattr] = OWN(lnx_noxattr), [LNX_getxattr] = OWN(lnx_noxattr),
    [LNX_lgetxattr] = OWN(lnx_noxattr), [LNX_fgetxattr] = OWN(lnx_noxattr), [LNX_listxattr] = OWN(lnx_noxattr), [LNX_llistxattr] = OWN(lnx_noxattr),
    [LNX_flistxattr] = OWN(lnx_noxattr), [LNX_removexattr] = OWN(lnx_noxattr), [LNX_lremovexattr] = OWN(lnx_noxattr), [LNX_fremovexattr] = OWN(lnx_noxattr),
    /* linux-signals (docs/COMPAT.md): sending signals with a siginfo, real-time signals */
    [LNX_kill] = OWN(lnx_kill), [LNX_tkill] = OWN(lnx_tkill), [LNX_tgkill] = OWN(lnx_tgkill),
    [LNX_rt_sigqueueinfo] = OWN(lnx_rt_sigqueueinfo), [LNX_rt_tgsigqueueinfo] = OWN(lnx_rt_tgsigqueueinfo), [LNX_waitid] = OWN(lnx_waitid), [LNX_wait4] = OWN(lnx_wait4),
    /* linux-signals: interval timers, POSIX timers, clocks (timers.c; nanosleep, clock_nanosleep and clock_gettime are in the table above, their code moved there) */
    [LNX_getitimer] = OWN(lnx_getitimer), [LNX_setitimer] = OWN(lnx_setitimer), [LNX_clock_getres] = OWN(lnx_clock_getres),
    [LNX_times] = OWN(lnx_times), [LNX_getrusage] = OWN(lnx_getrusage),
    [LNX_timer_create] = OWN(lnx_timer_create), [LNX_timer_settime] = OWN(lnx_timer_settime), [LNX_timer_gettime] = OWN(lnx_timer_gettime),
    [LNX_timer_getoverrun] = OWN(lnx_timer_getoverrun), [LNX_timer_delete] = OWN(lnx_timer_delete),
    /* linux-signals: the id calls the C library's set-id broadcast is made of (ids.c), and the supplementary groups */
    [LNX_setreuid] = OWN(lnx_setreuid), [LNX_setregid] = OWN(lnx_setregid), [LNX_setresuid] = OWN(lnx_setresuid), [LNX_getresuid] = OWN(lnx_getresuid),
    [LNX_setresgid] = OWN(lnx_setresgid), [LNX_getresgid] = OWN(lnx_getresgid), [LNX_getgroups] = NATIVE(SYS_getgroups), [LNX_setgroups] = NATIVE(SYS_setgroups),
    /* sockets (kernel/linux/net.c): the ones whose arguments and results are the native ones go straight to the native handlers */
    [LNX_socket] = OWN(lnx_socket), [LNX_socketpair] = OWN(lnx_socketpair), [LNX_connect] = NATIVE(SYS_connect), [LNX_accept] = NATIVE(SYS_accept),
    [LNX_accept4] = OWN(lnx_accept4), [LNX_bind] = NATIVE(SYS_bind), [LNX_listen] = NATIVE(SYS_listen), [LNX_shutdown] = NATIVE(SYS_shutdown),
    [LNX_getsockname] = NATIVE(SYS_getsockname), [LNX_getpeername] = NATIVE(SYS_getpeername), [LNX_sendto] = NATIVE(SYS_sendto),
    [LNX_recvfrom] = NATIVE(SYS_recvfrom), [LNX_sendmsg] = OWN(lnx_sendmsg), [LNX_recvmsg] = OWN(lnx_recvmsg), [LNX_sendmmsg] = OWN(lnx_sendmmsg),
    [LNX_recvmmsg] = OWN(lnx_recvmmsg), [LNX_setsockopt] = NATIVE(SYS_setsockopt), [LNX_getsockopt] = NATIVE(SYS_getsockopt), [LNX_sendfile] = OWN(lnx_sendfile),
    /* waiting for events (kernel/linux/epoll.c; poll itself is in the table above) */
    [LNX_ppoll] = OWN(lnx_ppoll), [LNX_select] = OWN(lnx_select), [LNX_pselect6] = OWN(lnx_pselect6), [LNX_epoll_create] = OWN(lnx_epoll_create),
    [LNX_epoll_create1] = OWN(lnx_epoll_create1), [LNX_epoll_ctl] = OWN(lnx_epoll_ctl), [LNX_epoll_wait] = OWN(lnx_epoll_wait), [LNX_epoll_pwait] = OWN(lnx_epoll_pwait),
    [LNX_epoll_pwait2] = OWN(lnx_epoll_pwait2), [LNX_eventfd] = OWN(lnx_eventfd), [LNX_eventfd2] = OWN(lnx_eventfd2), [LNX_timerfd_create] = OWN(lnx_timerfd_create),
    [LNX_timerfd_settime] = OWN(lnx_timerfd_settime), [LNX_timerfd_gettime] = OWN(lnx_timerfd_gettime),
};

static const char *const g_names[LINUX_NSYSCALLS] = {
#include "names.inc"
};

static bool g_trace;                                         /* "linux.trace" on the kernel command line: every call is logged with its result */
static u64  g_reported[LINUX_NSYSCALLS / 64];                /* calls already named as "not implemented" */

static const char *name_of(u64 nr) { return nr < LINUX_NSYSCALLS && g_names[nr] ? g_names[nr] : "?"; }

static void report_missing(u64 nr) {
    if (nr >= LINUX_NSYSCALLS) return;                       /* not a system call at all (programs probe with odd numbers): ENOSYS, and silence */
    if (g_reported[nr / 64] & (1ULL << (nr % 64))) return;
    g_reported[nr / 64] |= 1ULL << (nr % 64);
    klog(TAG, "pid %d (%s): Linux system call %lu (%s) is not implemented", me()->pid, me()->name, (unsigned long)nr, name_of(nr));
}

static void trace(TrapFrame *tf, u64 nr, bool returned, long ret) {
    if (returned)
        klog(TAG, "[%d] %s(0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld", me()->pid, name_of(nr), (unsigned long)tf->rdi, (unsigned long)tf->rsi,
             (unsigned long)tf->rdx, (unsigned long)tf->r10, ret);
    else
        klog(TAG, "[%d] %s(0x%lx, 0x%lx, 0x%lx, 0x%lx) ...", me()->pid, name_of(nr), (unsigned long)tf->rdi, (unsigned long)tf->rsi,
             (unsigned long)tf->rdx, (unsigned long)tf->r10);
}

long linux_syscall(TrapFrame *tf) {
    u64 nr = tf->rax;
    long ret;
    const LinuxCall *c = nr < LINUX_NSYSCALLS ? &g_calls[nr] : NULL;
    if (g_trace && (nr == LNX_exit || nr == LNX_exit_group || nr == LNX_execve)) trace(tf, nr, false, 0);       /* these may not come back */
    if (c && c->fn) {
        ret = c->fn(tf);
    } else if (c && c->native) {
        SyscallFn f = syscall_native(c->native - 1);
        ret = f ? f(tf) : -ENOSYS;
    } else {
        report_missing(nr);
        ret = -ENOSYS;
    }
    if (g_trace) trace(tf, nr, true, ret);
    return ret;
}

static void linux_init(void) {
    g_trace = cmdline_flag("linux.trace");
}
INITCALL(linux_init);
