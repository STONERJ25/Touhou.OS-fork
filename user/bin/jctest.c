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

/* ---------------------------------------------------------------- terminals
 *
 * The test plays terminal emulator: it owns a pty master and starts a "shell" (a session leader on the slave) that runs a
 * job in its own process group, exactly the way an interactive shell does. The shell reports what it sees on its standard
 * output, and the test looks for those lines on the master. */

typedef struct { int mfd; char buf[8192]; size_t len, pos; } Term;

static int term_expect(Term *t, const char *needle, int timeout_ms) {
    long end = now_ms() + timeout_ms;
    for (;;) {
        char *at = strstr(t->buf + t->pos, needle);
        if (at) { t->pos = (size_t)(at - t->buf) + strlen(needle); return 1; }
        if (now_ms() > end) return 0;
        struct pollfd p = { t->mfd, POLLIN, 0 };
        if (poll(&p, 1, 20) > 0 && (p.revents & POLLIN)) {
            if (t->len + 1 >= sizeof t->buf) { t->len = t->pos = 0; t->buf[0] = 0; }
            ssize_t n = read(t->mfd, t->buf + t->len, sizeof t->buf - 1 - t->len);
            if (n > 0) { t->len += (size_t)n; t->buf[t->len] = 0; }
            else if (n == 0) return strstr(t->buf + t->pos, needle) != NULL;
        }
    }
}

static int term_seen(Term *t, const char *needle) {             /* anywhere in what has been read so far */
    return strstr(t->buf, needle) != NULL;
}

static int term_seen_before(Term *t, const char *needle, size_t upto) {     /* ... within the first `upto` bytes */
    char *at = strstr(t->buf, needle);
    return at && (size_t)(at - t->buf) < upto;
}

static void term_send(Term *t, const char *s) { write(t->mfd, s, strlen(s)); }

enum { M_CTRLZ, M_BGREAD, M_BGREAD_IGN, M_TOSTOP, M_TOSTOP_IGN, M_BGTCSET, M_ORPHAN_EIO };

static void job_body(int mode) {
    char buf[256];
    switch (mode) {
    case M_CTRLZ:
        for (;;) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) { dprintf(1, "jobeof\n"); _exit(7); }
            write(1, "job:", 4);
            write(1, buf, (size_t)n);
        }
    case M_BGREAD: {
        ssize_t n = read(0, buf, sizeof buf);
        dprintf(1, "BGREAD=%d\n", (int)n);
        _exit(0);
    }
    case M_BGREAD_IGN: {
        signal(SIGTTIN, SIG_IGN);
        ssize_t n = read(0, buf, sizeof buf);
        dprintf(1, "READ=%d errno=%d\n", (int)n, errno);
        _exit(0);
    }
    case M_TOSTOP:
        write(1, "hello\n", 6);
        dprintf(1, "wrote\n");
        _exit(0);
    case M_TOSTOP_IGN:
        signal(SIGTTOU, SIG_IGN);
        write(1, "hello\n", 6);
        dprintf(1, "wrote\n");
        _exit(0);
    case M_BGTCSET: {
        struct termios t;
        tcgetattr(0, &t);
        int rc = tcsetattr(0, TCSANOW, &t);
        dprintf(1, "TCSET=%d\n", rc);
        _exit(0);
    }
    }
    _exit(0);
}

/* The shell: runs one job and narrates. Never returns. */
static void pty_shell(const char *spath, int mode) {
    setsid();
    int sfd = open(spath, O_RDWR);
    if (sfd < 0) _exit(90);
    dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
    if (sfd > 2) close(sfd);
    signal(SIGTTOU, SIG_IGN);                               /* a shell must be able to call tcsetpgrp from the background */
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    pid_t shellpg = getpgrp();
    struct termios tio;
    tcgetattr(0, &tio);
    if (mode == M_TOSTOP || mode == M_TOSTOP_IGN) { tio.c_lflag |= TOSTOP; tcsetattr(0, TCSANOW, &tio); }

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGTTOU, SIG_DFL); signal(SIGTTIN, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        if (mode == M_ORPHAN_EIO) {                         /* become a background process in an orphaned group */
            pid_t leaf = fork();
            if (leaf > 0) _exit(0);                         /* the middle process leaves: its child's group is orphaned */
            nap(100);
            char c;
            ssize_t n = read(0, &c, 1);
            dprintf(1, "ORPHAN READ=%d errno=%d\n", (int)n, errno);
            _exit(0);
        }
        job_body(mode);
    }
    setpgid(pid, pid);
    if (mode == M_CTRLZ) tcsetpgrp(0, pid);                 /* the job gets the terminal; the others run in the background */
    dprintf(1, "RUN\n");
    int st;
    for (;;) {
        pid_t r = waitpid(pid, &st, WUNTRACED | WCONTINUED);
        if (r < 0) { if (errno == EINTR) continue; dprintf(1, "WAITERR %d\n", errno); _exit(91); }
        if (WIFSTOPPED(st)) {
            dprintf(1, "STOPPED sig=%d\n", WSTOPSIG(st));
            if (mode == M_CTRLZ) {
                tcsetpgrp(0, shellpg);                      /* take the terminal back, like a shell does after ^Z */
                char line[128];
                ssize_t n = read(0, line, sizeof line - 1);
                if (n < 0) n = 0;
                line[n] = 0;
                dprintf(1, "SHELLGOT:%s", line);
                tcsetpgrp(0, pid);
                kill(-pid, SIGCONT);
                dprintf(1, "CONT\n");
                continue;
            }
            if (mode == M_TOSTOP) {
                tio.c_lflag &= ~(tcflag_t)TOSTOP;
                tcsetattr(0, TCSANOW, &tio);
                kill(-pid, SIGCONT);
                continue;
            }
            kill(pid, SIGKILL);
            continue;
        }
        if (WIFCONTINUED(st)) { dprintf(1, "CONTINUED\n"); continue; }
        if (WIFEXITED(st)) dprintf(1, "DONE code=%d\n", WEXITSTATUS(st));
        else dprintf(1, "DONE signal=%d\n", WTERMSIG(st));
        if (mode == M_ORPHAN_EIO) nap(1500);                /* stay on the terminal until the orphan has tried to read it */
        _exit(0);
    }
}

static pid_t start_shell(Term *t, int mode) {
    char spath[40];
    memset(t, 0, sizeof *t);
    t->mfd = ptyopen(spath, sizeof spath);
    CHECK(t->mfd >= 0);
    pid_t sh = fork();
    if (sh == 0) {
        close(t->mfd);
        pty_shell(spath, mode);
    }
    return sh;
}

static void finish_shell(Term *t, pid_t sh) {
    int st;
    if (wait_for(sh, &st, 0, 5000) != sh) { kill(sh, SIGKILL); wait_for(sh, &st, 0, 3000); }
    close(t->mfd);
}

static void test_ctrl_z(void) {
    SECTION("terminals: Ctrl-Z stops the foreground job, the shell gets the terminal back, fg continues it");
    Term t;
    pid_t sh = start_shell(&t, M_CTRLZ);
    CHECK(term_expect(&t, "RUN", 10000));
    term_send(&t, "one\n");
    CHECK(term_expect(&t, "job:one", 10000));
    term_send(&t, "\x1a");                                  /* VSUSP: SIGTSTP for the foreground group */
    CHECK(term_expect(&t, "STOPPED sig=20", 10000));
    term_send(&t, "typed\n");                               /* the shell is in front now: the stopped job must not take this */
    CHECK(term_expect(&t, "SHELLGOT:typed", 10000));
    CHECK(term_expect(&t, "CONT", 10000));
    CHECK(term_expect(&t, "CONTINUED", 10000));
    term_send(&t, "two\n");
    CHECK(term_expect(&t, "job:two", 10000));
    CHECK(!term_seen(&t, "job:typed"));
    term_send(&t, "\x04");                                  /* ^D at the start of a line: end of file for the job */
    CHECK(term_expect(&t, "jobeof", 10000));
    CHECK(term_expect(&t, "DONE code=7", 10000));
    finish_shell(&t, sh);
}

static void test_background_io(void) {
    SECTION("terminals: background read (SIGTTIN), TOSTOP writes and tcsetattr (SIGTTOU), EIO for ignored or orphaned");
    Term t;
    pid_t sh;

    sh = start_shell(&t, M_BGREAD);
    CHECK(term_expect(&t, "STOPPED sig=21", 10000));        /* SIGTTIN */
    CHECK(term_expect(&t, "DONE signal=9", 10000));
    finish_shell(&t, sh);

    sh = start_shell(&t, M_BGREAD_IGN);
    CHECK(term_expect(&t, "READ=-1 errno=5", 10000));       /* EIO: SIGTTIN is ignored */
    CHECK(term_expect(&t, "DONE code=0", 10000));
    finish_shell(&t, sh);

    sh = start_shell(&t, M_TOSTOP);
    CHECK(term_expect(&t, "STOPPED sig=22", 10000));        /* SIGTTOU: the write waited */
    CHECK(!term_seen_before(&t, "hello", t.pos));           /* nothing was written before the stop report ... */
    CHECK(term_expect(&t, "hello", 10000));                 /* ... and it went through once TOSTOP was cleared and the job continued */
    CHECK(term_expect(&t, "DONE code=0", 10000));
    finish_shell(&t, sh);

    sh = start_shell(&t, M_TOSTOP_IGN);
    CHECK(term_expect(&t, "hello", 10000));                 /* SIGTTOU ignored: the write goes through at once */
    CHECK(term_expect(&t, "DONE code=0", 10000));
    CHECK(!term_seen(&t, "STOPPED"));
    finish_shell(&t, sh);

    sh = start_shell(&t, M_BGTCSET);
    CHECK(term_expect(&t, "STOPPED sig=22", 10000));        /* tcsetattr from the background */
    CHECK(term_expect(&t, "DONE signal=9", 10000));
    finish_shell(&t, sh);

    sh = start_shell(&t, M_ORPHAN_EIO);
    CHECK(term_expect(&t, "ORPHAN READ=-1 errno=5", 10000));    /* an orphaned group gets EIO instead of SIGTTIN */
    finish_shell(&t, sh);
}

/* ---------------------------------------------------------------- orphaned process groups */

static volatile int g_hup;
static void on_hup(int sig) { (void)sig; g_hup = 1; }
static void on_hup_count(int sig) { (void)sig; g_shared[1]++; }

static void test_orphaned_group(void) {
    SECTION("orphaned process groups: SIGHUP then SIGCONT for stopped members, stop signals dropped");
    int pfd[2];
    CHECK(pipe(pfd) == 0);
    int st;

    /* P starts B in a new group, waits until B is stopped, and exits: B's group is orphaned and holds a stopped member */
    pid_t p = fork();
    if (p == 0) {
        pid_t b = fork();
        if (b == 0) {
            setpgid(0, 0);
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = on_hup;
            sigaction(SIGHUP, &sa, NULL);
            raise(SIGSTOP);                                 /* stops here until somebody continues it */
            write(pfd[1], g_hup ? "H" : "c", 1);            /* H: the SIGHUP was there when we ran again */
            _exit(0);
        }
        int bs;
        pid_t r = wait_for(b, &bs, WUNTRACED, 10000);
        _exit(r == b && WIFSTOPPED(bs) ? 0 : 1);
    }
    CHECK(p > 0);
    CHECK(wait_for(p, &st, 0, 15000) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    char c = 0;
    struct pollfd pf = { pfd[0], POLLIN, 0 };
    CHECK(poll(&pf, 1, 10000) == 1 && read(pfd[0], &c, 1) == 1 && c == 'H');

    /* nobody stopped: nothing is sent. And inside an orphaned group SIGTSTP is dropped, while SIGSTOP still stops. */
    g_shared[0] = g_shared[1] = 0;
    p = fork();
    if (p == 0) {
        pid_t b = fork();
        if (b == 0) {
            setpgid(0, 0);
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = on_hup_count;
            sigaction(SIGHUP, &sa, NULL);
            for (;;) { g_shared[0]++; nap(1); }
        }
        write(pfd[1], &b, sizeof b);
        _exit(0);                                           /* leave: B's group is orphaned, but nobody in it is stopped */
    }
    CHECK(wait_for(p, &st, 0, 15000) == p && WIFEXITED(st));
    pid_t b = 0;
    pf.fd = pfd[0];
    CHECK(poll(&pf, 1, 10000) == 1 && read(pfd[0], &b, sizeof b) == (ssize_t)sizeof b && b > 0);
    CHECK(wait_counter(&g_shared[0], 3, 5000));
    nap(200);
    CHECK(g_shared[1] == 0);                                /* no SIGHUP */
    kill(b, SIGTSTP);
    nap(150);
    CHECK(proc_state(b) != 'T');                            /* dropped: the group is orphaned */
    kill(b, SIGSTOP);
    CHECK(wait_state(b, 'T', 5000));                        /* SIGSTOP cannot be dropped */
    kill(b, SIGKILL);
    close(pfd[0]);
    close(pfd[1]);
}

int main(void) {
    printf("jctest: the shrine's job control test begins\n");
    setup_shared();
    test_stop_continue();
    test_stop_semantics();
    test_ctrl_z();
    test_background_io();
    test_orphaned_group();
    if (g_fail) printf("JCTEST FAIL (%d failures)\n", g_fail);
    else printf("JCTEST PASS\n");
    return g_fail ? 1 : 0;
}
