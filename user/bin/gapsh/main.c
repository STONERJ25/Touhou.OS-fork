/* gapsh entry point: option handling, startup files, the interactive read-eval loop and the themed prompt. */
#include "gapsh.h"

void run_exit_trap(void);
extern char **environ;

static const char *user_name(void) {
    const char *u = var_get("USER");
    if (u && *u) return u;
    return getuid() == 0 ? "root" : "gensokyo-resident";
}

/* Builds the prompt: either $PS1 (with \u \h \w \W \$ \n \e escapes) or the built-in shrine-red default. */
static char *build_prompt(void) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    const char *home = var_get("HOME");
    char shown[PATH_MAX];
    if (home && *home && !strncmp(cwd, home, strlen(home)) && (cwd[strlen(home)] == 0 || cwd[strlen(home)] == '/'))
        snprintf(shown, sizeof shown, "~%s", cwd + strlen(home));
    else
        strlcpy(shown, cwd, sizeof shown);

    const char *ps1 = var_get("PS1");
    char host[64];
    if (gethostname(host, sizeof host) < 0) strcpy(host, "gensokyo");
    char sigil = getuid() == 0 ? '#' : '$';

    char out[1024];
    size_t n = 0;
    if (!ps1) {
        if (g_status) n += (size_t)snprintf(out + n, sizeof out - n, "\x1b[31m[%d]\x1b[0m ", g_status);
        n += (size_t)snprintf(out + n, sizeof out - n,
                              "\x1b[1;31m%s\x1b[0m\x1b[90m@\x1b[0m\x1b[1;37m%s\x1b[0m \x1b[1;36m%s\x1b[0m %c ", user_name(), host, shown, sigil);
        return xstrdup(out);
    }
    for (const char *p = ps1; *p && n + 64 < sizeof out; p++) {
        if (*p != '\\') { out[n++] = *p; continue; }
        p++;
        switch (*p) {
        case 'u': n += (size_t)snprintf(out + n, sizeof out - n, "%s", user_name()); break;
        case 'h': n += (size_t)snprintf(out + n, sizeof out - n, "%s", host); break;
        case 'w': n += (size_t)snprintf(out + n, sizeof out - n, "%s", shown); break;
        case 'W': { const char *b = strrchr(shown, '/'); n += (size_t)snprintf(out + n, sizeof out - n, "%s", b && b[1] ? b + 1 : shown); break; }
        case '$': out[n++] = sigil; break;
        case 'n': out[n++] = '\n'; break;
        case 'e': out[n++] = 27; break;
        case '[': case ']': break;
        case '\\': out[n++] = '\\'; break;
        case 0: p--; break;
        default: out[n++] = '\\'; out[n++] = *p; break;
        }
    }
    out[n] = 0;
    return xstrdup(out);
}

static void source_if_exists(const char *path) {
    if (access(path, R_OK) == 0) run_file(path, 0, NULL);
}

static void interactive_loop(void) {
    for (;;) {
        jobs_reap();
        char *prompt = build_prompt();
        char *line = edit_readline(prompt);
        free(prompt);
        g_line_no++;
        g_interrupted = 0;
        if (!line) {                                       /* Ctrl-D */
            if (exit_blocked_by_jobs()) continue;
            break;
        }
        char *src = line;
        Node *prog = NULL;
        int failed = 0;
        for (;;) {
            Lexer lx;
            lex_init(&lx, src);
            prog = parse_program(&lx);
            if (prog || (!lx.incomplete && !lx.error)) break;
            if (lx.error) { shell_error("%s", lx.errmsg); g_status = 2; failed = 1; break; }
            char *more = edit_readline("\x1b[90m>\x1b[0m ");
            if (!more) { shell_error("unexpected end of input"); failed = 1; break; }
            char *joined = xasprintf("%s\n%s", src, more);
            free(more);
            if (src != line) free(src);
            src = joined;
        }
        if (src[0] && !failed) history_add(src);
        if (prog && !failed) {
            run_node(prog);
            fflush(NULL);
        }
        if (src != line) free(src);
        free(line);
        if (g_exit_requested) break;
    }
}

int main(int argc, char **argv, char **envp) {
    int login = 0, force_interactive = 0, i = 1;
    const char *command = NULL;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l') login = 1;
            else if (*o == 'i') force_interactive = 1;
            else if (*o == 'c') { if (i + 1 < argc) command = argv[++i]; else { fprintf(stderr, "gapsh: -c needs an argument\n"); return 2; } break; }
            else if (*o == 's' || *o == 'e' || *o == 'x') { }
            else { fprintf(stderr, "gapsh: unknown option -%c\n", *o); return 2; }
        }
    }

    vars_init(envp);
    if (!var_get("PATH")) { var_set("PATH", "/bin:/sbin:/usr/bin"); var_export("PATH"); }
    if (!var_get("HOME")) { var_set("HOME", "/"); var_export("HOME"); }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) { var_set("PWD", cwd); var_export("PWD"); }
    var_set("SHELL", "/bin/gapsh");
    var_set("IFS", " \t\n");
    exec_init();

    const char *script = NULL;
    if (!command && i < argc) script = argv[i++];

    g_interactive = !command && !script && isatty(0) && isatty(2);
    if (force_interactive) g_interactive = isatty(0);

    if (g_interactive) {
        /* Started in the background (`gapsh &`): wait to be brought to the front instead of taking the terminal from
         * the shell that started us. SIGTTIN stops us until fg; a group nobody could continue drops the signal, so the
         * wait is bounded (about a second of running time) and then the shell takes the terminal as before. */
        for (int tries = 0; tries < 100; tries++) {
            pid_t fgp = tcgetpgrp(0);
            if (fgp <= 1 || fgp == getpgrp() || kill(-fgp, 0) < 0) break;
            kill(-getpgrp(), SIGTTIN);
            usleep(10000);
        }
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        pid_t me = getpid();
        if (getpgrp() != me) setpgid(0, 0);
        tcsetpgrp(0, me);
        g_shell_pgid = me;
    }

    static char *empty[1] = { NULL };
    g_pos_params = empty;
    g_npos = 0;

    if (login || g_interactive) {
        if (login) source_if_exists("/etc/profile");
        const char *home = var_get("HOME");
        if (home) {
            char *rc = xasprintf("%s/.gaprc", home);
            source_if_exists(rc);
            free(rc);
        }
    }

    int status = 0;
    if (command) {
        if (i < argc) { g_arg0 = argv[i++]; g_pos_params = argv + i; g_npos = argc - i; }
        status = run_string(command, "-c");
    } else if (script) {
        status = run_file(script, argc - (i - 1), argv + (i - 1));
    } else {
        if (g_interactive) {
            history_load();
            if (login && !getenv("NOMOTD")) {                   /* terminal windows skip the banner */
                FILE *m = fopen("/etc/motd", "r");
                if (m) {
                    int ch;                                 /* "\e" in the file means ESC, so the motd can carry colours */
                    while ((ch = fgetc(m)) != EOF) {
                        if (ch == '\\') {
                            int nx = fgetc(m);
                            if (nx == 'e') { putchar(27); continue; }
                            putchar(ch);
                            if (nx == EOF) break;
                            ch = nx;
                        }
                        putchar(ch);
                    }
                    fclose(m);
                }
            }
            interactive_loop();
            jobs_hangup_stopped();
            history_save();
            status = g_status;
        } else {
            char *line = NULL;
            size_t cap = 0;
            char *all = xstrdup("");
            ssize_t n;
            while ((n = getline(&line, &cap, stdin)) >= 0) {
                char *tmp = xasprintf("%s%s", all, line);
                free(all);
                all = tmp;
            }
            status = run_string(all, "stdin");
        }
    }
    run_exit_trap();
    fflush(NULL);
    return g_exit_requested ? g_status : status;
}
