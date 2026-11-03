/*
 * sftp [-Bqv] [-b batchfile] [-F ssh_config] [-i identity_file] [-o ssh_option] [-P port] [user@]host[:path]
 *
 * An interactive file transfer session over SFTP. Commands (a word may be quoted with double quotes):
 *   ls [-l] [-a] [path]      pwd           cd [path]          get [-r] [-p] remote [local]       mkdir path        rm path          rename old new
 *   lls [args]               lpwd          lcd [path]         put [-r] [-p] local [remote]       rmdir path        chmod mode path  ln [-s] target path
 *   !command (runs a local shell command)                      help        bye / exit / quit
 * With -b the commands are read from a file (or "-" for standard input) without prompts and the first one that fails ends the run with status 1, unless its line
 * starts with "-". Remote relative paths are relative to the directory `cd` last went to.
 */
#include <sshc.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static SshcSftp *g_s;
static char g_cwd[1024] = "/";
static int g_errors;
static bool g_progress;
static bool g_batch;

static void usage(void) {
    fprintf(stderr, "usage: sftp [-Bqv] [-b batchfile] [-F ssh_config] [-i identity_file] [-o ssh_option] [-P port] [user@]host[:path]\n");
    exit(1);
}

/* ------------------------------------------------------------------------------------------------ paths */

/* An absolute remote path for what the user typed: relative names are taken from the current remote directory. */
static void rpath(const char *p, char *out, size_t cap) {
    if (p[0] == '/') sshc_cpy(out, cap, p);
    else if (p[0] == '~' && (p[1] == 0 || p[1] == '/')) snprintf(out, cap, "./%s", p[1] ? p + 2 : "");     /* handled by the server: relative to home */
    else sshc_join(out, cap, g_cwd, p);
}

/* ------------------------------------------------------------------------------------------------ progress and errors */

typedef struct { char name[64]; long start, last; } Meter;
static Meter g_meter;

static void problem(void *ctx, const char *what, const char *why) {
    (void)ctx;
    if (g_progress) fputc('\n', stdout);
    printf("%s: %s\n", what, why);
    g_errors++;
}

static void meter_begin(void *ctx, const char *name) {
    (void)ctx;
    sshc_cpy(g_meter.name, sizeof g_meter.name, name);
    g_meter.start = sshc_mono_ms();
    g_meter.last = 0;
    if (!g_progress) printf("%s\n", name);
}

static void meter_progress(void *ctx, uint64_t done, uint64_t total) {
    (void)ctx;
    if (!g_progress) return;
    long now = sshc_mono_ms();
    if (now - g_meter.last < 400 && done != total) return;
    g_meter.last = now;
    long el = now - g_meter.start;
    double rate = el > 0 ? (double)done * 1000.0 / (double)el : 0;
    unsigned pct = total ? (unsigned)(done * 100 / total) : 100;
    printf("\r%-28.28s %3u%% %8llu %9.1fKB/s ", g_meter.name, pct, (unsigned long long)done, rate / 1024);
    fflush(stdout);
}

static void meter_end(void *ctx, bool ok) {
    (void)ctx;
    if (g_progress && ok) fputc('\n', stdout);
}

static SshcXfer g_x = { false, false, NULL, problem, meter_begin, meter_progress, meter_end };

/* ------------------------------------------------------------------------------------------------ listing */

typedef struct { SshcDirEnt *e; int n, cap; } Listing;

static int add_entry(const SshcDirEnt *e, void *ctx) {
    Listing *l = ctx;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64;
        SshcDirEnt *ne = realloc(l->e, (size_t)nc * sizeof *ne);
        if (!ne) return -1;
        l->e = ne;
        l->cap = nc;
    }
    l->e[l->n++] = *e;
    return 0;
}

static int by_entry_name(const void *a, const void *b) { return strcmp(((const SshcDirEnt *)a)->name, ((const SshcDirEnt *)b)->name); }

static void long_format(const SshcDirEnt *e, char *out, size_t cap) {
    if (e->longname[0]) { sshc_cpy(out, cap, e->longname); return; }
    char mode[11] = "----------";
    uint32_t p = e->a.perm;
    if ((p & 0170000) == 0040000) mode[0] = 'd';
    else if ((p & 0170000) == 0120000) mode[0] = 'l';
    for (int i = 0; i < 9; i++) if (p & (1u << (8 - i))) mode[1 + i] = "rwxrwxrwx"[i];
    char when[32] = "";
    time_t t = (time_t)e->a.mtime;
    struct tm tmv;
    if (gmtime_r(&t, &tmv)) strftime(when, sizeof when, "%b %e %H:%M", &tmv);
    snprintf(out, cap, "%s %4u %-8u %-8u %8llu %s %s", mode, 1u, (unsigned)e->a.uid, (unsigned)e->a.gid, (unsigned long long)e->a.size, when, e->name);
}

static void cmd_ls(int argc, char **argv) {
    bool lng = false, all = false;
    int k = 1;
    for (; k < argc && argv[k][0] == '-' && argv[k][1]; k++)
        for (char *p = argv[k] + 1; *p; p++) { if (*p == 'l') lng = true; else if (*p == 'a') all = true; }
    char path[1024];
    rpath(k < argc ? argv[k] : ".", path, sizeof path);
    Listing l = { NULL, 0, 0 };
    SshcAttrs a;
    if (sshc_sftp_stat(g_s, path, &a, true) == 0 && !((a.flags & SSHC_ATTR_PERM) && (a.perm & 0170000) == 0040000)) {         /* a single file */
        SshcDirEnt e;
        memset(&e, 0, sizeof e);
        sshc_cpy(e.name, sizeof e.name, sshc_basename(path));
        e.a = a;
        char line[700];
        long_format(&e, line, sizeof line);
        puts(lng ? line : e.name);
        return;
    }
    if (sshc_sftp_readdir(g_s, path, add_entry, &l) != 0) { printf("Can't ls: \"%s\" not found\n", path); g_errors++; free(l.e); return; }
    qsort(l.e, (size_t)l.n, sizeof *l.e, by_entry_name);
    int shown = 0, width = 0;
    for (int i = 0; i < l.n; i++) {
        if (!all && l.e[i].name[0] == '.') continue;
        if ((int)strlen(l.e[i].name) > width) width = (int)strlen(l.e[i].name);
    }
    int cols = lng ? 1 : (80 / (width + 2) > 0 ? 80 / (width + 2) : 1);
    for (int i = 0; i < l.n; i++) {
        if (!all && l.e[i].name[0] == '.') continue;
        if (lng) {
            char line[700];
            long_format(&l.e[i], line, sizeof line);
            puts(line);
        } else {
            printf("%-*s", width + 2, l.e[i].name);
            if (++shown % cols == 0) putchar('\n');
        }
    }
    if (!lng && shown % cols) putchar('\n');
    free(l.e);
}

/* ------------------------------------------------------------------------------------------------ commands */

static void fail_remote(const char *what) {
    printf("%s: %s\n", what, sshc_sftp_err(g_s));
    g_errors++;
}

static void cmd_get(int argc, char **argv) {
    SshcXfer x = g_x;
    int k = 1;
    for (; k < argc && argv[k][0] == '-' && argv[k][1]; k++)
        for (char *p = argv[k] + 1; *p; p++) { if (*p == 'r' || *p == 'R') x.recursive = true; else if (*p == 'p' || *p == 'P') x.preserve = true; }
    if (k >= argc) { puts("usage: get [-rp] remote [local]"); g_errors++; return; }
    char (*list)[1024] = NULL;
    int count = 0;
    char path[1024];
    rpath(argv[k], path, sizeof path);
    if (sshc_xfer_glob(g_s, path, &list, &count) != 0 || count == 0) { printf("File \"%s\" not found.\n", path); g_errors++; free(list); return; }
    struct stat st;
    const char *dest = k + 1 < argc ? argv[k + 1] : NULL;
    bool ddir = dest && stat(dest, &st) == 0 && S_ISDIR(st.st_mode);
    if (count > 1 && dest && !ddir) { printf("Multiple files: \"%s\" is not a directory\n", dest); g_errors++; free(list); return; }
    for (int i = 0; i < count && !sshc_sftp_dead(g_s); i++) {
        char local[1024];
        if (!dest) sshc_cpy(local, sizeof local, sshc_basename(list[i]));
        else if (ddir) sshc_join(local, sizeof local, dest, sshc_basename(list[i]));
        else sshc_cpy(local, sizeof local, dest);
        if (!g_progress) printf("Fetching %s to %s\n", list[i], local);
        sshc_xfer_download(g_s, &x, list[i], local);
    }
    free(list);
}

static void cmd_put(int argc, char **argv) {
    SshcXfer x = g_x;
    int k = 1;
    for (; k < argc && argv[k][0] == '-' && argv[k][1]; k++)
        for (char *p = argv[k] + 1; *p; p++) { if (*p == 'r' || *p == 'R') x.recursive = true; else if (*p == 'p' || *p == 'P') x.preserve = true; }
    if (k >= argc) { puts("usage: put [-rp] local [remote]"); g_errors++; return; }
    char dest[1024];
    if (k + 1 < argc) rpath(argv[k + 1], dest, sizeof dest);
    else sshc_cpy(dest, sizeof dest, g_cwd);
    bool ddir = k + 1 >= argc || sshc_xfer_remote_is_dir(g_s, dest);
    char target[1024];
    if (ddir) sshc_join(target, sizeof target, dest, sshc_basename(argv[k]));
    else sshc_cpy(target, sizeof target, dest);
    if (!g_progress) printf("Uploading %s to %s\n", argv[k], target);
    sshc_xfer_upload(g_s, &x, argv[k], target);
}

static int parse_mode(const char *s, uint32_t *mode) {
    char *end;
    unsigned long v = strtoul(s, &end, 8);
    if (end == s || *end || v > 07777) return -1;
    *mode = (uint32_t)v;
    return 0;
}

static void help(void) {
    puts("Available commands:\n"
         "bye                                Quit sftp\n"
         "cd path                            Change remote directory to 'path'\n"
         "chmod mode path                    Change permissions of file 'path' to 'mode'\n"
         "exit                               Quit sftp\n"
         "get [-rp] remote [local]           Download file\n"
         "help                               Display this help text\n"
         "lcd path                           Change local directory to 'path'\n"
         "lls [ls-options [path]]            Display local directory listing\n"
         "lpwd                               Print local working directory\n"
         "ls [-la] [path]                    Display remote directory listing\n"
         "mkdir path                         Create remote directory\n"
         "put [-rp] local [remote]           Upload file\n"
         "pwd                                Display remote working directory\n"
         "quit                               Quit sftp\n"
         "rename oldpath newpath             Rename remote file\n"
         "rm path                            Delete remote file\n"
         "rmdir path                         Remove remote directory\n"
         "ln [-s] oldpath newpath            Link remote file (-s for symlink)\n"
         "!command                           Execute 'command' in local shell");
}

/* Runs one command line. Returns -1 to quit. */
static int run_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    if (!*line || *line == '#') return 0;
    if (*line == '!') {
        if (line[1]) { int rc = system(line + 1); (void)rc; }
        return 0;
    }
    char buf[2048], *argv[16];
    int argc = sshc_words(line, buf, sizeof buf, argv, 16);
    if (argc == 0) return 0;
    const char *cmd = argv[0];
    char p1[1024], p2[1024];
    if (!strcmp(cmd, "bye") || !strcmp(cmd, "exit") || !strcmp(cmd, "quit")) return -1;
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { help(); return 0; }
    if (!strcmp(cmd, "pwd")) { printf("Remote working directory: %s\n", g_cwd); return 0; }
    if (!strcmp(cmd, "lpwd")) { char d[1024]; if (getcwd(d, sizeof d)) printf("Local working directory: %s\n", d); return 0; }
    if (!strcmp(cmd, "lcd")) {
        const char *to = argc > 1 ? argv[1] : sshc_home();
        if (chdir(to) != 0) { printf("local chdir to %s failed: %s\n", to, strerror(errno)); g_errors++; }
        return 0;
    }
    if (!strcmp(cmd, "lls")) {
        char sh[1100] = "ls";
        for (int i = 1; i < argc; i++) { strncat(sh, " ", sizeof sh - strlen(sh) - 1); strncat(sh, argv[i], sizeof sh - strlen(sh) - 1); }
        int rc = system(sh);
        (void)rc;
        return 0;
    }
    if (!strcmp(cmd, "cd")) {
        rpath(argc > 1 ? argv[1] : ".", p1, sizeof p1);
        char real[1024];
        if (sshc_sftp_realpath(g_s, p1, real, sizeof real) != 0) { fail_remote(p1); return 0; }
        if (!sshc_xfer_remote_is_dir(g_s, real)) { printf("Can't change directory: Can't check target\n"); g_errors++; return 0; }
        sshc_cpy(g_cwd, sizeof g_cwd, real);
        return 0;
    }
    if (!strcmp(cmd, "ls") || !strcmp(cmd, "dir")) { cmd_ls(argc, argv); return 0; }
    if (!strcmp(cmd, "get")) { cmd_get(argc, argv); return 0; }
    if (!strcmp(cmd, "put")) { cmd_put(argc, argv); return 0; }
    if (!strcmp(cmd, "mkdir") && argc == 2) { rpath(argv[1], p1, sizeof p1); if (sshc_sftp_mkdir(g_s, p1, 0755) != 0) fail_remote(p1); return 0; }
    if (!strcmp(cmd, "rmdir") && argc == 2) { rpath(argv[1], p1, sizeof p1); if (sshc_sftp_rmdir(g_s, p1) != 0) fail_remote(p1); return 0; }
    if (!strcmp(cmd, "rm") && argc == 2) {
        rpath(argv[1], p1, sizeof p1);
        if (!g_progress) printf("Removing %s\n", p1);
        if (sshc_sftp_remove(g_s, p1) != 0) fail_remote(p1);
        return 0;
    }
    if ((!strcmp(cmd, "rename") || !strcmp(cmd, "mv")) && argc == 3) {
        rpath(argv[1], p1, sizeof p1);
        rpath(argv[2], p2, sizeof p2);
        if (sshc_sftp_rename(g_s, p1, p2) != 0) fail_remote(p1);
        return 0;
    }
    if (!strcmp(cmd, "chmod") && argc == 3) {
        uint32_t mode;
        if (parse_mode(argv[1], &mode) != 0) { printf("Invalid mode.\n"); g_errors++; return 0; }
        rpath(argv[2], p1, sizeof p1);
        SshcAttrs a;
        memset(&a, 0, sizeof a);
        a.flags = SSHC_ATTR_PERM;
        a.perm = mode;
        if (!g_progress) printf("Changing mode on %s\n", p1);
        if (sshc_sftp_setstat(g_s, p1, &a) != 0) fail_remote(p1);
        return 0;
    }
    if (!strcmp(cmd, "ln") || !strcmp(cmd, "symlink")) {
        int k = 1;
        bool sym = !strcmp(cmd, "symlink");
        if (argc > 1 && !strcmp(argv[1], "-s")) { sym = true; k = 2; }
        if (argc - k != 2) { puts("usage: ln [-s] oldpath newpath"); g_errors++; return 0; }
        if (!sym) { puts("Hard links are not supported by SFTP version 3: use ln -s"); g_errors++; return 0; }
        rpath(argv[k + 1], p2, sizeof p2);
        if (sshc_sftp_symlink(g_s, argv[k], p2) != 0) fail_remote(p2);
        return 0;
    }
    printf("Invalid command.\n");
    g_errors++;
    return 0;
}

int main(int argc, char **argv) {
    SshcConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *config_file = NULL, *batch_file = NULL;
    int verbose = 0;
    bool quiet = false;
    char err[300];
    int i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        for (char *p = a + 1; *p; p++) {
            const char *val = NULL;
            if (strchr("PiFob", *p)) {
                val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!val) usage();
            }
            switch (*p) {
            case 'P': sshc_cfg_set(&cfg, "Port", val, err, sizeof err); break;
            case 'i': sshc_cfg_set(&cfg, "IdentityFile", val, err, sizeof err); break;
            case 'F': config_file = val; break;
            case 'o': if (sshc_cfg_set_line(&cfg, val, err, sizeof err) != 0) { fprintf(stderr, "sftp: %s\n", err); return 1; } break;
            case 'b': batch_file = val; sshc_cfg_set(&cfg, "BatchMode", "yes", err, sizeof err); break;
            case 'B': sshc_cfg_set(&cfg, "BatchMode", "yes", err, sizeof err); break;
            case 'q': quiet = true; break;
            case 'v': if (verbose < 3) verbose++; break;
            case '4': case 'C': case '6': break;
            default: usage();
            }
            if (val) break;
        }
    }
    if (i >= argc || i + 1 < argc) usage();

    char uh[300];
    sshc_cpy(uh, sizeof uh, argv[i]);
    char *colon = strchr(uh, ':');
    const char *start_path = NULL;
    if (colon) { *colon = 0; start_path = colon + 1; }
    const char *user = NULL;
    char *host = sshc_split_userhost(uh, &user);
    if (user && sshc_cfg_set(&cfg, "User", user, err, sizeof err) != 0) { fprintf(stderr, "sftp: %s\n", err); return 1; }
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (sshc_load_configs(&cfg, config_file, host, err, sizeof err) != 0) { fprintf(stderr, "sftp: %s\n", err); return 1; }
    SshcSetup setup;
    if (sshc_login_setup(&setup, &cfg, host, quiet ? -1 : verbose) != 0) { fprintf(stderr, "sftp: %s\n", setup.err); return 1; }
    g_batch = batch_file != NULL;
    g_progress = !quiet && !g_batch && isatty(STDOUT_FILENO);

    char why[400];
    SshcConn *c = sshc_login(&setup.lg, why, sizeof why);
    if (!c) { fprintf(stderr, "%s\n", why); return 255; }
    g_s = sshc_sftp_start(c);
    if (!g_s) { fprintf(stderr, "sftp: %s\n", c->err); sshc_conn_free(c); return 255; }
    if (sshc_sftp_realpath(g_s, ".", g_cwd, sizeof g_cwd) != 0) sshc_cpy(g_cwd, sizeof g_cwd, "/");
    if (!quiet && !g_batch) printf("Connected to %s.\n", host);
    if (start_path && *start_path) {
        char line[1100];
        snprintf(line, sizeof line, "cd \"%s\"", start_path);
        run_line(line);
    }

    FILE *in = stdin;
    if (batch_file && strcmp(batch_file, "-")) {
        in = fopen(batch_file, "r");
        if (!in) { fprintf(stderr, "sftp: %s: %s\n", batch_file, strerror(errno)); return 1; }
    }
    bool interactive = !g_batch && isatty(STDIN_FILENO);
    char line[2048];
    for (;;) {
        if (interactive) { fputs("sftp> ", stdout); fflush(stdout); }
        if (!fgets(line, sizeof line, in)) break;
        bool ignore_error = false;
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (g_batch && *l == '-') { ignore_error = true; l++; }
        int before = g_errors;
        if (run_line(l) < 0) break;
        if (g_batch && g_errors > before) {
            if (ignore_error) g_errors = before;
            else break;
        }
        fflush(stdout);
        if (sshc_sftp_dead(g_s)) break;
    }
    int rc = (g_errors && g_batch) ? 1 : 0;
    if (sshc_sftp_dead(g_s)) { fprintf(stderr, "Connection lost.\n"); rc = 1; }
    sshc_sftp_end(g_s);
    sshc_disconnect(c, SSHC_DISC_BY_APPLICATION, "disconnected by user");
    sshc_conn_free(c);
    sshc_cfg_free(&cfg);
    return rc;
}
