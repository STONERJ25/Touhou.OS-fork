/*
 * User and group id calls of the Linux personality that the native system calls do not have in this form (docs/COMPAT.md): setreuid, setregid, setresuid,
 * getresuid, setresgid, getresgid. setuid, setgid, getuid, geteuid, getgid, getegid, getgroups and setgroups are the native calls.
 *
 * A Linux thread has credentials of its own, and the C library keeps them equal by sending its internal signal 33 to every thread, each of which makes the system
 * call itself (the set-id broadcast of glibc: tests/linux/src/glibc_setxid.c). Here the credentials belong to the process, so the calls act once and the
 * broadcast finds nothing left to do; what matters is that every one of these calls exists, because glibc's seteuid, setegid and the rest are built on them.
 * There is no saved set-user-ID: the saved id reads as the effective one and is not checked, and a program that is not privileged may only take an id it has
 * (its real or its effective one) -- the rule of setresuid(2) without the saved id. Written from the manual pages.
 */
#include "linux.h"
#include "kernel.h"
#include "numbers.h"
#include "calls.h"

#define KEEP ((u32)-1)                                  /* "leave this id alone" */

static inline Process *me(void) { return current->proc; }

static bool may_take(u32 current_effective, u32 real, u32 id) { return id == real || id == current_effective; }

/* setresuid(r, e, s) and setreuid(r, e) (s = KEEP): root sets anything, anybody else only ids it has. */
static long set_uids(u32 r, u32 e, u32 s) {
    Process *p = me();
    if (p->cred.uid != 0) {
        u32 want[3] = { r, e, s };
        for (int i = 0; i < 3; i++)
            if (want[i] != KEEP && !may_take(p->cred.uid, p->ruid, want[i])) return -EPERM;
    }
    if (r != KEEP) p->ruid = r;
    if (e != KEEP) p->cred.uid = e;
    return 0;
}

static long set_gids(u32 r, u32 e, u32 s) {
    Process *p = me();
    if (p->cred.uid != 0) {                             /* the right to change group ids is the right of root (CAP_SETGID), not a matter of the group id itself */
        u32 want[3] = { r, e, s };
        for (int i = 0; i < 3; i++)
            if (want[i] != KEEP && !may_take(p->cred.gid, p->rgid, want[i])) return -EPERM;
    }
    if (r != KEEP) p->rgid = r;
    if (e != KEEP) p->cred.gid = e;
    return 0;
}

long lnx_setresuid(TrapFrame *tf) { return set_uids((u32)A0(tf), (u32)A1(tf), (u32)A2(tf)); }
long lnx_setreuid(TrapFrame *tf) { return set_uids((u32)A0(tf), (u32)A1(tf), KEEP); }
long lnx_setresgid(TrapFrame *tf) { return set_gids((u32)A0(tf), (u32)A1(tf), (u32)A2(tf)); }
long lnx_setregid(TrapFrame *tf) { return set_gids((u32)A0(tf), (u32)A1(tf), KEEP); }

/* getresuid(&r, &e, &s) and getresgid: the saved id is reported as the effective one. */
static long get_ids(TrapFrame *tf, u32 real, u32 effective) {
    u32 v[3] = { real, effective, effective };
    for (int i = 0; i < 3; i++)
        if (copy_to_user((void *)(i == 0 ? A0(tf) : i == 1 ? A1(tf) : A2(tf)), &v[i], sizeof v[i])) return -EFAULT;
    return 0;
}

long lnx_getresuid(TrapFrame *tf) { return get_ids(tf, me()->ruid, me()->cred.uid); }
long lnx_getresgid(TrapFrame *tf) { return get_ids(tf, me()->rgid, me()->cred.gid); }
