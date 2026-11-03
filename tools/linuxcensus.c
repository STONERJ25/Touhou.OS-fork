/* linuxcensus: runs a Linux program under ptrace and counts the system calls it makes (and, for a few calls, which flag values it uses).
 *
 *   linuxcensus PROGRAM [ARGS...]            prints  S <number> <name> <count>   and   H <number> <name> <argument index> <value in hex> <count>
 *
 * A host tool for docs/COMPAT.md ("what does a Linux program ask of the kernel?"): it runs on a Linux (here WSL), follows forks, clones and execs, and
 * reads each call from the kernel with PTRACE_GET_SYSCALL_INFO. The names come from the host's own asm/unistd_64.h when it can be read (only to print them).
 * The programs it runs are test inputs: nothing of them goes into the tree. Written for this project from ptrace(2) and syscalls(2); no code from strace.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NSYS 1024
static unsigned long g_count[NSYS];
static char g_name[NSYS][40];

typedef struct { int nr, arg; unsigned long long val; unsigned long n; } Hist;
static Hist g_hist[8192];
static int g_nhist;

/* the calls whose flag-like argument is worth a histogram (x86-64 numbers) and which argument it is */
static const struct { int nr, arg; } WATCH[] = {
    { 9, 2 }, { 9, 3 },                 /* mmap: prot, flags */
    { 10, 2 },                          /* mprotect: prot */
    { 28, 2 },                          /* madvise: advice */
    { 56, 0 },                          /* clone: flags */
    { 435, 0 },                         /* clone3: (pointer: not decoded) */
    { 202, 1 },                         /* futex: op */
    { 16, 1 },                          /* ioctl: request */
    { 72, 1 },                          /* fcntl: command */
    { 157, 0 },                         /* prctl: option */
    { 158, 0 },                         /* arch_prctl: code */
    { 257, 2 },                         /* openat: flags */
    { 2, 1 },                           /* open: flags */
    { 41, 0 }, { 41, 1 },               /* socket: domain, type */
    { 228, 0 },                         /* clock_gettime: clock */
    { 302, 1 },                         /* prlimit64: resource */
    { 318, 2 },                         /* getrandom: flags */
    { 13, 0 },                          /* rt_sigaction: signal */
    { 334, 3 },                         /* rseq: flags */
};

static void note(int nr, int arg, unsigned long long val) {
    for (int i = 0; i < g_nhist; i++)
        if (g_hist[i].nr == nr && g_hist[i].arg == arg && g_hist[i].val == val) { g_hist[i].n++; return; }
    if (g_nhist < (int)(sizeof g_hist / sizeof g_hist[0])) g_hist[g_nhist++] = (Hist){ nr, arg, val, 1 };
}

static void load_names(void) {
    static const char *paths[] = { "/usr/include/x86_64-linux-gnu/asm/unistd_64.h", "/usr/include/asm/unistd_64.h", NULL };
    for (int p = 0; paths[p]; p++) {
        FILE *f = fopen(paths[p], "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char nm[64];
            int nr;
            if (sscanf(line, "#define __NR_%63s %d", nm, &nr) == 2 && nr >= 0 && nr < NSYS) snprintf(g_name[nr], sizeof g_name[nr], "%s", nm);
        }
        fclose(f);
        return;
    }
}

static const char *name_of(int nr) { return nr >= 0 && nr < NSYS && g_name[nr][0] ? g_name[nr] : "?"; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: linuxcensus PROGRAM [ARGS...]\n"); return 2; }
    load_names();
    pid_t root = fork();
    if (root < 0) { perror("fork"); return 1; }
    if (root == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execvp(argv[1], argv + 1);
        fprintf(stderr, "linuxcensus: cannot run %s: %s\n", argv[1], strerror(errno));
        _exit(127);
    }
    int status;
    waitpid(root, &status, 0);
    ptrace(PTRACE_SETOPTIONS, root, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_EXITKILL);
    ptrace(PTRACE_SYSCALL, root, 0, 0);
    int live = 1, root_status = 0;
    pid_t pid;
    while (live > 0 && (pid = waitpid(-1, &status, __WALL)) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            live--;
            if (pid == root) root_status = status;
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        int sig = WSTOPSIG(status), event = status >> 16, inject = 0;
        if (sig == (SIGTRAP | 0x80)) {
            struct __ptrace_syscall_info info;
            memset(&info, 0, sizeof info);
            if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, (void *)sizeof info, &info) > 0 && info.op == PTRACE_SYSCALL_INFO_ENTRY && info.entry.nr < NSYS) {
                g_count[info.entry.nr]++;
                for (unsigned w = 0; w < sizeof WATCH / sizeof WATCH[0]; w++)
                    if (WATCH[w].nr == (int)info.entry.nr) note(WATCH[w].nr, WATCH[w].arg, info.entry.args[WATCH[w].arg]);
            }
        } else if (sig == SIGTRAP && event) {
            if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) live++;     /* the child is traced from its first instruction */
        } else if (sig == SIGSTOP) {
            inject = 0;                                                    /* a new tracee's first stop */
        } else if (sig != SIGTRAP) {
            inject = sig;                                                  /* a signal for the program itself */
        }
        ptrace(PTRACE_SYSCALL, pid, 0, inject);
    }
    unsigned long total = 0;
    for (int i = 0; i < NSYS; i++) {
        total += g_count[i];
        if (g_count[i]) printf("S %d %s %lu\n", i, name_of(i), g_count[i]);
    }
    for (int i = 0; i < g_nhist; i++) printf("H %d %s %d %llx %lu\n", g_hist[i].nr, name_of(g_hist[i].nr), g_hist[i].arg, g_hist[i].val, g_hist[i].n);
    fprintf(stderr, "linuxcensus: %s: %lu calls, exit status %d\n", argv[1], total, WIFEXITED(root_status) ? WEXITSTATUS(root_status) : 128 + WTERMSIG(root_status));
    return 0;
}
