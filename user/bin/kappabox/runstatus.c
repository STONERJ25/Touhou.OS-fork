/* runstatus: runs programs and says how each one ended, the way the kernel reports it (docs/RUN_IN_GUEST.md).
 *
 * A shell folds "killed by signal 8" into the status 136, which cannot be told from a program that really exits with 136, and it gives a program that never ends all
 * the time it likes. This applet keeps the exit code and the signal apart, bounds the run with a time limit, and keeps the program's standard output and standard error
 * together in a file (one open file description for both, so their order is the order of the writes). It exists for tools/run-in-guest.py, which gives it hundreds of
 * programs in one boot and reads one record line per program from the console, and it is just as good by hand:
 *
 *   runstatus [-t SECONDS] [-C DIR] [-i FILE] [-o FILE [-e FILE]] PROGRAM [ARGUMENT...]
 *       Runs the program (found through PATH when the name has no slash), then says on standard error how it ended: "exit 3", "killed by signal 8 (SIGFPE)",
 *       "timed out after 5 s". The status of runstatus is the program's; 128+N after a signal, 124 after a time-out, 126 or 127 when the program could not be started.
 *       The program's input and output stay on the terminal unless -i FILE (standard input) and -o FILE (standard output and standard error, together) say otherwise;
 *       with -e FILE as well, standard error goes to that file and standard output alone to the -o file.
 *   runstatus -b MANIFEST [-w DIR] [-m SUBDIR]... [-t SECONDS] [-c BYTES] [-s] [-k]
 *       Runs every program of a manifest, one after the other, each in a fresh working directory DIR/ID (DIR defaults to /tmp/runstatus.PID; each -m SUBDIR is made
 *       inside it first), with standard input from /dev/null unless the job names a file, and prints one record per program (below). The directory of a job is removed
 *       afterwards unless -k.
 *
 * Every run is its own process group, and the whole group is killed when the program has ended (a program that left children behind does not leak them into the
 * next one). The time limit is -t (10 s by default) or the job's own; a program that is still running at the limit gets SIGKILL. A program that writes more than the
 * cap (-c, 256 KiB by default) to its output file gets SIGKILL as well.
 *
 * The manifest is a sequence of NUL-terminated strings, so any argument fits in it (the host side is tools/run-in-guest.py):
 *     "RSM1"  COUNT
 *     then COUNT times:  ID  TIMEOUT_MS  STDIN  NFILES  (DEST SRC){NFILES}  PATH  ARGC  ARG{ARGC}
 *   ID is a decimal number that names the job (its directory and its record); TIMEOUT_MS is decimal, 0 for the default; STDIN is a file name or "" for /dev/null;
 *   each (DEST SRC) pair copies the file SRC to the relative name DEST in the job's directory before the run; PATH is what is executed and ARG... is the argument
 *   vector the program receives (ARG 0 is the program's own name, which need not be PATH).
 *
 * The records go to standard output, each on a line of its own (a newline comes first, so that it starts a line whatever was printed before):
 *     @@RS-START COUNT
 *     @@RS ID EXIT SIGNAL FLAGS MS LENGTH BASE64 [ELENGTH EBASE64]
 *     @@RS-DONE COUNT
 *   EXIT and SIGNAL are decimal numbers, "-" for the one that does not apply (a program that exited has no signal, one that a signal killed has no exit code); FLAGS is
 *   "-" or letters: T = the time limit ended it, C = the output cap ended it, X = it could not be started (the output is then the reason). MS is the time it ran in
 *   milliseconds. LENGTH is the number of bytes of output and BASE64 is the output itself (standard alphabet, with padding; empty for no output), so no byte of it
 *   can be mistaken for the record around it. Standard output and standard error are one stream (one file, so their order is the order of the writes) unless -s asks
 *   for two: then LENGTH and BASE64 are standard output alone and ELENGTH and EBASE64 are standard error (the order between the two streams is not kept).
 */
#include "kappabox.h"
#include <stdint.h>

#define RS_DEFAULT_LIMIT_MS 10000L
#define RS_DEFAULT_CAP      (256L << 10)
#define RS_MAX_ARGS         4096
#define RS_MAX_JOBS         1000000
#define RS_MAX_SUBDIRS      16

struct job {
    long id;
    long timeout_ms;                /* 0: the default */
    const char *stdin_path;         /* NULL: inherited, "": /dev/null */
    int nfiles;
    char **files;                   /* DEST, SRC, DEST, SRC ... */
    const char *path;
    int argc;
    char **argv;
};

struct outcome {
    int started;                    /* 0: could not be started, reason in err */
    int err;
    const char *what;               /* the step that failed, for the message: "run", "open the input file of", ... */
    int signaled, code, sig;        /* how it ended: exit code, or the signal that killed it */
    int timed_out, capped;
    long ms;
    char *out;                      /* what it wrote to its output file (malloc'd), outlen bytes */
    size_t outlen;
    char *errout;                   /* with a file of its own for standard error (-s, -e): what it wrote there */
    size_t erroutlen;
};

static long g_cap = RS_DEFAULT_CAP;
static int g_split;                 /* -s: standard output and standard error in separate files, and in separate fields of the records */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static const char *signal_name(int s) {
    switch (s) {
    case SIGHUP: return "SIGHUP";   case SIGINT: return "SIGINT";   case SIGQUIT: return "SIGQUIT";
    case SIGILL: return "SIGILL";   case SIGTRAP: return "SIGTRAP"; case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";   case SIGFPE: return "SIGFPE";   case SIGKILL: return "SIGKILL";
    case SIGUSR1: return "SIGUSR1"; case SIGSEGV: return "SIGSEGV"; case SIGUSR2: return "SIGUSR2";
    case SIGPIPE: return "SIGPIPE"; case SIGALRM: return "SIGALRM"; case SIGTERM: return "SIGTERM";
    default: return "?";
    }
}

/* mkdir -p */
static int mkpath(const char *path) {
    char buf[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) { errno = ENAMETOOLONG; return -1; }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        char c = buf[i];
        buf[i] = 0;
        if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -1;
        buf[i] = c;
    }
    return 0;
}

/* A relative name that stays inside its directory: no leading slash, no ".." component. */
static int inside_ok(const char *name) {
    if (!name[0] || name[0] == '/') return 0;
    for (const char *p = name; *p;) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!e) break;
        p = e + 1;
    }
    return 1;
}

/* Reads what is in the file (at most the cap) into a malloc'd buffer. */
static void slurp(int fd, char **buf, size_t *len) {
    struct stat st;
    size_t want = fstat(fd, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
    if ((long)want > g_cap) want = (size_t)g_cap;
    *buf = NULL;
    *len = 0;
    if (!want) return;
    *buf = malloc(want);
    lseek(fd, 0, SEEK_SET);
    size_t got = 0;
    while (*buf && got < want) {
        ssize_t r = read(fd, *buf + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    *len = got;
}

static int over_cap(int fd) {
    struct stat st;
    return fd >= 0 && fstat(fd, &st) == 0 && st.st_size > g_cap;
}

/* Runs one program. cwd, outpath and errpath may be NULL (inherit the working directory, inherit the terminal). With outpath the program's standard output and standard error go
 * to that file together, unless errpath gives standard error a file of its own; o->out and o->errout then hold what was written. */
static void run_one(const struct job *j, long limit_ms, const char *cwd, const char *outpath, const char *errpath, struct outcome *o) {
    memset(o, 0, sizeof *o);
    o->what = "run";
    int infd = -1, outfd = -1, errfd = -1, ep[2] = { -1, -1 };
    if (j->stdin_path) {
        infd = open(j->stdin_path[0] ? j->stdin_path : "/dev/null", O_RDONLY);
        if (infd < 0) { o->err = errno; o->what = "open the input of"; goto fail; }
    }
    if (outpath) {
        outfd = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (outfd < 0) { o->err = errno; o->what = "open the output file of"; goto fail; }
    }
    if (outpath && errpath) {
        errfd = open(errpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (errfd < 0) { o->err = errno; o->what = "open the error file of"; goto fail; }
    }
    if (pipe(ep) < 0) { o->err = errno; o->what = "make a pipe for"; goto fail; }
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);                  /* closed by a successful exec: an empty pipe afterwards means the program started */
    fcntl(ep[0], F_SETFL, O_NONBLOCK);
    fflush(stdout);
    fflush(stderr);

    long t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0) { o->err = errno; o->what = "fork for"; goto fail; }
    if (pid == 0) {
        setpgid(0, 0);
        close(ep[0]);
        if (infd >= 0) { dup2(infd, 0); if (infd > 2) close(infd); }
        if (outfd >= 0) {
            dup2(outfd, 1);
            dup2(errfd >= 0 ? errfd : outfd, 2);
            if (outfd > 2) close(outfd);
            if (errfd > 2) close(errfd);
        }
        int e = 0;
        if (cwd && chdir(cwd) < 0) e = errno;
        else { execvp(j->path, j->argv); e = errno; }
        if (write(ep[1], &e, sizeof e) < 0) {}
        _exit(127);
    }
    close(ep[1]);
    ep[1] = -1;
    setpgid(pid, pid);                                  /* both sides do it, so that neither loses the race */

    int status = 0, done = 0;
    long deadline = limit_ms > 0 ? t0 + limit_ms : 0, nap = 1;
    for (int polls = 0;; polls++) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { done = 1; break; }
        if (r < 0 && errno != EINTR) break;
        if (deadline && now_ms() >= deadline) { o->timed_out = 1; break; }
        if (over_cap(outfd) || over_cap(errfd)) { o->capped = 1; break; }
        nap_ms(nap);
        if (polls == 20) nap = 4;                       /* short programs are noticed quickly, long ones are not polled hard */
    }
    if (!done) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    kill(-pid, SIGKILL);                                /* whatever else is left of its group */
    o->ms = now_ms() - t0;

    int exec_err = 0;
    if (read(ep[0], &exec_err, sizeof exec_err) == (ssize_t)sizeof exec_err && exec_err) { o->err = exec_err; goto fail; }
    o->started = 1;
    if (WIFEXITED(status)) o->code = WEXITSTATUS(status);
    else { o->signaled = 1; o->sig = WTERMSIG(status); }

    if (outfd >= 0) slurp(outfd, &o->out, &o->outlen);
    if (errfd >= 0) slurp(errfd, &o->errout, &o->erroutlen);
    goto cleanup;

fail:
    o->started = 0;
cleanup:
    if (infd >= 0) close(infd);
    if (outfd >= 0) close(outfd);
    if (errfd >= 0) close(errfd);
    if (ep[0] >= 0) close(ep[0]);
    if (ep[1] >= 0) close(ep[1]);
}

static void put_b64(const unsigned char *p, size_t n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char buf[4100];
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        buf[k++] = tab[v >> 18 & 63];
        buf[k++] = tab[v >> 12 & 63];
        buf[k++] = i + 1 < n ? tab[v >> 6 & 63] : '=';
        buf[k++] = i + 2 < n ? tab[v & 63] : '=';
        if (k >= 4096) { fwrite(buf, 1, k, stdout); k = 0; }
    }
    if (k) fwrite(buf, 1, k, stdout);
}

static void print_record(const struct job *j, const struct outcome *o) {
    char ex[16] = "-", sg[16] = "-", fl[8];
    int k = 0;
    const char *text = o->out;
    size_t len = o->outlen;
    char why[400];
    if (!o->started) {
        len = (size_t)snprintf(why, sizeof why, "cannot %s '%s': %s", o->what ? o->what : "run", j->path, strerror(o->err));
        text = why;
        fl[k++] = 'X';
    } else if (o->signaled) snprintf(sg, sizeof sg, "%d", o->sig);
    else snprintf(ex, sizeof ex, "%d", o->code);
    if (o->timed_out) fl[k++] = 'T';
    if (o->capped) fl[k++] = 'C';
    if (!k) fl[k++] = '-';
    fl[k] = 0;
    printf("\n@@RS %ld %s %s %s %ld %zu ", j->id, ex, sg, fl, o->ms, len);
    if (text) put_b64((const unsigned char *)text, len);
    if (g_split) {                                      /* standard error follows as a second length and payload */
        printf(" %zu ", o->started ? o->erroutlen : (size_t)0);
        if (o->started && o->errout) put_b64((const unsigned char *)o->errout, o->erroutlen);
    }
    fputc('\n', stdout);
    fflush(stdout);
}

/* ---- the manifest ---- */

struct cursor { char *p, *end; };

static const char *next_str(struct cursor *c) {
    if (c->p >= c->end) return NULL;
    char *z = memchr(c->p, 0, (size_t)(c->end - c->p));
    if (!z) return NULL;
    const char *s = c->p;
    c->p = z + 1;
    return s;
}

static int next_long(struct cursor *c, long *out) {
    const char *s = next_str(c);
    char *e;
    if (!s || !*s) return -1;
    long v = strtol(s, &e, 10);
    if (*e || v < 0) return -1;
    *out = v;
    return 0;
}

/* Reads the manifest into jobs. Returns the number of jobs, or -1 after saying what is wrong. */
static long read_manifest(const char *file, struct job **out) {
    int fd = open(file, O_RDONLY);
    if (fd < 0) { kb_error("%s: %s", file, strerror(errno)); return -1; }
    size_t len = 0;
    char *buf = kb_read_all(fd, &len);
    close(fd);
    if (!buf) { kb_error("%s: cannot read it", file); return -1; }
    struct cursor c = { buf, buf + len };
    const char *magic = next_str(&c);
    long count;
    if (!magic || strcmp(magic, "RSM1") || next_long(&c, &count) < 0 || count > RS_MAX_JOBS) { kb_error("%s: not a runstatus manifest", file); return -1; }
    struct job *jobs = calloc((size_t)count + 1, sizeof *jobs);
    for (long i = 0; i < count; i++) {
        struct job *j = &jobs[i];
        long nfiles, argc;
        const char *stdin_path;
        if (next_long(&c, &j->id) < 0 || next_long(&c, &j->timeout_ms) < 0 || !(stdin_path = next_str(&c)) || next_long(&c, &nfiles) < 0 || nfiles > RS_MAX_ARGS) goto bad;
        j->stdin_path = stdin_path;
        j->nfiles = (int)nfiles;
        j->files = calloc((size_t)nfiles * 2 + 1, sizeof(char *));
        for (long f = 0; f < nfiles * 2; f++)
            if (!(j->files[f] = (char *)next_str(&c))) goto bad;
        if (!(j->path = next_str(&c)) || !*j->path || next_long(&c, &argc) < 0 || argc > RS_MAX_ARGS) goto bad;
        j->argc = (int)argc;
        j->argv = calloc((size_t)argc + 1, sizeof(char *));
        for (long a = 0; a < argc; a++)
            if (!(j->argv[a] = (char *)next_str(&c))) goto bad;
        continue;
    bad:
        kb_error("%s: the manifest is damaged (job %ld of %ld)", file, i + 1, count);
        return -1;
    }
    *out = jobs;
    return count;
}

static int run_batch(const char *manifest, const char *base_in, char **subdirs, int nsub, long default_ms, int keep) {
    struct job *jobs;
    long n = read_manifest(manifest, &jobs);
    if (n < 0) return 125;
    char base[256];
    if (base_in) snprintf(base, sizeof base, "%s", base_in);
    else snprintf(base, sizeof base, "/tmp/runstatus.%d", (int)getpid());
    if (mkpath(base) < 0) { kb_error("%s: %s", base, strerror(errno)); return 125; }
    printf("\n@@RS-START %ld\n", n);
    fflush(stdout);

    for (long i = 0; i < n; i++) {
        struct job *j = &jobs[i];
        char dir[320], outp[330], errp[330], path[700];
        snprintf(dir, sizeof dir, "%s/%ld", base, j->id);
        snprintf(outp, sizeof outp, "%s/%ld.out", base, j->id);
        snprintf(errp, sizeof errp, "%s/%ld.err", base, j->id);
        struct outcome o;
        int prep = mkpath(dir) < 0 ? errno : 0;
        for (int s = 0; !prep && s < nsub; s++) {
            snprintf(path, sizeof path, "%s/%s", dir, subdirs[s]);
            if (mkpath(path) < 0) prep = errno;
        }
        for (int f = 0; !prep && f < j->nfiles; f++) {
            const char *dest = j->files[2 * f], *src = j->files[2 * f + 1];
            if (!inside_ok(dest)) { prep = EINVAL; break; }
            snprintf(path, sizeof path, "%s/%s", dir, dest);
            char *slash = strrchr(path, '/');
            *slash = 0;
            if (mkpath(path) < 0) { prep = errno; break; }
            *slash = '/';
            if (kb_copy_file(src, path, 1) < 0) prep = errno ? errno : EIO;
        }
        if (prep) {
            memset(&o, 0, sizeof o);
            o.err = prep;                               /* started stays 0: the record says why */
            o.what = "set up the directory of";
        } else {
            long limit = j->timeout_ms > 0 ? j->timeout_ms : default_ms;
            run_one(j, limit, dir, outp, g_split ? errp : NULL, &o);
        }
        print_record(j, &o);
        free(o.out);
        free(o.errout);
        if (!keep) {
            kb_remove_tree(dir, 1, 0);
            unlink(outp);
            if (g_split) unlink(errp);
        }
    }
    printf("\n@@RS-DONE %ld\n", n);
    fflush(stdout);
    return 0;
}

static int parse_ms(const char *s, long *out) {
    char *e;
    double v = strtod(s, &e);
    if (e == s || *e || v < 0 || v > 1e6) return -1;
    *out = (long)(v * 1000.0 + 0.5);
    return 0;
}

APPLET(runstatus) {
    const char *usage = "usage: runstatus [-t SECONDS] [-C DIR] [-i FILE] [-o FILE [-e FILE]] PROGRAM [ARGUMENT...]\n"
                        "       runstatus -b MANIFEST [-w DIR] [-m SUBDIR]... [-t SECONDS] [-c BYTES] [-s] [-k]";
    long limit_ms = RS_DEFAULT_LIMIT_MS;
    const char *dir = NULL, *infile = NULL, *outfile = NULL, *errfile = NULL, *manifest = NULL, *base = NULL;
    char *subdirs[RS_MAX_SUBDIRS];
    int nsub = 0, keep = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-k")) { keep = 1; continue; }
        if (!strcmp(a, "-s")) { g_split = 1; continue; }
        if (!a[2] && strchr("tCioebwmc", a[1])) {
            if (i + 1 >= argc) { fprintf(stderr, "%s\n", usage); return 125; }
            const char *v = argv[++i];
            switch (a[1]) {
            case 't': if (parse_ms(v, &limit_ms) < 0) { kb_error("bad time '%s'", v); return 125; } break;
            case 'C': dir = v; break;
            case 'i': infile = v; break;
            case 'o': outfile = v; break;
            case 'e': errfile = v; break;
            case 'b': manifest = v; break;
            case 'w': base = v; break;
            case 'm': if (nsub < RS_MAX_SUBDIRS && inside_ok(v)) subdirs[nsub++] = (char *)v; else { kb_error("bad -m '%s'", v); return 125; } break;
            case 'c': g_cap = atol(v); if (g_cap <= 0) { kb_error("bad cap '%s'", v); return 125; } break;
            }
            continue;
        }
        fprintf(stderr, "%s\n", usage);
        return 125;
    }
    if (manifest) {
        if (i < argc) { fprintf(stderr, "%s\n", usage); return 125; }
        return run_batch(manifest, base, subdirs, nsub, limit_ms, keep);
    }
    if (i >= argc) { fprintf(stderr, "%s\n", usage); return 125; }

    if (errfile && !outfile) { kb_error("-e needs -o: without it the program's output stays on the terminal"); return 125; }
    struct job j = { .id = 0, .stdin_path = infile, .path = argv[i], .argc = argc - i, .argv = &argv[i] };
    struct outcome o;
    run_one(&j, limit_ms, dir, outfile, errfile, &o);
    int rc;
    if (!o.started) {
        kb_error("cannot %s '%s': %s", o.what, j.path, strerror(o.err));
        rc = o.err == ENOENT ? 127 : 126;
    } else if (o.timed_out) {
        fprintf(stderr, "runstatus: timed out after %ld ms (killed)\n", limit_ms);
        rc = 124;
    } else if (o.capped) {
        fprintf(stderr, "runstatus: wrote more than %ld bytes (killed)\n", g_cap);
        rc = 124;
    } else if (o.signaled) {
        fprintf(stderr, "runstatus: killed by signal %d (%s)\n", o.sig, signal_name(o.sig));
        rc = 128 + o.sig;
    } else {
        fprintf(stderr, "runstatus: exit %d\n", o.code);
        rc = o.code;
    }
    free(o.out);
    free(o.errout);
    return rc;
}
