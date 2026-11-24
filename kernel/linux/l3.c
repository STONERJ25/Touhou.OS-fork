/*
 * L3 of docs/COMPAT.md ("State: L3"): the calls that Python, its standard library and the programs around them make beyond the first levels' set. A big interpreter asks for
 * everything a system has: shared anonymous memory that a forked child can write to, vectored reads and writes at an offset, space reservation for a file, the holes of a file,
 * file creation by mknod, anonymous memory files, the scheduling queries, and a directory for POSIX shared memory. Each is here because a real program or a library function failed
 * without it (the symptom is named at the call).
 *
 * Written from the manual pages (section 2: mmap, preadv, fallocate, lseek, copy_file_range, mknod, memfd_create, sched_setscheduler and the others below) and compared with a
 * real Linux by the Python corpus (tests/linux/py, tests/case_linux_py.py); no implementation of Linux, glibc or CPython was read.
 */
#include "linux.h"
#include "initcall.h"
#include "kernel.h"
#include "cpu.h"
#include "calls.h"
#include "numbers.h"

static inline Process *me(void) { return current->proc; }

/* ------------------------------------------------------------------ mmap: shared anonymous memory */

/* mmap(addr, length, prot, flags, fd, offset). Everything is the native call except an anonymous mapping that is MAP_SHARED: the native mapping of anonymous memory is a private
 * one, which a fork copies, so a child's writes never reached the parent (Python's mmap.mmap(-1, n), the shared memory of many a C program, is exactly this mapping; the first
 * symptom was multiprocessing's Value and Array staying zero). Here the pages are allocated at once and mapped as shared pages (the same VMA_IO mapping that a tmpfs file gets
 * with MAP_SHARED): fork gives the child the same frames, each process drops its reference when it unmaps or exits, and the frames go when the last one has. No demand paging
 * for these, so the length is capped at half of the free memory (a larger request is ENOMEM). mremap of such a mapping is EINVAL, as for the other shared mappings. */
long lnx_mmap(TrapFrame *tf) {
    int flags = (int)A3(tf);
    if (!((flags & MAP_ANONYMOUS) && (flags & MAP_SHARED))) {
        SyscallFn native = syscall_native(SYS_mmap);
        return native ? native(tf) : -ENOSYS;
    }
    u64 addr = A0(tf), len = A1(tf);
    u32 prot = (u32)A2(tf);
    if (!len || len > ~0UL - PAGE_SIZE) return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (len / PAGE_SIZE > pmm_free_pages_count() / 2) return -ENOMEM;
    AddrSpace *as = &me()->as;
    mutex_lock(&as->lock);
    long rc = 0;
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1)) rc = -EINVAL;
        else uvm_unmap(as, addr, len);
    } else {
        addr = uvm_find_free(as, len);
        if (!addr) rc = -ENOMEM;
    }
    if (!rc) rc = uvm_map(as, addr, len, prot, VMA_IO);
    if (!rc) {
        for (u64 o = 0; o < len; o += PAGE_SIZE) {
            paddr_t p = oom_alloc_user_page(true, true);        /* memory for a user process: it must not eat the kernel's reserve, and a shortage goes to the OOM killer (kernel/mm/oom.c) */
            if (!p || vmm_map(as->pml4, addr + o, p, pte_flags_for(prot, 0))) {
                if (p) pmm_free_page(p);
                uvm_unmap(as, addr, len);                    /* gives back the pages mapped so far */
                rc = -ENOMEM;
                break;
            }
        }
    }
    mutex_unlock(&as->lock);
    return rc ? rc : (long)addr;
}

/* ------------------------------------------------------------------ file positions and sizes */

/* lseek(fd, offset, whence): the native call, and SEEK_DATA and SEEK_HOLE (3 and 4), which the native one lacks. A file here has no holes: the data starts at the offset and the
 * only hole is the one at the end; an offset at or beyond the end is ENXIO. Programs that copy sparse files (cp, tar, rsync, Python's shutil in newer versions) ask. */
#define LNX_SEEK_DATA 3
#define LNX_SEEK_HOLE 4

long lnx_lseek(TrapFrame *tf) {
    int whence = (int)A2(tf);
    if (whence != LNX_SEEK_DATA && whence != LNX_SEEK_HOLE) {
        SyscallFn native = syscall_native(SYS_lseek);
        return native ? native(tf) : -ENOSYS;
    }
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    long rc;
    s64 off = (s64)A1(tf);
    if (!f->vn || !S_ISREG(f->vn->mode)) rc = S_ISDIR(f->vn ? f->vn->mode : 0) ? -EINVAL : -ESPIPE;
    else if (off < 0 || (u64)off >= f->vn->size) rc = -ENXIO;
    else {
        f->off = whence == LNX_SEEK_DATA ? (u64)off : f->vn->size;
        rc = (long)f->off;
    }
    file_put(f);
    return rc;
}

/* fallocate(fd, mode, offset, len): mode 0 makes sure the file is at least offset + len long (the new space reads as zeros: a file here is sparse until it is written), and
 * FALLOC_FL_KEEP_SIZE (1) has nothing to reserve. Punching holes, zeroing, collapsing and inserting ranges are EOPNOTSUPP (the glibc posix_fallocate gave up with ENOSYS before:
 * Python's os.posix_fallocate raised OSError). */
long lnx_fallocate(TrapFrame *tf) {
    int mode = (int)A1(tf);
    s64 off = (s64)A2(tf), len = (s64)A3(tf);
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    long rc = 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY) rc = -EBADF;
    else if (off < 0 || len <= 0 || off > INT64_MAX - len) rc = -EINVAL;
    else if (!f->vn || S_ISFIFO(f->vn->mode)) rc = -ESPIPE;
    else if (S_ISDIR(f->vn->mode)) rc = -EISDIR;
    else if (!S_ISREG(f->vn->mode)) rc = -ENODEV;
    else if (mode & ~1) rc = -EOPNOTSUPP;
    else if (!(mode & 1) && (u64)(off + len) > f->vn->size) rc = vfs_ftruncate(f, (u64)(off + len));
    file_put(f);
    return rc;
}

/* ------------------------------------------------------------------ vectored reads and writes at an offset */

/* preadv(fd, iov, iovcnt, offset), pwritev, and the -v2 forms with flags (RWF_HIPRI, RWF_DSYNC, RWF_SYNC and RWF_NOWAIT are hints here; RWF_APPEND is not supported). An offset
 * of -1 in the -v2 forms means "the file position", as readv and writev do. On x86-64 the offset is one register (the high half in the next is zero). Only files with positions
 * have offsets (ESPIPE otherwise). */

static long do_pvec(TrapFrame *tf, int native_nr, s64 pos, bool v2, u32 flags) {
    if (v2 && (flags & ~0xfu)) return -EOPNOTSUPP;
    if (pos < 0 && !(v2 && pos == -1)) return -EINVAL;
    File *f = fd_get_ref(me(), (int)A0(tf));
    if (!f) return -EBADF;
    long rc;
    if (pos == -1) {
        rc = lnx_iov(tf, native_nr);
    } else if (!f->vn || !S_ISREG(f->vn->mode)) {
        rc = -ESPIPE;
    } else {
        u64 saved = f->off;
        f->off = (u64)pos;
        rc = lnx_iov(tf, native_nr);
        f->off = saved;
    }
    file_put(f);
    return rc;
}

long lnx_preadv(TrapFrame *tf) { return do_pvec(tf, SYS_read, (s64)A3(tf), false, 0); }
long lnx_pwritev(TrapFrame *tf) { return do_pvec(tf, SYS_write, (s64)A3(tf), false, 0); }
long lnx_preadv2(TrapFrame *tf) { return do_pvec(tf, SYS_read, (s64)A3(tf), true, (u32)A5(tf)); }
long lnx_pwritev2(TrapFrame *tf) { return do_pvec(tf, SYS_write, (s64)A3(tf), true, (u32)A5(tf)); }

/* copy_file_range(fd_in, off_in, fd_out, off_out, len, flags): copies between two regular files through a kernel buffer; an offset pointer that is not NULL gives the position to use
 * and is advanced, a NULL one uses (and advances) the file's own position. The ranges of one file must not overlap (EINVAL). Fewer bytes than asked come back at the end of the
 * input. The call used to answer ENOSYS silently (cp falls back to read and write); a program that asks for it by name (Python's os.copy_file_range) now gets it. */
long lnx_copy_file_range(TrapFrame *tf) {
    if (A5(tf)) return -EINVAL;
    s64 offin = -1, offout = -1;
    if (A1(tf) && copy_from_user(&offin, (void *)A1(tf), sizeof offin)) return -EFAULT;
    if (A3(tf) && copy_from_user(&offout, (void *)A3(tf), sizeof offout)) return -EFAULT;
    if ((A1(tf) && offin < 0) || (A3(tf) && offout < 0)) return -EINVAL;
    u64 len = MIN(A4(tf), (u64)0x7ffff000);
    File *in = fd_get_ref(me(), (int)A0(tf));
    File *out = fd_get_ref(me(), (int)A2(tf));
    long rc = 0;
    u8 *buf = NULL;
    if (!in || !out) rc = -EBADF;
    else if ((in->flags & O_ACCMODE) == O_WRONLY || (out->flags & O_ACCMODE) == O_RDONLY || (out->flags & O_APPEND)) rc = -EBADF;
    else if (!in->vn || !out->vn) rc = -EINVAL;
    else if (S_ISDIR(in->vn->mode) || S_ISDIR(out->vn->mode)) rc = -EISDIR;
    else if (!S_ISREG(in->vn->mode) || !S_ISREG(out->vn->mode)) rc = -EINVAL;
    else {
        u64 a = A1(tf) ? (u64)offin : in->off, b = A3(tf) ? (u64)offout : out->off;
        if (in->vn == out->vn && a < b + len && b < a + len) rc = -EINVAL;
        else if (!(buf = kmalloc(65536))) rc = -ENOMEM;
        else {
            u64 saved_in = in->off, saved_out = out->off, done = 0;
            while (done < len) {
                size_t chunk = (size_t)MIN(len - done, (u64)65536);
                in->off = a + done;
                ssize_t n = vfs_read(in, buf, chunk);
                if (n <= 0) { if (n < 0 && !done) rc = n; break; }
                out->off = b + done;
                ssize_t w = vfs_write(out, buf, (size_t)n);
                if (w <= 0) { if (w < 0 && !done) rc = w; break; }
                done += (u64)w;
                if (w < n) break;
            }
            in->off = A1(tf) ? saved_in : a + done;                      /* an explicit offset leaves the file position alone */
            out->off = A3(tf) ? saved_out : b + done;
            if (!rc) {
                s64 na = offin + (s64)done, nb = offout + (s64)done;
                if ((A1(tf) && copy_to_user((void *)A1(tf), &na, sizeof na)) || (A3(tf) && copy_to_user((void *)A3(tf), &nb, sizeof nb))) rc = -EFAULT;
                else rc = (long)done;
            }
        }
    }
    kfree(buf);
    if (in) file_put(in);
    if (out) file_put(out);
    return rc;
}

/* splice(fd_in, off_in, fd_out, off_out, len, flags): moves data between two descriptors of which at least one is a pipe (EINVAL otherwise); a pipe has no offset (ESPIPE when one is
 * given), a file with a NULL offset pointer uses and advances its position, with an offset pointer it reads or writes there and the pointer is advanced. Here the data goes through
 * a kernel buffer (nothing is moved by reference): one call moves what is available up to a buffer of 64 KiB and returns the amount, and callers loop, as they must on a real system,
 * where a pipe's capacity limits a call too. SPLICE_F_NONBLOCK makes the pipe ends non-blocking for the call; SPLICE_F_MOVE, MORE and GIFT are hints. 0 is the end of the input. */
#define LNX_SPLICE_F_NONBLOCK 2u
#define LNX_SPLICE_F_ALL      0xfu

static bool is_pipe(File *f) { return f && f->vn && S_ISFIFO(f->vn->mode); }

long lnx_splice(TrapFrame *tf) {
    u32 flags = (u32)A5(tf);
    if (flags & ~LNX_SPLICE_F_ALL) return -EINVAL;
    s64 offin = -1, offout = -1;
    if (A1(tf) && copy_from_user(&offin, (void *)A1(tf), sizeof offin)) return -EFAULT;
    if (A3(tf) && copy_from_user(&offout, (void *)A3(tf), sizeof offout)) return -EFAULT;
    File *in = fd_get_ref(me(), (int)A0(tf));
    File *out = fd_get_ref(me(), (int)A2(tf));
    long rc = 0;
    u8 *buf = NULL;
    if (!in || !out) rc = -EBADF;
    else if ((in->flags & O_ACCMODE) == O_WRONLY || (out->flags & O_ACCMODE) == O_RDONLY) rc = -EBADF;
    else if (!is_pipe(in) && !is_pipe(out)) rc = -EINVAL;
    else if ((is_pipe(in) && A1(tf)) || (is_pipe(out) && A3(tf))) rc = -ESPIPE;
    else if ((A1(tf) && offin < 0) || (A3(tf) && offout < 0)) rc = -EINVAL;
    else if ((A1(tf) && (!in->vn || !S_ISREG(in->vn->mode))) || (A3(tf) && (!out->vn || !S_ISREG(out->vn->mode)))) rc = -ESPIPE;
    else if (!(buf = kmalloc(65536))) rc = -ENOMEM;
    else {
        u64 saved_in = in->off, saved_out = out->off;
        int in_flags = in->flags, out_flags = out->flags;
        if (flags & LNX_SPLICE_F_NONBLOCK) {
            if (is_pipe(in)) in->flags |= O_NONBLOCK;
            if (is_pipe(out)) out->flags |= O_NONBLOCK;
        }
        if (A1(tf)) in->off = (u64)offin;
        if (A3(tf)) out->off = (u64)offout;
        size_t chunk = (size_t)MIN(A4(tf), (u64)65536);
        ssize_t n = chunk ? vfs_read(in, buf, chunk) : 0;
        ssize_t w = 0;
        while (n > 0 && w < n) {                                   /* the output may take less than it was given: the rest follows */
            ssize_t k = vfs_write(out, buf + w, (size_t)(n - w));
            if (k <= 0) { if (!w) w = k; break; }
            w += k;
        }
        if (n < 0) rc = n;
        else if (w < 0) rc = w;
        else rc = (long)(n > 0 ? w : 0);
        u64 moved = rc > 0 ? (u64)rc : 0;
        if (A1(tf)) { s64 na = offin + (s64)moved; in->off = saved_in; if (rc >= 0 && copy_to_user((void *)A1(tf), &na, sizeof na)) rc = -EFAULT; }
        if (A3(tf)) { s64 nb = offout + (s64)moved; out->off = saved_out; if (rc >= 0 && copy_to_user((void *)A3(tf), &nb, sizeof nb)) rc = -EFAULT; }
        in->flags = in_flags;
        out->flags = out_flags;
    }
    kfree(buf);
    if (in) file_put(in);
    if (out) file_put(out);
    return rc;
}

/* ------------------------------------------------------------------ mknod, memfd_create */

/* mknod(path, mode, dev) and mknodat: a regular file (the type bits may be left out). Device nodes need privileges this system does not hand out (EPERM, as for a user without
 * CAP_MKNOD), and a socket file cannot be made yet (EPERM too). A named pipe (S_IFIFO: mkfifo(3)) is made on tmpfs, which holds the Linux root and /tmp (2026-11-24: the node
 * is created as an empty file and turned into a FIFO before anybody else has it; its opens are kernel/fs/pipe.c's fifo_open); elsewhere it is EPERM.
 * The symptom of the missing call was os.mkfifo and os.mknod raising ENOSYS. */
static long do_mknod(int dirfd, u64 upath, u32 mode) {
    u32 type = mode & S_IFMT;
    if (!type) type = S_IFREG;
    if (type != S_IFREG && type != S_IFIFO) return (type == S_IFCHR || type == S_IFBLK || type == S_IFSOCK) ? -EPERM : -EINVAL;
    char *path;
    VNode *base;
    File *hold, *f;
    long rc = at_path(dirfd, upath, &path, &base, &hold);
    if (rc) return rc;
    rc = vfs_open(path, O_CREAT | O_EXCL | (type == S_IFIFO ? O_RDONLY : O_WRONLY), mode & 07777 & ~me()->umask, &me()->cred, base, &f);
    if (rc == 0 && type == S_IFIFO) {
        VNode *v = f->vn;
        bool tmp = v->mount && v->mount->type == &g_tmpfs_type && S_ISREG(v->mode) && v->size == 0;
        if (tmp) v->mode = S_IFIFO | (v->mode & 07777);
        file_put(f);
        if (!tmp) { vfs_unlink(path, &me()->cred, base); rc = -EPERM; }
    } else if (rc == 0) file_put(f);
    at_done(path, hold);
    return rc;
}

long lnx_mknod(TrapFrame *tf) { return do_mknod(AT_FDCWD, A0(tf), (u32)A1(tf)); }

/* The native personality's mknod (SYS_mknod: mkfifo(3) of the project's libc and the `mkfifo` command) is the same function: a path of a native process is resolved
 * from its own working directory and the real root by at_path. Registered here and not in kernel/sys/syscalls.c's table because the work is here. */
static long sys_mknod(TrapFrame *tf) { return do_mknod(AT_FDCWD, A0(tf), (u32)A1(tf)); }
static void native_mknod_init(void) { syscall_register(SYS_mknod, sys_mknod); }
INITCALL(native_mknod_init);
long lnx_mknodat(TrapFrame *tf) { return do_mknod((int)A0(tf), A1(tf), (u32)A2(tf)); }

/* memfd_create(name, flags): an anonymous file in memory (a tmpfs file that is unlinked at once). libffi uses one for the executable memory of closures, and Python's
 * os.memfd_create hands it out. The name is only for /proc/self/fd ("/memfd:NAME (deleted)"). Sealing is accepted and not enforced. */
#define LNX_MFD_CLOEXEC       0x1u
#define LNX_MFD_ALLOW_SEALING 0x2u

long lnx_memfd_create(TrapFrame *tf) {
    unsigned flags = (unsigned)A1(tf);
    if (flags & ~(LNX_MFD_CLOEXEC | LNX_MFD_ALLOW_SEALING)) return -EINVAL;
    char name[250];
    int n = strncpy_from_user(name, (const char *)A0(tf), sizeof name);
    if (n < 0) return n;
    if (n >= (int)sizeof name) return -EINVAL;
    static unsigned counter;
    char path[64], shown[300];
    snprintf(path, sizeof path, "/BambooForest/.memfd-%u", __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED));
    File *f;
    long rc = vfs_open(path, O_RDWR | O_CREAT | O_EXCL, 0777, NULL, NULL, &f);     /* the kernel's own lookup: the real /tmp, whatever root the caller has */
    if (rc) return rc;
    vfs_unlink(path, NULL, NULL);
    snprintf(shown, sizeof shown, "/memfd:%s (deleted)", name);
    file_set_path(f, shown);
    rc = fd_install(me(), f, 0, (flags & LNX_MFD_CLOEXEC) ? FD_CLOEXEC : 0);
    if (rc < 0) file_put(f);
    return rc;
}

/* ------------------------------------------------------------------ scheduling queries */

/* The scheduler has one policy, the ordinary time-sharing one (SCHED_OTHER) with niceness (getpriority and setpriority are the native calls). The queries answer for it, and the
 * calls that would change the policy accept what is already true (SCHED_OTHER, SCHED_BATCH and SCHED_IDLE at priority 0) and refuse the real-time policies with EPERM, as a system
 * does for a user without the privilege. os.sched_getscheduler, os.sched_getparam and the like raised ENOSYS before. */
#define LNX_SCHED_OTHER 0
#define LNX_SCHED_FIFO  1
#define LNX_SCHED_RR    2
#define LNX_SCHED_BATCH 3
#define LNX_SCHED_IDLE  5
#define LNX_SCHED_RESET_ON_FORK 0x40000000

static long sched_target(u64 pid) {
    if (pid > 0x7fffffffUL) return -EINVAL;
    if (pid == 0) return 0;
    Process *t = process_by_pid((int)pid);
    return t && t->state != PROC_ZOMBIE ? 0 : -ESRCH;
}

long lnx_sched_getscheduler(TrapFrame *tf) {
    long rc = sched_target(A0(tf));
    return rc ? rc : LNX_SCHED_OTHER;
}

long lnx_sched_getparam(TrapFrame *tf) {
    long rc = sched_target(A0(tf));
    if (rc) return rc;
    if (!A1(tf)) return -EINVAL;
    int prio = 0;
    return copy_to_user((void *)A1(tf), &prio, sizeof prio);
}

long lnx_sched_setparam(TrapFrame *tf) {
    long rc = sched_target(A0(tf));
    int prio;
    if (rc) return rc;
    if (!A1(tf)) return -EINVAL;
    if (copy_from_user(&prio, (void *)A1(tf), sizeof prio)) return -EFAULT;
    return prio == 0 ? 0 : -EINVAL;
}

long lnx_sched_setscheduler(TrapFrame *tf) {
    long rc = sched_target(A0(tf));
    int policy = (int)A1(tf) & ~LNX_SCHED_RESET_ON_FORK, prio;
    if (rc) return rc;
    if (!A2(tf) || policy < 0) return -EINVAL;
    if (copy_from_user(&prio, (void *)A2(tf), sizeof prio)) return -EFAULT;
    switch (policy) {
    case LNX_SCHED_OTHER: case LNX_SCHED_BATCH: case LNX_SCHED_IDLE: return prio == 0 ? 0 : -EINVAL;
    case LNX_SCHED_FIFO: case LNX_SCHED_RR: return prio >= 1 && prio <= 99 ? -EPERM : -EINVAL;
    default: return -EINVAL;
    }
}

long lnx_sched_get_priority_max(TrapFrame *tf) {
    switch ((int)A0(tf)) {
    case LNX_SCHED_OTHER: case LNX_SCHED_BATCH: case LNX_SCHED_IDLE: return 0;
    case LNX_SCHED_FIFO: case LNX_SCHED_RR: return 99;
    default: return -EINVAL;
    }
}

long lnx_sched_get_priority_min(TrapFrame *tf) {
    switch ((int)A0(tf)) {
    case LNX_SCHED_OTHER: case LNX_SCHED_BATCH: case LNX_SCHED_IDLE: return 0;
    case LNX_SCHED_FIFO: case LNX_SCHED_RR: return 1;
    default: return -EINVAL;
    }
}

/* sched_rr_get_interval(pid, timespec): the time slice. This scheduler's slices depend on the niceness (kernel/sched/task.c); the default one is 100 ms. */
long lnx_sched_rr_get_interval(TrapFrame *tf) {
    long rc = sched_target(A0(tf));
    if (rc) return rc;
    struct { s64 sec, nsec; } ts = { 0, 100000000 };
    return copy_to_user((void *)A1(tf), &ts, sizeof ts);
}

/* getcpu(cpu, node, cache): the one processor, on the one node. */
long lnx_getcpu(TrapFrame *tf) {
    u32 zero = 0;
    if (A0(tf) && copy_to_user((void *)A0(tf), &zero, sizeof zero)) return -EFAULT;
    if (A1(tf) && copy_to_user((void *)A1(tf), &zero, sizeof zero)) return -EFAULT;
    return 0;
}

/* ------------------------------------------------------------------ pseudo-terminals */

/* open("/dev/ptmx"): a Linux program asks for a new pseudo-terminal by opening this node and learns the number of the slave with ioctl(TIOCGPTN); glibc's posix_openpt, openpty
 * and forkpty, and so Python's os.openpty, pty.fork and pty.spawn, work this way. The native way is the call SYS_ptyopen, which makes the pair and hands out the master; this is
 * that call with no buffer for the slave's name, plus the flags of open(2). The slave is /dev/pts/N from the start (TIOCSPTLCK, "unlock", has nothing to unlock). */
#define LNX_O_NOCTTY 0400

long lnx_open_ptmx(int flags) {
    if ((flags & O_ACCMODE) != O_RDWR) return -EINVAL;
    SyscallFn open_pair = syscall_native(SYS_ptyopen);
    if (!open_pair) return -ENOSYS;
    TrapFrame t;
    memset(&t, 0, sizeof t);                                  /* no buffer: the master's descriptor is all the caller gets */
    long fd = open_pair(&t);
    if (fd < 0) return fd == -ENOSPC ? -EAGAIN : fd;          /* all the pairs are in use: EAGAIN, as a system out of pseudo-terminals says */
    File *f = fd_get(me(), (int)fd);
    if (f) {
        if (flags & O_NONBLOCK) f->flags |= O_NONBLOCK;
        file_set_path(f, "/dev/ptmx");
    }
    if (flags & O_CLOEXEC) fd_setflags(me(), (int)fd, FD_CLOEXEC, 0);
    return fd;
}

/* The ioctls of a master that only the Linux numbers have: the number of the slave, and the lock that unlockpt() clears. ENOTTY for a descriptor that is no master. */
#define LNX_TIOCGPTN   0x80045430u
#define LNX_TIOCSPTLCK 0x40045431u
#define LNX_TIOCGPTLCK 0x80045439u

long lnx_pty_ioctl(int fd, u64 req, u64 arg) {
    File *f = fd_get_ref(me(), fd);
    if (!f) return -EBADF;
    int index = pty_master_index(f);
    file_put(f);
    if (index < 0) return -ENOTTY;
    int value = 0;
    switch (req) {
    case LNX_TIOCGPTN: value = index; break;
    case LNX_TIOCGPTLCK: break;                               /* never locked */
    case LNX_TIOCSPTLCK: return copy_from_user(&value, (void *)arg, sizeof value) ? -EFAULT : 0;
    default: return -ENOTTY;
    }
    return copy_to_user((void *)arg, &value, sizeof value);
}

/* ------------------------------------------------------------------ /dev/shm */

/* POSIX shared memory (shm_open) and named semaphores (sem_open) keep their files in /dev/shm, which the C library takes for granted: Python's multiprocessing (every Lock, Queue,
 * Event and Pool starts with sem_open) failed with ENOENT without it. The directory is made once at boot in the machine's own /dev (the Linux root passes /dev through) and is
 * writable by everybody, like /tmp. */
void linux_make_shm_dir(void) {
    VNode *dev = devfs_root(), *d = NULL;
    if (dev && dev->ops->mkdir && dev->ops->mkdir(dev, "shm", 01777, &d) == 0) vnode_put(d);
    static const VNodeOps no_ops = { 0 };                        /* /dev/ptmx exists so that stat and access find it; open() of it is answered by lnx_open_ptmx */
    devfs_register("ptmx", S_IFCHR | 0666, &no_ops, NULL, 0x0502);
}
