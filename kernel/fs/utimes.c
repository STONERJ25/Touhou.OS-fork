/* utimes(): set the access and modification times of a file (tar, unzip, gunzip and cp -p restore them). */
#include "process.h"
#include "kernel.h"
#include "initcall.h"

/* The caller must own the file (or be root); "now" for both times only needs write permission, as in POSIX. */
static long sys_utimes(TrapFrame *tf) {
    char *path = kmalloc(PATH_MAX);
    if (!path) return -ENOMEM;
    long rc = 0;
    int n = strncpy_from_user(path, (const char *)A0(tf), PATH_MAX);
    if (n < 0) rc = n;
    else if (n == 0) rc = -ENOENT;
    struct gtimespec ts[2];
    bool have_times = A1(tf) != 0;
    if (!rc && have_times && copy_from_user(ts, (void *)A1(tf), sizeof ts)) rc = -EFAULT;
    if (!rc) {
        Process *p = current->proc;
        VNode *v;
        rc = vfs_lookup(path, p->cwd, &p->cred, (A2(tf) & 1) ? LOOKUP_NOFOLLOW : 0, &v);
        if (rc == 0) {
            u64 now = time_now_sec();
            bool want_now = !have_times || (ts[0].tv_nsec == UTIME_NOW && ts[1].tv_nsec == UTIME_NOW);
            if (p->cred.uid != 0 && p->cred.uid != v->uid) rc = want_now ? vfs_access(v, &p->cred, W_OK) : -EPERM;
            if (rc == 0) {
                u64 old_atime = v->atime, old_mtime = v->mtime, old_ctime = v->ctime;
                if (!have_times) {
                    v->atime = v->mtime = now;
                } else {
                    for (int i = 0; i < 2; i++) {
                        u64 *dst = i == 0 ? &v->atime : &v->mtime;
                        if (ts[i].tv_nsec == UTIME_OMIT) continue;
                        *dst = ts[i].tv_nsec == UTIME_NOW ? now : (ts[i].tv_sec < 0 ? 0 : (u64)ts[i].tv_sec);
                    }
                }
                v->ctime = now;
                if (v->ops->settimes) {                                                       /* filesystems that keep their own copy */
                    rc = v->ops->settimes(v, v->atime, v->mtime);
                    if (rc) { v->atime = old_atime; v->mtime = old_mtime; v->ctime = old_ctime; }   /* it did not stick (read-only volume) */
                }
            }
            vnode_put(v);
        }
    }
    kfree(path);
    return rc;
}

static void utimes_init(void) { syscall_register(SYS_utimes, sys_utimes); }
INITCALL(utimes_init);
