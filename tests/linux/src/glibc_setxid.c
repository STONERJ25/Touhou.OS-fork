/*
 * setuid and its relatives in a threaded program, through a statically linked C library. On Linux every thread has credentials of its own, so the C library sends
 * its internal real-time signal 33 to each other thread with tgkill, and each thread makes the system call itself; the caller waits until all of them did. The
 * kernel has to deliver that signal with the siginfo of tgkill (the library ignores any other) or setuid never returns. The program changes nothing for real (it
 * sets the ids it already has) so that it behaves the same for any user; a watchdog ends it with a message if a call hangs.
 * A real Linux prints the same lines (tools/linux-oracle.py); tests/case_linux_signals.py compares.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define THREADS 3

static void watchdog(int sig) {
    (void)sig;
    static const char msg[] = "the watchdog ended the program: a set-id call never returned\n";
    write(1, msg, sizeof msg - 1);
    _exit(3);
}

static void nap(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static pthread_barrier_t gate;
static volatile int phase;
static uid_t seen_uid[THREADS], seen_euid[THREADS];
static gid_t seen_gid[THREADS];
static pid_t tids[THREADS];

static void *member(void *arg) {
    int i = (int)(long)arg;
    tids[i] = (pid_t)syscall(SYS_gettid);
    pthread_barrier_wait(&gate);                        /* every thread exists */
    while (phase < 1) nap(2);                           /* the main thread is changing ids; this thread is in user mode or asleep */
    pthread_barrier_wait(&gate);                        /* ... and is done */
    seen_uid[i] = (uid_t)syscall(SYS_getuid);
    seen_euid[i] = (uid_t)syscall(SYS_geteuid);
    seen_gid[i] = (gid_t)syscall(SYS_getgid);
    pthread_barrier_wait(&gate);
    return NULL;
}

static int all_same(uid_t want_uid, uid_t want_euid, gid_t want_gid) {
    int ok = 1;
    for (int i = 0; i < THREADS; i++) ok &= seen_uid[i] == want_uid && seen_euid[i] == want_euid && seen_gid[i] == want_gid;
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, watchdog);
    alarm(60);
    uid_t uid = getuid(), euid = geteuid();
    gid_t gid = getgid(), egid = getegid();

    pthread_barrier_init(&gate, NULL, THREADS + 1);
    pthread_t th[THREADS];
    for (long i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, member, (void *)i);
    pthread_barrier_wait(&gate);

    printf("setuid(own uid) with %d other threads: %d\n", THREADS, setuid(uid));
    printf("setgid(own gid): %d\n", setgid(gid));
    printf("seteuid(own euid): %d\n", seteuid(euid));
    printf("setegid(own egid): %d\n", setegid(egid));
    printf("setreuid(-1, -1): %d\n", setreuid((uid_t)-1, (uid_t)-1));
    printf("setregid(-1, -1): %d\n", setregid((gid_t)-1, (gid_t)-1));
    printf("setresuid(-1, -1, -1): %d\n", setresuid((uid_t)-1, (uid_t)-1, (uid_t)-1));
    printf("setresgid(-1, -1, -1): %d\n", setresgid((gid_t)-1, (gid_t)-1, (gid_t)-1));
    phase = 1;
    pthread_barrier_wait(&gate);
    pthread_barrier_wait(&gate);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    printf("every thread sees the same ids as before: %d\n", all_same(uid, euid, gid));

    /* a thread that is created after the change, and a change made from a thread other than the first */
    phase = 0;
    pthread_barrier_destroy(&gate);
    pthread_barrier_init(&gate, NULL, THREADS + 1);
    for (long i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, member, (void *)i);
    pthread_barrier_wait(&gate);
    printf("setuid again with new threads: %d\n", setuid(uid));
    phase = 1;
    pthread_barrier_wait(&gate);
    pthread_barrier_wait(&gate);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    printf("every thread sees the same ids again: %d\n", all_same(uid, euid, gid));

    /* a call that fails fails everywhere and reports the error: only root could become somebody else, so a mismatch is tested against the answer for uid 0 */
    if (uid != 0) {
        errno = 0;
        int r = setuid(0);
        printf("setuid(0) as an ordinary user: %d, errno %d (EPERM %d)\n", r, errno, errno == EPERM);
    }
    printf("ids unchanged afterwards: %d\n", getuid() == uid && geteuid() == euid && getgid() == gid && getegid() == egid);
    return 0;
}
