/* cow_test: what a copy-on-write fork must keep true (docs/COW.md; tests/case_cow.py runs it with and without "cow=off").
 *
 *   cow_test             every check below; one line "COW <name> ok" or "COW <name> FAIL ..." each, then COWTEST PASS or COWTEST FAIL
 *   cow_test pressure    touch 60% of the free memory, fork (an eager copy cannot), let the child write to every page: the machine runs out of memory in the middle of the
 *                        copies and the OOM killer ends one of the two; prints "COW pressure forked" when the fork itself worked
 *
 * The line "COW share_kb=N" says how much free memory a fork of a process with 32 MiB of touched memory costs while the child lives: page tables only with copy-on-write,
 * the whole 32 MiB with the eager copy. */
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

#define PG 4096UL
#define MIB (1024UL * 1024)

static int g_fail;
static char g_data[3 * PG] = { 1 };                /* the data segment */
static char g_bss[3 * PG];

static void result(const char *name, int ok, const char *why) {
    if (ok) printf("COW %s ok\n", name);
    else { printf("COW %s FAIL %s\n", name, why ? why : ""); g_fail++; }
    fflush(stdout);
}

static long memfree_kb(void) {
    char buf[1024];
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = strstr(buf, "MemFree:");
    return p ? atol(p + 8) : -1;
}

static int wait_status(pid_t pid) {
    int st = -1;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}

static int all_bytes(const char *p, size_t n, char v) {
    for (size_t i = 0; i < n; i++) if (p[i] != v) return 0;
    return 1;
}

/* A child writes, then the parent writes: neither sees the other's bytes. The child waits (pipe) for the parent's write before it looks again. */
static void isolate(const char *name, char *page) {
    int go[2], done[2];
    pipe(go); pipe(done);
    memset(page, 'P', PG);
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        if (!all_bytes(page, PG, 'P')) _exit(10);
        memset(page, 'C', PG);                      /* the child's write */
        write(done[1], "x", 1);
        read(go[0], &c, 1);                         /* the parent has written by now */
        _exit(all_bytes(page, PG, 'C') ? 0 : 11);
    }
    char c;
    read(done[0], &c, 1);
    int ok = all_bytes(page, PG, 'P');              /* the child's write did not reach us */
    memset(page, 'Q', PG);
    write(go[1], "x", 1);
    int st = wait_status(pid);
    ok = ok && st == 0 && all_bytes(page, PG, 'Q');
    close(go[0]); close(go[1]); close(done[0]); close(done[1]);
    char why[64];
    snprintf(why, sizeof why, "child status %d", st);
    result(name, ok, why);
}

/* The parent writes first while the child only reads afterwards: the child must still see what was there at the fork. */
static void parent_first(char *page) {
    int go[2];
    pipe(go);
    memset(page, 'A', PG);
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        read(go[0], &c, 1);
        _exit(all_bytes(page, PG, 'A') ? 0 : 12);
    }
    memset(page, 'B', PG);
    write(go[1], "x", 1);
    int st = wait_status(pid);
    close(go[0]); close(go[1]);
    result("parent_writes_first", st == 0 && all_bytes(page, PG, 'B'), "the child saw the parent's write");
}

static void leak(void) {
    for (int i = 0; i < 5; i++) { pid_t p = fork(); if (p == 0) _exit(0); wait_status(p); }       /* warm up: page tables, kernel stacks */
    long before = memfree_kb();
    int bad = 0;
    for (int i = 0; i < 200; i++) {
        pid_t p = fork();
        if (p < 0) { bad++; break; }
        if (p == 0) { g_bss[i % (3 * PG)] = (char)i; _exit(0); }
        g_bss[(i * 7) % (3 * PG)] = (char)i;        /* the parent writes too: breaks while the child may still be there */
        if (wait_status(p) != 0) bad++;
    }
    long after = memfree_kb();
    char why[96];
    snprintf(why, sizeof why, "bad=%d free before %ld kB after %ld kB", bad, before, after);
    printf("COW leak_drift_kb=%ld\n", before - after);
    result("leak_200_forks", !bad && before - after < 256 && after - before < 2048, why);
}

static void fork_exec(void) {
    int bad = 0;
    for (int i = 0; i < 100; i++) {
        pid_t p = fork();
        if (p < 0) { bad++; break; }
        if (p == 0) { execl("/bin/true", "true", (char *)NULL); _exit(99); }
        if (wait_status(p) != 0) bad++;
    }
    result("fork_exec_100", !bad, "a child did not exec /bin/true");
}

/* Parent, child and grandchild all map the same page at first; each writes its own letter and must keep it. */
static void chain(char *page) {
    memset(page, '0', PG);
    pid_t c = fork();
    if (c == 0) {
        pid_t g = fork();
        if (g == 0) {
            if (!all_bytes(page, PG, '0')) _exit(20);
            memset(page, '2', PG);
            _exit(all_bytes(page, PG, '2') ? 0 : 21);
        }
        memset(page, '1', PG);
        int st = wait_status(g);
        _exit(st ? st : all_bytes(page, PG, '1') ? 0 : 22);
    }
    int st = wait_status(c);
    int ok = st == 0 && all_bytes(page, PG, '0');
    memset(page, '9', PG);
    char why[64];
    snprintf(why, sizeof why, "status %d", st);
    result("chain", ok && all_bytes(page, PG, '9'), why);
}

/* Eight children share one page with us; each is let go in turn, writes its number over the page and checks it. */
static void many(char *page) {
    enum { N = 8 };
    int go[N][2];
    pid_t pid[N];
    memset(page, 'M', PG);
    for (int i = 0; i < N; i++) {
        pipe(go[i]);
        pid[i] = fork();
        if (pid[i] == 0) {
            char c;
            read(go[i][0], &c, 1);
            if (!all_bytes(page, PG, 'M')) _exit(30);
            memset(page, 'a' + i, PG);
            _exit(all_bytes(page, PG, (char)('a' + i)) ? 0 : 31);
        }
    }
    int bad = 0;
    for (int i = 0; i < N; i++) {
        write(go[i][1], "x", 1);
        if (wait_status(pid[i]) != 0) bad++;
        if (!all_bytes(page, PG, 'M')) bad++;
        close(go[i][0]); close(go[i][1]);
    }
    memset(page, 'Z', PG);
    result("many_children_one_page", !bad && all_bytes(page, PG, 'Z'), "a child or the parent saw another's write");
}

/* The kernel writes into a shared page: read() from a pipe, clock_gettime, in the child and then in the parent while another child still shares the page. */
static void kernel_writes(char *page) {
    int data[2], go[2];
    pipe(data); pipe(go);
    memset(page, 'K', PG);
    write(data[1], "0123456789abcdef", 16);
    pid_t pid = fork();
    if (pid == 0) {
        if (read(data[0], page + 100, 8) != 8) _exit(40);                       /* the kernel's copy_to_user into a shared page */
        if (memcmp(page + 100, "01234567", 8) || page[99] != 'K' || page[108] != 'K') _exit(41);
        struct timespec *ts = (struct timespec *)(page + 2048);
        if (clock_gettime(1, ts) != 0) _exit(42);
        if (ts->tv_sec == 0x4B4B4B4B4B4B4B4BL) _exit(43);                       /* still the K bytes: nothing was written */
        _exit(0);
    }
    int st = wait_status(pid);
    int ok = st == 0 && all_bytes(page, PG, 'K');                               /* none of it reached the parent */
    pid = fork();                                                               /* now the parent is the one the kernel writes for */
    if (pid == 0) {
        char c;
        read(go[0], &c, 1);
        _exit(all_bytes(page, PG, 'K') ? 0 : 44);
    }
    int ok2 = read(data[0], page + 200, 8) == 8 && !memcmp(page + 200, "89abcdef", 8);
    struct timespec *ts = (struct timespec *)(page + 3000);
    ok2 = ok2 && clock_gettime(1, ts) == 0 && ts->tv_sec != 0x4B4B4B4B4B4B4B4BL;
    write(go[1], "x", 1);
    int st2 = wait_status(pid);
    close(data[0]); close(data[1]); close(go[0]); close(go[1]);
    char why[96];
    snprintf(why, sizeof why, "child %d, parent read %d, second child %d", st, ok2, st2);
    result("kernel_writes", ok && ok2 && st2 == 0, why);
}

/* A thread sleeps in pthread_cond_wait (a futex) on a word in a page that the fork would share; after the fork the parent changes the word and wakes it. */
static struct { pthread_mutex_t mu; pthread_cond_t cv; int flag; int woke; } *g_sync;

static void *sleeper(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_sync->mu);
    while (!g_sync->flag) pthread_cond_wait(&g_sync->cv, &g_sync->mu);
    g_sync->woke = 1;
    pthread_mutex_unlock(&g_sync->mu);
    return NULL;
}

static void threads_and_futex(char *page) {
    g_sync = (void *)page;
    memset(page, 0, PG);
    pthread_mutex_init(&g_sync->mu, NULL);
    pthread_cond_init(&g_sync->cv, NULL);
    pthread_t t;
    pthread_create(&t, NULL, sleeper, NULL);
    struct timespec nap = { 0, 100 * 1000000 };
    nanosleep(&nap, NULL);                          /* the thread is asleep on the futex now */
    int go[2];
    pipe(go);
    pid_t pid = fork();                             /* fork in a threaded program, a waiter on the page */
    if (pid == 0) {
        /* the child: one thread, a copy of the locks; the same dance in its own memory */
        char c;
        read(go[0], &c, 1);
        if (g_sync->flag || g_sync->woke) _exit(50);                            /* the parent's write must not be here */
        pthread_mutex_init(&g_sync->mu, NULL);
        pthread_cond_init(&g_sync->cv, NULL);
        pthread_t t2;
        if (pthread_create(&t2, NULL, sleeper, NULL)) _exit(51);
        nanosleep(&nap, NULL);
        pthread_mutex_lock(&g_sync->mu);
        g_sync->flag = 1;
        pthread_cond_broadcast(&g_sync->cv);
        pthread_mutex_unlock(&g_sync->mu);
        pthread_join(t2, NULL);
        _exit(g_sync->woke ? 0 : 52);
    }
    pthread_mutex_lock(&g_sync->mu);
    g_sync->flag = 1;
    pthread_cond_broadcast(&g_sync->cv);
    pthread_mutex_unlock(&g_sync->mu);
    pthread_join(t, NULL);                          /* hangs here if the wake-up went to a page the sleeper is not on */
    int woke = g_sync->woke;
    write(go[1], "x", 1);
    int st = wait_status(pid);
    close(go[0]); close(go[1]);
    char why[64];
    snprintf(why, sizeof why, "woke %d child %d", woke, st);
    result("threads_and_futex", woke && st == 0, why);
}

static void shared_stays_shared(void) {
    int fd = open("/tmp/cow_shared", O_RDWR | O_CREAT | O_TRUNC, 0600);
    char zero[PG];
    memset(zero, 0, sizeof zero);
    int ok = fd >= 0 && write(fd, zero, PG) == (long)PG;
    char *m = ok ? mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (m == MAP_FAILED) { result("map_shared", 0, "mmap"); return; }
    pid_t pid = fork();
    if (pid == 0) { memset(m, 'S', PG); _exit(0); }
    int st = wait_status(pid);
    result("map_shared", st == 0 && all_bytes(m, PG, 'S'), "the child's write to a MAP_SHARED page is not in the parent");
    munmap(m, PG);
    close(fd);
    unlink("/tmp/cow_shared");
}

static void mprotect_and_munmap(void) {
    char *m = mmap(NULL, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { result("mprotect_munmap", 0, "mmap"); return; }
    memset(m, 'm', 3 * PG);
    int go[2];
    pipe(go);
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        read(go[0], &c, 1);
        _exit(all_bytes(m, 3 * PG, 'm') ? 0 : 60);  /* all three pages as they were, whatever the parent did to its mapping */
    }
    int ok = mprotect(m, 3 * PG, PROT_READ) == 0 && mprotect(m, 3 * PG, PROT_READ | PROT_WRITE) == 0;      /* the shared pages must not come back writable */
    memset(m, 'n', PG);
    ok = ok && munmap(m + PG, PG) == 0;             /* the middle of a shared range */
    memset(m + 2 * PG, 'o', PG);
    write(go[1], "x", 1);
    int st = wait_status(pid);
    close(go[0]); close(go[1]);
    ok = ok && st == 0 && all_bytes(m, PG, 'n') && all_bytes(m + 2 * PG, PG, 'o');
    munmap(m, PG);
    munmap(m + 2 * PG, PG);
    char why[64];
    snprintf(why, sizeof why, "child %d", st);
    result("mprotect_munmap", ok, why);
}

/* What a fork costs while the child lives. */
static void share_cost(void) {
    size_t n = 32 * MIB;
    char *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { result("share_cost", 0, "mmap"); return; }
    for (size_t i = 0; i < n; i += PG) m[i] = (char)(i >> 12);
    int go[2];
    pipe(go);
    long before = memfree_kb();
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        read(go[0], &c, 1);
        int ok = 1;
        for (size_t i = 0; i < n; i += PG) if (m[i] != (char)(i >> 12)) ok = 0;
        _exit(ok ? 0 : 70);
    }
    long during = pid > 0 ? memfree_kb() : before;
    m[0] = 'x';
    m[n - 1] = 'y';
    write(go[1], "x", 1);
    int st = pid > 0 ? wait_status(pid) : -1;
    close(go[0]); close(go[1]);
    munmap(m, n);
    printf("COW share_kb=%ld\n", before - during);
    result("share_cost", st == 0, "the child's view of 32 MiB changed");
}

static int pressure(void) {
    long free_kb = memfree_kb();
    size_t n = (size_t)free_kb * 1024 / 10 * 6 & ~(PG - 1);
    char *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { printf("COW pressure FAIL mmap\n"); return 1; }
    for (size_t i = 0; i < n; i += PG) m[i] = 1;
    printf("COW pressure touched %lu kB of %ld kB free\n", (unsigned long)(n / 1024), free_kb);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { printf("COW pressure fork failed: errno %d\n", errno); return 1; }
    if (pid == 0) {
        for (size_t i = 0; i < n; i += PG) m[i] = 2;        /* every write needs a page: there are not enough */
        _exit(0);
    }
    printf("COW pressure forked\n");
    fflush(stdout);
    int st = wait_status(pid);
    printf("COW pressure child status %d\n", st);            /* (not reached when the killer picked the parent) */
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "pressure")) return pressure();
    char stackpage[2 * PG];
    char *heap = malloc(3 * PG);
    char *anon = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *onstack = (char *)(((unsigned long)stackpage + PG - 1) & ~(PG - 1));
    char *inheap = (char *)(((unsigned long)heap + PG - 1) & ~(PG - 1));
    if (!heap || anon == MAP_FAILED) { printf("COWTEST FAIL setup\n"); return 1; }
    isolate("isolate_data", g_data + PG);
    isolate("isolate_bss", g_bss + PG);
    isolate("isolate_heap", inheap);
    isolate("isolate_stack", onstack - PG < stackpage ? onstack : onstack);
    isolate("isolate_mmap", anon);
    parent_first(anon + PG);
    chain(inheap);
    many(anon);
    kernel_writes(g_data + PG);
    threads_and_futex(anon + PG);
    shared_stays_shared();
    mprotect_and_munmap();
    fork_exec();
    leak();
    share_cost();
    printf(g_fail ? "COWTEST FAIL\n" : "COWTEST PASS\n");
    return g_fail ? 1 : 0;
}
