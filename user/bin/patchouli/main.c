/*
 * patchouli: Patchouli's manual viewer.
 *
 *   patchouli [SECTION] NAME...       show manual pages (to the pager on a terminal, plain text otherwise)
 *   patchouli -k WORD...              apropos: pages whose name or description mentions WORD
 *   patchouli -f NAME...              whatis: one-line descriptions of the pages called NAME
 *   patchouli -l                      list every page
 *   patchouli -w NAME                 where: the file a page lives in
 *   patchouli --check-all             parse every page and report mistakes
 */
#include "patchouli.h"
#include <getopt.h>
#include <signal.h>

static int opt_text, opt_all, opt_raw, opt_width;

static void usage(FILE *f) {
    fprintf(f,
        "usage: patchouli [options] [SECTION] NAME...    show manual pages\n"
        "       patchouli -k WORD...                     search names and descriptions (apropos)\n"
        "       patchouli -f NAME...                     one-line descriptions (whatis)\n"
        "       patchouli -l                             list all pages\n"
        "       patchouli -w NAME...                     print the file a page lives in\n"
        "       patchouli --check-all                    parse every page and report mistakes\n"
        "       patchouli --check FILE...                the same for particular files\n"
        "options:\n"
        "  -a, --all           show the page from every section that has it\n"
        "  -t, --text          plain text, no pager (the default when standard output is not a terminal)\n"
        "  -W, --width=N       longest line to produce (default: the terminal width, at most 78)\n"
        "  -r, --raw           print the page source instead of the formatted page\n"
        "  -h, --help          this help           -V, --version   version\n"
        "Pages live in %s (or the directories in $PATCHOULI_PATH).\n"
        "Try: patchouli patchouli   (the manual for the manual)   and   patchouli gensokyo   (the lay of the land)\n",
        DEFAULT_ROOT);
}

static int term_cols(void) {
    const char *e = getenv("COLUMNS");
    if (e && atoi(e) > 0) return atoi(e);
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 80;
}

static int page_width(int tty) {
    if (opt_width > 0) return opt_width < 30 ? 30 : opt_width;
    const char *mw = getenv("MANWIDTH");
    if (mw && atoi(mw) > 0) return atoi(mw) < 30 ? 30 : atoi(mw);
    int w = tty ? term_cols() - 2 : 78;
    if (w > 78) w = 78;
    if (w < 30) w = 30;
    return w;
}

/* ------------------------------------------------------------------ output */

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int use_pager(const char **pager) {
    const char *p = getenv("PAGER");
    if (!p) p = "patchouli-pager";
    if (!*p || !strcmp(p, "cat")) return 0;
    *pager = p;
    return 1;
}

/* Feeds the text to the pager over a pipe. */
static int run_pager(const char *pager, const char *title, const PtBuf *data) {
    int fds[2];
    if (pipe(fds) < 0) return -1;
    signal(SIGPIPE, SIG_IGN);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        dup2(fds[0], 0);
        close(fds[0]);
        close(fds[1]);
        const char *base = strrchr(pager, '/');
        base = base ? base + 1 : pager;
        if (strpbrk(pager, " \t")) {
            execl("/bin/gapsh", "gapsh", "-c", pager, (char *)NULL);
        } else if (!strcmp(base, "patchouli-pager") || !strcmp(base, "less") || !strcmp(base, "more")) {
            execlp(pager, pager, "-R", "--title", title, (char *)NULL);
        } else {
            execlp(pager, pager, (char *)NULL);
        }
        fprintf(stderr, "patchouli: cannot start the pager '%s': %s\n", pager, strerror(errno));
        _exit(127);
    }
    close(fds[0]);
    int rc = write_all(fds[1], data->p ? data->p : "", data->len);
    close(fds[1]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) return -1;
    (void)rc;
    return 0;
}

static void emit(const PtBuf *data, const char *title, int tty) {
    const char *pager;
    if (tty && !opt_text && use_pager(&pager)) {
        if (run_pager(pager, title, data) == 0) return;
    }
    write_all(1, data->p ? data->p : "", data->len);
}

/* ------------------------------------------------------------------ showing pages */

static int render_file(const char *path, PtBuf *out, int width, int ansi) {
    size_t len;
    char *src = read_file(path, &len);
    if (!src) { fprintf(stderr, "patchouli: %s: %s\n", path, strerror(errno)); return -1; }
    if (opt_raw) { pb_add(out, src, len); free(src); return 0; }
    Page pg;
    memset(&pg, 0, sizeof pg);
    pw_parse(&pg, src, len);
    if (!pg.name) {
        fprintf(stderr, "patchouli: %s: not a manual page (no .page line)\n", path);
        pw_free(&pg);
        free(src);
        return -1;
    }
    pw_render(&pg, out, width, ansi);
    pw_free(&pg);
    free(src);
    return 0;
}

static int is_path(const char *s) { return strchr(s, '/') != NULL || (strlen(s) > 3 && !strcmp(s + strlen(s) - 3, ".pw")); }

static int show(Index *ix, const char *section, char **names, int n) {
    int tty = isatty(1);
    int ansi = tty && !opt_text;
    int width = page_width(tty);
    PtBuf out = { 0 };
    char title[200] = "";
    int rc = 0, shown = 0;
    for (int i = 0; i < n; i++) {
        if (is_path(names[i])) {
            if (shown) pb_adds(&out, "\n");
            if (render_file(names[i], &out, width, ansi) < 0) rc = 1;
            else { shown++; snprintf(title, sizeof title, "%s", names[i]); }
            continue;
        }
        Entry *e = index_find(ix, section, names[i]);
        if (!e) {
            fprintf(stderr, "patchouli: no manual entry for %s%s%s -- the library has no such book (try: patchouli -k %s)\n",
                    names[i], section ? " in section " : "", section ? section : "", names[i]);
            rc = 1;
            continue;
        }
        /* the page itself, plus (with -a) the pages of the same name in other sections; an alias leads to its page */
        const char *paths[16];
        int np = 0;
        paths[np++] = e->path;
        if (opt_all) {
            for (int k = 0; k < ix->n && np < 16; k++) {
                Entry *c = &ix->e[k];
                if (strcmp(c->name, e->name) || (section && strcmp(c->sec, section))) continue;
                int dup = 0;
                for (int q = 0; q < np; q++) if (!strcmp(paths[q], c->path)) dup = 1;
                if (!dup) paths[np++] = c->path;
            }
        }
        for (int q = 0; q < np; q++) {
            if (shown) pb_adds(&out, "\n");
            if (render_file(paths[q], &out, width, ansi) < 0) rc = 1;
            else { shown++; snprintf(title, sizeof title, "%s(%s)", e->target ? e->target : e->name, e->sec); }
        }
    }
    if (shown) {
        if (shown > 1) snprintf(title, sizeof title, "%d manual pages", shown);
        emit(&out, title, tty);
    }
    pb_free(&out);
    return rc;
}

/* ------------------------------------------------------------------ listing */

static void print_entry(const Entry *e) {
    char left[160];
    snprintf(left, sizeof left, "%s(%s)", e->name, e->sec);
    printf("%-22s - %s\n", left, e->desc);
}

static int contains_nocase(const char *h, const char *needle) { return strcasestr(h, needle) != NULL; }

static int cmd_apropos(Index *ix, char **words, int n) {
    int found = 0;
    for (int i = 0; i < ix->n; i++) {
        Entry *e = &ix->e[i];
        for (int k = 0; k < n; k++)
            if (contains_nocase(e->name, words[k]) || contains_nocase(e->desc, words[k])) { print_entry(e); found++; break; }
    }
    if (!found) { fprintf(stderr, "patchouli: nothing appropriate (the library has nothing on that)\n"); return 1; }
    return 0;
}

static int cmd_whatis(Index *ix, char **names, int n) {
    int rc = 0;
    for (int k = 0; k < n; k++) {
        int found = 0;
        for (int i = 0; i < ix->n; i++) if (!strcmp(ix->e[i].name, names[k])) { print_entry(&ix->e[i]); found++; }
        if (!found) { fprintf(stderr, "%s: nothing appropriate\n", names[k]); rc = 1; }
    }
    return rc;
}

static int cmd_where(Index *ix, const char *section, char **names, int n) {
    int rc = 0;
    for (int k = 0; k < n; k++) {
        Entry *e = index_find(ix, section, names[k]);
        if (e) puts(e->path);
        else { fprintf(stderr, "patchouli: no manual entry for %s\n", names[k]); rc = 1; }
    }
    return rc;
}

/* ------------------------------------------------------------------ checking */

static int check_one(Index *ix, const char *path, const char *dir_section, const char *base, int *problems) {
    size_t len;
    char *src = read_file(path, &len);
    if (!src) { printf("%s:1: cannot read the file: %s\n", path, strerror(errno)); (*problems)++; return -1; }
    Page pg;
    memset(&pg, 0, sizeof pg);
    pw_parse(&pg, src, len);
    *problems += pw_check(&pg, path, dir_section, base, ix, stdout);
    /* rendering must not crash and must produce something */
    PtBuf out = { 0 };
    pw_render(&pg, &out, 72, 0);
    if (out.len < 20) { printf("%s:1: the page renders to nothing\n", path); (*problems)++; }
    pb_free(&out);
    pw_free(&pg);
    free(src);
    return 0;
}

static int cmd_check_all(Index *ix) {
    int pages = 0, problems = 0;
    for (int r = 0; r < ix->nroots; r++) {
        DIR *d = opendir(ix->roots[r]);
        if (!d) { printf("%s: cannot open the page directory: %s\n", ix->roots[r], strerror(errno)); problems++; continue; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s/%s", ix->roots[r], de->d_name);
            DIR *sd = opendir(dir);
            if (!sd) continue;
            struct dirent *fe;
            while ((fe = readdir(sd))) {
                size_t l = strlen(fe->d_name);
                if (l < 4 || strcmp(fe->d_name + l - 3, ".pw")) {
                    if (fe->d_name[0] != '.') { printf("%s/%s:1: not a .pw file\n", dir, fe->d_name); problems++; }
                    continue;
                }
                char path[PATH_MAX], base[256];
                snprintf(path, sizeof path, "%s/%s", dir, fe->d_name);
                snprintf(base, sizeof base, "%.*s", (int)(l - 3), fe->d_name);
                check_one(ix, path, de->d_name, base, &problems);
                pages++;
            }
            closedir(sd);
        }
        closedir(d);
    }
    printf("%d pages checked, %d problem%s\n", pages, problems, problems == 1 ? "" : "s");
    return problems ? 1 : 0;
}

static int cmd_check_files(Index *ix, char **files, int n) {
    int problems = 0;
    for (int i = 0; i < n; i++) {
        char base[256];
        const char *slash = strrchr(files[i], '/');
        const char *b = slash ? slash + 1 : files[i];
        size_t l = strlen(b);
        snprintf(base, sizeof base, "%.*s", (int)(l > 3 && !strcmp(b + l - 3, ".pw") ? l - 3 : l), b);
        check_one(ix, files[i], NULL, base, &problems);
    }
    printf("%d page%s checked, %d problem%s\n", n, n == 1 ? "" : "s", problems, problems == 1 ? "" : "s");
    return problems ? 1 : 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    enum { M_SHOW, M_APROPOS, M_WHATIS, M_LIST, M_WHERE, M_CHECKALL, M_CHECK } mode = M_SHOW;
    static const struct option longopts[] = {
        { "all", 0, NULL, 'a' }, { "text", 0, NULL, 't' }, { "width", 1, NULL, 'W' }, { "raw", 0, NULL, 'r' },
        { "apropos", 0, NULL, 'k' }, { "whatis", 0, NULL, 'f' }, { "list", 0, NULL, 'l' }, { "where", 0, NULL, 'w' },
        { "check-all", 0, NULL, 1000 }, { "check", 0, NULL, 1001 }, { "help", 0, NULL, 'h' }, { "version", 0, NULL, 'V' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "atW:rkflwhV", longopts, NULL)) != -1) {
        switch (c) {
        case 'a': opt_all = 1; break;
        case 't': opt_text = 1; break;
        case 'W': opt_width = atoi(optarg); break;
        case 'r': opt_raw = 1; break;
        case 'k': mode = M_APROPOS; break;
        case 'f': mode = M_WHATIS; break;
        case 'l': mode = M_LIST; break;
        case 'w': mode = M_WHERE; break;
        case 1000: mode = M_CHECKALL; break;
        case 1001: mode = M_CHECK; break;
        case 'h': usage(stdout); return 0;
        case 'V': puts("patchouli 1.0 (TouhouOS) -- Patchouli's manual viewer"); return 0;
        default: usage(stderr); return 2;
        }
    }
    char **args = argv + optind;
    int nargs = argc - optind;

    Index ix;
    index_load(&ix);

    switch (mode) {
    case M_LIST:
        for (int i = 0; i < ix.n; i++) print_entry(&ix.e[i]);
        return 0;
    case M_CHECKALL:
        return cmd_check_all(&ix);
    case M_CHECK:
        if (!nargs) { fprintf(stderr, "patchouli: --check needs file names\n"); return 2; }
        return cmd_check_files(&ix, args, nargs);
    case M_APROPOS:
        if (!nargs) { fprintf(stderr, "patchouli: -k needs a word to look for\n"); return 2; }
        return cmd_apropos(&ix, args, nargs);
    default:
        break;
    }
    if (!nargs) {
        fprintf(stderr, "patchouli: which manual page do you want? (try: patchouli patchouli, or patchouli -l for the whole library)\n");
        return 1;
    }
    const char *section = NULL;
    if (nargs > 1 && strlen(args[0]) == 1 && args[0][0] >= '1' && args[0][0] <= '9') {
        section = args[0];
        args++;
        nargs--;
    }
    if (mode == M_WHATIS) return cmd_whatis(&ix, args, nargs);
    if (mode == M_WHERE) return cmd_where(&ix, section, args, nargs);
    if (ix.n == 0) {
        int all_paths = 1;
        for (int i = 0; i < nargs; i++) if (!is_path(args[i])) all_paths = 0;
        if (!all_paths) {
            fprintf(stderr, "patchouli: the library is empty: no pages found under %s\n", ix.nroots ? ix.roots[0] : DEFAULT_ROOT);
            return 1;
        }
    }
    return show(&ix, section, args, nargs);
}
