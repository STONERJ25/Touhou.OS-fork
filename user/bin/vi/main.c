/* main.c: globals, command line, the main loop. */
#include "vi.h"

char fname[MAXPATH];
int file_ro, view_mode;
Opts o = { .number = 0, .rnu = 0, .list = 0, .ic = 0, .scs = 0, .ws = 1, .hls = 0, .is = 0, .wrap = 1, .et = 0, .ai = 0, .ro = 0,
           .showmode = 1, .ruler = 1, .cursorline = 0, .magic = 1, .ts = 8, .sw = 8, .sts = 0, .tw = 0 };
Pos cur;
int curswant;
int mode = M_NORMAL;
long topline;
Pos marks[26], mark_ctx = { -1, 0 }, mark_lt = { -1, 0 }, mark_gt = { -1, 0 }, mark_lb = { -1, 0 }, mark_rb = { -1, 0 }, mark_dot = { -1, 0 };
Pos mark_ins = { -1, 0 };
PtTerm term;
int quit_flag, exit_status;
int vis_mode;
Pos vis_start;
char **argfiles;
int nargfiles, argidx;

static void usage(void) {
    static const char u[] =
        "usage: vi [-R] [-c command] [+line | +/pattern | +command] [file ...]\n"
        "  -R  read-only (view)   -c  run an ex command after loading   +N  start on line N\n";
    ssize_t w = write(2, u, sizeof u - 1);
    (void)w;
}

static void load_first(void) {
    if (nargfiles == 0) {
        buf_new_empty();
        fname[0] = 0;
        return;
    }
    snprintf(fname, sizeof fname, "%s", argfiles[0]);
    int nlines;
    long nbytes;
    int rc = buf_load(fname, &nlines, &nbytes);
    if (rc == 1) set_msg("\"%s\" [New]", fname);
    else if (rc < 0) {
        if (errno == EISDIR) set_err("\"%s\" is a directory", fname);
        else set_err("\"%s\" %s", fname, strerror(errno));
    } else set_msg("\"%s\"%s %dL, %ldB", fname, noeol ? " [noeol]" : "", nlines, nbytes);
    if (rc == 0 && access(fname, W_OK) != 0) { o.ro = 1; file_ro = 1; set_msg("\"%s\"%s [readonly] %dL, %ldB", fname, noeol ? " [noeol]" : "", nlines, nbytes); }
}

int main(int argc, char **argv) {
    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    if (!strcmp(prog, "view")) { view_mode = 1; o.ro = 1; }
    char *startup[32];
    int nstartup = 0;
    argfiles = calloc((size_t)argc + 1, sizeof *argfiles);
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if ((a[0] == '+' && a[1] != 0) || !strcmp(a, "+")) {
            if (nstartup < 32) {
                char *cmd = malloc(strlen(a) + 8);
                if (a[1] == 0) strcpy(cmd, "$");
                else if (isdigit((unsigned char)a[1])) strcpy(cmd, a + 1);
                else if (a[1] == '/') sprintf(cmd, "%s", a + 1);
                else strcpy(cmd, a + 1);
                startup[nstartup++] = cmd;
            }
            continue;
        }
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--version")) { puts("vi (TouhouOS) 1.0"); return 0; }
            if (!strcmp(a, "--help")) { usage(); return 0; }
            if (!strcmp(a, "-c") || !strcmp(a, "-S") || !strcmp(a, "-s")) {
                if (!strcmp(a, "-c") && i + 1 < argc) { if (nstartup < 32) startup[nstartup++] = strdup(argv[++i]); continue; }
                if (!strcmp(a, "-c")) { usage(); return 2; }
                continue;
            }
            if (!strncmp(a, "-c", 2)) { if (nstartup < 32) startup[nstartup++] = strdup(a + 2); continue; }
            for (const char *f = a + 1; *f; f++) {
                switch (*f) {
                case 'R': view_mode = 1; o.ro = 1; break;
                case 'v': case 'e': case 'n': case 'b': case 'l': case 'r': case 'm': case 'N': case 'u': case 'T': break;
                default: usage(); return 2;
                }
            }
            continue;
        }
        argfiles[nargfiles++] = argv[i];
    }
    for (; i < argc; i++) argfiles[nargfiles++] = argv[i];

    for (int k = 0; k < 26; k++) marks[k].l = -1;
    if (pt_term_open(&term) != 0) {
        const char *m = "vi: standard input and output must be a terminal\n";
        ssize_t w = write(2, m, strlen(m));
        (void)w;
        return 1;
    }
    buf_new_empty();
    pt_term_enter(&term, 1);
    scr_init();
    load_first();
    if (nl < 1) buf_new_empty();
    cur.l = 0; cur.c = 0;
    for (int k = 0; k < nstartup; k++) {
        const char *c = startup[k];
        if (isdigit((unsigned char)c[0]) && !c[strspn(c, "0123456789")]) {
            long n = atol(c);
            cur.l = n < 1 ? 0 : imin(nl - 1, n - 1);
            cur.c = first_nonblank(cur.l);
        } else if (c[0] == '/') {
            Pos out;
            char pat[256];
            snprintf(pat, sizeof pat, "%s", c + 1);
            char *end = strrchr(pat, '/');
            if (end && end > pat && end[-1] != '\\') *end = 0;
            Pos from = { 0, -1 };
            snprintf(last_search, sizeof last_search, "%s", pat);
            if (search_find(pat, 1, from, &out, 1, 1) > 0) { cur = out; }
            else set_err("E486: Pattern not found: %s", pat);
        } else ex_execute(c);
        fix_cursor();
    }
    fix_cursor();
    curswant = vcol_end(cur.l, cur.c);
    while (!quit_flag) normal_command();
    pt_term_leave(&term);
    return exit_status;
}
