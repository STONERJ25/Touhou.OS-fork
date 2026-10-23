/* User-space test suite: exercises libc and the system call layer. Run with "utest" on the kernel command line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <poll.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <pty.h>
#include <gensokyo.h>

static int g_fail;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno); g_fail++; } } while (0)
#define SECTION(name) printf("utest: %s\n", name)

static volatile int g_sig_seen;
static void on_usr1(int sig) { g_sig_seen = sig; }

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int depth(int n) {
    volatile char pad[512];
    pad[0] = (char)n;
    return n == 0 ? pad[0] : depth(n - 1) + 1;
}

static void test_stdio(void) {
    SECTION("printf/sprintf/sscanf");
    char buf[128];
    snprintf(buf, sizeof buf, "%d|%5d|%-5d|%05d|%x|%#X|%o|%c|%s|%.3s|%lu", 42, 42, 42, 42, 255, 255, 8, 'k', "str", "abcdef", 123456789012UL);
    CHECK(!strcmp(buf, "42|   42|42   |00042|ff|0XFF|10|k|str|abc|123456789012"));
    snprintf(buf, sizeof buf, "%.2f %6.1f %e %g %g", 3.14159, 2.5, 12345.678, 0.0001, 1e10);
    CHECK(!strcmp(buf, "3.14    2.5 1.234568e+04 0.0001 1e+10"));
    snprintf(buf, sizeof buf, "%.0f %.1f %f", 0.5, 0.05, -1.5);
    printf("  floats: %s\n", buf);
    int a = 0, b = 0;
    char word[32] = "";
    double d = 0;
    CHECK(sscanf("12 -7 hello 2.5", "%d %d %31s %lf", &a, &b, word, &d) == 4);
    CHECK(a == 12 && b == -7 && !strcmp(word, "hello") && d > 2.49 && d < 2.51);
    CHECK(strtol("0x1f", NULL, 0) == 31 && strtol("-42", NULL, 10) == -42);
}

static void test_memory(void) {
    SECTION("malloc/free/realloc, mmap, deep stack");
    void *p[200];
    for (int i = 0; i < 200; i++) { p[i] = malloc((size_t)(i * 37 % 5000) + 1); CHECK(p[i]); memset(p[i], i, (size_t)(i * 37 % 5000) + 1); }
    for (int i = 0; i < 200; i += 2) free(p[i]);
    for (int i = 1; i < 200; i += 2) { CHECK(((unsigned char *)p[i])[0] == (unsigned char)i); free(p[i]); }
    char *s = malloc(8);
    strcpy(s, "reimu");
    s = realloc(s, 100000);
    CHECK(!strcmp(s, "reimu"));
    free(s);
    char *big = malloc(1 << 20);
    CHECK(big);
    big[(1 << 20) - 1] = 7;
    free(big);
    char *m = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    m[0] = 1; m[(1 << 16) - 1] = 2;
    CHECK(munmap(m, 1 << 16) == 0);
    CHECK(depth(2000) == 2000);              /* ~1 MiB of stack: needs demand paging */
    int arr[] = { 5, 3, 9, 1, 7 };
    qsort(arr, 5, sizeof(int), cmp_int);
    CHECK(arr[0] == 1 && arr[4] == 9);
}

static void test_files(void) {
    SECTION("files and directories");
    CHECK(mkdir("/tmp/ut", 0755) == 0 || errno == EEXIST);
    int fd = open("/tmp/ut/a.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "gensokyo", 8) == 8);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char buf[16] = "";
    CHECK(read(fd, buf, 15) == 8 && !strcmp(buf, "gensokyo"));
    CHECK(lseek(fd, 4, SEEK_SET) == 4 && read(fd, buf, 4) == 4 && !memcmp(buf, "okyo", 4));
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 8 && S_ISREG(st.st_mode));
    close(fd);
    CHECK(rename("/tmp/ut/a.txt", "/tmp/ut/b.txt") == 0);
    CHECK(stat("/tmp/ut/a.txt", &st) < 0 && errno == ENOENT);
    CHECK(symlink("b.txt", "/tmp/ut/link") == 0);
    char target[32] = "";
    CHECK(readlink("/tmp/ut/link", target, sizeof target - 1) == 5);
    fd = open("/tmp/ut/link", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, 8) == 8);
    close(fd);
    DIR *d = opendir("/tmp/ut");
    int names = 0;
    CHECK(d);
    for (struct dirent *e; d && (e = readdir(d));) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names++;
    if (d) closedir(d);
    CHECK(names == 2);
    CHECK(unlink("/tmp/ut/link") == 0 && unlink("/tmp/ut/b.txt") == 0 && rmdir("/tmp/ut") == 0);
    CHECK(chdir("/Hakurei") == 0);
    char cwd[64];
    CHECK(getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/Hakurei"));
    CHECK(access("/etc", F_OK) == 0);
    chdir("/");
    FILE *f = fopen("/tmp/ut2.txt", "w");
    CHECK(f);
    fprintf(f, "line %d\nline %d\n", 1, 2);
    fclose(f);
    f = fopen("/tmp/ut2.txt", "r");
    char line[32];
    CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "line 1\n") && fgets(line, sizeof line, f) && !fgets(line, sizeof line, f));
    if (f) fclose(f);
    unlink("/tmp/ut2.txt");
}

static void test_processes(void) {
    SECTION("fork/exec/wait/pipe");
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        execl("/bin/hello", "hello", "one", "two", (char *)NULL);
        _exit(111);
    }
    CHECK(pid > 0);
    close(fds[1]);
    char out[512];
    ssize_t n = 0, total = 0;
    while ((n = read(fds[0], out + total, sizeof out - 1 - (size_t)total)) > 0) total += n;
    out[total] = 0;
    close(fds[0]);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(strstr(out, "Hello from Gensokyo") && strstr(out, "argv[2] = two"));

    pid = fork();
    if (pid == 0) _exit(42);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    CHECK(waitpid(-1, &status, 0) < 0 && errno == ECHILD);

    int ready = 0;
    CHECK(pipe(fds) == 0);
    struct pollfd pf = { fds[0], POLLIN, 0 };
    CHECK(poll(&pf, 1, 20) == 0);
    write(fds[1], "x", 1);
    ready = poll(&pf, 1, 20);
    CHECK(ready == 1 && (pf.revents & POLLIN));
    close(fds[0]); close(fds[1]);
}

static void test_signals(void) {
    SECTION("signals");
    struct sigaction sa = { on_usr1, 0, 0 };
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    g_sig_seen = 0;
    raise(SIGUSR1);
    CHECK(g_sig_seen == SIGUSR1);

    pid_t pid = fork();
    if (pid == 0) { for (;;) pause(); }
    usleep(20000);
    CHECK(kill(pid, SIGKILL) == 0);
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

    pid = fork();
    if (pid == 0) { volatile int *p = (volatile int *)0; *p = 1; _exit(0); }
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    pid = fork();
    if (pid == 0) { signal(SIGTERM, SIG_IGN); raise(SIGTERM); _exit(7); }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7);
}

static void test_time_math(void) {
    SECTION("time and math");
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    usleep(30000);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 25 && ms < 200);
    CHECK(time(NULL) > 1700000000);
    CHECK(fabs(sin(M_PI / 6) - 0.5) < 1e-9);
    CHECK(fabs(cos(0) - 1.0) < 1e-12);
    CHECK(fabs(sqrt(2.0) - 1.4142135623730951) < 1e-12);
    CHECK(fabs(exp(1.0) - M_E) < 1e-9);
    CHECK(fabs(log(M_E) - 1.0) < 1e-9);
    CHECK(fabs(pow(2.0, 10.0) - 1024.0) < 1e-6);
    CHECK(fabs(atan2(1, 1) - M_PI_4) < 1e-9);
    CHECK(floor(-1.5) == -2.0 && ceil(1.2) == 2.0);
}

static void test_misc(void) {
    SECTION("environment, strings, credentials");
    setenv("GAPTEST", "yukari", 1);
    CHECK(getenv("GAPTEST") && !strcmp(getenv("GAPTEST"), "yukari"));
    unsetenv("GAPTEST");
    CHECK(!getenv("GAPTEST"));
    char *tok, *save, str[] = "a,b;c";
    int n = 0;
    for (tok = strtok_r(str, ",;", &save); tok; tok = strtok_r(NULL, ",;", &save)) n++;
    CHECK(n == 3);
    CHECK(getuid() == 0);

    mkdir("/tmp/priv", 0700);
    int fd = open("/tmp/priv/secret", O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    close(fd);
    pid_t pid = fork();
    if (pid == 0) {
        setuid(1000);
        fd = open("/tmp/priv/secret", O_RDONLY);
        _exit(fd < 0 && errno == EACCES ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unlink("/tmp/priv/secret");
    rmdir("/tmp/priv");

    jmp_buf jb;
    volatile int hit = 0;
    if (setjmp(jb) == 0) { hit = 1; longjmp(jb, 1); }
    CHECK(hit == 1);

    struct gsysinfo si;
    CHECK(gap_sysinfo(&si) == 0 && si.total_ram > (64u << 20) && si.nprocs >= 1);
}

/* Sockets, shared memory, pseudo-terminals and the device nodes the window server relies on. */
static void test_ipc(void) {
    SECTION("sockets, shared memory, ptys");
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(write(sv[0], "ping", 4) == 4);
    char b[16] = {0};
    CHECK(read(sv[1], b, sizeof b) == 4 && !memcmp(b, "ping", 4));
    CHECK(write(sv[1], "pong!", 5) == 5 && read(sv[0], b, sizeof b) == 5);
    close(sv[0]);
    CHECK(read(sv[1], b, sizeof b) == 0);                       /* end of file once the peer is gone */
    CHECK(write(sv[1], "x", 1) < 0 && errno == EPIPE);
    close(sv[1]);

    /* A named socket and a forked client */
    unlink("/tmp/ut.sock");
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/tmp/ut.sock");
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 4) == 0);
    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(s2, (struct sockaddr *)&sa, sizeof sa) < 0 && errno == EADDRINUSE);
    close(s2);
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&sa, sizeof sa) < 0) _exit(2);
        write(c, "hello gap", 9);
        char r[16] = {0};
        int n = (int)read(c, r, sizeof r);
        _exit(n == 6 && !memcmp(r, "reply!", 6) ? 0 : 3);
    }
    int conn = accept(ls, NULL, NULL);
    CHECK(conn >= 0);
    memset(b, 0, sizeof b);
    CHECK(read(conn, b, sizeof b) == 9 && !memcmp(b, "hello gap", 9));
    struct gpeercred pc;
    CHECK(getpeercred(conn, &pc) == 0 && pc.pid == pid);
    CHECK(write(conn, "reply!", 6) == 6);
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(conn);
    close(ls);
    int cl = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(cl, (struct sockaddr *)&sa, sizeof sa) < 0 && errno == ECONNREFUSED);   /* nobody listens any more */
    close(cl);
    unlink("/tmp/ut.sock");

    /* A shared mapping of a tmpfs file is visible to a forked child and survives deleting the file */
    int fd = open("/tmp/ut.shm", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && ftruncate(fd, 8192) == 0);
    volatile unsigned *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    m[0] = 0x1234;
    m[1024] = 0x5678;
    pid = fork();
    if (pid == 0) {
        _exit(m[0] == 0x1234 && m[1024] == 0x5678 ? (m[2] = 99, 0) : 1);
    }
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(m[2] == 99);                                          /* the child's write is ours too */
    CHECK(unlink("/tmp/ut.shm") == 0);
    CHECK(m[0] == 0x1234);                                      /* still mapped after the unlink */
    CHECK(munmap((void *)m, 8192) == 0);
    close(fd);

    /* Pseudo-terminal: master writes become input, slave output comes back with CRLF */
    char spath[40];
    int mfd = ptyopen(spath, sizeof spath);
    CHECK(mfd >= 0 && !strncmp(spath, "/dev/pts/", 9));
    int sfd = open(spath, O_RDWR | O_NOCTTY);
    CHECK(sfd >= 0);
    CHECK(write(mfd, "ls -l\n", 6) == 6);
    memset(b, 0, sizeof b);
    CHECK(read(sfd, b, sizeof b) == 6 && !memcmp(b, "ls -l\n", 6));          /* canonical mode: the whole line */
    CHECK(read(mfd, b, sizeof b) == 7 && !memcmp(b, "ls -l\r\n", 7));       /* ... and it was echoed back */
    CHECK(write(sfd, "out\n", 4) == 4);
    memset(b, 0, sizeof b);
    CHECK(read(mfd, b, sizeof b) == 5 && !memcmp(b, "out\r\n", 5));
    struct winsize ws = { 30, 100, 0, 0 };
    CHECK(ioctl(mfd, TIOCSWINSZ, &ws) == 0);
    struct winsize got;
    CHECK(ioctl(sfd, TIOCGWINSZ, &got) == 0 && got.ws_row == 30 && got.ws_col == 100);
    CHECK(isatty(sfd));
    char *tn = ttyname(sfd);
    CHECK(tn && !strncmp(tn, "/dev/pts/", 9));
    close(sfd);
    CHECK(read(mfd, b, sizeof b) == 0);                                     /* the program side is gone: end of file */
    close(mfd);

    /* The window server's devices */
    int fb = open("/dev/fb0", O_RDWR);
    if (fb >= 0) {
        struct gfbinfo info;
        CHECK(ioctl(fb, FBIOGET_INFO, &info) == 0 && info.width >= 640 && info.height >= 480 && info.bpp == 32 && info.pitch >= info.width * 4);
        unsigned *px = mmap(NULL, (size_t)info.pitch * info.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
        CHECK(px != MAP_FAILED);
        close(fb);
        if (px != MAP_FAILED) munmap(px, (size_t)info.pitch * info.height);
    } else printf("  note: no /dev/fb0\n");
    int ev = open("/dev/input/events", O_RDONLY | O_NONBLOCK);
    CHECK(ev >= 0);
    struct ginput_event ie;
    CHECK(read(ev, &ie, sizeof ie) < 0 && errno == EAGAIN);
    close(ev);
}

int main(void) {
    printf("utest: the shrine's test suite begins\n");
    test_ipc();
    test_stdio();
    test_memory();
    test_files();
    test_processes();
    test_signals();
    test_time_math();
    test_misc();
    if (g_fail) printf("UTEST FAIL (%d failures)\n", g_fail);
    else printf("UTEST PASS\n");
    return g_fail ? 1 : 0;
}
