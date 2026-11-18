/* rlimtest: getrlimit, setrlimit and prlimit as a program sees them (docs/HARDENING.md, "Resource limits"; kernel/sys/rlimit.c).
 *
 * Installed as /usr/lib/kernel-tests/rlimtest and run by tests/case_hardening2.py at the shell as an ordinary user (started as root it gives its privileges up first: root
 * is exempt from the process limit and may raise hard limits, which is not what is tested here). Every check that changes a limit runs in a child of its own, so the
 * limits of this process stay as they were; a child exits 0 when all its checks held and prints what did not.
 *
 *   defaults        the values a fresh process has, and the answers to bad requests (EINVAL for a resource that does not exist, for a soft limit above the hard one)
 *   hard            lowering a hard limit sticks (EPERM when it is raised again); the soft limit may move anywhere up to the hard one
 *   nofile          EMFILE at the soft limit, EBADF from dup2 beyond it, the table usable again after a close; inheritance across fork and execve
 *   nproc           EAGAIN from fork at the limit, in the child that set it and below it in the children it makes
 *   as, data        mmap, sbrk and malloc fail with ENOMEM / NULL beyond the limit, work again after munmap; a read-only map does not count as data
 *   stack           a stack deeper than the limit ends the program with SIGSEGV, one inside it does not; the default (8 MiB) allows deep recursion
 *   prlimit         another process of the same user can be given a limit (and show it back), init's is out of reach (EPERM), a pid that does not exist is ESRCH
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define MIB (1024UL * 1024)

static const char *g_self = "/usr/lib/kernel-tests/rlimtest";
static int g_fail;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno); g_fail++; } } while (0)

static int set(int res, rlim_t cur, rlim_t max) {
    struct rlimit r = { cur, max };
    return setrlimit(res, &r);
}

static rlim_t cur_of(int res) { struct rlimit r; return getrlimit(res, &r) ? (rlim_t)-2 : r.rlim_cur; }
static rlim_t max_of(int res) { struct rlimit r; return getrlimit(res, &r) ? (rlim_t)-2 : r.rlim_max; }

/* Runs fn in a child; returns its exit status, or 1000 + the signal that ended it. */
static int in_child(int (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork (errno %d)\n", errno); return -1; }
    if (pid == 0) { g_fail = 0; fn(); fflush(stdout); _exit(g_fail ? 1 : 0); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}

/* ---------------------------------------------------------------- defaults and the rules */

static int t_defaults(void) {
    struct rlimit r;
    for (int i = 0; i < RLIM_NLIMITS; i++) CHECK(getrlimit(i, &r) == 0 && r.rlim_cur <= r.rlim_max);
    errno = 0;
    CHECK(getrlimit(RLIM_NLIMITS, &r) == -1 && errno == EINVAL);
    CHECK(getrlimit(-1, &r) == -1 && errno == EINVAL);
    CHECK(cur_of(RLIMIT_NOFILE) == 64 && max_of(RLIMIT_NOFILE) == 64);
    CHECK(cur_of(RLIMIT_NPROC) == 128 && max_of(RLIMIT_NPROC) == 128);
    CHECK(cur_of(RLIMIT_STACK) == 8 * MIB && max_of(RLIMIT_STACK) == RLIM_INFINITY);
    CHECK(cur_of(RLIMIT_CORE) == 0 && max_of(RLIMIT_CORE) == RLIM_INFINITY);
    CHECK(cur_of(RLIMIT_AS) == RLIM_INFINITY && max_of(RLIMIT_AS) == RLIM_INFINITY);
    CHECK(cur_of(RLIMIT_DATA) == RLIM_INFINITY);
    errno = 0;
    CHECK(set(RLIMIT_AS, 100, 50) == -1 && errno == EINVAL);                      /* soft above hard */
    errno = 0;
    CHECK(setrlimit(RLIMIT_AS, NULL) == -1 && errno == EFAULT);
    CHECK(getrlimit(RLIMIT_AS, (struct rlimit *)8) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(set(RLIMIT_NOFILE, 64, 65) == -1 && errno == EPERM);                    /* more descriptors than the table has */
    return g_fail;
}

static int t_hard(void) {
    CHECK(set(RLIMIT_CORE, 100, 200) == 0);                                       /* lowering the hard limit from "unlimited" */
    errno = 0;
    CHECK(set(RLIMIT_CORE, 100, 300) == -1 && errno == EPERM);                    /* ... and it cannot go up again */
    errno = 0;
    CHECK(set(RLIMIT_CORE, 100, RLIM_INFINITY) == -1 && errno == EPERM);
    CHECK(set(RLIMIT_CORE, 200, 200) == 0 && cur_of(RLIMIT_CORE) == 200);         /* the soft one may rise to the hard one */
    CHECK(set(RLIMIT_CORE, 0, 200) == 0 && cur_of(RLIMIT_CORE) == 0);             /* and fall */
    errno = 0;
    CHECK(set(RLIMIT_CORE, 201, 201) == -1 && errno == EPERM);
    CHECK(set(RLIMIT_CORE, 50, 100) == 0 && max_of(RLIMIT_CORE) == 100);          /* the hard limit may be lowered again */
    struct rlimit old, nw = { 7, 9 };
    CHECK(prlimit(0, RLIMIT_CPU, &nw, &old) == 0 && old.rlim_cur == RLIM_INFINITY);      /* the old value comes back with the change */
    CHECK(prlimit(0, RLIMIT_CPU, NULL, &old) == 0 && old.rlim_cur == 7 && old.rlim_max == 9);
    CHECK(prlimit(0, RLIMIT_CPU, &nw, NULL) == 0);
    return g_fail;
}

/* ---------------------------------------------------------------- descriptors */

static int t_nofile(void) {
    int open_now = 0;                                                             /* descriptors already open below the limit we are about to set */
    for (int i = 0; i < 16; i++) if (fcntl(i, F_GETFD) >= 0) open_now++;
    CHECK(set(RLIMIT_NOFILE, 16, 16) == 0);
    int fds[64], n = 0;
    for (; n < 64; n++) { fds[n] = open("/dev/null", O_RDONLY); if (fds[n] < 0) break; }
    CHECK(errno == EMFILE);
    CHECK(n == 16 - open_now);                                                    /* exactly the free slots below the limit */
    errno = 0;
    CHECK(dup2(0, 20) == -1 && errno == EBADF);                                   /* beyond the limit */
    CHECK(dup2(0, 15) == 15);                                                     /* inside it (the last slot) */
    int p[2];
    errno = 0;
    CHECK(pipe(p) == -1 && errno == EMFILE);                                      /* no room for two */
    close(fds[0]);
    close(fds[1]);
    CHECK(pipe(p) == 0);                                                          /* two slots are free again */
    CHECK(set(RLIMIT_NOFILE, 8, 16) == 0 && cur_of(RLIMIT_NOFILE) == 8);          /* the soft limit lowered under the open ones: nothing is closed, nothing new opens */
    errno = 0;
    CHECK(open("/dev/null", O_RDONLY) == -1 && errno == EMFILE);
    CHECK(set(RLIMIT_NOFILE, 16, 16) == 0);
    for (int i = 2; i < n; i++) close(fds[i]);
    close(p[0]);
    close(p[1]);
    CHECK(open("/dev/null", O_RDONLY) >= 0);
    return g_fail;
}

static int t_nofile_child_exec(void) {                                            /* the limit set before a fork, seen after it and after an execve */
    CHECK(set(RLIMIT_NOFILE, 9, 12) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        if (cur_of(RLIMIT_NOFILE) != 9 || max_of(RLIMIT_NOFILE) != 12) _exit(3);   /* inherited across fork */
        execl(g_self, "rlimtest", "--probe-nofile", (char *)NULL);                /* and across execve */
        _exit(4);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return g_fail;
}

/* ---------------------------------------------------------------- processes */

static int t_nproc(void) {
    CHECK(set(RLIMIT_NPROC, 1, 1) == 0);                                          /* the user already has more than one process (this one, at least) */
    errno = 0;
    pid_t pid = fork();
    if (pid == 0) _exit(0);
    CHECK(pid == -1 && errno == EAGAIN);
    errno = 0;
    CHECK(set(RLIMIT_NPROC, 2, 2) == -1 && errno == EPERM);                       /* the hard limit stays down */
    return g_fail;
}

/* ---------------------------------------------------------------- memory */

static int t_as(void) {
    CHECK(set(RLIMIT_AS, 48 * MIB, 48 * MIB) == 0);
    void *a = mmap(NULL, 16 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *b = mmap(NULL, 16 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED);
    errno = 0;
    void *c = mmap(NULL, 32 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(c == MAP_FAILED && errno == ENOMEM);
    errno = 0;
    void *c2 = mmap(NULL, 32 * MIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   /* PROT_NONE counts too: it is address space */
    CHECK(c2 == MAP_FAILED && errno == ENOMEM);
    errno = 0;
    CHECK(malloc(64 * MIB) == NULL);
    CHECK(sbrk(64 * MIB) == (void *)-1);
    munmap(a, 16 * MIB);
    munmap(b, 16 * MIB);
    void *d = mmap(NULL, 40 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);     /* the space is free again */
    CHECK(d != MAP_FAILED);
    unsigned char *q = d;
    q[0] = 1; q[40 * MIB - 1] = 2;                                                /* a demand-zero map is usable */
    CHECK(q[0] == 1 && q[40 * MIB - 1] == 2);
    errno = 0;
    CHECK(set(RLIMIT_AS, 64 * MIB, 64 * MIB) == -1 && errno == EPERM);            /* nor may the hard one go up */
    return g_fail;
}

static int t_data(void) {
    CHECK(set(RLIMIT_DATA, 8 * MIB, RLIM_INFINITY) == 0);
    void *a = mmap(NULL, 4 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(a != MAP_FAILED);
    errno = 0;
    void *b = mmap(NULL, 8 * MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(b == MAP_FAILED && errno == ENOMEM);
    void *c = mmap(NULL, 32 * MIB, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   /* nothing writable: not data */
    CHECK(c != MAP_FAILED);
    return g_fail;
}

/* Recursion that really uses 4 KiB of stack per level: the address of the frame escapes into an empty asm statement, so the compiler cannot shrink the array to the two
 * bytes that are read (it did, when the array was only volatile: a frame of 32 bytes, and no stack test at all). */
static int deep(int levels) {
    char frame[4096];
    frame[0] = (char)levels;
    frame[4095] = (char)levels;
    __asm__ volatile ("" : : "r"(frame) : "memory");
    int r = levels > 0 ? deep(levels - 1) : 0;
    __asm__ volatile ("" : : "r"(frame) : "memory");
    return r + frame[0] - frame[4095] + 1;
}

static int t_stack_inside(void) { CHECK(set(RLIMIT_STACK, 512 * 1024, RLIM_INFINITY) == 0); CHECK(deep(60) > 0); return g_fail; }       /* 240 KiB of frames in 512 KiB */
static int t_stack_over(void)   { CHECK(set(RLIMIT_STACK, 128 * 1024, RLIM_INFINITY) == 0); deep(100); return 0; }                    /* 400 KiB in 128 KiB: SIGSEGV */
static int t_stack_default(void) { CHECK(deep(1500) > 0); return g_fail; }                                                            /* 6 MiB in the default 8 */

/* ---------------------------------------------------------------- prlimit on another process */

static int t_prlimit(void) {
    int up[2], down[2];
    CHECK(pipe(up) == 0 && pipe(down) == 0);
    pid_t kid = fork();
    if (kid == 0) {
        char c;
        read(down[0], &c, 1);                                                     /* wait until the parent has set the limit */
        rlim_t cur = cur_of(RLIMIT_NOFILE);
        write(up[1], &cur, sizeof cur);
        _exit(0);
    }
    struct rlimit nw = { 11, 11 }, old;
    CHECK(prlimit(kid, RLIMIT_NOFILE, &nw, &old) == 0 && old.rlim_cur == 64);
    write(down[1], "x", 1);
    rlim_t seen = 0;
    CHECK(read(up[0], &seen, sizeof seen) == (ssize_t)sizeof seen && seen == 11);
    waitpid(kid, NULL, 0);
    errno = 0;
    CHECK(prlimit(1, RLIMIT_NOFILE, &nw, NULL) == -1 && errno == EPERM);          /* init belongs to root */
    errno = 0;
    CHECK(prlimit(1, RLIMIT_NOFILE, NULL, &old) == -1 && errno == EPERM);
    errno = 0;
    CHECK(prlimit(99999, RLIMIT_NOFILE, &nw, NULL) == -1 && errno == ESRCH);
    errno = 0;
    CHECK(prlimit(-5, RLIMIT_NOFILE, &nw, NULL) == -1 && (errno == ESRCH || errno == EINVAL));
    return g_fail;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--probe-nofile"))                          /* the execve'd copy of t_nofile_child_exec */
        return cur_of(RLIMIT_NOFILE) == 9 && max_of(RLIMIT_NOFILE) == 12 ? 0 : 5;
    if (getuid() == 0) { setgid(5000); setuid(5000); }
    printf("rlimtest: resource limits begin\n");
    struct { const char *name; int (*fn)(void); int expect; } tests[] = {
        { "defaults", t_defaults, 0 }, { "hard", t_hard, 0 }, { "nofile", t_nofile, 0 }, { "nofile fork+exec", t_nofile_child_exec, 0 },
        { "nproc", t_nproc, 0 }, { "as", t_as, 0 }, { "data", t_data, 0 }, { "stack inside", t_stack_inside, 0 },
        { "stack over", t_stack_over, 1000 + SIGSEGV }, { "stack default", t_stack_default, 0 }, { "prlimit", t_prlimit, 0 },
    };
    int failed = 0;
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int rc = in_child(tests[i].fn);
        printf("  %-18s %s\n", tests[i].name, rc == tests[i].expect ? "ok" : "FAILED");
        if (rc != tests[i].expect) { printf("    (status %d, wanted %d)\n", rc, tests[i].expect); failed++; }
    }
    printf(failed ? "RLIMTEST FAIL\n" : "RLIMTEST PASS\n");
    return failed ? 1 : 0;
}
