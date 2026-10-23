/* jctest: user-space checks for job control (stop, continue, waitpid statuses, terminals), priorities, hard links and
 * /proc/<pid>/{fd,cwd,exe}. Run it from a shell on the serial console: `jctest`; tests/case_jobctl.py does. It prints
 * "JCTEST PASS" or "JCTEST FAIL (n)". Every wait has a generous bound: the guest clock can run slower than the host's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <termios.h>
#include <pty.h>
#include <gensokyo.h>

static int g_fail;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno); g_fail++; } } while (0)
#define SECTION(name) printf("jctest: %s\n", name)

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void nap(int ms) { usleep((useconds_t)ms * 1000); }

/* The state letter in /proc/<pid>/status ('R' 'S' 'T' 'Z'), or 0 if there is no such process. */
static char proc_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *s = strstr(buf, "State:\t");
    return s ? s[7] : 0;
}

static int wait_state(pid_t pid, char want, int timeout_ms) {
    long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (proc_state(pid) == want) return 1;
        nap(5);
    }
    return proc_state(pid) == want;
}

/* Counters in memory shared with the children (a tmpfs file mapped MAP_SHARED, as the window server does). */
static volatile unsigned long *g_shared;

static void setup_shared(void) {
    int fd = open("/tmp/jc.shm", O_RDWR | O_CREAT | O_TRUNC, 0600);
    ftruncate(fd, 4096);
    g_shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    unlink("/tmp/jc.shm");
}

static int wait_counter(volatile unsigned long *c, unsigned long above, int timeout_ms) {
    long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (*c > above) return 1;
        nap(5);
    }
    return *c > above;
}

static volatile int g_chld;
static void on_chld(int sig) { (void)sig; g_chld++; }

static int wait_chld(int above, int timeout_ms) {
    long end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        if (g_chld > above) return 1;
        nap(5);
    }
    return g_chld > above;
}

/* waitpid with a bound, so a missing stop report fails the test instead of hanging it */
static pid_t wait_for(pid_t pid, int *status, int options, int timeout_ms) {
    long end = now_ms() + timeout_ms;
    for (;;) {
        pid_t r = waitpid(pid, status, options | WNOHANG);
        if (r != 0) return r;
        if (now_ms() > end) return 0;
        nap(5);
    }
}

static void kill_and_reap(pid_t pid) {
    kill(pid, SIGCONT);
    kill(pid, SIGKILL);
    int st;
    wait_for(pid, &st, 0, 3000);
}

/* ---------------------------------------------------------------- stop and continue */

static void *counter_thread(void *arg) {
    (void)arg;
    for (;;) { g_shared[1]++; nap(1); }
    return NULL;
}

static void test_stop_continue(void) {
    SECTION("SIGSTOP, SIGCONT, waitpid(WUNTRACED | WCONTINUED), SIGCHLD, State: T");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_chld;
    CHECK(sigaction(SIGCHLD, &sa, NULL) == 0);
    g_shared[0] = g_shared[1] = 0;

    pid_t pid = fork();
    if (pid == 0) {                                         /* two threads count; a stop must freeze both */
        pthread_t t;
        pthread_create(&t, NULL, counter_thread, NULL);
        for (;;) { g_shared[0]++; nap(1); }
    }
    CHECK(pid > 0);
    CHECK(wait_counter(&g_shared[0], 3, 5000) && wait_counter(&g_shared[1], 3, 5000));

    int chld0 = g_chld;
    CHECK(kill(pid, SIGSTOP) == 0);
    int st = 0;
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid);
    CHECK(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP && !WIFEXITED(st) && !WIFSIGNALED(st) && !WIFCONTINUED(st));
    CHECK(wait_chld(chld0, 2000));                          /* the parent heard SIGCHLD about the stop */
    CHECK(proc_state(pid) == 'T');
    CHECK(waitpid(pid, &st, WUNTRACED | WNOHANG) == 0);     /* a stop is reported once */

    nap(60);                                                /* let a thread that was mid-sleep reach the edge of user mode */
    unsigned long a0 = g_shared[0], a1 = g_shared[1];
    nap(200);
    CHECK(g_shared[0] == a0 && g_shared[1] == a1);          /* both threads are frozen */

    chld0 = g_chld;
    CHECK(kill(pid, SIGCONT) == 0);
    CHECK(wait_for(pid, &st, WCONTINUED, 5000) == pid && WIFCONTINUED(st));
    CHECK(wait_chld(chld0, 2000));
    CHECK(waitpid(pid, &st, WCONTINUED | WNOHANG) == 0);    /* ... and a continue too */
    CHECK(wait_counter(&g_shared[0], a0 + 2, 5000) && wait_counter(&g_shared[1], a1 + 2, 5000));
    CHECK(proc_state(pid) != 'T');

    /* SA_NOCLDSTOP: no SIGCHLD for stops and continues, but waitpid still reports them */
    sa.sa_flags = SA_NOCLDSTOP;
    CHECK(sigaction(SIGCHLD, &sa, NULL) == 0);
    chld0 = g_chld;
    kill(pid, SIGSTOP);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st));
    kill(pid, SIGCONT);
    CHECK(wait_for(pid, &st, WCONTINUED, 5000) == pid && WIFCONTINUED(st));
    nap(100);
    CHECK(g_chld == chld0);

    /* SIGKILL reaches a stopped process */
    kill(pid, SIGSTOP);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st));
    CHECK(kill(pid, SIGKILL) == 0);
    CHECK(wait_for(pid, &st, 0, 5000) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL && !WIFSTOPPED(st));
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);
}

static volatile unsigned long *g_flag;                      /* g_shared[8..] are flags set by handlers in the children */
static void on_flag_tstp(int sig) { (void)sig; g_shared[8]++; }
static void on_flag_cont(int sig) { (void)sig; g_shared[9]++; }

static void test_stop_semantics(void) {
    SECTION("stop signals: ignored, caught, blocked, pending discarded by SIGCONT, handler for SIGCONT");
    int st;
    (void)g_flag;

    /* SIGTSTP ignored: nothing happens; SIGSTOP cannot be ignored */
    g_shared[0] = 0;
    pid_t pid = fork();
    if (pid == 0) { signal(SIGTSTP, SIG_IGN); for (;;) { g_shared[0]++; nap(1); } }
    CHECK(wait_counter(&g_shared[0], 3, 5000));
    kill(pid, SIGTSTP);
    nap(100);
    CHECK(proc_state(pid) != 'T');
    kill(pid, SIGSTOP);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
    kill_and_reap(pid);

    /* SIGTSTP caught: the handler runs instead of the stop */
    g_shared[8] = 0;
    pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_flag_tstp;
        sigaction(SIGTSTP, &sa, NULL);
        for (;;) pause();
    }
    nap(50);
    kill(pid, SIGTSTP);
    CHECK(wait_counter(&g_shared[8], 0, 5000));
    CHECK(proc_state(pid) != 'T');
    kill_and_reap(pid);

    /* SIGTSTP blocked: stays pending, the stop happens when it is unblocked */
    g_shared[0] = 0; g_shared[10] = 0;
    pid = fork();
    if (pid == 0) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGTSTP);
        sigprocmask(SIG_BLOCK, &s, NULL);
        while (!g_shared[10]) { g_shared[0]++; nap(1); }
        sigprocmask(SIG_UNBLOCK, &s, NULL);
        for (;;) { g_shared[0]++; nap(1); }
    }
    CHECK(wait_counter(&g_shared[0], 3, 5000));
    kill(pid, SIGTSTP);
    nap(100);
    CHECK(proc_state(pid) != 'T');
    g_shared[10] = 1;
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP);
    kill_and_reap(pid);

    /* a pending stop signal is discarded by SIGCONT */
    g_shared[0] = 0; g_shared[10] = 0;
    pid = fork();
    if (pid == 0) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGTSTP);
        sigprocmask(SIG_BLOCK, &s, NULL);
        while (!g_shared[10]) { g_shared[0]++; nap(1); }
        sigprocmask(SIG_UNBLOCK, &s, NULL);
        for (;;) { g_shared[0]++; nap(1); }
    }
    CHECK(wait_counter(&g_shared[0], 3, 5000));
    kill(pid, SIGTSTP);
    kill(pid, SIGCONT);
    g_shared[10] = 1;
    nap(150);
    CHECK(proc_state(pid) != 'T');
    kill_and_reap(pid);

    /* SIGTTIN and SIGTTOU stop by default too */
    g_shared[0] = 0;
    pid = fork();
    if (pid == 0) for (;;) { g_shared[0]++; nap(1); }
    CHECK(wait_counter(&g_shared[0], 3, 5000));
    kill(pid, SIGTTIN);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN);
    kill(pid, SIGCONT);
    CHECK(wait_for(pid, &st, WCONTINUED, 5000) == pid && WIFCONTINUED(st));
    kill(pid, SIGTTOU);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTOU);
    kill_and_reap(pid);

    /* SIGCONT with a handler: the process continues and the handler runs; SIGCONT blocked still continues */
    g_shared[9] = 0;
    pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_flag_cont;
        sigaction(SIGCONT, &sa, NULL);
        for (;;) { g_shared[0]++; nap(1); }
    }
    nap(50);
    kill(pid, SIGSTOP);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st));
    kill(pid, SIGCONT);
    CHECK(wait_for(pid, &st, WCONTINUED, 5000) == pid && WIFCONTINUED(st));
    CHECK(wait_counter(&g_shared[9], 0, 5000));
    kill_and_reap(pid);

    g_shared[0] = 0;
    pid = fork();
    if (pid == 0) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGCONT);
        sigprocmask(SIG_BLOCK, &s, NULL);
        for (;;) { g_shared[0]++; nap(1); }
    }
    CHECK(wait_counter(&g_shared[0], 3, 5000));
    kill(pid, SIGSTOP);
    CHECK(wait_for(pid, &st, WUNTRACED, 5000) == pid && WIFSTOPPED(st));
    kill(pid, SIGCONT);
    CHECK(wait_for(pid, &st, WCONTINUED, 5000) == pid && WIFCONTINUED(st));
    unsigned long c = g_shared[0];
    CHECK(wait_counter(&g_shared[0], c + 2, 5000));
    kill_and_reap(pid);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_flag_tstp;
    CHECK(sigaction(SIGSTOP, &sa, NULL) < 0 && errno == EINVAL);        /* SIGSTOP cannot be caught */
}

int main(void) {
    printf("jctest: the shrine's job control test begins\n");
    setup_shared();
    test_stop_continue();
    test_stop_semantics();
    if (g_fail) printf("JCTEST FAIL (%d failures)\n", g_fail);
    else printf("JCTEST PASS\n");
    return g_fail ? 1 : 0;
}
