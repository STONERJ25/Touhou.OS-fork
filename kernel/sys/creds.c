/* Supplementary groups: getgroups / setgroups. */
#include "process.h"
#include "kernel.h"
#include "initcall.h"

static long sys_getgroups(TrapFrame *tf) {
    Cred *c = &current->proc->cred;
    long want = (long)A0(tf);
    if (want == 0) return (long)c->ngroups;
    if ((u32)want < c->ngroups) return -EINVAL;
    if (c->ngroups && copy_to_user((void *)A1(tf), c->groups, c->ngroups * sizeof(u32))) return -EFAULT;
    return (long)c->ngroups;
}

static long sys_setgroups(TrapFrame *tf) {
    Cred *c = &current->proc->cred;
    if (c->uid != 0) return -EPERM;
    u64 n = A0(tf);
    if (n > NGROUPS_MAX) return -EINVAL;
    u32 tmp[NGROUPS_MAX];
    if (n && copy_from_user(tmp, (const void *)A1(tf), n * sizeof(u32))) return -EFAULT;
    memcpy(c->groups, tmp, n * sizeof(u32));
    c->ngroups = (u32)n;
    return 0;
}

static void creds_init(void) {
    syscall_register(SYS_getgroups, sys_getgroups);
    syscall_register(SYS_setgroups, sys_setgroups);
}
INITCALL(creds_init);
