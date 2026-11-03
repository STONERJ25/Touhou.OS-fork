/*
 * scp [-346BCpqrv] [-F ssh_config] [-i identity_file] [-o ssh_option] [-P port] source ... target
 *
 * Copies files between this machine and one other, over SFTP (the way OpenSSH's scp has worked since version 9: the old rcp protocol, which runs a remote scp
 * and talks through shell quoting, is not used). A remote operand is [user@]host:path (a relative path starts in the home directory, an empty one is the home
 * directory itself); a local one is anything else. Sources are all local or all remote, and the target is the other kind. Copies from one remote machine to
 * another are not done (they would run through this one, which scp -3 does; this one does not).
 *
 *   -r recursive      -p keep modification times and modes      -q quiet (no progress meter)      -B never ask (BatchMode)      -P port      -i, -o, -F as for ssh
 * A remote name with * or ? in its last part is expanded on the server. Exit status 0, or 1 when something could not be copied (and 255 when the connection failed).
 */
#include <sshc.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static bool g_progress;
static int g_errors;

static void usage(void) {
    fprintf(stderr, "usage: scp [-346BCpqrv] [-F ssh_config] [-i identity_file] [-o ssh_option] [-P port] source ... target\n");
    exit(1);
}

/* Is the operand remote? [user@]host:path, where nothing before the colon is a slash. Splits it. */
static bool split_remote(const char *arg, char *userhost, size_t uhcap, const char **path) {
    if (!strncmp(arg, "scp://", 6)) {
        const char *rest = arg + 6, *slash = strchr(rest, '/');
        size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
        if (n >= uhcap) return false;
        memcpy(userhost, rest, n);
        userhost[n] = 0;
        *path = slash ? slash + 1 : "";
        return true;
    }
    const char *colon = strchr(arg, ':');
    const char *slash = strchr(arg, '/');
    if (!colon || (slash && slash < colon) || colon == arg) return false;
    size_t n = (size_t)(colon - arg);
    if (n >= uhcap) return false;
    memcpy(userhost, arg, n);
    userhost[n] = 0;
    *path = colon + 1;
    return true;
}

/* ------------------------------------------------------------------------------------------------ the progress meter and the error report */

typedef struct { char name[64]; long start, last; } Meter;

static void problem(void *ctx, const char *what, const char *why) {
    (void)ctx;
    if (g_progress) fputc('\n', stderr);
    fprintf(stderr, "scp: %s: %s\n", what, why);
    g_errors++;
}

static void meter_begin(void *ctx, const char *name) {
    Meter *m = ctx;
    sshc_cpy(m->name, sizeof m->name, name);
    m->start = sshc_mono_ms();
    m->last = 0;
}

static void meter_progress(void *ctx, uint64_t done, uint64_t total) {
    Meter *m = ctx;
    if (!g_progress) return;
    long now = sshc_mono_ms();
    if (now - m->last < 400 && done != total) return;
    m->last = now;
    long el = now - m->start;
    double rate = el > 0 ? (double)done * 1000.0 / (double)el : 0;
    unsigned pct = total ? (unsigned)(done * 100 / total) : 100;
    char rs[32];
    if (rate >= 1048576) snprintf(rs, sizeof rs, "%.1fMB/s", rate / 1048576);
    else snprintf(rs, sizeof rs, "%.1fKB/s", rate / 1024);
    fprintf(stderr, "\r%-28.28s %3u%% %8llu %10s %02ld:%02ld ", m->name, pct, (unsigned long long)done, rs, el / 60000, (el / 1000) % 60);
    fflush(stderr);
}

static void meter_end(void *ctx, bool ok) {
    (void)ctx;
    if (g_progress && ok) fputc('\n', stderr);
}

int main(int argc, char **argv) {
    SshcConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *config_file = NULL;
    int verbose = 0;
    bool quiet = false;
    Meter meter;
    memset(&meter, 0, sizeof meter);
    SshcXfer x = { false, false, &meter, problem, meter_begin, meter_progress, meter_end };
    char err[300];
    int i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        for (char *p = a + 1; *p; p++) {
            const char *val = NULL;
            if (strchr("PiFoSl", *p)) {
                val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!val) usage();
            }
            switch (*p) {
            case 'P': sshc_cfg_set(&cfg, "Port", val, err, sizeof err); break;
            case 'i': sshc_cfg_set(&cfg, "IdentityFile", val, err, sizeof err); break;
            case 'F': config_file = val; break;
            case 'o': if (sshc_cfg_set_line(&cfg, val, err, sizeof err) != 0) { fprintf(stderr, "scp: %s\n", err); return 1; } break;
            case 'l': break;
            case 'r': x.recursive = true; break;
            case 'p': x.preserve = true; break;
            case 'q': quiet = true; break;
            case 'v': if (verbose < 3) verbose++; break;
            case 'B': sshc_cfg_set(&cfg, "BatchMode", "yes", err, sizeof err); break;
            case '4': case 'C': case '3': case '6': break;
            default: usage();
            }
            if (val) break;
        }
    }
    if (argc - i < 2) usage();
    int nsrc = argc - i - 1;
    char **src = argv + i;
    const char *target = argv[argc - 1];

    /* which side is remote */
    char uh[300], uh_t[300];
    const char *rpath_t = NULL;
    bool target_remote = split_remote(target, uh_t, sizeof uh_t, &rpath_t);
    bool any_src_remote = false, all_src_remote = true;
    if (nsrc > 64) { fprintf(stderr, "scp: too many files\n"); return 1; }
    const char *spath[64];
    char suh[64][300];
    for (int k = 0; k < nsrc; k++) {
        bool r = split_remote(src[k], suh[k], sizeof suh[k], &spath[k]);
        any_src_remote |= r;
        all_src_remote &= r;
    }
    if (target_remote && any_src_remote) { fprintf(stderr, "scp: copies between two remote hosts are not supported\n"); return 1; }
    if (!target_remote && !any_src_remote) { fprintf(stderr, "scp: one side must be a remote host: [user@]host:path\n"); return 1; }
    if (any_src_remote && !all_src_remote) { fprintf(stderr, "scp: all sources must be remote when one is\n"); return 1; }
    sshc_cpy(uh, sizeof uh, target_remote ? uh_t : suh[0]);
    if (!target_remote) for (int k = 1; k < nsrc; k++) if (strcmp(suh[k], uh) != 0) { fprintf(stderr, "scp: all remote sources must be on the same host\n"); return 1; }

    const char *user = NULL;
    char *host = sshc_split_userhost(uh, &user);
    if (user && sshc_cfg_set(&cfg, "User", user, err, sizeof err) != 0) { fprintf(stderr, "scp: %s\n", err); return 1; }
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (sshc_load_configs(&cfg, config_file, host, err, sizeof err) != 0) { fprintf(stderr, "scp: %s\n", err); return 1; }
    SshcSetup setup;
    if (sshc_login_setup(&setup, &cfg, host, quiet ? -1 : verbose) != 0) { fprintf(stderr, "scp: %s\n", setup.err); return 1; }
    g_progress = !quiet && isatty(STDERR_FILENO) && sshc_verbose >= 0;

    char why[400];
    SshcConn *c = sshc_login(&setup.lg, why, sizeof why);
    if (!c) { fprintf(stderr, "%s\n", why); return 255; }
    SshcSftp *s = sshc_sftp_start(c);
    if (!s) { fprintf(stderr, "scp: %s\n", c->err); sshc_conn_free(c); return 255; }

    if (target_remote) {
        const char *tpath = *rpath_t ? rpath_t : ".";
        bool tdir = sshc_xfer_remote_is_dir(s, tpath) || (strlen(tpath) > 1 && tpath[strlen(tpath) - 1] == '/');
        if (nsrc > 1 && !tdir) problem(NULL, tpath, "Not a directory");
        else for (int k = 0; k < nsrc && !sshc_sftp_dead(s); k++) {
            char dest[1024];
            if (tdir) sshc_join(dest, sizeof dest, tpath, sshc_basename(src[k]));
            else sshc_cpy(dest, sizeof dest, tpath);
            sshc_xfer_upload(s, &x, src[k], dest);
        }
    } else {
        struct stat st;
        bool tdir = stat(target, &st) == 0 && S_ISDIR(st.st_mode);
        int total = 0;
        char (*all)[1024] = NULL;
        for (int k = 0; k < nsrc; k++) {
            char (*one)[1024] = NULL;
            int cnt = 0;
            if (sshc_xfer_glob(s, *spath[k] ? spath[k] : ".", &one, &cnt) != 0) { problem(NULL, spath[k], sshc_sftp_err(s)); continue; }
            if (cnt == 0) { problem(NULL, spath[k], "No such file or directory"); free(one); continue; }
            char (*grown)[1024] = realloc(all, 1024 * (size_t)(total + cnt));
            if (!grown) { free(one); break; }
            all = grown;
            memcpy(all + total, one, 1024 * (size_t)cnt);
            total += cnt;
            free(one);
        }
        if (total > 1 && !tdir) problem(NULL, target, "Not a directory");
        else for (int k = 0; k < total && !sshc_sftp_dead(s); k++) {
            char dest[1024];
            if (tdir) sshc_join(dest, sizeof dest, target, sshc_basename(all[k]));
            else sshc_cpy(dest, sizeof dest, target);
            sshc_xfer_download(s, &x, all[k], dest);
        }
        free(all);
    }
    if (sshc_sftp_dead(s) && g_errors == 0) g_errors++;
    sshc_sftp_end(s);
    sshc_disconnect(c, SSHC_DISC_BY_APPLICATION, "disconnected by user");
    sshc_conn_free(c);
    sshc_cfg_free(&cfg);
    return g_errors ? 1 : 0;
}
