/* getrlimit, setrlimit, prlimit: resource limits (kernel/sys/rlimit.c, docs/HARDENING.md). */
#include "syscall_priv.h"
#include <sys/resource.h>

int getrlimit(int resource, struct rlimit *rlim) { return (int)__ret(__sc2(SYS_getrlimit, resource, (long)rlim)); }

int setrlimit(int resource, const struct rlimit *rlim) { return (int)__ret(__sc2(SYS_setrlimit, resource, (long)rlim)); }

int prlimit(pid_t pid, int resource, const struct rlimit *new_limit, struct rlimit *old_limit) {
    return (int)__ret(__sc4(SYS_prlimit, pid, resource, (long)new_limit, (long)old_limit));
}
