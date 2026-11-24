/* forkbench: what a fork costs (docs/COW.md, "Measurements"). Run it once on a kernel started with "cow=off" and once without, and compare.
 *
 *   forkbench            fork+wait of a process with 1, 16 and 64 MiB of touched private memory (the time of one fork+wait, and the free memory a living child costs),
 *                        then 200 fork+exec of /bin/true
 *
 * One line per number: "FORKBENCH name=value". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define MIB (1024UL * 1024)

static long now_us(void) {
    struct timespec ts;
    clock_gettime(1, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
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

static void bench(unsigned mib) {
    size_t n = mib * MIB;
    char *m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { printf("FORKBENCH mmap_%u=failed\n", mib); return; }
    for (size_t i = 0; i < n; i += 4096) m[i] = 1;
    int rounds = mib >= 64 ? 5 : 20, failed = 0;
    long t0 = now_us();
    for (int i = 0; i < rounds; i++) {
        pid_t p = fork();
        if (p < 0) { failed++; continue; }
        if (p == 0) _exit(0);
        waitpid(p, NULL, 0);
    }
    long per = (now_us() - t0) / rounds;
    int go[2];
    pipe(go);
    long before = memfree_kb();
    pid_t p = fork();
    if (p == 0) { char c; read(go[0], &c, 1); _exit(0); }
    long during = memfree_kb();
    write(go[1], "x", 1);
    if (p > 0) waitpid(p, NULL, 0); else failed++;
    close(go[0]); close(go[1]);
    printf("FORKBENCH fork_wait_%umib_us=%ld\nFORKBENCH child_cost_%umib_kb=%ld\nFORKBENCH failed_%umib=%d\n", mib, per, mib, before - during, mib, failed);
    fflush(stdout);
    munmap(m, n);
}

int main(void) {
    bench(1);
    bench(16);
    bench(64);
    long t0 = now_us();
    int bad = 0;
    for (int i = 0; i < 200; i++) {
        pid_t p = fork();
        if (p == 0) { execl("/bin/true", "true", (char *)NULL); _exit(99); }
        int st = 0;
        if (p < 0 || waitpid(p, &st, 0) != p || st != 0) bad++;
    }
    printf("FORKBENCH fork_exec_200_ms=%ld\nFORKBENCH fork_exec_bad=%d\nFORKBENCH done\n", (now_us() - t0) / 1000, bad);
    return 0;
}
