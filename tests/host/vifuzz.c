/* vifuzz: the guest side of tests/host/vi_diff.py (a differential test of vi against the host's vim). It is not built by default:
 * `vi_diff.py gen` copies it to user/bin/vifuzz.c for the run and `vi_diff.py clean` removes it again.
 * usage: vifuzz cases.dat [from [to]]
 * cases.dat: "VFZ1", then per case: u32 name_len name, u32 in_len in, u32 keys_len keys, u32 out_len out (little endian).
 * Each case writes /tmp/x (the input) and /tmp/k (the keys), runs "vi -s /tmp/k /tmp/x" and compares /tmp/x with out. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>

static unsigned char *data;
static size_t dlen, dpos;

static unsigned get32(void) {
    unsigned v = 0;
    if (dpos + 4 > dlen) return 0;
    v = data[dpos] | (data[dpos + 1] << 8) | (data[dpos + 2] << 16) | ((unsigned)data[dpos + 3] << 24);
    dpos += 4;
    return v;
}

static int write_file(const char *path, const void *p, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        c += w;
        n -= (size_t)w;
    }
    close(fd);
    return 0;
}

static unsigned char *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    unsigned char *b = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap); }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    *len = n;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vifuzz cases.dat [from [to]]\n"); return 2; }
    int from = argc > 2 ? atoi(argv[2]) : 0, to = argc > 3 ? atoi(argv[3]) : 1 << 30;
    data = read_file(argv[1], &dlen);
    if (!data || dlen < 4 || memcmp(data, "VFZ1", 4)) { fprintf(stderr, "vifuzz: bad cases file\n"); return 2; }
    dpos = 4;
    setenv("LINES", "24", 1);
    setenv("COLUMNS", "80", 1);
    int idx = 0, ran = 0, bad = 0, hangs = 0, crashes = 0;
    while (dpos < dlen && idx < to) {
        unsigned nl = get32();
        const char *name = (const char *)data + dpos;
        dpos += nl;
        unsigned il = get32();
        const unsigned char *in = data + dpos;
        dpos += il;
        unsigned kl = get32();
        const unsigned char *keys = data + dpos;
        dpos += kl;
        unsigned ol = get32();
        const unsigned char *out = data + dpos;
        dpos += ol;
        if (idx++ < from) continue;
        ran++;
        unlink("/tmp/x");
        if (write_file("/tmp/x", in, il) || write_file("/tmp/k", keys, kl)) { printf("ERR %d cannot write\n", idx - 1); continue; }
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/vi", "vi", "-s", "/tmp/k", "/tmp/x", (char *)NULL);
            _exit(127);
        }
        int st = 0, done = 0;
        time_t t0 = time(NULL);
        while (!done) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) { done = 1; break; }
            if (r < 0 && errno != EINTR) { done = 1; break; }
            if (time(NULL) - t0 > 20) { kill(pid, SIGKILL); waitpid(pid, &st, 0); hangs++; printf("HANG %d %.*s\n", idx - 1, (int)nl, name); done = 2; break; }
            struct timespec ts = { 0, 5 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (done == 2) continue;
        if (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) >= 128)) {
            crashes++;
            printf("CRASH %d %.*s status=%d\n", idx - 1, (int)nl, name, WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st));
            continue;
        }
        size_t gl = 0;
        unsigned char *got = read_file("/tmp/x", &gl);
        if (!got || gl != ol || memcmp(got, out, ol)) {
            bad++;
            printf("DIFF %d %.*s\nGOT ", idx - 1, (int)nl, name);
            for (size_t i = 0; got && i < gl && i < 3000; i++) printf("%02x", got[i]);
            printf("\n");
        }
        free(got);
    }
    printf("DONE ran=%d diff=%d hang=%d crash=%d\n", ran, bad, hangs, crashes);
    return 0;
}
