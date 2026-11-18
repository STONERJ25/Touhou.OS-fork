/* oomtest: the programs behind the out-of-memory and resource-limit tests (docs/HARDENING.md, "The OOM killer"; tests/case_hardening2.py).
 *
 * Installed as /usr/lib/kernel-tests/oomtest, run by the tests at the shell as an ordinary user (or as root through kerneltest=oomtest ktargs=mode:hog: a hog
 * started as root gives up its privileges first, so that it is an ordinary victim, and a fork bomb that is root would never meet the process limit).
 *
 *   oomtest hog [CHUNK_KiB] [DELAY_ms]   map CHUNK_KiB (default 1024) at a time and touch every page until the kernel ends the program; DELAY_ms between chunks (default 0)
 *   oomtest work SECONDS                 do honest work that needs fresh memory all the time (map, fill, check, unmap): prints OOMWORK PASS if every round was right
 *   oomtest forkbomb [LIMIT]             fork children that sleep until fork fails; the failure must be EAGAIN, and the count must not pass LIMIT if one is given
 *   oomtest tmpfill [DIR]                write 1 MiB blocks to a file in DIR (default /tmp) until the write fails; the failure must be ENOSPC; then delete the file
 *   oomtest fdleak                       open /dev/null until it fails; the failure must be EMFILE; then close everything and open again
 *   oomtest forkfat MiB CHILDREN         map and touch MiB, then fork CHILDREN children that sleep: fork copies every page, so the machine runs out of memory in the middle
 *                                        of a fork; the killer must end children (the youngest of equal size), the parent must live and get its children
 *   oomtest hogthreads N                 a hog with N more threads, all mapping and touching: the victim is one process with several threads, and all of them must go
 *
 * Every mode but hog and hogthreads prints OOMTEST PASS or OOMTEST FAIL and exits 0 or 1. (They never end by themselves.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define MIB (1024UL * 1024)

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(1, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static void drop_privileges(void) {
    if (getuid() == 0) { setgid(5000); setuid(5000); }
}

/* ---------------------------------------------------------------- hog */

static int hog(long chunk_kib, long delay_ms) {
    drop_privileges();
    size_t chunk = (size_t)chunk_kib * 1024;
    size_t total = 0, next_report = 8 * MIB;
    printf("hog: starts, %ld KiB at a time\n", chunk_kib);
    fflush(stdout);
    for (;;) {
        unsigned char *p = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { printf("hog: mmap failed (errno %d) after %zu MiB\n", errno, total / MIB); fflush(stdout); sleep_ms(200); continue; }
        for (size_t o = 0; o < chunk; o += 4096) p[o] = (unsigned char)(o >> 12);
        total += chunk;
        if (total >= next_report) { printf("hog: %zu MiB\n", total / MIB); fflush(stdout); next_report += 16 * MIB; }
        if (delay_ms) sleep_ms(delay_ms);
    }
}

/* ---------------------------------------------------------------- hog with threads */

static void *hog_thread(void *arg) {
    (void)arg;
    for (;;) {
        unsigned char *p = mmap(NULL, MIB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { sleep_ms(50); continue; }
        for (size_t o = 0; o < MIB; o += 4096) p[o] = 1;
    }
    return NULL;
}

/* One process, several threads that all fault in fresh memory: the victim is the process, so every thread has to go (the one that was picked, the ones that fault while it dies, the one
 * that is asleep), and the kernel stacks and the thread count must come back. */
static int hogthreads(long n) {
    drop_privileges();
    pthread_t t[8];
    if (n > 8) n = 8;
    for (long i = 0; i < n; i++) pthread_create(&t[i], NULL, hog_thread, NULL);
    printf("hogthreads: %ld threads besides the first start\n", n);
    fflush(stdout);
    hog_thread(NULL);
    return 0;
}

/* ---------------------------------------------------------------- work */

/* Rounds of: map 64 KiB, fill it with a pattern that depends on the round, read it back and check, unmap. The memory is fresh every time, so whenever the machine is short of
 * it this program is the one whose page fault meets the reserve: it has to be made to wait for the killer's victim, and live. */
static int work(long secs) {
    drop_privileges();
    long end = now_ms() + secs * 1000;
    long rounds = 0, bad = 0;
    while (now_ms() < end) {
        size_t len = 64 * 1024;
        unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { bad++; printf("  work: mmap failed (errno %d)\n", errno); sleep_ms(20); continue; }
        for (size_t o = 0; o < len; o++) p[o] = (unsigned char)(o * 7 + rounds);
        for (size_t o = 0; o < len; o++) if (p[o] != (unsigned char)(o * 7 + rounds)) { bad++; break; }
        munmap(p, len);
        rounds++;
        if (rounds % 64 == 0) sleep_ms(1);
    }
    printf("work: %ld rounds, %ld bad\n", rounds, bad);
    printf(bad || rounds == 0 ? "OOMWORK FAIL\n" : "OOMWORK PASS\n");
    return bad || rounds == 0;
}

/* ---------------------------------------------------------------- fork bomb */

static int forkbomb(long limit) {
    drop_privileges();
    pid_t kids[1024];
    int n = 0, err = 0;
    for (; n < 1024; n++) {
        pid_t pid = fork();
        if (pid < 0) { err = errno; break; }
        if (pid == 0) { for (;;) pause(); }
        kids[n] = pid;
    }
    printf("forkbomb: %d children, fork then failed with errno %d (%s)\n", n, err, err == EAGAIN ? "EAGAIN" : "other");
    for (int i = 0; i < n; i++) kill(kids[i], SIGKILL);
    for (int i = 0; i < n; i++) waitpid(kids[i], NULL, 0);
    pid_t again = fork();                                   /* with the children gone, forking works again */
    if (again == 0) _exit(0);
    int ok = again > 0 && waitpid(again, NULL, 0) == again;
    int fail = 0;
    if (err != EAGAIN) { printf("  FAIL: the failure was not EAGAIN\n"); fail = 1; }
    if (limit && n >= limit) { printf("  FAIL: %d children, the limit was %ld\n", n, limit); fail = 1; }
    if (n < 2) { printf("  FAIL: too few children\n"); fail = 1; }
    if (!ok) { printf("  FAIL: fork did not work again after the children were gone\n"); fail = 1; }
    printf(fail ? "OOMTEST FAIL\n" : "OOMTEST PASS\n");
    return fail;
}

/* ---------------------------------------------------------------- fork of a big process */

/* fork copies the whole address space, so a big process that keeps forking uses up the machine from inside fork itself: the allocation that fails is the parent's, in the
 * middle of copying, and the victim is one of the children it made (the youngest of equal size), not the parent. The parent waits for the victim's memory and goes on. */
static int forkfat(long mib, long want) {
    drop_privileges();
    size_t len = (size_t)mib * MIB;
    unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { printf("  FAIL: mmap of %ld MiB (errno %d)\n", mib, errno); return 1; }
    for (size_t o = 0; o < len; o += 4096) p[o] = (unsigned char)(o >> 12);
    pid_t kids[1024];
    int nk = 0, created = 0, ended_by_killer = 0, failures = 0;
    long end = now_ms() + 60000;
    while (created < want && nk < 1024 && now_ms() < end) {
        pid_t pid = fork();
        if (pid == 0) { for (;;) pause(); }
        if (pid < 0) { failures++; sleep_ms(20); }
        else { kids[nk++] = pid; created++; }
        int st;
        pid_t r;
        while ((r = waitpid(-1, &st, WNOHANG)) > 0) {                       /* a child that died while we were forking: the killer's */
            if (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL) ended_by_killer++;
            for (int i = 0; i < nk; i++) if (kids[i] == r) kids[i] = 0;
        }
    }
    for (int i = 0; i < nk; i++) if (kids[i]) kill(kids[i], SIGKILL);
    while (waitpid(-1, NULL, 0) > 0) { }
    int intact = 1;
    for (size_t o = 0; o < len; o += 4096) if (p[o] != (unsigned char)(o >> 12)) { intact = 0; break; }
    printf("forkfat: %d children made of %ld MiB each, %d ended by the killer, %d forks failed, the parent's memory is %s\n", created, mib, ended_by_killer, failures,
           intact ? "intact" : "DAMAGED");
    int fail = created < want || !intact;
    if (created < want) printf("  FAIL: only %d of %ld children\n", created, want);
    printf(fail ? "OOMTEST FAIL\n" : "OOMTEST PASS\n");
    return fail;
}

/* ---------------------------------------------------------------- tmpfs filler */

static int tmpfill(const char *dir) {
    drop_privileges();
    char path[128];
    snprintf(path, sizeof path, "%s/oomtest-fill.%d", dir, (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { printf("  FAIL: cannot create %s (errno %d)\n", path, errno); return 1; }
    static char block[MIB];
    memset(block, 0x5a, sizeof block);
    long blocks = 0;
    int err = 0;
    for (;;) {
        ssize_t w = write(fd, block, sizeof block);
        if (w < 0) { err = errno; break; }
        if (w == 0) { err = ENOSPC; break; }
        blocks += w / (long)MIB;                              /* a short write at the limit counts for nothing here; the next call then fails */
        if (blocks > 100000) break;
    }
    printf("tmpfill: %ld MiB written, then errno %d (%s)\n", blocks, err, err == ENOSPC ? "ENOSPC" : "other");
    close(fd);
    unlink(path);
    int fail = err != ENOSPC || blocks < 1;
    printf(fail ? "OOMTEST FAIL\n" : "OOMTEST PASS\n");
    return fail;
}

/* ---------------------------------------------------------------- descriptor leak */

static int fdleak(void) {
    int fds[256], n = 0, err = 0;
    for (; n < 256; n++) {
        fds[n] = open("/dev/null", O_RDONLY);
        if (fds[n] < 0) { err = errno; break; }
    }
    printf("fdleak: %d descriptors, then errno %d (%s)\n", n, err, err == EMFILE ? "EMFILE" : "other");
    for (int i = 0; i < n; i++) close(fds[i]);
    int again = open("/dev/null", O_RDONLY);
    int fail = err != EMFILE || again < 0 || n < 3;
    if (again >= 0) close(again);
    printf(fail ? "OOMTEST FAIL\n" : "OOMTEST PASS\n");
    return fail;
}

/* ---------------------------------------------------------------- main */

/* init starts a kerneltest as `NAME --config k:v,k:v`; the shell runs it as `oomtest MODE args`. Both are understood. */
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    long a = argc > 2 ? atol(argv[2]) : 0, b = argc > 3 ? atol(argv[3]) : 0;
    if (!strcmp(mode, "--config") && argc > 2) {
        char *m = strstr(argv[2], "mode:");
        mode = m ? m + 5 : "hog";
        char *e = strchr(mode, ',');
        if (e) *e = 0;
        a = b = 0;
    }
    if (!strcmp(mode, "hog")) return hog(a ? a : 1024, b);
    if (!strcmp(mode, "work")) return work(a ? a : 10);
    if (!strcmp(mode, "forkbomb")) return forkbomb(a);
    if (!strcmp(mode, "tmpfill")) return tmpfill(argc > 2 ? argv[2] : "/tmp");
    if (!strcmp(mode, "fdleak")) return fdleak();
    if (!strcmp(mode, "forkfat")) return forkfat(a ? a : 16, b ? b : 24);
    if (!strcmp(mode, "hogthreads")) return hogthreads(a ? a : 3);
    fprintf(stderr, "usage: oomtest hog [KiB [ms]] | work SECONDS | forkbomb [LIMIT] | tmpfill [DIR] | fdleak | forkfat MiB CHILDREN | hogthreads N\n");
    return 2;
}
