/* Builtin commands. */
#include "gapsh.h"

extern int g_last_bg;
extern char **environ;

typedef struct Alias {
    char *name, *value;
    struct Alias *next;
} Alias;

static Alias *g_aliases;
static char *g_exit_trap;

const char *alias_lookup(const char *name) {
    for (Alias *a = g_aliases; a; a = a->next)
        if (!strcmp(a->name, name)) return a->value;
    return NULL;
}

static void alias_set(const char *name, const char *value) {
    for (Alias *a = g_aliases; a; a = a->next)
        if (!strcmp(a->name, name)) { free(a->value); a->value = xstrdup(value); return; }
    Alias *a = xmalloc(sizeof *a);
    a->name = xstrdup(name);
    a->value = xstrdup(value);
    a->next = g_aliases;
    g_aliases = a;
}

void run_exit_trap(void) {
    if (g_exit_trap) {
        char *t = g_exit_trap;
        g_exit_trap = NULL;
        run_string(t, "trap");
    }
}

static int bi_colon(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int bi_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

static int bi_echo(int argc, char **argv) {
    int nl = 1, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i] + 1;
        int ok = 1;
        for (; *o; o++) if (*o != 'n' && *o != 'e' && *o != 'E') ok = 0;
        if (!ok) break;
        for (o = argv[i] + 1; *o; o++) { if (*o == 'n') nl = 0; else if (*o == 'e') esc = 1; else esc = 0; }
    }
    for (; i < argc; i++) {
        const char *s = argv[i];
        if (!esc) fputs(s, stdout);
        else {
            for (; *s; s++) {
                if (*s != '\\' || !s[1]) { putchar(*s); continue; }
                s++;
                switch (*s) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break;
                case 'e': putchar(27); break;
                case 'c': return 0;
                case '\\': putchar('\\'); break;
                case '0': { int v = 0, k = 0; while (k < 3 && s[1] >= '0' && s[1] <= '7') { v = v * 8 + (*++s - '0'); k++; } putchar(v); break; }
                case 'x': { int v = 0, k = 0; while (k < 2 && isxdigit((unsigned char)s[1])) { char c = *++s; v = v * 16 + (isdigit((unsigned char)c) ? c - '0' : tolower(c) - 'a' + 10); k++; } putchar(v); break; }
                default: putchar('\\'); putchar(*s); break;
                }
            }
        }
        if (i + 1 < argc) putchar(' ');
    }
    if (nl) putchar('\n');
    return 0;
}

static int bi_printf(int argc, char **argv) {
    if (argc < 2) { shell_error("printf: usage: printf format [arguments]"); return 2; }
    const char *fmt = argv[1];
    int argi = 2;
    int consumed;
    do {
        consumed = 0;
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case 'a': putchar('\a'); break;
                case 'e': putchar(27); break;
                case '\\': putchar('\\'); break;
                case '0': { int v = 0, k = 0; while (k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (*++p - '0'); k++; } putchar(v); break; }
                case 0: p--; break;
                default: putchar('\\'); putchar(*p); break;
                }
                continue;
            }
            if (*p != '%') { putchar(*p); continue; }
            p++;
            if (*p == '%') { putchar('%'); continue; }
            char spec[32];
            size_t sn = 0;
            spec[sn++] = '%';
            while (*p && strchr("-+ #0123456789.", *p) && sn < sizeof spec - 4) spec[sn++] = *p++;
            char conv = *p;
            const char *arg = argi < argc ? argv[argi] : NULL;
            if (arg) { argi++; consumed = 1; }
            switch (conv) {
            case 'd': case 'i': {
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = 'd'; spec[sn] = 0;
                printf(spec, arg ? strtoll(arg, NULL, 0) : 0LL);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                spec[sn++] = 'l'; spec[sn++] = 'l'; spec[sn++] = conv; spec[sn] = 0;
                printf(spec, arg ? strtoull(arg, NULL, 0) : 0ULL);
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G':
                spec[sn++] = conv; spec[sn] = 0;
                printf(spec, arg ? strtod(arg, NULL) : 0.0);
                break;
            case 'c':
                putchar(arg ? arg[0] : 0);
                break;
            case 's':
                spec[sn++] = 's'; spec[sn] = 0;
                printf(spec, arg ? arg : "");
                break;
            case 'b': {
                const char *s = arg ? arg : "";
                for (; *s; s++) {
                    if (*s == '\\' && s[1] == 'n') { putchar('\n'); s++; }
                    else if (*s == '\\' && s[1] == 't') { putchar('\t'); s++; }
                    else putchar(*s);
                }
                break;
            }
            case 0: p--; break;
            default: putchar('%'); putchar(conv); break;
            }
        }
    } while (consumed && argi < argc);
    return 0;
}

static void set_pwd(void) {
    char cwd[PATH_MAX];
    const char *old = var_get("PWD");
    if (old) var_set("OLDPWD", old);
    if (getcwd(cwd, sizeof cwd)) var_set("PWD", cwd);
}

static int bi_cd(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : var_get("HOME");
    if (argc > 1 && !strcmp(argv[1], "-")) {
        dir = var_get("OLDPWD");
        if (!dir) { shell_error("cd: OLDPWD not set"); return 1; }
        printf("%s\n", dir);
    }
    if (!dir) { shell_error("cd: HOME not set"); return 1; }
    if (chdir(dir) < 0) { shell_error("cd: %s: %s", dir, strerror(errno)); return 1; }
    set_pwd();
    return 0;
}

static int bi_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) { shell_error("pwd: %s", strerror(errno)); return 1; }
    puts(cwd);
    return 0;
}

static char *g_dirstack[32];
static int g_ndirs;

static int bi_pushd(int argc, char **argv) {
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof cwd);
    if (argc < 2) { shell_error("pushd: no directory"); return 1; }
    if (chdir(argv[1]) < 0) { shell_error("pushd: %s: %s", argv[1], strerror(errno)); return 1; }
    if (g_ndirs < 32) g_dirstack[g_ndirs++] = xstrdup(cwd);
    set_pwd();
    return 0;
}

static int bi_popd(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!g_ndirs) { shell_error("popd: directory stack empty"); return 1; }
    char *d = g_dirstack[--g_ndirs];
    int r = chdir(d);
    free(d);
    set_pwd();
    return r < 0;
}

static int bi_dirs(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof cwd);
    printf("%s", cwd);
    for (int i = g_ndirs - 1; i >= 0; i--) printf(" %s", g_dirstack[i]);
    putchar('\n');
    return 0;
}

int g_line_no;                          /* counts the lines typed at the prompt */
int g_exit_warned = -10;                /* the line on which `exit` last warned about stopped jobs */

/* An interactive shell with stopped jobs warns once; an `exit` (or ^D) right after the warning leaves, and the stopped
 * jobs get SIGHUP and SIGCONT (see jobs_hangup_stopped). */
int exit_blocked_by_jobs(void) {
    if (!g_interactive || g_exit_warned >= g_line_no - 1 || !jobs_stopped()) return 0;
    fprintf(stderr, "There are stopped jobs. Like Sakuya's knives they hang frozen in time; "
                    "exit again to leave anyway (they get SIGHUP), or use jobs, fg and kill.\n");
    g_exit_warned = g_line_no;
    return 1;
}

static int bi_exit(int argc, char **argv) {
    if (exit_blocked_by_jobs()) return 1;
    g_status = argc > 1 ? atoi(argv[1]) & 0xFF : g_status;
    g_exit_requested = 1;
    return g_status;
}

static int bi_return(int argc, char **argv) {
    g_status = argc > 1 ? atoi(argv[1]) & 0xFF : g_status;
    g_return = 1;
    return g_status;
}

static int bi_break(int argc, char **argv) { g_break = argc > 1 ? atoi(argv[1]) : 1; return 0; }
static int bi_continue(int argc, char **argv) { g_continue = argc > 1 ? atoi(argv[1]) : 1; return 0; }

static void print_export(const char *name, const char *value, int exported, void *ctx) {
    (void)ctx;
    if (exported) printf("export %s=\"%s\"\n", name, value);
}

static void print_var(const char *name, const char *value, int exported, void *ctx) {
    (void)ctx; (void)exported;
    printf("%s=%s\n", name, value);
}

static int bi_export(int argc, char **argv) {
    if (argc == 1) { vars_each(print_export, NULL); return 0; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) continue;
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; var_set(argv[i], eq + 1); }
        var_export(argv[i]);
    }
    return 0;
}

static int bi_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "-v") && strcmp(argv[i], "-f")) var_unset(argv[i]);
    return 0;
}

static int bi_local(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) *eq = 0;
        local_declare(argv[i]);
        if (eq) var_set(argv[i], eq + 1);
        else var_set(argv[i], "");
    }
    return 0;
}

static int bi_set(int argc, char **argv) {
    if (argc == 1) { vars_each(print_var, NULL); return 0; }
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) i++;   /* options like -e, -x: accepted, ignored */
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i <= argc) {
        char **np = xmalloc((size_t)(argc - i + 1) * sizeof(char *));
        for (int k = i; k < argc; k++) np[k - i] = xstrdup(argv[k]);
        np[argc - i] = NULL;
        g_pos_params = np;
        g_npos = argc - i;
    }
    return 0;
}

static int bi_shift(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n > g_npos) { shell_error("shift: shift count out of range"); return 1; }
    g_pos_params += n;
    g_npos -= n;
    return 0;
}

static int bi_alias(int argc, char **argv) {
    if (argc == 1) {
        for (Alias *a = g_aliases; a; a = a->next) printf("alias %s='%s'\n", a->name, a->value);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; alias_set(argv[i], eq + 1); }
        else if (alias_lookup(argv[i])) printf("alias %s='%s'\n", argv[i], alias_lookup(argv[i]));
        else { shell_error("alias: %s: not found", argv[i]); return 1; }
    }
    return 0;
}

static int bi_unalias(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        for (Alias **pp = &g_aliases; *pp; pp = &(*pp)->next) {
            if (!strcmp((*pp)->name, argv[i])) { Alias *a = *pp; *pp = a->next; free(a->name); free(a->value); free(a); break; }
        }
    }
    return 0;
}

static int bi_type(int argc, char **argv) {
    int st = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "-v")) continue;
        const char *al = alias_lookup(argv[i]);
        char *path;
        if (al) printf("%s is aliased to '%s'\n", argv[i], al);
        else if (find_function(argv[i])) printf("%s is a function\n", argv[i]);
        else if (is_builtin(argv[i])) printf("%s is a shell builtin\n", argv[i]);
        else if ((path = find_in_path(argv[i]))) { printf("%s is %s\n", argv[i], path); free(path); }
        else { fprintf(stderr, "gapsh: type: %s: not found\n", argv[i]); st = 1; }
    }
    return st;
}

static int bi_command(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "-v")) {
        char *path;
        if (is_builtin(argv[2]) || find_function(argv[2])) { puts(argv[2]); return 0; }
        if ((path = find_in_path(argv[2]))) { puts(path); free(path); return 0; }
        return 1;
    }
    if (argc > 1) {                                     /* run bypassing functions/aliases */
        char *path = find_in_path(argv[1]);
        if (is_builtin(argv[1])) return run_builtin(argc - 1, argv + 1);
        if (!path) { shell_error("%s: command not found", argv[1]); return 127; }
        int st = run_program(path, argv + 1);
        free(path);
        return st;
    }
    return 0;
}

static int bi_source(int argc, char **argv) {
    if (argc < 2) { shell_error("%s: filename argument required", argv[0]); return 2; }
    char *path = strchr(argv[1], '/') ? xstrdup(argv[1]) : find_in_path(argv[1]);
    if (!path) path = xstrdup(argv[1]);
    int st = run_file(path, argc - 1, argv + 1);
    free(path);
    return st;
}

static int bi_eval(int argc, char **argv) {
    size_t len = 1;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    char *s = xmalloc(len);
    s[0] = 0;
    for (int i = 1; i < argc; i++) { if (i > 1) strcat(s, " "); strcat(s, argv[i]); }
    int st = run_string(s, "eval");
    free(s);
    return st;
}

static int bi_exec(int argc, char **argv) {
    if (argc < 2) return 0;
    char *path = find_in_path(argv[1]);
    if (!path) { shell_error("exec: %s: not found", argv[1]); return 127; }
    char **env = vars_environ();
    fflush(NULL);
    execve(path, argv + 1, env);
    shell_error("exec: %s: %s", argv[1], strerror(errno));
    return 126;
}

static int bi_read(int argc, char **argv) {
    const char *prompt = NULL;
    int raw = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-r")) raw = 1;
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) prompt = argv[++i];
    }
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    char line[4096];
    size_t n = 0;
    int c, got = 0;
    char ch;
    while (n < sizeof line - 1) {
        ssize_t r = read(0, &ch, 1);
        if (r <= 0) break;
        got = 1;
        c = ch;
        if (c == '\n') break;
        if (c == '\\' && !raw) {
            if (read(0, &ch, 1) <= 0) break;
            if (ch == '\n') continue;
            c = ch;
        }
        line[n++] = (char)c;
    }
    line[n] = 0;
    const char *names[16];
    int nn = 0;
    for (; i < argc && nn < 16; i++) names[nn++] = argv[i];
    if (!nn) { names[0] = "REPLY"; nn = 1; }
    const char *p = line;
    for (int k = 0; k < nn; k++) {
        while (*p == ' ' || *p == '\t') p++;
        if (k == nn - 1) { var_set(names[k], p); break; }
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t') e++;
        char *w = xasprintf("%.*s", (int)(e - p), p);
        var_set(names[k], w);
        free(w);
        p = e;
    }
    return got ? 0 : 1;
}

/* ---- test / [ ---- */

static int test_file(const char *op, const char *path) {
    struct stat st;
    if (op[1] == 'e') return stat(path, &st) == 0;
    if (op[1] == 'L' || op[1] == 'h') return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
    if (stat(path, &st) != 0) return 0;
    switch (op[1]) {
    case 'f': return S_ISREG(st.st_mode);
    case 'd': return S_ISDIR(st.st_mode);
    case 'c': return S_ISCHR(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case 's': return st.st_size > 0;
    case 'r': return access(path, R_OK) == 0;
    case 'w': return access(path, W_OK) == 0;
    case 'x': return access(path, X_OK) == 0;
    default: return 0;
    }
}

static int test_or(char **a, int n, int *pos);

static int test_primary(char **a, int n, int *pos) {
    if (*pos >= n) return 0;
    const char *t = a[*pos];
    if (!strcmp(t, "!")) { (*pos)++; return !test_primary(a, n, pos); }
    if (!strcmp(t, "(")) {
        (*pos)++;
        int v = test_or(a, n, pos);
        if (*pos < n && !strcmp(a[*pos], ")")) (*pos)++;
        return v;
    }
    /* a binary operator in the middle word takes precedence over a unary reading */
    if (*pos + 2 < n) {
        const char *op = a[*pos + 1];
        const char *l = a[*pos], *r = a[*pos + 2];
        int is_bin = !strcmp(op, "=") || !strcmp(op, "==") || !strcmp(op, "!=") || !strcmp(op, "<") || !strcmp(op, ">") ||
                     !strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-lt") || !strcmp(op, "-le") ||
                     !strcmp(op, "-gt") || !strcmp(op, "-ge") || !strcmp(op, "-nt") || !strcmp(op, "-ot");
        if (is_bin) {
            *pos += 3;
            if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(l, r);
            if (!strcmp(op, "!=")) return strcmp(l, r) != 0;
            if (!strcmp(op, "<")) return strcmp(l, r) < 0;
            if (!strcmp(op, ">")) return strcmp(l, r) > 0;
            if (!strcmp(op, "-nt") || !strcmp(op, "-ot")) {
                struct stat s1, s2;
                if (stat(l, &s1) || stat(r, &s2)) return 0;
                return !strcmp(op, "-nt") ? s1.st_mtime > s2.st_mtime : s1.st_mtime < s2.st_mtime;
            }
            long x = strtol(l, NULL, 10), y = strtol(r, NULL, 10);
            if (!strcmp(op, "-eq")) return x == y;
            if (!strcmp(op, "-ne")) return x != y;
            if (!strcmp(op, "-lt")) return x < y;
            if (!strcmp(op, "-le")) return x <= y;
            if (!strcmp(op, "-gt")) return x > y;
            return x >= y;
        }
    }
    if (t[0] == '-' && t[1] && !t[2] && *pos + 1 < n) {
        const char *arg = a[*pos + 1];
        *pos += 2;
        if (t[1] == 'z') return arg[0] == 0;
        if (t[1] == 'n') return arg[0] != 0;
        if (t[1] == 't') return isatty(atoi(arg));
        return test_file(t, arg);
    }
    (*pos)++;
    return t[0] != 0;
}

static int test_and(char **a, int n, int *pos) {
    int v = test_primary(a, n, pos);
    while (*pos < n && !strcmp(a[*pos], "-a")) { (*pos)++; int r = test_primary(a, n, pos); v = v && r; }
    return v;
}

static int test_or(char **a, int n, int *pos) {
    int v = test_and(a, n, pos);
    while (*pos < n && !strcmp(a[*pos], "-o")) { (*pos)++; int r = test_and(a, n, pos); v = v || r; }
    return v;
}

static int bi_test(int argc, char **argv) {
    int n = argc - 1;
    char **a = argv + 1;
    if (!strcmp(argv[0], "[")) {
        if (n < 1 || strcmp(a[n - 1], "]")) { shell_error("[: missing ']'"); return 2; }
        n--;
    }
    if (n == 0) return 1;
    int pos = 0;
    int v = test_or(a, n, &pos);
    return v ? 0 : 1;
}

/* ---- jobs, signals, history, misc ---- */

static int bi_jobs(int argc, char **argv) { return jobs_command(argc, argv, "jobs"); }
static int bi_fg(int argc, char **argv) { return jobs_command(argc, argv, "fg"); }
static int bi_bg(int argc, char **argv) { return jobs_command(argc, argv, "bg"); }

static int bi_wait(int argc, char **argv) { return jobs_command(argc, argv, "wait"); }
static int bi_disown(int argc, char **argv) { return jobs_command(argc, argv, "disown"); }

static const struct { const char *name; int num; } g_signames[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "ILL", SIGILL }, { "ABRT", SIGABRT }, { "KILL", SIGKILL },
    { "USR1", SIGUSR1 }, { "SEGV", SIGSEGV }, { "USR2", SIGUSR2 }, { "PIPE", SIGPIPE }, { "ALRM", SIGALRM },
    { "TERM", SIGTERM }, { "CHLD", SIGCHLD }, { "CONT", SIGCONT }, { "STOP", SIGSTOP }, { "TSTP", SIGTSTP },
};

static int sig_from_name(const char *s) {
    if (isdigit((unsigned char)s[0])) return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (unsigned i = 0; i < sizeof g_signames / sizeof g_signames[0]; i++)
        if (!strcmp(g_signames[i].name, s)) return g_signames[i].num;
    return -1;
}

static int bi_kill(int argc, char **argv) {
    int sig = SIGTERM, i = 1;
    if (argc > 1 && !strcmp(argv[1], "-l")) {
        for (unsigned k = 0; k < sizeof g_signames / sizeof g_signames[0]; k++) printf("%d) SIG%s\n", g_signames[k].num, g_signames[k].name);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-s")) { sig = sig_from_name(argv[2]); i = 3; }
    else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) { sig = sig_from_name(argv[1] + 1); i = 2; }
    if (sig < 0) { shell_error("kill: invalid signal"); return 1; }
    if (i >= argc) { shell_error("kill: usage: kill [-s sig | -sig] pid ..."); return 2; }
    int st = 0;
    for (; i < argc; i++) {
        if (argv[i][0] == '%') { if (job_kill(argv[i], sig)) st = 1; continue; }     /* a job: its whole group */
        pid_t pid = atoi(argv[i]);
        if (kill(pid, sig) < 0) { shell_error("kill: (%s) - %s", argv[i], strerror(errno)); st = 1; }
    }
    return st;
}

static int bi_history(int argc, char **argv) {
    (void)argc; (void)argv;
    history_print();
    return 0;
}

static int bi_umask(int argc, char **argv) {
    if (argc > 1) { umask((mode_t)strtol(argv[1], NULL, 8)); return 0; }
    mode_t m = umask(022);
    umask(m);
    printf("%04o\n", m);
    return 0;
}

static int bi_trap(int argc, char **argv) {
    if (argc < 3) return 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "EXIT") || !strcmp(argv[i], "0")) {
            free(g_exit_trap);
            g_exit_trap = strcmp(argv[1], "-") ? xstrdup(argv[1]) : NULL;
        } else {
            int sig = sig_from_name(argv[i]);
            if (sig > 0) signal(sig, argv[1][0] == 0 ? SIG_IGN : SIG_DFL);
        }
    }
    return 0;
}

static int bi_ulimit(int argc, char **argv) { (void)argc; (void)argv; puts("unlimited"); return 0; }

static int bi_getopts(int argc, char **argv) {
    if (argc < 3) return 2;
    static int idx = 1, pos = 0;
    const char *opts = argv[1];
    const char *name = argv[2];
    char **args = argc > 3 ? argv + 3 : g_pos_params - 1;
    int nargs = argc > 3 ? argc - 3 : g_npos;
    const char *oi = var_get("OPTIND");
    if (oi) idx = atoi(oi);
    if (idx < 1) idx = 1;
    if (idx > nargs || args[idx][0] != '-' || !args[idx][1] || !strcmp(args[idx], "--")) { if (idx <= nargs && args[idx] && !strcmp(args[idx], "--")) idx++; var_set("OPTIND", xasprintf("%d", idx)); return 1; }
    if (!pos) pos = 1;
    char c = args[idx][pos];
    const char *o = strchr(opts, c);
    char val[2] = { c, 0 };
    if (!o || c == ':') { var_set(name, "?"); var_set("OPTARG", val); }
    else if (o[1] == ':') {
        var_set(name, val);
        if (args[idx][pos + 1]) var_set("OPTARG", &args[idx][pos + 1]);
        else if (idx + 1 <= nargs) var_set("OPTARG", args[++idx]);
        pos = 0;
        idx++;
        var_set("OPTIND", xasprintf("%d", idx));
        return 0;
    } else var_set(name, val);
    if (!args[idx][++pos]) { pos = 0; idx++; }
    var_set("OPTIND", xasprintf("%d", idx));
    return 0;
}

static int bi_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\x1b[1;31mgapsh\x1b[0m -- the shell of Gensokyo. Every command passes through a gap in the Boundary.\n\n");
    printf("builtins:  cd pwd echo printf exit export unset set shift local alias unalias type command\n");
    printf("           source . eval exec read test [ jobs fg bg wait disown kill history umask\n");
    printf("           trap getopts pushd popd dirs help true false :\n\n");
    printf("syntax:    pipes | && || ; & ( ) { }   if/elif/else/fi   while/until/do/done   for x in ...\n");
    printf("           case/esac   name() { ... }   $var ${var:-x} $(cmd) $((1+2))   globs * ? [a-z]\n");
    printf("           < > >> 2> 2>&1 &> <<EOF heredocs\n\n");
    printf("keys:      Up/Down history, Tab completion, Ctrl-A/E home/end, Ctrl-K/U/W kill, Ctrl-L clear,\n");
    printf("           Ctrl-R search, Ctrl-D exit, Ctrl-C cancel; Ctrl-Z stops the running program (fg, bg)\n\n");
    printf("more:      'patchouli' for manual pages, 'omikuji' for a fortune, 'spellmgr' to see running spell cards.\n");
    return 0;
}

typedef struct { const char *name; int (*fn)(int, char **); } Builtin;

static const Builtin g_builtins[] = {
    { ":", bi_colon }, { "true", bi_colon }, { "false", bi_false }, { "echo", bi_echo }, { "printf", bi_printf },
    { "cd", bi_cd }, { "pwd", bi_pwd }, { "pushd", bi_pushd }, { "popd", bi_popd }, { "dirs", bi_dirs },
    { "exit", bi_exit }, { "return", bi_return }, { "break", bi_break }, { "continue", bi_continue },
    { "export", bi_export }, { "unset", bi_unset }, { "local", bi_local }, { "set", bi_set }, { "shift", bi_shift },
    { "alias", bi_alias }, { "unalias", bi_unalias }, { "type", bi_type }, { "command", bi_command },
    { "source", bi_source }, { ".", bi_source }, { "eval", bi_eval }, { "exec", bi_exec }, { "read", bi_read },
    { "test", bi_test }, { "[", bi_test }, { "jobs", bi_jobs }, { "fg", bi_fg }, { "bg", bi_bg }, { "wait", bi_wait },
    { "disown", bi_disown },
    { "kill", bi_kill }, { "history", bi_history }, { "umask", bi_umask }, { "trap", bi_trap }, { "ulimit", bi_ulimit },
    { "getopts", bi_getopts }, { "help", bi_help }, { "readonly", bi_export },
};

int is_builtin(const char *name) {
    for (unsigned i = 0; i < sizeof g_builtins / sizeof g_builtins[0]; i++)
        if (!strcmp(g_builtins[i].name, name)) return 1;
    return 0;
}

int run_builtin(int argc, char **argv) {
    for (unsigned i = 0; i < sizeof g_builtins / sizeof g_builtins[0]; i++)
        if (!strcmp(g_builtins[i].name, argv[0])) return g_builtins[i].fn(argc, argv);
    return 127;
}

/* names for tab completion */
const char *builtin_name(int i) {
    return (unsigned)i < sizeof g_builtins / sizeof g_builtins[0] ? g_builtins[i].name : NULL;
}
