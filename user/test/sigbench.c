/* sigbench: what a native signal costs, in nanoseconds per operation (docs/COMPAT.md, "Real-time signals").
 *
 * Signal delivery is on the hot path of every program, so the Linux real-time-signal work measured it before and after: this is the loop it used.
 * Run in the text shell as /ScarletDevilMansion/lib/kernel-tests/sigbench (the kernel-tests directory of the image); it prints one line per operation:
 *
 *   getpid        a system call that does nothing (the floor)
 *   kill+handler  kill(getpid(), SIGUSR1): generation, delivery, the frame on the user stack, the handler, sigreturn
 *   tgkill        the same through the thread-directed path (raise)
 *   sigaction     install a handler
 *   sigprocmask   block and unblock (two calls)
 *
 * The clock is the system's millisecond uptime, so every loop runs for a fraction of a second. On a PC that other VMs share the wall clock moves the numbers by 15 % or
 * more; to compare two kernels, give QEMU `-icount shift=0,sleep=off` (tools/qemu.py: Qemu(extra=[...])), where virtual time is the count of guest instructions (one
 * per nanosecond) and every run prints the same numbers to the digit, and boot both kernels with the same initrd: the position of the program's variables in its pages
 * changes the cost of its stores in QEMU's translator by a factor of three. */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

static volatile int g_hits;

static void on_usr1(int sig) { (void)sig; g_hits++; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(1, &ts);                      /* CLOCK_MONOTONIC */
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Runs op() until at least 400 ms have passed and prints the cost of one call. The batch is 1000 calls, so the clock is read rarely. */
static void bench(const char *name, void (*op)(void), int per_call) {
    long t0 = now_ms(), calls = 0, t1;
    do {
        for (int i = 0; i < 1000; i++) op();
        calls += 1000;
        t1 = now_ms();
    } while (t1 - t0 < 400);
    long ns = (t1 - t0) * 1000000L / (calls * per_call);
    printf("%-14s %6ld ns per operation  (%ld operations)\n", name, ns, calls * per_call);
}

static void op_getpid(void) { (void)getpid(); }
static void op_kill(void) { kill(getpid(), SIGUSR1); }
static void op_raise(void) { raise(SIGUSR1); }

static void op_sigaction(void) {
    struct sigaction sa = { on_usr1, SA_RESTART, 0 };
    sigaction(SIGUSR2, &sa, NULL);
}

static void op_sigprocmask(void) {
    sigset_t s = 1U << SIGUSR2;
    sigprocmask(SIG_BLOCK, &s, NULL);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
}

int main(void) {
    struct sigaction sa = { on_usr1, SA_RESTART, 0 };
    sigaction(SIGUSR1, &sa, NULL);
    for (int round = 0; round < 3; round++) {
        printf("round %d\n", round + 1);
        bench("getpid", op_getpid, 1);
        bench("kill+handler", op_kill, 1);
        bench("tgkill", op_raise, 1);
        bench("sigaction", op_sigaction, 1);
        bench("sigprocmask", op_sigprocmask, 2);
    }
    printf("handler ran %d times\n", g_hits);
    return 0;
}
