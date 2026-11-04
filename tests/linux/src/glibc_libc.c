/*
 * A tour of what an ordinary C program asks of the system, through a statically linked C library: arguments and environment, the heap (small blocks from brk,
 * a large one from mmap), sorting, floating-point formatting and parsing, files (create, write, read back, stat, unlink, errors), processes (fork, wait, pipe),
 * clocks, the working directory, atexit and the exit status. Every line it prints is something the manual pages promise, so a real Linux prints the same;
 * tests/case_linux.py compares the output under TouhouOS with the stored output of a real Linux (tools/linux-oracle.py).
 *
 * Run as:  FOO=bar glibc_libc alpha beta     (the status is 3)
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void bye(void) { printf("atexit handler ran\n"); }

int main(int argc, char **argv) {
    atexit(bye);
    printf("argc=%d\n", argc);
    for (int i = 1; i < argc; i++) printf("argv[%d]=%s\n", i, argv[i]);
    const char *foo = getenv("FOO");
    printf("FOO=%s\n", foo ? foo : "(unset)");

    char *small = malloc(100);
    memset(small, 'a', 99);
    small[99] = 0;
    printf("small block: %zu bytes\n", strlen(small));
    free(small);
    char *big = malloc(1 << 20);
    memset(big, 7, 1 << 20);
    long sum = 0;
    for (int i = 0; i < (1 << 20); i += 4096) sum += big[i];
    printf("large block: sum %ld\n", sum);
    free(big);
    void *many[200];
    for (int i = 0; i < 200; i++) { many[i] = malloc(1000 + i); memset(many[i], i, 1000 + i); }
    int good = 1;
    for (int i = 0; i < 200; i++) if (((unsigned char *)many[i])[1000 + i - 1] != (unsigned char)i) good = 0;
    for (int i = 0; i < 200; i++) free(many[i]);
    printf("200 blocks intact: %d\n", good);

    int v[] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof v[0], cmp);
    printf("sorted: %d %d %d %d %d\n", v[0], v[1], v[2], v[3], v[4]);
    printf("float: %.3f %e %g\n", 3.14159, 12345.678, 0.0001);
    printf("strtod: %.2f\n", strtod("2.5e3", NULL) / 7);
    printf("math: %.4f %.1f %.4f\n", sqrt(2.0), pow(2.0, 10.0), sin(1.0));
    printf("integer: %d %ld %u %x %5d|%-5d|\n", -42, 1234567890123L, 4000000000u, 255, 42, 42);
    char buf[64];
    snprintf(buf, sizeof buf, "%s-%03d", "id", 7);
    printf("snprintf: %s\n", buf);

    const char *path = "/tmp/lnx_glibc_libc.txt";
    FILE *f = fopen(path, "w");
    fprintf(f, "line %d\n", 1);
    fputs("second line\n", f);
    fclose(f);
    f = fopen(path, "r");
    char line[64];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
    struct stat st;
    int r = stat(path, &st);
    printf("stat: %d size %ld regular %d\n", r, (long)st.st_size, S_ISREG(st.st_mode) ? 1 : 0);
    errno = 0;
    int fd = open("/no/such/file", O_RDONLY);
    printf("open of a missing file: %d, errno %d (%s)\n", fd, errno, strerror(errno));
    unlink(path);
    errno = 0;
    r = stat(path, &st);
    printf("after unlink: stat %d, errno %d\n", r, errno);

    fflush(stdout);
    pid_t c = fork();
    if (c == 0) _exit(5);
    int status = 0;
    waitpid(c, &status, 0);
    printf("child: exited %d, status %d\n", WIFEXITED(status) ? 1 : 0, WEXITSTATUS(status));
    int p[2];
    pipe(p);
    write(p[1], "through a pipe\n", 15);
    char pb[32];
    ssize_t n = read(p[0], pb, 31);
    pb[n] = 0;
    fputs(pb, stdout);
    close(p[0]);
    close(p[1]);

    printf("time is after 2020: %d\n", time(NULL) > 1577836800 ? 1 : 0);
    struct timespec ts;
    printf("monotonic clock: %d\n", clock_gettime(CLOCK_MONOTONIC, &ts));
    char cwd[256];
    printf("getcwd: %d\n", getcwd(cwd, sizeof cwd) != NULL);
    printf("pid is positive: %d\n", getpid() > 0);
    return 3;
}
