/* Executor: simple commands, pipelines, lists, compound commands, redirections, functions and job control. */
#include "gapsh.h"

int g_status;
int g_interactive;
int g_shell_pgid;
int g_exit_requested;
int g_break, g_continue, g_return;
int g_last_bg;

static int g_stage_child;               /* running inside a forked pipeline stage: external commands exec in place */

void shell_error(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "gapsh: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ functions */

typedef struct Func {
    char *name;
    Node *body;
    struct Func *next;
} Func;

static Func *g_funcs;

void define_function(const char *name, Node *body) {
    for (Func *f = g_funcs; f; f = f->next) {
        if (!strcmp(f->name, name)) { f->body = body; return; }
    }
    Func *f = xmalloc(sizeof *f);
    f->name = xstrdup(name);
    f->body = body;
    f->next = g_funcs;
    g_funcs = f;
}

Node *find_function(const char *name) {
    for (Func *f = g_funcs; f; f = f->next)
        if (!strcmp(f->name, name)) return f->body;
    return NULL;
}

/* ------------------------------------------------------------------ jobs
 *
 * A job is what one command started: a program, a pipeline, a subshell or a command ending in `&`. An interactive shell
 * does job control (docs/JOBCONTROL.md, "What the shell has to do"): every job gets a process group of its own, the
 * foreground job gets the terminal, Ctrl-Z stops it and hands the terminal back to the shell, and `fg`, `bg`, `jobs`,
 * `wait`, `disown` and `kill %N` work on the table below. Scripts and the shell's own children have no job control: their
 * jobs stay in the shell's process group and are only waited for, as before. */

enum { P_RUNNING, P_STOPPED, P_DONE };
enum { J_RUNNING, J_STOPPED, J_DONE };

#define MAXJOBS 64
#define MAXPROCS 32

typedef struct Job {
    int id;
    int pgid;                            /* the job's process group, or its first process without job control */
    int own_group;                       /* it has a group of its own: signals go to -pgid, it can have the terminal */
    int pids[MAXPROCS];
    int pstate[MAXPROCS];                /* P_* of each process */
    int pstatus[MAXPROCS];               /* the waitpid word of a finished process */
    int npids;
    int state;                           /* J_*: running while one process runs, stopped when the others stopped */
    int stopsig;                         /* why it stopped last */
    int foreground;                      /* the shell waits for it in front: nothing to report before the prompt */
    int notify;                          /* stopped or finished in the background: report it before the next prompt */
    unsigned long seq;                   /* when it was last put in the background or stopped: decides %+ and %- */
    int has_tmodes;
    struct termios tmodes;               /* its terminal settings, saved when it stopped, put back by fg */
    char *cmd;
} Job;

static Job *g_jobs[MAXJOBS];
static unsigned long g_job_seq;
static struct termios g_shell_tmodes;    /* the shell's own terminal settings, put back when a job stops or is killed */
static int g_have_shell_tmodes;
int g_interrupted;                       /* ^C killed or ^Z stopped the foreground job: the rest of the line is dropped */

static void job_free(Job *j) {
    for (int i = 0; i < MAXJOBS; i++) if (g_jobs[i] == j) g_jobs[i] = NULL;
    free(j->cmd);
    free(j);
}

static Job *job_add(int pgid, int own_group, const int *pids, int n, const char *cmd) {
    int slot = -1;
    for (int i = 0; i < MAXJOBS && slot < 0; i++) if (!g_jobs[i]) slot = i;
    for (int i = 0; i < MAXJOBS && slot < 0; i++)           /* full: forget a finished job nobody asked about */
        if (g_jobs[i]->state == J_DONE && !g_jobs[i]->foreground) { job_free(g_jobs[i]); slot = i; }
    if (slot < 0) return NULL;
    Job *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->id = slot + 1;
    j->pgid = pgid;
    j->own_group = own_group;
    j->npids = n > MAXPROCS ? MAXPROCS : n;
    memcpy(j->pids, pids, (size_t)j->npids * sizeof(int));
    j->state = J_RUNNING;
    j->seq = ++g_job_seq;
    j->cmd = xstrdup(cmd && *cmd ? cmd : "(commands)");
    g_jobs[slot] = j;
    return j;
}

static void job_recompute(Job *j) {
    int running = 0, stopped = 0;
    for (int k = 0; k < j->npids; k++) {
        if (j->pstate[k] == P_RUNNING) running++;
        else if (j->pstate[k] == P_STOPPED) stopped++;
    }
    int old = j->state;
    j->state = running ? J_RUNNING : stopped ? J_STOPPED : J_DONE;
    if (j->state == old) return;
    if (j->state == J_STOPPED) j->seq = ++g_job_seq;
    if (!j->foreground && j->state != J_RUNNING) j->notify = 1;
}

/* Records what waitpid said about pid. Returns its job, or NULL for a process the table does not know. */
static Job *job_note(int pid, int status) {
    for (int i = 0; i < MAXJOBS; i++) {
        Job *j = g_jobs[i];
        if (!j) continue;
        for (int k = 0; k < j->npids; k++) {
            if (j->pids[k] != pid || j->pstate[k] == P_DONE) continue;
            if (WIFSTOPPED(status)) { j->pstate[k] = P_STOPPED; j->stopsig = WSTOPSIG(status); }
            else if (WIFCONTINUED(status)) j->pstate[k] = P_RUNNING;
            else { j->pstate[k] = P_DONE; j->pstatus[k] = status; }
            job_recompute(j);
            return j;
        }
    }
    return NULL;
}

/* Collects every change of a child that has already happened, without blocking. */
static void jobs_collect(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) job_note(pid, status);
}

/* Blocks for the next change of any child. -1 when there is no child left or a signal interrupted the wait (errno). */
static int jobs_wait_one(void) {
    int status;
    pid_t pid = waitpid(-1, &status, WUNTRACED | WCONTINUED);
    if (pid < 0) return -1;
    job_note(pid, status);
    return 0;
}

/* The status word of a job: that of its last process (a pipeline's status is its last command's). */
static int job_word(Job *j) { return j->pstatus[j->npids - 1]; }

static int exit_status_of(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void job_signal(Job *j, int sig) {
    if (j->own_group) { kill(-j->pgid, sig); return; }
    for (int k = 0; k < j->npids; k++) if (j->pstate[k] != P_DONE) kill(j->pids[k], sig);
}

static void job_continue(Job *j) {
    job_signal(j, SIGCONT);
    for (int k = 0; k < j->npids; k++) if (j->pstate[k] == P_STOPPED) j->pstate[k] = P_RUNNING;
    if (j->state == J_STOPPED) j->state = J_RUNNING;
}

/* %+ is the job stopped most recently, or else the one put in the background most recently; %- is the one before. */
static void job_current(Job **cur, Job **prev) {
    *cur = *prev = NULL;
    for (int pass = 0; pass < 2; pass++) {
        Job *best = NULL;
        for (int i = 0; i < MAXJOBS; i++) {
            Job *j = g_jobs[i];
            if (!j || j->state == J_DONE || j->foreground || j == *cur) continue;
            int js = j->state == J_STOPPED, bs = best && best->state == J_STOPPED;
            if (!best || js > bs || (js == bs && j->seq > best->seq)) best = j;
        }
        if (pass == 0) *cur = best;
        else *prev = best;
    }
}

static char job_mark(Job *j) {
    Job *cur, *prev;
    job_current(&cur, &prev);
    return j == cur ? '+' : j == prev ? '-' : ' ';
}

static const char *signal_text(int sig) {
    switch (sig) {
    case SIGHUP: return "Hangup";
    case SIGINT: return "Interrupt";
    case SIGQUIT: return "Quit";
    case SIGILL: return "Illegal instruction";
    case SIGABRT: return "Aborted";
    case SIGKILL: return "Killed";
    case SIGSEGV: return "Segmentation fault";
    case SIGPIPE: return "Broken pipe";
    case SIGALRM: return "Alarm clock";
    case SIGTERM: return "Terminated";
    default: return NULL;
    }
}

static void job_state_text(Job *j, char *out, size_t size) {
    if (j->state == J_RUNNING) { snprintf(out, size, "Running"); return; }
    if (j->state == J_STOPPED) {
        const char *why = j->stopsig == SIGTTIN ? " (tty input)" : j->stopsig == SIGTTOU ? " (tty output)" :
                          j->stopsig == SIGSTOP ? " (signal)" : "";
        snprintf(out, size, "Stopped%s", why);
        return;
    }
    int w = job_word(j);
    const char *t = WIFSIGNALED(w) ? signal_text(WTERMSIG(w)) : NULL;
    if (WIFEXITED(w) && WEXITSTATUS(w)) snprintf(out, size, "Exit %d", WEXITSTATUS(w));
    else if (WIFEXITED(w)) snprintf(out, size, "Done");
    else if (t) snprintf(out, size, "%s", t);
    else snprintf(out, size, "Signal %d", WTERMSIG(w));
}

/* "[1]+  Stopped                 cat | tr a-z A-Z", the layout other shells use. pids: the process ids as well. */
static void job_print(FILE *f, Job *j, int pids) {
    char state[48];
    job_state_text(j, state, sizeof state);
    fprintf(f, "[%d]%c  ", j->id, job_mark(j));
    if (pids) for (int k = 0; k < j->npids; k++) fprintf(f, "%d ", j->pids[k]);
    fprintf(f, "%-24s%s\n", state, j->cmd);
}

/* Before each prompt: what the background jobs did since the last one. */
void jobs_reap(void) {
    jobs_collect();
    for (int i = 0; i < MAXJOBS; i++) {
        Job *j = g_jobs[i];
        if (!j || !j->notify) continue;
        j->notify = 0;
        if (g_interactive) job_print(stderr, j, 0);
        if (j->state == J_DONE) job_free(j);
    }
    fflush(stderr);
}

int jobs_stopped(void) {
    jobs_collect();
    int n = 0;
    for (int i = 0; i < MAXJOBS; i++) if (g_jobs[i] && g_jobs[i]->state == J_STOPPED) n++;
    return n;
}

/* The shell is leaving: nobody could continue its stopped jobs, so they get a hangup and are woken to hear it. */
void jobs_hangup_stopped(void) {
    for (int i = 0; i < MAXJOBS; i++) {
        Job *j = g_jobs[i];
        if (j && j->state == J_STOPPED) { job_signal(j, SIGHUP); job_signal(j, SIGCONT); }
    }
}

/* Waits for a job in the foreground until it finishes or, with job control, stops. cont: the job is stopped and fg
 * continues it. Returns the shell status: the exit status, 128 + the signal that killed it, or 128 + the stop signal. */
static int job_wait_fg(Job *j, int cont) {
    int jc = g_interactive && j->own_group;
    j->foreground = 1;
    j->notify = 0;
    if (jc) {
        g_have_shell_tmodes = tcgetattr(0, &g_shell_tmodes) == 0;
        if (cont && j->has_tmodes) tcsetattr(0, TCSADRAIN, &j->tmodes);
        tcsetpgrp(0, j->pgid);
    }
    if (cont) job_continue(j);
    while (j->state == J_RUNNING || (!jc && j->state == J_STOPPED)) {
        if (jobs_wait_one() < 0 && errno != EINTR) {        /* no child left: someone else collected them */
            for (int k = 0; k < j->npids; k++) j->pstate[k] = P_DONE;
            job_recompute(j);
        }
    }
    j->foreground = 0;
    int w = job_word(j);
    int signalled = j->state == J_DONE && WIFSIGNALED(w);
    if (jc) {
        if (j->state == J_STOPPED) j->has_tmodes = tcgetattr(0, &j->tmodes) == 0;
        tcsetpgrp(0, g_shell_pgid);
        if ((j->state == J_STOPPED || signalled) && g_have_shell_tmodes) tcsetattr(0, TCSADRAIN, &g_shell_tmodes);
    }
    if (j->state == J_STOPPED) {
        j->notify = 0;
        job_print(stderr, j, 0);
        fflush(stderr);
        g_interrupted = 1;
        return 128 + j->stopsig;
    }
    if (jc && signalled && WTERMSIG(w) == SIGINT) g_interrupted = 1;
    job_free(j);
    return exit_status_of(w);
}

/* A job specification: %N, %% / %+ / % (the current job), %- (the previous one), %NAME (the command starts with
 * NAME), %?TEXT (the command contains TEXT); a plain number is a job number as well. NULL means the current job. */
static Job *job_find(const char *spec, const char *who) {
    Job *cur, *prev;
    job_current(&cur, &prev);
    if (!spec || !strcmp(spec, "%") || !strcmp(spec, "%%") || !strcmp(spec, "%+")) {
        if (!cur) shell_error("%s: no current job", who);
        return cur;
    }
    if (!strcmp(spec, "%-")) {
        Job *j = prev ? prev : cur;
        if (!j) shell_error("%s: no previous job", who);
        return j;
    }
    const char *s = spec[0] == '%' ? spec + 1 : spec;
    if (isdigit((unsigned char)*s)) {
        char *end;
        long id = strtol(s, &end, 10);
        if (!*end && id >= 1 && id <= MAXJOBS && g_jobs[id - 1]) return g_jobs[id - 1];
        shell_error("%s: %s: no such job", who, spec);
        return NULL;
    }
    int contains = *s == '?';
    if (contains) s++;
    Job *found = NULL;
    for (int i = 0; spec[0] == '%' && *s && i < MAXJOBS; i++) {
        Job *j = g_jobs[i];
        if (!j || (contains ? !strstr(j->cmd, s) : strncmp(j->cmd, s, strlen(s)) != 0)) continue;
        if (found) { shell_error("%s: %s: ambiguous job spec", who, spec); return NULL; }
        found = j;
    }
    if (!found) shell_error("%s: %s: no such job", who, spec);
    return found;
}

static Job *job_of_pid(int pid) {
    for (int i = 0; i < MAXJOBS; i++) {
        Job *j = g_jobs[i];
        if (!j) continue;
        for (int k = 0; k < j->npids; k++) if (j->pids[k] == pid) return j;
    }
    return NULL;
}

/* kill %N signals the whole job. A stopped job is continued as well (unless the signal is a stop or KILL), or it would
 * only hear the signal once someone continued it. */
int job_kill(const char *spec, int sig) {
    jobs_collect();
    Job *j = job_find(spec, "kill");
    if (!j) return 1;
    if (j->state == J_DONE) return 0;
    job_signal(j, sig);
    if (j->state == J_STOPPED && sig != SIGCONT && sig != SIGSTOP && sig != SIGTSTP && sig != SIGTTIN && sig != SIGTTOU &&
        sig != SIGKILL)
        job_signal(j, SIGCONT);
    return 0;
}

static volatile int g_wait_sigint;
static void on_wait_sigint(int sig) { (void)sig; g_wait_sigint = 1; }

/* wait [ID...]: until the given jobs or processes finish, or every running job. A stop does not end the wait. In an
 * interactive shell ^C does (status 130), so a job that never finishes cannot hold the prompt hostage. */
static int wait_command(int argc, char **argv) {
    struct sigaction sa, old;
    if (g_interactive) {
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_wait_sigint;
        sigaction(SIGINT, &sa, &old);
    }
    g_wait_sigint = 0;
    int st = 0;
    jobs_collect();
    if (argc < 2) {
        for (;;) {
            int running = 0;
            for (int i = 0; i < MAXJOBS; i++) if (g_jobs[i] && g_jobs[i]->state == J_RUNNING) running = 1;
            if (!running || g_wait_sigint) break;
            if (jobs_wait_one() < 0 && errno != EINTR) break;
        }
        if (g_wait_sigint) st = 130;
        else for (int i = 0; i < MAXJOBS; i++) if (g_jobs[i] && g_jobs[i]->state == J_DONE) job_free(g_jobs[i]);
    }
    for (int i = 1; i < argc && !g_wait_sigint; i++) {
        Job *j;
        int k = -1;
        if (argv[i][0] == '%') {
            if (!(j = job_find(argv[i], "wait"))) { st = 127; continue; }
        } else {
            int pid = atoi(argv[i]);
            j = pid > 0 ? job_of_pid(pid) : NULL;
            if (!j) {                                           /* not in the table: wait for the process itself */
                int w = 0;
                pid_t r;
                while ((r = waitpid(pid, &w, 0)) < 0 && errno == EINTR && !g_wait_sigint) { }
                st = g_wait_sigint ? 130 : r < 0 ? 127 : exit_status_of(w);
                continue;
            }
            for (k = 0; j->pids[k] != pid; k++) { }
        }
        if (g_interactive && j->state == J_STOPPED)
            fprintf(stderr, "gapsh: wait: job %d is stopped; it can only finish after fg or bg (^C stops waiting)\n", j->id);
        while ((k >= 0 ? j->pstate[k] != P_DONE : j->state != J_DONE) && !g_wait_sigint)
            if (jobs_wait_one() < 0 && errno != EINTR) break;
        if (g_wait_sigint) { st = 130; break; }
        st = exit_status_of(k >= 0 ? j->pstatus[k] : job_word(j));
        if (j->state == J_DONE) job_free(j);
    }
    if (g_interactive) sigaction(SIGINT, &old, NULL);
    return st;
}

/* The jobs named by the operands from argv[i] on (or the current job when there are none). Returns how many. */
static int job_list(int argc, char **argv, int i, const char *who, Job **list, int *st) {
    int n = 0;
    if (i >= argc) {
        Job *j = job_find(NULL, who);
        if (j) list[n++] = j;
        else *st = 1;
    }
    for (; i < argc; i++) {
        Job *j = job_find(argv[i], who);
        if (j) list[n++] = j;
        else *st = 1;
    }
    return n;
}

/* The builtins jobs, fg, bg, disown and wait. */
int jobs_command(int argc, char **argv, const char *which) {
    if (!strcmp(which, "wait")) return wait_command(argc, argv);
    jobs_collect();
    Job *list[MAXJOBS];
    int n = 0, st = 0, i = 1;
    if (!strcmp(which, "jobs")) {
        int pids = 0, pgids = 0;
        for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
            for (const char *o = argv[i] + 1; *o; o++) {
                if (*o == 'l') pids = 1;
                else if (*o == 'p') pgids = 1;
                else { shell_error("jobs: -%c: unknown option (it knows -l and -p)", *o); return 2; }
            }
        }
        if (i < argc) n = job_list(argc, argv, i, "jobs", list, &st);
        else for (int k = 0; k < MAXJOBS; k++) if (g_jobs[k]) list[n++] = g_jobs[k];
        for (int k = 0; k < n; k++) {
            if (pgids) printf("%d\n", list[k]->pgid);
            else job_print(stdout, list[k], pids);
        }
        fflush(stdout);
        for (int k = 0; k < n; k++) {                           /* a finished job has been reported now */
            if (list[k]->state == J_DONE) job_free(list[k]);
            else list[k]->notify = 0;
        }
        return st;
    }
    if (!strcmp(which, "disown")) {
        int all = 0;
        for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
            if (!strcmp(argv[i], "-a")) all = 1;
            else if (strcmp(argv[i], "-h")) { shell_error("disown: %s: unknown option", argv[i]); return 2; }
        }
        if (all) { for (int k = 0; k < MAXJOBS; k++) if (g_jobs[k]) list[n++] = g_jobs[k]; }
        else n = job_list(argc, argv, i, "disown", list, &st);
        for (int k = 0; k < n; k++) {
            if (list[k]->state == J_STOPPED) job_continue(list[k]);    /* nobody could continue it later */
            job_free(list[k]);
        }
        return st;
    }
    if (!strcmp(which, "fg")) {
        if (argc > 2) { shell_error("fg: one job at a time"); return 2; }
        Job *j = job_find(argc > 1 ? argv[1] : NULL, "fg");
        if (!j) return 1;
        printf("%s\n", j->cmd);
        fflush(stdout);
        return job_wait_fg(j, j->state == J_STOPPED);
    }
    n = job_list(argc, argv, 1, "bg", list, &st);                /* bg */
    for (int k = 0; k < n; k++) {
        Job *j = list[k];
        if (j->state == J_RUNNING) { shell_error("bg: job %d is already running in the background", j->id); continue; }
        if (j->state != J_STOPPED) continue;
        job_continue(j);
        j->seq = ++g_job_seq;
        printf("[%d]%c %s &\n", j->id, job_mark(j), j->cmd);
    }
    fflush(stdout);
    return st;
}

/* ------------------------------------------------------------------ path lookup */

char *find_in_path(const char *name) {
    if (strchr(name, '/')) return access(name, X_OK) == 0 ? xstrdup(name) : NULL;
    const char *path = var_get("PATH");
    if (!path) path = "/bin:/sbin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char *cand = xasprintf("%.*s/%s", (int)len, len ? path : ".", name);
        struct stat st;
        if (access(cand, X_OK) == 0 && stat(cand, &st) == 0 && S_ISREG(st.st_mode)) return cand;
        free(cand);
        if (!end) break;
        path = end + 1;
    }
    return NULL;
}

/* "Did you mean ...?" for typos: nearest entry of /bin within edit distance 2. */
static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 30 || lb > 30) return 99;
    int d[32][32];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = a[i - 1] == b[j - 1] ? 0 : 1;
            int m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}

static void suggest(const char *cmd) {
    char best[64] = "";
    int bestd = 3;
    DIR *dir = opendir("/bin");
    if (dir) {
        for (struct dirent *e; (e = readdir(dir));) {
            if (e->d_name[0] == '.') continue;
            int d = edit_distance(cmd, e->d_name);
            if (d < bestd) { bestd = d; strlcpy(best, e->d_name, sizeof best); }
        }
        closedir(dir);
    }
    if (best[0]) fprintf(stderr, "gapsh: %s: command not found. Did Yukari's gap lead you astray? Perhaps you meant '%s'.\n", cmd, best);
    else fprintf(stderr, "gapsh: %s: command not found (it is not in Gensokyo)\n", cmd);
}

/* ------------------------------------------------------------------ redirections */

typedef struct Saved {
    int fd, saved;
    struct Saved *next;
} Saved;

static void save_fd(Saved **list, int fd) {
    for (Saved *s = *list; s; s = s->next) if (s->fd == fd) return;
    Saved *s = xmalloc(sizeof *s);
    s->fd = fd;
    s->saved = fcntl(fd, F_DUPFD, 10);
    if (s->saved >= 0) fcntl(s->saved, F_SETFD, FD_CLOEXEC);
    s->next = *list;
    *list = s;
}

static void restore_fds(Saved *list) {
    while (list) {
        Saved *next = list->next;
        if (list->saved >= 0) { dup2(list->saved, list->fd); close(list->saved); }
        else close(list->fd);
        free(list);
        list = next;
    }
}

static int apply_redirs(Redir *r, Saved **saved) {
    for (; r; r = r->next) {
        if (saved) save_fd(saved, r->fd);
        if (r->kind == R_HEREDOC) {
            char *body = r->heredoc_quoted ? xstrdup(r->target) : expand_heredoc(r->target);
            int fds[2];
            if (pipe(fds) < 0) { free(body); return -1; }
            size_t len = strlen(body);
            if (len <= 8192) {
                write(fds[1], body, len);
                close(fds[1]);
            } else {
                pid_t p = fork();
                if (p == 0) { close(fds[0]); write(fds[1], body, len); _exit(0); }
                close(fds[1]);
            }
            free(body);
            dup2(fds[0], r->fd);
            close(fds[0]);
            continue;
        }
        char *target = expand_string(r->target);
        if (r->kind == R_DUPOUT || r->kind == R_DUPIN) {
            if (!strcmp(target, "-")) close(r->fd);
            else if (isdigit((unsigned char)target[0])) {
                if (dup2(atoi(target), r->fd) < 0) { shell_error("%s: bad file descriptor", target); free(target); return -1; }
            } else if (r->kind == R_DUPOUT) {                       /* >&file behaves like &> file */
                int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) { shell_error("%s: %s", target, strerror(errno)); free(target); return -1; }
                dup2(fd, 1); dup2(fd, 2); close(fd);
            }
            free(target);
            continue;
        }
        int flags, fd;
        switch (r->kind) {
        case R_IN: flags = O_RDONLY; break;
        case R_APPEND: case R_OUTERR_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        }
        fd = open(target, flags, 0666);
        if (fd < 0) { shell_error("%s: %s", target, strerror(errno)); free(target); return -1; }
        if (r->kind == R_OUTERR || r->kind == R_OUTERR_APPEND) { dup2(fd, 1); dup2(fd, 2); }
        else dup2(fd, r->fd);
        close(fd);
        free(target);
    }
    return 0;
}

/* ------------------------------------------------------------------ commands */

static char *node_text(Node *n);

static void reset_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* In the child that starts a job of an interactive shell: its own process group (pgid 0: a new one) and, for a
 * foreground job, the terminal. Done here as well as in the parent so that it holds whichever side runs first, and
 * before reset_signals, while SIGTTOU is still ignored and the tcsetpgrp cannot stop the child. */
static void child_job_setup(int pgid, int foreground) {
    if (!g_interactive) return;
    setpgid(0, pgid);
    if (foreground) tcsetpgrp(0, pgid ? pgid : getpid());
}

static void exec_external(char **argv, Assign *assigns) {
    for (Assign *a = assigns; a; a = a->next) {
        char *v = expand_string(a->value);
        var_set(a->name, v);
        var_export(a->name);
        free(v);
    }
    char *path = find_in_path(argv[0]);
    if (!path) {
        if (strchr(argv[0], '/')) shell_error("%s: %s", argv[0], errno == EACCES ? strerror(EACCES) : "No such file or directory");
        else suggest(argv[0]);
        _exit(127);
    }
    char **env = vars_environ();
    execve(path, argv, env);
    shell_error("%s: %s", path, strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int call_function(Node *body, char **argv, int argc) {
    char **old_pos = g_pos_params;
    int old_n = g_npos;
    g_pos_params = argv + 1;
    g_npos = argc - 1;
    local_push();
    int st = run_node(body);
    local_pop();
    g_pos_params = old_pos;
    g_npos = old_n;
    if (g_return) { g_return = 0; st = g_status; }
    return st;
}

static int alias_depth;
const char *alias_lookup(const char *name);

static int run_simple(Node *n) {
    /* assignments only */
    if (n->nwords == 0) {
        Saved *saved = NULL;
        if (n->redirs && apply_redirs(n->redirs, &saved) < 0) { restore_fds(saved); return 1; }
        for (Assign *a = n->assigns; a; a = a->next) {
            char *v = expand_string(a->value);
            var_set(a->name, v);
            free(v);
        }
        restore_fds(saved);
        return 0;
    }

    int argc;
    char **argv = expand_words(n->words, n->nwords, &argc);
    if (argc == 0) { free(argv); return 0; }

    /* alias expansion of the command word */
    if (alias_depth < 8 && !strchr(n->words[0], 1) && !strchr(n->words[0], 3)) {
        const char *al = alias_lookup(argv[0]);
        if (al) {
            size_t len = strlen(al) + 2;
            for (int i = 1; i < argc; i++) len += strlen(argv[i]) * 4 + 4;
            char *line = xmalloc(len);
            strcpy(line, al);
            for (int i = 1; i < argc; i++) {
                strcat(line, " '");
                for (char *p = argv[i]; *p; p++) {
                    if (*p == '\'') strcat(line, "'\\''");
                    else strncat(line, p, 1);
                }
                strcat(line, "'");
            }
            alias_depth++;
            int st = run_string(line, "alias");
            alias_depth--;
            free(line);
            return st;
        }
    }

    const char *cmd = argv[0];
    Node *func = find_function(cmd);

    if (func || is_builtin(cmd)) {
        Saved *saved = NULL;
        if (apply_redirs(n->redirs, &saved) < 0) { restore_fds(saved); return 1; }
        /* temporary assignments are visible during a function call */
        int st;
        if (func) {
            for (Assign *a = n->assigns; a; a = a->next) {
                char *v = expand_string(a->value);
                var_set(a->name, v);
                free(v);
            }
            st = call_function(func, argv, argc);
        } else {
            st = run_builtin(argc, argv);
        }
        fflush(stdout);
        restore_fds(saved);
        return st;
    }

    if (g_stage_child) {                                    /* this process exists only to run the command */
        if (apply_redirs(n->redirs, NULL) < 0) _exit(1);
        exec_external(argv, n->assigns);
    }

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { shell_error("fork: %s", strerror(errno)); return 1; }
    if (pid == 0) {
        child_job_setup(0, 1);
        reset_signals();
        if (apply_redirs(n->redirs, NULL) < 0) _exit(1);
        exec_external(argv, n->assigns);
    }
    if (g_interactive) {                                    /* job control: the program is a job of its own */
        setpgid(pid, pid);
        char *text = node_text(n);
        Job *j = job_add(pid, 1, &pid, 1, text);
        free(text);
        if (j) return job_wait_fg(j, 0);
        tcsetpgrp(0, pid);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (g_interactive) tcsetpgrp(0, g_shell_pgid);
    return exit_status_of(status);
}

/* `command PROGRAM ...`: like an ordinary external command, without functions and aliases. */
int run_program(char *path, char **argv) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { shell_error("fork: %s", strerror(errno)); return 1; }
    if (pid == 0) {
        child_job_setup(0, 1);
        reset_signals();
        char **env = vars_environ();
        execve(path, argv, env);
        shell_error("%s: %s", path, strerror(errno));
        _exit(126);
    }
    if (g_interactive) {
        setpgid(pid, pid);
        char *text = xstrdup(argv[0]);
        for (int i = 1; argv[i]; i++) { char *t = xasprintf("%s %s", text, argv[i]); free(text); text = t; }
        Job *j = job_add(pid, 1, &pid, 1, text);
        free(text);
        if (j) return job_wait_fg(j, 0);
        tcsetpgrp(0, pid);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (g_interactive) tcsetpgrp(0, g_shell_pgid);
    return exit_status_of(status);
}

static void collect_stages(Node *n, Node **stages, int *count) {
    while (n->kind == N_PIPE && *count < 31) {
        stages[(*count)++] = n->a;
        n = n->b;
    }
    stages[(*count)++] = n;
}

static int run_pipeline(Node *n, int background, const char *text) {
    Node *stages[32];
    int ns = 0;
    collect_stages(n, stages, &ns);
    int pids[32], prev_read = -1, pgid = 0;
    fflush(NULL);
    for (int i = 0; i < ns; i++) {
        int fds[2] = { -1, -1 };
        if (i < ns - 1 && pipe(fds) < 0) { shell_error("pipe: %s", strerror(errno)); return 1; }
        pid_t pid = fork();
        if (pid < 0) { shell_error("fork: %s", strerror(errno)); return 1; }
        if (pid == 0) {
            child_job_setup(pgid, !background && i == 0);  /* the first process makes the group, the others join it */
            reset_signals();
            if (prev_read >= 0) { dup2(prev_read, 0); close(prev_read); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); }
            if (fds[0] >= 0) close(fds[0]);
            g_stage_child = 1;
            g_interactive = 0;
            int st = run_node(stages[i]);
            fflush(NULL);
            _exit(st);
        }
        if (!pgid) pgid = pid;
        if (g_interactive) setpgid(pid, pgid);
        pids[i] = pid;
        if (prev_read >= 0) close(prev_read);
        if (fds[1] >= 0) close(fds[1]);
        prev_read = fds[0];
    }
    Job *j = job_add(pgid, g_interactive, pids, ns, text);    /* the whole pipeline is one job */
    if (background) {
        g_last_bg = pids[ns - 1];
        if (g_interactive && j) fprintf(stderr, "[%d] %d\n", j->id, pids[ns - 1]);
        return 0;
    }
    if (j) return job_wait_fg(j, 0);                        /* its status is that of the last stage */
    if (g_interactive) tcsetpgrp(0, pgid);
    int status = 0, last = 0;
    for (int i = 0; i < ns; i++) { while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR) { } if (i == ns - 1) last = status; }
    if (g_interactive) tcsetpgrp(0, g_shell_pgid);
    return exit_status_of(last);
}

static int case_match(const char *pat_word, const char *subject) {
    char *pat = expand_string(pat_word);
    int m = glob_match(pat, subject);
    free(pat);
    return m;
}

int run_node(Node *n) {
    if (!n) return 0;
    if (g_exit_requested || g_interrupted) return g_status;
    int st = 0;
    switch (n->kind) {
    case N_SIMPLE:
        st = run_simple(n);
        break;
    case N_PIPE:
        st = run_pipeline(n, 0, node_text(n));
        break;
    case N_NOT:
        st = !run_node(n->a);
        break;
    case N_AND:
        st = run_node(n->a);
        g_status = st;
        if (st == 0 && !g_exit_requested && !g_break && !g_continue && !g_return) st = run_node(n->b);
        break;
    case N_OR:
        st = run_node(n->a);
        g_status = st;
        if (st != 0 && !g_exit_requested && !g_break && !g_continue && !g_return) st = run_node(n->b);
        break;
    case N_LIST:
        st = run_node(n->a);
        g_status = st;
        if (!g_exit_requested && !g_break && !g_continue && !g_return) st = run_node(n->b);
        break;
    case N_BG: {
        Node *inner = n->a;
        char *text = node_text(inner);
        if (g_interactive && inner->kind == N_PIPE) {      /* job control: every stage is a member of the job */
            run_pipeline(inner, 1, text);
            free(text);
            st = 0;
            break;
        }
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            child_job_setup(0, 0);
            reset_signals();
            if (!g_interactive) {                           /* without job control a background job must not read the
                                                               terminal; with it, reading stops the job (SIGTTIN) */
                int nul = open("/dev/null", O_RDONLY);
                if (nul >= 0) { dup2(nul, 0); close(nul); }
            }
            g_interactive = 0;
            g_stage_child = inner->kind == N_SIMPLE;        /* a lone external command can exec in place */
            int s = run_node(inner);
            fflush(NULL);
            _exit(s);
        }
        if (g_interactive) setpgid(pid, pid);
        Job *j = job_add(pid, g_interactive, &pid, 1, text);
        free(text);
        g_last_bg = pid;
        if (g_interactive && j) fprintf(stderr, "[%d] %d\n", j->id, pid);
        st = 0;
        break;
    }
    case N_SUBSHELL: {
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            child_job_setup(0, 1);
            Saved *sv = NULL;
            if (apply_redirs(n->redirs, &sv) < 0) _exit(1);
            reset_signals();
            g_interactive = 0;
            int s = run_node(n->a);
            fflush(NULL);
            _exit(s);
        }
        if (g_interactive) {                                /* job control: the subshell is a job, ^Z stops all of it */
            setpgid(pid, pid);
            char *text = node_text(n);
            Job *j = job_add(pid, 1, &pid, 1, text);
            free(text);
            if (j) { st = job_wait_fg(j, 0); break; }
            tcsetpgrp(0, pid);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        if (g_interactive) tcsetpgrp(0, g_shell_pgid);
        st = exit_status_of(status);
        break;
    }
    case N_GROUP: {
        Saved *sv = NULL;
        if (apply_redirs(n->redirs, &sv) < 0) { restore_fds(sv); st = 1; break; }
        st = run_node(n->a);
        fflush(stdout);
        restore_fds(sv);
        break;
    }
    case N_IF:
        if (run_node(n->a) == 0) st = run_node(n->b);
        else if (n->c) st = run_node(n->c);
        else st = 0;
        break;
    case N_WHILE:
    case N_UNTIL:
        st = 0;
        for (;;) {
            int c = run_node(n->a);
            if (g_exit_requested || g_return || g_interrupted) break;
            if ((n->kind == N_WHILE) != (c == 0)) break;
            st = run_node(n->b);
            if (g_exit_requested || g_return || g_interrupted) break;
            if (g_break) { g_break--; break; }
            if (g_continue) { if (--g_continue) break; }
        }
        break;
    case N_FOR: {
        int cnt;
        char **items;
        if (n->nwords < 0) { items = g_pos_params; cnt = g_npos; }
        else items = expand_words(n->words, n->nwords, &cnt);
        st = 0;
        for (int i = 0; i < cnt; i++) {
            var_set(n->name, items[i]);
            st = run_node(n->a);
            if (g_exit_requested || g_return || g_interrupted) break;
            if (g_break) { g_break--; break; }
            if (g_continue) { if (--g_continue) break; }
        }
        break;
    }
    case N_CASE: {
        char *subject = expand_string(n->words[0]);
        st = 0;
        for (CaseItem *it = n->items; it; it = it->next) {
            int hit = 0;
            for (int i = 0; i < it->npat && !hit; i++) hit = case_match(it->patterns[i], subject);
            if (hit) { st = run_node(it->body); break; }
        }
        free(subject);
        break;
    }
    case N_FUNC:
        define_function(n->name, n->a);
        st = 0;
        break;
    }
    g_status = st;
    return st;
}

/* ------------------------------------------------------------------ text for job display */

typedef struct { char *s; size_t len, cap; } Text;

static void tx_put(Text *t, const char *w, int strip) {    /* strip: drop the quoting marker bytes of a word */
    for (; *w; w++) {
        if (strip && (unsigned char)*w < 6) continue;
        if (t->len + 2 > t->cap) { t->cap = t->cap * 2 + 64; t->s = xrealloc(t->s, t->cap); }
        t->s[t->len++] = *w;
        t->s[t->len] = 0;
    }
}

static void tx_node(Text *t, Node *n, int depth) {
    static const char *ops[] = { "<", ">", ">>", "<&", ">&", "<<", "&>", "&>>" };
    if (!n) return;
    if (depth > 12) { tx_put(t, "...", 0); return; }
    switch (n->kind) {
    case N_SIMPLE: {
        const char *sep = "";
        for (Assign *a = n->assigns; a; a = a->next) {
            tx_put(t, sep, 0); tx_put(t, a->name, 0); tx_put(t, "=", 0); tx_put(t, a->value, 1);
            sep = " ";
        }
        for (int i = 0; i < n->nwords; i++) { tx_put(t, sep, 0); tx_put(t, n->words[i], 1); sep = " "; }
        for (Redir *r = n->redirs; r; r = r->next) {
            int deffd = r->kind == R_IN || r->kind == R_DUPIN || r->kind == R_HEREDOC ? 0 : 1;
            char fd[16] = "";
            if (r->fd != deffd && r->kind != R_OUTERR && r->kind != R_OUTERR_APPEND) snprintf(fd, sizeof fd, "%d", r->fd);
            tx_put(t, sep, 0); tx_put(t, fd, 0); tx_put(t, ops[r->kind], 0);
            if (r->kind == R_HEREDOC) tx_put(t, "...", 0);
            else tx_put(t, r->target, 1);
            sep = " ";
        }
        break;
    }
    case N_PIPE: tx_node(t, n->a, depth + 1); tx_put(t, " | ", 0); tx_node(t, n->b, depth + 1); break;
    case N_AND: tx_node(t, n->a, depth + 1); tx_put(t, " && ", 0); tx_node(t, n->b, depth + 1); break;
    case N_OR: tx_node(t, n->a, depth + 1); tx_put(t, " || ", 0); tx_node(t, n->b, depth + 1); break;
    case N_LIST:
        tx_node(t, n->a, depth + 1);
        if (n->b) { tx_put(t, n->a && n->a->kind == N_BG ? " " : "; ", 0); tx_node(t, n->b, depth + 1); }
        break;
    case N_BG: tx_node(t, n->a, depth + 1); tx_put(t, " &", 0); break;
    case N_NOT: tx_put(t, "! ", 0); tx_node(t, n->a, depth + 1); break;
    case N_SUBSHELL: tx_put(t, "(", 0); tx_node(t, n->a, depth + 1); tx_put(t, ")", 0); break;
    case N_GROUP: tx_put(t, "{ ", 0); tx_node(t, n->a, depth + 1); tx_put(t, "; }", 0); break;
    case N_IF: tx_put(t, "if ", 0); tx_node(t, n->a, depth + 1); tx_put(t, "; then ...", 0); break;
    case N_WHILE: tx_put(t, "while ", 0); tx_node(t, n->a, depth + 1); tx_put(t, "; do ...", 0); break;
    case N_UNTIL: tx_put(t, "until ", 0); tx_node(t, n->a, depth + 1); tx_put(t, "; do ...", 0); break;
    case N_FOR: tx_put(t, "for ", 0); tx_put(t, n->name ? n->name : "", 0); tx_put(t, " ...", 0); break;
    case N_CASE: tx_put(t, "case ", 0); if (n->nwords > 0) tx_put(t, n->words[0], 1); tx_put(t, " ...", 0); break;
    case N_FUNC: tx_put(t, n->name ? n->name : "", 0); tx_put(t, "() ...", 0); break;
    }
}

/* The command as `jobs` shows it: the words as typed, without the quotes. */
static char *node_text(Node *n) {
    Text t = { xmalloc(64), 0, 64 };
    t.s[0] = 0;
    tx_node(&t, n, 0);
    return t.s;
}

/* ------------------------------------------------------------------ running source text and files */

int run_string(const char *src, const char *name) {
    Lexer lx;
    lex_init(&lx, src);
    Node *prog = parse_program(&lx);
    if (!prog) {
        if (lx.error || lx.incomplete) {
            shell_error("%s: %s", name, lx.incomplete ? "unexpected end of input" : lx.errmsg);
            return 2;
        }
        return 0;
    }
    return run_node(prog);
}

int run_file(const char *path, int argc, char **argv) {
    FILE *f = fopen(path, "r");
    if (!f) { shell_error("%s: %s", path, strerror(errno)); return 127; }
    size_t cap = 4096, len = 0;
    char *src = xmalloc(cap);
    for (;;) {
        if (len + 1024 > cap) { cap *= 2; src = xrealloc(src, cap); }
        size_t r = fread(src + len, 1, cap - len - 1, f);
        if (!r) break;
        len += r;
    }
    src[len] = 0;
    fclose(f);
    char **old = g_pos_params;
    int oldn = g_npos;
    char *old0 = g_arg0;
    if (argc > 0) { g_arg0 = argv[0]; g_pos_params = argv + 1; g_npos = argc - 1; }
    int st = run_string(src[0] == '#' && src[1] == '!' ? strchr(src, '\n') ? strchr(src, '\n') : "" : src, path);
    g_pos_params = old;
    g_npos = oldn;
    g_arg0 = old0;
    free(src);
    return st;
}

void exec_init(void) {
    g_shell_pgid = getpgrp();
}
