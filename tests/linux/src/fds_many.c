/* More than 64 descriptors (docs/COMPAT.md, "Descriptors, FIFOs, signalfd and pidfd"): RLIMIT_NOFILE as getrlimit(2) and /proc/PID/limits show it, EMFILE at the
 * soft limit, dup2 / F_DUPFD beyond it (EBADF / EINVAL), raising it, select(2) on descriptor 1000, poll(2)'s nfds bound, fork and execve with close-on-exec flags
 * above 64, /proc/self/fd and close_range(2). Written from the manual pages getrlimit(2), dup(2), fcntl(2), select(2), poll(2), close_range(2) and proc(5); the
 * program prints only facts those pages promise, so a real Linux and TouhouOS print the same lines. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

static void fact(const char *what, long v) { printf("%s: %ld\n", what, v); fflush(stdout); }
static const char *ename(int e) { return e == EMFILE ? "EMFILE" : e == EBADF ? "EBADF" : e == EINVAL ? "EINVAL" : e == EPERM ? "EPERM" : e == 0 ? "none" : "other"; }

static int count_proc_fds(void) {                       /* the entries of /proc/self/fd, without the descriptor the listing itself uses */
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.' && atoi(e->d_name) != dirfd(d)) n++;
    closedir(d);
    return n;
}

static int is_open(int fd) { return fcntl(fd, F_GETFD) >= 0; }

static int child_after_exec(void) {                     /* the program again, after execve: which of the high descriptors survived */
    fact("after exec: 1000 open (no close-on-exec)", is_open(1000));
    fact("after exec: 1500 open (close-on-exec)", is_open(1500));
    fact("after exec: 2000 open (no close-on-exec)", is_open(2000));
    fact("after exec: 70 open (close-on-exec)", is_open(70));
    fact("after exec: 71 open (no close-on-exec)", is_open(71));
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "exec-child")) return child_after_exec();
    close_range(3, ~0U, 0);                             /* exactly 0, 1 and 2 are open from here on */
    struct rlimit rl = { 1024, 4096 };
    fact("setrlimit NOFILE 1024/4096", setrlimit(RLIMIT_NOFILE, &rl));
    getrlimit(RLIMIT_NOFILE, &rl);
    fact("soft", (long)rl.rlim_cur);
    fact("hard", (long)rl.rlim_max);

    /* fill the table with pipes and duplicates until the soft limit says no */
    int last = -1, pipes = 0, dups = 0;
    errno = 0;
    for (;;) {
        int p[2];
        if (pipe(p) == 0) { pipes++; last = p[1]; }
        else break;
        int d = dup(p[0]);
        if (d < 0) break;
        dups++;
        last = d;
    }
    fact("stopped with", 0); printf("errno: %s\n", ename(errno));
    fact("highest descriptor", last);
    fact("pipes made", pipes);
    fact("dups made", dups);
    close(last);                                        /* a free slot for the listing's own descriptor */
    fact("open per /proc/self/fd", count_proc_fds());
    errno = 0;
    fact("dup2 to 1024", dup2(0, 1024)); printf("errno: %s\n", ename(errno));
    errno = 0;
    fact("F_DUPFD from 1024", fcntl(0, F_DUPFD, 1024)); printf("errno: %s\n", ename(errno));
    errno = 0;
    struct pollfd *pf = calloc(1025, sizeof *pf);
    for (int i = 0; i < 1025; i++) pf[i].fd = -1;
    fact("poll with 1025 entries", poll(pf, 1025, 0)); printf("errno: %s\n", ename(errno));
    fact("poll with 1024 entries", poll(pf, 1024, 0));

    /* free all but 0..2, raise the soft limit and use high numbers */
    close_range(3, ~0U, 0);
    fact("open after close_range", count_proc_fds());
    rl.rlim_cur = 2048;
    fact("raise soft to 2048", setrlimit(RLIMIT_NOFILE, &rl));
    rl.rlim_cur = 4096; rl.rlim_max = 4097;
    errno = 0;
    fact("raise hard above 4096 (not root, or above the kernel's table)", setrlimit(RLIMIT_NOFILE, &rl) == -1 && errno == EPERM);
    getrlimit(RLIMIT_NOFILE, &rl);
    fact("soft now", (long)rl.rlim_cur);
    FILE *lim = fopen("/proc/self/limits", "r");
    char line[256];
    while (lim && fgets(line, sizeof line, lim))
        if (!strncmp(line, "Max open files", 14)) { unsigned long s = 0, h = 0; sscanf(line + 14, "%lu %lu", &s, &h); fact("limits: soft", (long)s); fact("limits: hard", (long)h); }
    if (lim) fclose(lim);

    int p[2];
    pipe(p);
    fact("dup2 to 1000", dup2(p[0], 1000));
    fact("dup2 to 2000", dup2(p[1], 2000));
    fact("F_DUPFD_CLOEXEC from 1500", fcntl(p[0], F_DUPFD_CLOEXEC, 1500));
    fact("F_DUPFD from 1500 (next free)", fcntl(p[0], F_DUPFD, 1500));
    close(1501);
    fact("dup3 to 70 with O_CLOEXEC", dup3(p[0], 70, O_CLOEXEC));
    fact("dup3 to 71", dup3(p[0], 71, 0));
    fact("FD_CLOEXEC of 1500", fcntl(1500, F_GETFD) & FD_CLOEXEC);
    errno = 0;
    fact("dup2 to 2048", dup2(0, 2048)); printf("errno: %s\n", ename(errno));

    /* select on descriptor 1000: nothing to read, then one byte */
    fd_set rd;
    FD_ZERO(&rd); FD_SET(1000, &rd);
    struct timeval tv = { 0, 0 };
    fact("select 1000, empty", select(1001, &rd, NULL, NULL, &tv));
    write(2000, "x", 1);
    FD_ZERO(&rd); FD_SET(1000, &rd);
    tv.tv_sec = 1;
    fact("select 1000 after a write", select(1001, &rd, NULL, NULL, &tv));
    fact("1000 is in the set", FD_ISSET(1000, &rd) != 0);
    char c;
    fact("read 1000", read(1000, &c, 1));
    struct pollfd one = { .fd = 2000, .events = POLLOUT };
    fact("poll 2000 writable", poll(&one, 1, 0));
    fact("open per /proc/self/fd", count_proc_fds());

    /* fork: the child has every descriptor; execve keeps the ones without FD_CLOEXEC */
    pid_t kid = fork();
    if (kid == 0) {
        printf("child: 1000 %d, 1500 %d, 2000 %d, 70 %d, open %d\n", is_open(1000), is_open(1500), is_open(2000), is_open(70), count_proc_fds());
        fflush(stdout);
        write(2000, "y", 1);
        execl("/proc/self/exe", argv[0], "exec-child", (char *)NULL);
        execl(argv[0], argv[0], "exec-child", (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(kid, &st, 0);
    fact("child exit status", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    fact("the child's byte arrived on 1000", read(1000, &c, 1) == 1 && c == 'y');
    fact("close 2000", close(2000));
    fact("close 2000 again", close(2000));
    return 0;
}
