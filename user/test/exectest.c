/* exectest: execve's limits as seen from user space (docs/HARDENING.md).
 *
 * Run by tests/case_hardening.py as /usr/lib/kernel-tests/exectest. The program execs itself with argument lists of every
 * shape; the copy that comes out the other end (--child) checks that everything arrived intact. Limits (kernel/include/process.h):
 * 32768 entries per vector, 128 KiB per string (NUL included), 512 KiB for all strings plus their pointers; E2BIG beyond that. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define MAX_ARGS   32768
#define MAX_STRING (128 * 1024)

static const char *g_self = "/usr/lib/kernel-tests/exectest";
static int g_fail;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno); g_fail++; } } while (0)

#define SCRIPT_PATH "/tmp/exectest-script.sh"
#define SCRIPT_ARGS 2000

/* ---------------------------------------------------------------- the child: verifies what it was given */

static char pattern_char(size_t i) { return (char)('A' + i % 26); }

/* exectest --child many N        N further arguments "a0", "a1", ...
 * exectest --child big LEN       one further argument of LEN bytes: ABC...Z repeated
 * exectest --child empty N       N further empty arguments
 * exectest --child env N         N environment entries "E<i>=v<i>"; no further arguments
 * exectest --child bigenv LEN    environment entry BIG=<LEN bytes>
 * exectest --child none ...      only has to start */
static int child(int argc, char **argv) {
    if (argc < 3) return 90;
    const char *kind = argv[2];
    if (!strcmp(kind, "many")) {
        long n = atol(argv[3]);
        if (argc != 4 + n) return 91;
        for (long i = 0; i < n; i++) {
            char want[24];
            snprintf(want, sizeof want, "a%ld", i);
            if (strcmp(argv[4 + i], want)) return 92;
        }
        return argv[argc] == NULL ? 0 : 93;
    }
    if (!strcmp(kind, "big")) {
        long len = atol(argv[3]);
        if (argc != 5 || strlen(argv[4]) != (size_t)len) return 94;
        for (long i = 0; i < len; i++)
            if (argv[4][i] != pattern_char((size_t)i)) return 95;
        return 0;
    }
    if (!strcmp(kind, "empty")) {
        long n = atol(argv[3]);
        if (argc != 4 + n) return 96;
        for (long i = 0; i < n; i++)
            if (argv[4 + i][0]) return 97;
        return 0;
    }
    if (!strcmp(kind, "env")) {
        long n = atol(argv[3]);
        for (long i = 0; i < n; i++) {
            char name[24], want[24];
            snprintf(name, sizeof name, "E%ld", i);
            snprintf(want, sizeof want, "v%ld", i);
            const char *v = getenv(name);
            if (!v || strcmp(v, want)) return 98;
        }
        long count = 0;
        for (char **e = environ; *e; e++) count++;
        return count == n ? 0 : 99;
    }
    if (!strcmp(kind, "bigenv")) {
        long len = atol(argv[3]);
        const char *v = getenv("BIG");
        if (!v || strlen(v) != (size_t)len) return 100;
        for (long i = 0; i < len; i++)
            if (v[i] != pattern_char((size_t)i)) return 101;
        return 0;
    }
    if (!strcmp(kind, "none")) return 0;
    return 102;
}

/* The interpreter side of the script test: the kernel runs "exectest SCRIPT a0 a1 ...". */
static int script_child(int argc, char **argv) {
    if (argc != 2 + SCRIPT_ARGS || strcmp(argv[1], SCRIPT_PATH)) return 110;
    for (int i = 0; i < SCRIPT_ARGS; i++) {
        char want[24];
        snprintf(want, sizeof want, "a%d", i);
        if (strcmp(argv[2 + i], want)) return 111;
    }
    return 0;
}

/* ---------------------------------------------------------------- the parent: builds the lists and execs */

static char *make_pattern(size_t len) {
    char *s = malloc(len + 1);
    for (size_t i = 0; i < len; i++) s[i] = pattern_char(i);
    s[len] = 0;
    return s;
}

/* Forks, execs `prog` with argv/envp in the child, and returns the child's exit status (300 + n if signal n killed it). If the exec
 * itself fails the child exits with 200 + errno. */
static int run_prog(const char *prog, char **argv, char **envp) {
    pid_t pid = fork();
    if (pid == 0) {
        execve(prog, argv, envp);
        _exit(200 + (errno & 0x3f));
    }
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid) return -1;
    if (WIFSIGNALED(st)) return 300 + WTERMSIG(st);
    return WEXITSTATUS(st);
}

static int run(char **argv, char **envp) { return run_prog(g_self, argv, envp); }

#define E2BIG_EXIT (200 + E2BIG)
#define EFAULT_EXIT (200 + EFAULT)

static char *g_no_env[1];

static void test_many(long n) {
    char **av = malloc((size_t)(n + 5) * sizeof *av);
    char **store = malloc((size_t)(n + 1) * sizeof *store);
    char num[16];
    snprintf(num, sizeof num, "%ld", n);
    av[0] = (char *)g_self; av[1] = "--child"; av[2] = "many"; av[3] = num;
    for (long i = 0; i < n; i++) {
        store[i] = malloc(16);
        snprintf(store[i], 16, "a%ld", i);
        av[4 + i] = store[i];
    }
    av[4 + n] = NULL;
    int rc = run(av, g_no_env);
    printf("exectest: %ld short arguments -> %d\n", n, rc);
    CHECK(rc == 0);
    for (long i = 0; i < n; i++) free(store[i]);
    free(store);
    free(av);
}

static void test_big_arg(size_t len, int want) {
    char *s = make_pattern(len);
    char num[24];
    snprintf(num, sizeof num, "%zu", len);
    char *av[] = { (char *)g_self, "--child", "big", num, s, NULL };
    int rc = run(av, g_no_env);
    printf("exectest: one argument of %zu bytes -> %d (want %d)\n", len, rc, want);
    CHECK(rc == want);
    free(s);
}

static void test_empty_args(long n, int want) {
    char **av = malloc((size_t)(n + 6) * sizeof *av);
    char num[16];
    snprintf(num, sizeof num, "%ld", n);
    av[0] = (char *)g_self; av[1] = "--child"; av[2] = "empty"; av[3] = num;
    long total = 4;
    for (long i = 0; i < n; i++) av[total++] = "";
    av[total] = NULL;
    int rc = run(av, g_no_env);
    printf("exectest: %ld entries in argv -> %d (want %d)\n", total, rc, want);
    CHECK(rc == want);
    free(av);
}

/* k arguments of `each` bytes (the child only has to start) */
static void test_total(int k, size_t each, int want) {
    char **av = malloc((size_t)(k + 5) * sizeof *av);
    char *s = make_pattern(each);
    int n = 0;
    av[n++] = (char *)g_self; av[n++] = "--child"; av[n++] = "none";
    for (int i = 0; i < k; i++) av[n++] = s;
    av[n] = NULL;
    int rc = run(av, g_no_env);
    printf("exectest: %d arguments of %zu bytes (%zu in all) -> %d (want %d)\n", k, each, (size_t)k * each, rc, want);
    CHECK(rc == want);
    free(s);
    free(av);
}

static void test_env(long n) {
    char **ev = malloc((size_t)(n + 1) * sizeof *ev);
    for (long i = 0; i < n; i++) {
        ev[i] = malloc(32);
        snprintf(ev[i], 32, "E%ld=v%ld", i, i);
    }
    ev[n] = NULL;
    char num[16];
    snprintf(num, sizeof num, "%ld", n);
    char *av[] = { (char *)g_self, "--child", "env", num, NULL };
    int rc = run(av, ev);
    printf("exectest: %ld environment entries -> %d\n", n, rc);
    CHECK(rc == 0);
    for (long i = 0; i < n; i++) free(ev[i]);
    free(ev);
}

static void test_big_env(size_t len, int want) {
    char *val = make_pattern(len);
    char *entry = malloc(len + 8);
    snprintf(entry, len + 8, "BIG=%s", val);
    char *ev[] = { entry, NULL };
    char num[24];
    snprintf(num, sizeof num, "%zu", len);
    char *av[] = { (char *)g_self, "--child", "bigenv", num, NULL };
    int rc = run(av, ev);
    printf("exectest: one environment entry of %zu bytes -> %d (want %d)\n", len + 4, rc, want);
    CHECK(rc == want);
    free(entry);
    free(val);
}

/* argv and envp together over the total, each part fine on its own */
static void test_args_plus_env(void) {
    size_t each = 100 * 1024;
    char *s = make_pattern(each);
    char *entry = malloc(each + 8);
    snprintf(entry, each + 8, "K=%s", s);
    char *av[] = { (char *)g_self, "--child", "none", s, s, s, NULL };         /* 300 KiB */
    char *ev1[] = { entry, NULL };                                              /* 100 KiB more: 400 KiB, fine */
    int rc = run(av, ev1);
    printf("exectest: 300 KiB of arguments and 100 KiB of environment -> %d\n", rc);
    CHECK(rc == 0);
    char *ev3[] = { entry, entry, entry, NULL };                                /* 300 KiB more: 600 KiB, too much */
    rc = run(av, ev3);
    printf("exectest: 300 KiB of arguments and 300 KiB of environment -> %d (want %d)\n", rc, E2BIG_EXIT);
    CHECK(rc == E2BIG_EXIT);
    free(entry);
    free(s);
}

/* NULL vectors, pointers into nowhere */
static void test_odd_pointers(void) {
    int rc = run_prog("/bin/hello", NULL, NULL);                                /* argc == 0 is legal; hello may do what it likes about it */
    printf("exectest: NULL argv and envp -> %d\n", rc);
    CHECK(rc < 200 || rc >= 300);                                               /* it started: not an exec error */

    char *av1[] = { (char *)g_self, (char *)(unsigned long)0xFFFFFFFF80000000UL, NULL };      /* a string in the kernel half */
    rc = run(av1, g_no_env);
    printf("exectest: argument pointing into the kernel -> %d (want %d)\n", rc, EFAULT_EXIT);
    CHECK(rc == EFAULT_EXIT);

    char *page = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    if (page != MAP_FAILED) {
        memset(page, 'x', 4096);                                                /* no NUL anywhere in the first page */
        munmap(page + 4096, 4096);                                              /* the second page is gone */
        char *av2[] = { (char *)g_self, page + 4000, NULL };                    /* runs off the end of the mapping */
        rc = run(av2, g_no_env);
        printf("exectest: unterminated argument at the end of the mapping -> %d (want %d)\n", rc, EFAULT_EXIT);
        CHECK(rc == EFAULT_EXIT);
        char **bad = (char **)(page + 4096 - 8);                                /* the pointer array itself runs off the end */
        bad[0] = (char *)g_self;
        rc = run(bad, g_no_env);
        printf("exectest: argv that runs off the end of the mapping -> %d (want %d)\n", rc, EFAULT_EXIT);
        CHECK(rc == EFAULT_EXIT);
        munmap(page, 4096);
    }
}

/* A script with many arguments: the kernel puts the interpreter in front, the script's path next. */
static void test_script(void) {
    int fd = open(SCRIPT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    CHECK(fd >= 0);
    if (fd < 0) return;
    char line[160];
    int len = snprintf(line, sizeof line, "#!%s\n", g_self);
    CHECK(write(fd, line, (size_t)len) == len);
    close(fd);
    char **av = malloc((size_t)(SCRIPT_ARGS + 2) * sizeof *av);
    char **store = malloc((size_t)SCRIPT_ARGS * sizeof *store);
    av[0] = SCRIPT_PATH;
    for (int i = 0; i < SCRIPT_ARGS; i++) { store[i] = malloc(16); snprintf(store[i], 16, "a%d", i); av[1 + i] = store[i]; }
    av[1 + SCRIPT_ARGS] = NULL;
    int rc = run_prog(SCRIPT_PATH, av, g_no_env);
    printf("exectest: script with %d arguments -> %d\n", SCRIPT_ARGS, rc);
    CHECK(rc == 0);
    for (int i = 0; i < SCRIPT_ARGS; i++) free(store[i]);
    free(store);
    free(av);
    unlink(SCRIPT_PATH);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--child")) return child(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], SCRIPT_PATH)) return script_child(argc, argv);
    if (argc < 1) return 120;                           /* started with an empty argument list: nothing to test */
    if (argc >= 3 && !strcmp(argv[1], "--self")) g_self = argv[2];
    printf("exectest: execve limits begin\n");

    test_many(1);
    test_many(300);                         /* more than the old limit of 256 */
    test_many(5000);
    test_many(20000);
    test_big_arg(100 * 1024, 0);
    test_big_arg(MAX_STRING - 1, 0);        /* 131071 bytes + NUL = exactly the limit */
    test_big_arg(MAX_STRING, E2BIG_EXIT);   /* one more */
    test_big_arg(1000000, E2BIG_EXIT);
    test_empty_args(MAX_ARGS - 4, 0);                   /* 32768 entries: the vector limit */
    test_empty_args(MAX_ARGS - 3, E2BIG_EXIT);          /* 32769 */
    test_empty_args(60000, E2BIG_EXIT);
    test_total(4, 100 * 1024, 0);                       /* 400 KiB */
    test_total(5, 100 * 1024, 0);                       /* 500 KiB and 8 pointers: still under 512 KiB */
    test_total(6, 100 * 1024, E2BIG_EXIT);              /* 600 KiB */
    test_total(30, 100 * 1024, E2BIG_EXIT);             /* 3 MiB */
    test_env(5000);
    test_big_env(100 * 1024, 0);
    test_big_env(MAX_STRING, E2BIG_EXIT);
    test_args_plus_env();
    test_odd_pointers();
    test_script();

    printf(g_fail ? "EXECTEST FAIL (%d)\n" : "EXECTEST PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
