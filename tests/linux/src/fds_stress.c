/* Stress for the growing descriptor table (docs/FDS_REVIEW.md, finding 1): a process whose table is still the small one starts threads that all push it from 64
 * slots to 4096 at the same moment (dup2 and F_DUPFD to staggered high numbers), while they open, read flags, set close-on-exec, close and make pipes, and while
 * the main thread forks children that check an inherited high descriptor and exit. The growth happens once in the life of a table, so the whole thing is repeated
 * in fresh processes (the first process never grows its own). Every worker counts its mistakes and its open descriptors before and after; the output is the same
 * few facts on any system that gets it right. Written from dup(2), fcntl(2), fork(2), pthreads(7) and getrlimit(2). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS  6
#define LOOPS   150

static int g_errors;
static pthread_barrier_t g_start;

static void bad(void) { __sync_fetch_and_add(&g_errors, 1); }

static int count_open(void) {
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d);
    return n - 1;                                   /* the directory's own descriptor */
}

/* Thread k owns the numbers base .. base+899 (above every number the lowest-free rule hands out here), so nobody closes another thread's descriptor. */
static void *churn(void *arg) {
    int k = (int)(long)arg, base = 400 + k * 900;
    pthread_barrier_wait(&g_start);
    for (int i = 0; i < LOOPS; i++) {
        int fd = open("/dev/null", O_RDWR);
        if (fd < 0) { bad(); continue; }
        /* the first rounds climb through every table size: 64, 128, ... 4096, each thread at a different pace */
        int hi = i < 12 ? (48 << (i / 2 + (k & 1))) + k + 400 : base + (i * 37) % 900;
        if (hi >= 4000) hi = base + i;
        if (hi < base || hi >= base + 900) hi = base + i;
        if (dup2(fd, hi) != hi) bad();
        if (fcntl(hi, F_SETFD, FD_CLOEXEC)) bad();
        if (fcntl(hi, F_GETFD) != FD_CLOEXEC) bad();
        if (fcntl(fd, F_GETFD) != 0) bad();
        int pp[2];
        if (pipe2(pp, O_CLOEXEC)) bad();
        else {
            if (fcntl(pp[0], F_GETFD) != FD_CLOEXEC || fcntl(pp[1], F_GETFD) != FD_CLOEXEC) bad();
            char ch = 'q';
            if (write(pp[1], &ch, 1) != 1 || read(pp[0], &ch, 1) != 1) bad();
            close(pp[0]);
            close(pp[1]);
        }
        int d = fcntl(fd, F_DUPFD_CLOEXEC, base + 450 + i % 400);
        if (d < base + 450 || fcntl(d, F_GETFD) != FD_CLOEXEC) bad();
        if (d >= 0 && d != hi) close(d);
        if (write(hi, "x", 1) != 1) bad();
        if (close(hi)) bad();
        if (close(fd)) bad();
    }
    return NULL;
}

static int worker(void) {
    int before = count_open();
    pthread_t th[THREADS];
    pthread_barrier_init(&g_start, NULL, THREADS + 1);
    for (long k = 0; k < THREADS; k++) if (pthread_create(&th[k], NULL, churn, (void *)k)) return 100;
    pthread_barrier_wait(&g_start);
    int forks = 0;
    for (int i = 0; i < 6; i++) {                   /* children made while the table grows: each sees a consistent copy */
        int mine = 300 + i;
        int fd = open("/dev/null", O_RDWR);
        if (dup2(fd, mine) != mine) bad();
        close(fd);
        pid_t c = fork();
        if (!c) _exit(write(mine, "y", 1) == 1 && fcntl(mine, F_GETFD) == 0 ? 0 : 9);
        int st = 0;
        if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st)) bad(); else forks++;
        close(mine);
    }
    for (int k = 0; k < THREADS; k++) pthread_join(th[k], NULL);
    int after = count_open();
    if (before != after) return 101;
    if (forks != 6) return 102;
    return g_errors ? 103 : 0;
}

int main(void) {
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
    if (setrlimit(RLIMIT_NOFILE, &rl)) { printf("setrlimit failed\n"); return 1; }
    int before = count_open(), good = 0;
    for (int r = 0; r < ROUNDS; r++) {
        pid_t c = fork();
        if (!c) _exit(worker());
        int st = 0;
        waitpid(c, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) good++;
        else printf("round %d: status %#x\n", r, (unsigned)st);
    }
    printf("rounds without a mistake: %d of %d\n", good, ROUNDS);
    printf("descriptors of the first process before and after equal: %d\n", before == count_open());
    return 0;
}
