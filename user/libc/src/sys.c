/* System call wrappers, process startup, environment, signals, and other thin POSIX glue. */
#include "syscall_priv.h"
#include "pthread_impl.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <gensokyo.h>
#include <limits.h>


char **environ;

extern int main(int argc, char **argv, char **envp);
extern void __stdio_init(void);
extern void __restore_rt(void);

/* ---------------------------------------------------------------- process startup / exit */

#define MAX_ATEXIT 32
static void (*g_atexit[MAX_ATEXIT])(void);
static int g_natexit;

int atexit(void (*fn)(void)) {
    if (g_natexit >= MAX_ATEXIT) return -1;
    g_atexit[g_natexit++] = fn;
    return 0;
}

void exit(int status) {
    while (g_natexit > 0) g_atexit[--g_natexit]();
    fflush(NULL);
    _exit(status);
}

void _exit(int status) {
    __sc1(SYS_exit, status);
    for (;;) __asm__ volatile ("ud2");
}

void abort(void) {
    raise(SIGABRT);
    _exit(134);
}

void __libc_start(long *sp) {
    __pthread_init_main();                      /* before anything can touch errno */
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    environ = argv + argc + 1;
    __stdio_init();
    exit(main(argc, argv, environ));
}

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    fprintf(stderr, "assertion failed: %s (%s:%d, %s)\n", expr, file, line, func);
    abort();
}

/* ---------------------------------------------------------------- files */

ssize_t read(int fd, void *buf, size_t n) { return __ret(__sc3(SYS_read, fd, (long)buf, (long)n)); }
ssize_t write(int fd, const void *buf, size_t n) { return __ret(__sc3(SYS_write, fd, (long)buf, (long)n)); }
int close(int fd) { return (int)__ret(__sc1(SYS_close, fd)); }
off_t lseek(int fd, off_t off, int whence) { return __ret(__sc3(SYS_lseek, fd, off, whence)); }
int dup(int fd) { return (int)__ret(__sc1(SYS_dup, fd)); }
int dup2(int a, int b) { return (int)__ret(__sc2(SYS_dup2, a, b)); }
int pipe(int fds[2]) { return (int)__ret(__sc1(SYS_pipe, (long)fds)); }
int unlink(const char *p) { return (int)__ret(__sc1(SYS_unlink, (long)p)); }
int rmdir(const char *p) { return (int)__ret(__sc1(SYS_rmdir, (long)p)); }
int mkdir(const char *p, mode_t m) { return (int)__ret(__sc2(SYS_mkdir, (long)p, (long)m)); }
int rename(const char *a, const char *b) { return (int)__ret(__sc2(SYS_rename, (long)a, (long)b)); }
int remove(const char *p) {
    int r = unlink(p);
    if (r < 0 && errno == EISDIR) r = rmdir(p);
    return r;
}
int chdir(const char *p) { return (int)__ret(__sc1(SYS_chdir, (long)p)); }
int access(const char *p, int m) { return (int)__ret(__sc2(SYS_access, (long)p, m)); }
int symlink(const char *t, const char *p) { return (int)__ret(__sc2(SYS_symlink, (long)t, (long)p)); }
ssize_t readlink(const char *p, char *b, size_t n) { return __ret(__sc3(SYS_readlink, (long)p, (long)b, (long)n)); }
int truncate(const char *p, off_t n) { return (int)__ret(__sc2(SYS_truncate, (long)p, n)); }
int ftruncate(int fd, off_t n) { return (int)__ret(__sc2(SYS_ftruncate, fd, n)); }
int chmod(const char *p, mode_t m) { return (int)__ret(__sc2(SYS_chmod, (long)p, (long)m)); }
int chown(const char *p, uid_t u, gid_t g) { return (int)__ret(__sc3(SYS_chown, (long)p, (long)u, (long)g)); }
mode_t umask(mode_t m) { return (mode_t)__sc1(SYS_umask, (long)m); }
int link(const char *a, const char *b) { return (int)__ret(__sc2(SYS_link, (long)a, (long)b)); }
int sync(void) { return (int)__sc0(SYS_sync); }
int stat(const char *p, struct stat *st) { return (int)__ret(__sc2(SYS_stat, (long)p, (long)st)); }
int lstat(const char *p, struct stat *st) { return (int)__ret(__sc2(SYS_lstat, (long)p, (long)st)); }
int fstat(int fd, struct stat *st) { return (int)__ret(__sc2(SYS_fstat, fd, (long)st)); }

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return (int)__ret(__sc3(SYS_open, (long)path, flags, (long)mode));
}

int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return (int)__ret(__sc3(SYS_fcntl, fd, cmd, arg));
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)__ret(__sc3(SYS_ioctl, fd, (long)req, (long)arg));
}

int poll(struct gpollfd *fds, nfds_t n, int timeout) { return (int)__ret(__sc3(SYS_poll, (long)fds, (long)n, timeout)); }

char *getcwd(char *buf, size_t n) {
    long r = __ret(__sc2(SYS_getcwd, (long)buf, (long)n));
    return r < 0 ? NULL : buf;
}

int isatty(int fd) {
    struct termios t;
    if (tcgetattr(fd, &t) == 0) return 1;
    errno = ENOTTY;
    return 0;
}

int tcgetattr(int fd, struct termios *t) { return ioctl(fd, TCGETS, t); }
int tcsetattr(int fd, int when, const struct termios *t) {
    return ioctl(fd, when == TCSANOW ? TCSETS : when == TCSADRAIN ? TCSETSW : TCSETSF, t);
}
char *ttyname(int fd) {
    static char name[80];
    char n[64];
    if (!isatty(fd)) return NULL;
    memset(n, 0, sizeof n);
    if (ioctl(fd, TIOCGNAME, n) < 0 || !n[0]) strcpy(n, "console");
    snprintf(name, sizeof name, "/dev/%s", n);
    return name;
}

/* ---------------------------------------------------------------- processes */

extern volatile int __malloc_lock;
void __stdio_fork_prepare(void);
void __stdio_fork_parent(void);
void __stdio_fork_child(void);

/* The child of a multi-threaded process has one thread, so the locks that other threads might have held at the moment of the
 * fork would never be released: fork() takes the stdio locks (the list and the three standard streams) and the allocator
 * lock itself, in the order the library uses them (a stream, then the allocator), and lets go on both sides. The child also
 * resets the locks of every other stream: their holders do not exist there. */
pid_t fork(void) {
    __fork_run_prepare();
    __stdio_fork_prepare();
    __lock(&__malloc_lock);
    long r = __sc0(SYS_fork);
    if (r == 0) __pthread_self()->tid = (int)__sc0(SYS_gettid);    /* the copy of the control block still has the parent's tid */
    __unlock(&__malloc_lock);
    if (r == 0) __stdio_fork_child();
    else __stdio_fork_parent();
    if (r == 0) __fork_run_child();
    else if (r > 0) __fork_run_parent();
    return (pid_t)__ret(r);
}
pid_t getpid(void) { return (pid_t)__sc0(SYS_getpid); }
pid_t getppid(void) { return (pid_t)__sc0(SYS_getppid); }
pid_t getpgid(pid_t p) { return (pid_t)__ret(__sc1(SYS_getpgid, p)); }
pid_t getpgrp(void) { return getpgid(0); }
int setpgid(pid_t p, pid_t g) { return (int)__ret(__sc2(SYS_setpgid, p, g)); }
pid_t setsid(void) { return (pid_t)__ret(__sc0(SYS_setsid)); }
int tcsetpgrp(int fd, pid_t pg) { return ioctl(fd, TIOCSPGRP, &pg); }
pid_t tcgetpgrp(int fd) { int pg = 0; return ioctl(fd, TIOCGPGRP, &pg) < 0 ? -1 : pg; }
uid_t getuid(void) { return (uid_t)__sc0(SYS_getuid); }
uid_t geteuid(void) { return (uid_t)__sc0(SYS_geteuid); }
gid_t getgid(void) { return (gid_t)__sc0(SYS_getgid); }
gid_t getegid(void) { return (gid_t)__sc0(SYS_getegid); }
int setuid(uid_t u) { return (int)__ret(__sc1(SYS_setuid, (long)u)); }
int setgid(gid_t g) { return (int)__ret(__sc1(SYS_setgid, (long)g)); }
int execve(const char *p, char *const a[], char *const e[]) { return (int)__ret(__sc3(SYS_execve, (long)p, (long)a, (long)e)); }
int execv(const char *p, char *const a[]) { return execve(p, a, environ); }

int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/')) return execv(file, argv);
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/sbin";
    char buf[PATH_MAX];
    int saved = ENOENT;
    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        if (len + strlen(file) + 2 < sizeof buf) {
            memcpy(buf, path, len);
            buf[len] = '/';
            strcpy(buf + len + 1, file);
            execv(buf, argv);
            if (errno != ENOENT && errno != ENOTDIR) saved = errno;
        }
        if (!end) break;
        path = end + 1;
    }
    errno = saved;
    return -1;
}

static int exec_va(const char *path, const char *arg, va_list ap, int search) {
    char *argv[64];
    int n = 0;
    argv[n++] = (char *)arg;
    while (n < 63 && (argv[n] = va_arg(ap, char *)) != NULL) n++;
    argv[n] = NULL;
    return search ? execvp(path, argv) : execv(path, argv);
}

int execl(const char *path, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = exec_va(path, arg, ap, 0);
    va_end(ap);
    return r;
}

int execlp(const char *file, const char *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    int r = exec_va(file, arg, ap, 1);
    va_end(ap);
    return r;
}

pid_t waitpid(pid_t pid, int *status, int options) { return (pid_t)__ret(__sc3(SYS_waitpid, pid, (long)status, options)); }
pid_t wait(int *status) { return waitpid(-1, status, 0); }

int system(const char *cmd) {
    if (!cmd) return 1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/gapsh", "gapsh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    return status;
}

FILE *popen(const char *cmd, const char *mode) {
    int fds[2];
    if (pipe(fds) < 0) return NULL;
    int reading = mode[0] == 'r';
    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) {
        dup2(reading ? fds[1] : fds[0], reading ? 1 : 0);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/gapsh", "gapsh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(reading ? fds[1] : fds[0]);
    FILE *f = fdopen(reading ? fds[0] : fds[1], mode);
    return f;
}

int pclose(FILE *f) {
    fclose(f);
    int status = 0;
    wait(&status);
    return status;
}

/* ---------------------------------------------------------------- memory */

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    long r = __sc6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
    if ((unsigned long)r > (unsigned long)-4096L) { errno = (int)-r; return MAP_FAILED; }
    return (void *)r;
}
int munmap(void *a, size_t l) { return (int)__ret(__sc2(SYS_munmap, (long)a, (long)l)); }
int mprotect(void *a, size_t l, int p) { return (int)__ret(__sc3(SYS_mprotect, (long)a, (long)l, p)); }

static char *g_brk;

int brk(void *addr) {
    long r = __sc1(SYS_brk, (long)addr);
    if ((char *)r != (char *)addr) { errno = ENOMEM; return -1; }
    g_brk = (char *)r;
    return 0;
}

void *sbrk(long inc) {
    if (!g_brk) g_brk = (char *)__sc1(SYS_brk, 0);
    char *old = g_brk;
    if (inc == 0) return old;
    long r = __sc1(SYS_brk, (long)(old + inc));
    if ((char *)r != old + inc) { errno = ENOMEM; return (void *)-1; }
    g_brk = (char *)r;
    return old;
}

/* ---------------------------------------------------------------- time */

int clock_gettime(int clk, struct timespec *ts) {
    struct gtimespec g;
    long r = __ret(__sc2(SYS_clock_gettime, clk, (long)&g));
    if (r == 0) { ts->tv_sec = g.tv_sec; ts->tv_nsec = g.tv_nsec; }
    return (int)r;
}

time_t time(time_t *t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t) *t = ts.tv_sec;
    return ts.tv_sec;
}

clock_t clock(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / 1000;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    struct gtimespec g = { req->tv_sec, req->tv_nsec };
    (void)rem;
    return (int)__ret(__sc2(SYS_nanosleep, (long)&g, 0));
}

unsigned sleep(unsigned sec) {
    struct timespec ts = { sec, 0 };
    return nanosleep(&ts, NULL) < 0 ? sec : 0;
}

int usleep(useconds_t us) {
    struct timespec ts = { us / 1000000, (long)(us % 1000000) * 1000 };
    return nanosleep(&ts, NULL);
}

unsigned alarm(unsigned sec) { (void)sec; return 0; }
int pause(void) { return (int)__ret(__sc0(SYS_pause)); }

/* ---------------------------------------------------------------- signals */

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    struct gsigaction a, o;
    if (act) {
        a.handler = (unsigned long)act->sa_handler;
        a.flags = act->sa_flags;
        a.mask = act->sa_mask;
        a.restorer = (unsigned long)__restore_rt;
    }
    long r = __ret(__sc3(SYS_sigaction, sig, act ? (long)&a : 0, old ? (long)&o : 0));
    if (r == 0 && old) { old->sa_handler = (sighandler_t)o.handler; old->sa_flags = o.flags; old->sa_mask = (sigset_t)o.mask; }
    return (int)r;
}

sighandler_t signal(int sig, sighandler_t h) {
    struct sigaction a = { h, SA_RESTART, 0 }, o;
    if (sigaction(sig, &a, &o) < 0) return SIG_ERR;
    return o.sa_handler;
}

/* SIGCANCEL belongs to pthread_cancel (blocking it is how a thread disables cancellation), so applications never see it
 * in a mask and cannot change it through here. */
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    const unsigned bit = 1U << SIGCANCEL;
    long v = set ? (long)(*set & ~bit) : 0;
    if (!set) how = SIG_BLOCK;                                  /* a query: block nothing */
    if (set && how == SIG_SETMASK && __pthread_self()->cancel_disabled) v |= bit;
    long r = __sc3(SYS_sigprocmask, how, v, (long)old);
    if (r == 0 && old) *old &= ~bit;
    return (int)__ret(r);
}
/* Mask by value, like sigprocmask. sigsuspend always ends with -1/EINTR after a handler has run. */
int sigsuspend(const sigset_t *mask) { return (int)__ret(__sc1(SYS_sigsuspend, (long)*mask)); }

int sigtimedwait(const sigset_t *set, void *info, const struct timespec *timeout) {
    struct gtimespec g;
    long tp = 0;
    (void)info;                                 /* no siginfo: the number of the signal is the whole answer */
    if (timeout) { g.tv_sec = timeout->tv_sec; g.tv_nsec = timeout->tv_nsec; tp = (long)&g; }
    return (int)__ret(__sc2(SYS_sigtimedwait, (long)*set, tp));
}

int sigwait(const sigset_t *set, int *sig) {
    for (;;) {
        long r = __sc2(SYS_sigtimedwait, (long)*set, 0);
        if (r > 0) { *sig = (int)r; return 0; }
        if (r != -EINTR) return (int)-r;        /* EINTR: a handler ran for some other signal; keep waiting */
    }
}

int sigpending(sigset_t *set) {
    unsigned v = 0;
    long r = __ret(__sc1(SYS_sigpending, (long)&v));
    if (r == 0) *set = v;
    return (int)r;
}

int sigemptyset(sigset_t *s) { *s = 0; return 0; }
int sigfillset(sigset_t *s) { *s = ~0U; return 0; }
int sigaddset(sigset_t *s, int sig) { *s |= 1U << sig; return 0; }
int sigdelset(sigset_t *s, int sig) { *s &= ~(1U << sig); return 0; }
int kill(pid_t pid, int sig) { return (int)__ret(__sc2(SYS_kill, pid, sig)); }
int raise(int sig) { return (int)__ret(__sc3(SYS_tgkill, getpid(), gettid(), sig)); }     /* aimed at the calling thread */

/* ---------------------------------------------------------------- environment */

char *getenv(const char *name) {
    size_t n = strlen(name);
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    return NULL;
}

static int g_env_owned;

static int env_count(void) {
    int n = 0;
    while (environ && environ[n]) n++;
    return n;
}

static int env_grow(int extra) {
    int n = env_count();
    char **ne = malloc((size_t)(n + extra + 1) * sizeof(char *));
    if (!ne) return -1;
    for (int i = 0; i < n; i++) ne[i] = environ[i];
    ne[n] = NULL;
    if (g_env_owned) free(environ);
    environ = ne;
    g_env_owned = 1;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t nl = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (!strncmp(*e, name, nl) && (*e)[nl] == '=') {
            if (!overwrite) return 0;
            char *s = malloc(nl + strlen(value) + 2);
            if (!s) return -1;
            sprintf(s, "%s=%s", name, value);
            *e = s;
            return 0;
        }
    }
    if (env_grow(1)) return -1;
    char *s = malloc(nl + strlen(value) + 2);
    if (!s) return -1;
    sprintf(s, "%s=%s", name, value);
    int n = env_count();
    environ[n] = s;
    environ[n + 1] = NULL;
    return 0;
}

int putenv(char *str) {
    char *eq = strchr(str, '=');
    if (!eq) return unsetenv(str);
    char name[128];
    size_t nl = (size_t)(eq - str);
    if (nl >= sizeof name) return -1;
    memcpy(name, str, nl);
    name[nl] = 0;
    return setenv(name, eq + 1, 1);
}

int unsetenv(const char *name) {
    size_t nl = strlen(name);
    int n = env_count();
    for (int i = 0; i < n; i++) {
        if (!strncmp(environ[i], name, nl) && environ[i][nl] == '=') {
            for (int j = i; j < n; j++) environ[j] = environ[j + 1];
            return 0;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- directories */

struct DIR {
    int fd;
    struct dirent ent;
};

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof *d);
    if (!d) { close(fd); return NULL; }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d) {
    struct gdirent g;
    long r = __ret(__sc3(SYS_readdir, d->fd, (long)&g, 1));
    if (r <= 0) return NULL;
    d->ent.d_ino = g.d_ino;
    d->ent.d_type = (unsigned char)g.d_type;
    strlcpy(d->ent.d_name, g.d_name, sizeof d->ent.d_name);
    return &d->ent;
}

int closedir(DIR *d) {
    int r = close(d->fd);
    free(d);
    return r;
}

void rewinddir(DIR *d) { lseek(d->fd, 0, SEEK_SET); }

/* ---------------------------------------------------------------- misc */

int uname(struct utsname *u) { return (int)__ret(__sc1(SYS_uname, (long)u)); }

int gethostname(char *buf, size_t n) {
    struct utsname u;
    if (uname(&u) < 0) return -1;
    strlcpy(buf, u.nodename, n);
    return 0;
}

long sysconf(int name) {
    if (name == _SC_PAGESIZE) return 4096;
    if (name == _SC_NPROCESSORS_ONLN) return 1;
    if (name == _SC_CLK_TCK) return 100;
    errno = EINVAL;
    return -1;
}

int gap_sysinfo(struct gsysinfo *si) { return (int)__ret(__sc1(SYS_sysinfo, (long)si)); }
int gap_reboot(int how) { return (int)__ret(__sc1(SYS_reboot, how)); }
int gap_mount(const char *src, const char *target, const char *fstype, unsigned long flags) {
    return (int)__ret(__sc4(SYS_mount, (long)src, (long)target, (long)fstype, (long)flags));
}
int gap_umount(const char *target) { return (int)__ret(__sc1(SYS_umount, (long)target)); }
void gap_setname(const char *name) { __sc1(SYS_setname, (long)name); }
void gap_log(const char *msg) { __sc1(SYS_debuglog, (long)msg); }
long gap_time_ms(void) { return __sc0(SYS_gettime_ms); }
int gap_settime(long ms, long rtc_epoch) { return (int)__ret(__sc2(SYS_settime, ms, rtc_epoch)); }
int gap_getrandom(void *buf, unsigned long n) { return (int)__ret(__sc3(SYS_getrandom, (long)buf, (long)n, 0)); }

