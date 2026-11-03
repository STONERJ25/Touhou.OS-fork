/* Storage-related system calls: statfs() and mounting with options. */
#include "syscall_priv.h"
#include <sys/statfs.h>
#include <gensokyo.h>

int statfs(const char *path, struct statfs *buf) {
    return (int)__ret(__sc2(SYS_statfs, (long)path, (long)buf));
}

/* mount(2) with a comma-separated option string ("uid=1000,fmask=0133,force"); opts may be NULL. */
int gap_mount_opts(const char *src, const char *target, const char *fstype, unsigned long flags, const char *opts) {
    if (!opts) return gap_mount(src, target, fstype, flags);
    return (int)__ret(__sc6(SYS_mount, (long)src, (long)target, (long)fstype, (long)(flags | MNT_OPTS), (long)opts, 0));
}
