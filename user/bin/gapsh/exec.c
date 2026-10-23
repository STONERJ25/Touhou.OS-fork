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

/* ------------------------------------------------------------------ jobs */

typedef struct Job {
    int id;
    int pgid;
    int pids[32];
    int npids;
    int alive;                           /* members still running */
    int status;
    char *cmd;
    int reported;
} Job;

static Job *g_jobs[64];

static Job *job_add(int pgid, const int *pids, int n, const char *cmd) {
    for (int i = 0; i < 64; i++) {
        if (!g_jobs[i]) {
            Job *j = xmalloc(sizeof *j);
            memset(j, 0, sizeof *j);
            j->id = i + 1;
            j->pgid = pgid;
            j->npids = n > 32 ? 32 : n;
            memcpy(j->pids, pids, (size_t)j->npids * sizeof(int));
            j->alive = j->npids;
            j->cmd = xstrdup(cmd);
            g_jobs[i] = j;
            return j;
        }
    }
    return NULL;
}

static void job_note_exit(int pid, int status) {
    for (int i = 0; i < 64; i++) {
        Job *j = g_jobs[i];
        if (!j) continue;
        for (int k = 0; k < j->npids; k++) {
            if (j->pids[k] == pid) {
                j->pids[k] = 0;
                j->alive--;
                j->status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                return;
            }
        }
    }
}

void jobs_reap(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) job_note_exit(pid, status);
    for (int i = 0; i < 64; i++) {
        Job *j = g_jobs[i];
        if (j && j->alive <= 0) {
            if (g_interactive) fprintf(stderr, "[%d]+ Done      %s\n", j->id, j->cmd);
            free(j->cmd);
            free(j);
            g_jobs[i] = NULL;
        }
    }
}

int job_wait_fg(int idx) {
    Job *j = g_jobs[idx];
    if (!j) return 0;
    if (g_interactive) tcsetpgrp(0, j->pgid);
    while (j->alive > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) { if (errno == EINTR) continue; break; }
        job_note_exit(pid, status);
    }
    if (g_interactive) tcsetpgrp(0, g_shell_pgid);
    int st = j->status;
    free(j->cmd);
    free(j);
    g_jobs[idx] = NULL;
    return st;
}

int jobs_command(int argc, char **argv, const char *which) {
    if (!strcmp(which, "jobs")) {
        for (int i = 0; i < 64; i++)
            if (g_jobs[i]) printf("[%d]  Running   %s\n", g_jobs[i]->id, g_jobs[i]->cmd);
        return 0;
    }
    int idx = -1;
    if (argc > 1) {
        int id = atoi(argv[1][0] == '%' ? argv[1] + 1 : argv[1]);
        if (id >= 1 && id <= 64 && g_jobs[id - 1]) idx = id - 1;
    } else {
        for (int i = 63; i >= 0; i--) if (g_jobs[i]) { idx = i; break; }
    }
    if (idx < 0) { shell_error("%s: no such job", which); return 1; }
    if (!strcmp(which, "bg")) { printf("[%d]+ %s &\n", g_jobs[idx]->id, g_jobs[idx]->cmd); return 0; }
    printf("%s\n", g_jobs[idx]->cmd);
    return job_wait_fg(idx);
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
}

static int exit_status_of(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
        if (g_interactive) setpgid(0, 0);
        reset_signals();
        if (apply_redirs(n->redirs, NULL) < 0) _exit(1);
        exec_external(argv, n->assigns);
    }
    if (g_interactive) { setpgid(pid, pid); tcsetpgrp(0, pid); }
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
            if (g_interactive) setpgid(0, pgid ? pgid : 0);
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
    Job *j = job_add(pgid, pids, ns, text);
    if (background) {
        g_last_bg = pids[ns - 1];
        if (g_interactive && j) fprintf(stderr, "[%d] %d\n", j->id, pids[ns - 1]);
        return 0;
    }
    int idx = j ? j->id - 1 : -1;
    if (idx < 0) {
        int status = 0;
        for (int i = 0; i < ns; i++) waitpid(pids[i], &status, 0);
        return exit_status_of(status);
    }
    /* status of the pipeline is that of the last stage */
    int last_status = 0;
    if (g_interactive) tcsetpgrp(0, pgid);
    while (j->alive > 0) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) { if (errno == EINTR) continue; break; }
        if (pid == pids[ns - 1]) last_status = exit_status_of(status);
        job_note_exit(pid, status);
    }
    if (g_interactive) tcsetpgrp(0, g_shell_pgid);
    free(j->cmd);
    free(j);
    g_jobs[idx] = NULL;
    return last_status;
}

static int case_match(const char *pat_word, const char *subject) {
    char *pat = expand_string(pat_word);
    int m = glob_match(pat, subject);
    free(pat);
    return m;
}

int run_node(Node *n) {
    if (!n) return 0;
    if (g_exit_requested) return g_status;
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
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) {
            if (g_interactive) setpgid(0, 0);
            reset_signals();
            int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) { dup2(nul, 0); close(nul); }
            g_interactive = 0;
            g_stage_child = inner->kind == N_SIMPLE;        /* a lone external command can exec in place */
            int s = run_node(inner);
            fflush(NULL);
            _exit(s);
        }
        if (g_interactive) setpgid(pid, pid);
        int pids[1] = { pid };
        char *text = node_text(inner);
        Job *j = job_add(pid, pids, 1, text[0] ? text : "(background group)");
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
            Saved *sv = NULL;
            if (apply_redirs(n->redirs, &sv) < 0) _exit(1);
            reset_signals();
            g_interactive = 0;
            int s = run_node(n->a);
            fflush(NULL);
            _exit(s);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
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
            if (g_exit_requested || g_return) break;
            if ((n->kind == N_WHILE) != (c == 0)) break;
            st = run_node(n->b);
            if (g_exit_requested || g_return) break;
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
            if (g_exit_requested || g_return) break;
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

static void put_word(char **buf, size_t *len, size_t *cap, const char *w) {
    for (; *w; w++) {
        if ((unsigned char)*w < 6) continue;
        if (*len + 2 > *cap) { *cap *= 2; *buf = xrealloc(*buf, *cap); }
        (*buf)[(*len)++] = *w;
    }
}

static char *node_text(Node *n) {
    size_t cap = 128, len = 0;
    char *buf = xmalloc(cap);
    buf[0] = 0;
    while (n && n->kind == N_PIPE) {
        for (int i = 0; n->a && n->a->kind == N_SIMPLE && i < n->a->nwords; i++) { put_word(&buf, &len, &cap, n->a->words[i]); buf[len++] = ' '; }
        if (len + 4 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
        buf[len++] = '|'; buf[len++] = ' ';
        n = n->b;
    }
    if (n && n->kind == N_SIMPLE)
        for (int i = 0; i < n->nwords; i++) { put_word(&buf, &len, &cap, n->words[i]); if (i + 1 < n->nwords) buf[len++] = ' '; }
    buf[len] = 0;
    return buf;
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
