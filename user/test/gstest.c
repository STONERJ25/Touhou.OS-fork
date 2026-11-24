/* gstest: the per-thread GS base (SYS_gsbase, gap_set_gsbase; docs/WINDOWS.md, where winrun keeps the TEB of a Windows program).
 *
 * Run by tests/case_windows.py as /usr/lib/kernel-tests/gstest. Two threads with different bases read gs:[0] thousands of times across yields and sleeps
 * (every context switch must bring the right base back); a signal handler sees the interrupted thread's base and the base survives the handler; a new
 * thread inherits the base; a forked child inherits it; execve clears it (the program runs itself again with --exec). Prints one line per check, "gstest: ok"
 * at the end, exit status 0 (1 on the first failure). */
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <gensokyo.h>
#include <sys/wait.h>

static uint64_t g_a[4] = { 0x1111111111111111ull }, g_b[4] = { 0x2222222222222222ull };
static volatile uint64_t g_seen_in_handler;
static volatile int g_bad;

static uint64_t gs0(void) {
    uint64_t v;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(v));
    return v;
}

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAILED");
    fflush(stdout);
    if (!ok) _exit(1);
}

static void *other(void *arg) {
    (void)arg;
    gap_set_gsbase(g_b);
    for (int i = 0; i < 3000; i++) {
        if (gs0() != g_b[0]) g_bad = 1;
        if (i % 100 == 0) usleep(1000); else sched_yield();
    }
    return NULL;
}

static void *inherit(void *arg) {
    *(uint64_t *)arg = (uint64_t)gap_get_gsbase() == (uint64_t)g_a ? gs0() : 0;
    return NULL;
}

static void on_usr1(int sig) {
    (void)sig;
    g_seen_in_handler = gs0();
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--exec")) {
        check(gap_get_gsbase() == NULL, "execve clears the base");
        return 0;
    }
    check(gap_get_gsbase() == NULL, "a new program starts with base 0");
    check(gap_set_gsbase(g_a) == NULL && gap_get_gsbase() == g_a && gs0() == g_a[0], "set and read gs:[0]");
    check(gap_set_gsbase((void *)0x0000800000000000ull) == (void *)-22 && gap_get_gsbase() == g_a, "a kernel address is refused");

    pthread_t t;
    pthread_create(&t, NULL, other, NULL);
    for (int i = 0; i < 3000; i++) {
        if (gs0() != g_a[0]) g_bad = 1;
        if (i % 100 == 0) usleep(1000); else sched_yield();
    }
    pthread_join(t, NULL);
    check(!g_bad && gs0() == g_a[0], "two threads keep their own bases across switches");

    uint64_t got = 0;
    pthread_create(&t, NULL, inherit, &got);
    pthread_join(t, NULL);
    check(got == g_a[0], "a new thread inherits the base");

    signal(SIGUSR1, on_usr1);
    kill(getpid(), SIGUSR1);
    check(g_seen_in_handler == g_a[0] && gs0() == g_a[0], "a signal handler runs with the base and leaves it");

    pid_t pid = fork();
    if (pid == 0) _exit(gs0() == g_a[0] && gap_get_gsbase() == g_a ? 0 : 1);
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork inherits the base");

    pid = fork();
    if (pid == 0) {
        execl(argv[0], argv[0], "--exec", (char *)NULL);
        _exit(3);
    }
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the exec'd program saw base 0");
    printf("gstest: ok\n");
    return 0;
}
