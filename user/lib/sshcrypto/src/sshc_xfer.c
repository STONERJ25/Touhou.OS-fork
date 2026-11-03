/*
 * Copying files and directory trees over SFTP, the part scp and sftp have in common. A file goes through sshc_sftp_put/get with a progress callback; a directory (with
 * `recursive`) is created on the far side and its entries copied one by one, sorted by name so that the result does not depend on the order the file system lists.
 * Nothing here exits or prints: every failure goes to ui->problem and is counted, and the copy goes on with the next file, which is what people expect of a copy.
 */
#include <sshc.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>

const char *sshc_basename(const char *path) {
    static char buf[1024];
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;                  /* "dir/" is "dir" */
    size_t start = n;
    while (start > 0 && path[start - 1] != '/') start--;
    size_t len = n - start;
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, path + start, len);
    buf[len] = 0;
    return buf[0] ? buf : path;
}

void sshc_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t dl = strlen(dir);
    if (dl && dir[dl - 1] == '/') snprintf(out, cap, "%s%s", dir, name);
    else if (dl) snprintf(out, cap, "%s/%s", dir, name);
    else snprintf(out, cap, "%s", name);
}

bool sshc_xfer_remote_is_dir(SshcSftp *s, const char *path) {
    SshcAttrs a;
    return sshc_sftp_stat(s, path, &a, true) == 0 && (a.flags & SSHC_ATTR_PERM) && (a.perm & 0170000) == 0040000;
}

static void problem(const SshcXfer *x, const char *what, const char *why) {
    if (x->problem) x->problem(x->ctx, what, why);
}

/* ------------------------------------------------------------------------------------------------ upload */

int sshc_xfer_upload(SshcSftp *s, const SshcXfer *x, const char *local, const char *remote) {
    int errors = 0;
    struct stat st;
    if (stat(local, &st) != 0) { problem(x, local, strerror(errno)); return 1; }
    if (S_ISDIR(st.st_mode)) {
        if (!x->recursive) { problem(x, local, "not a regular file"); return 1; }
        if (!sshc_xfer_remote_is_dir(s, remote) && sshc_sftp_mkdir(s, remote, st.st_mode & 0777) != 0) { problem(x, remote, sshc_sftp_err(s)); return 1; }
        DIR *d = opendir(local);
        if (!d) { problem(x, local, strerror(errno)); return 1; }
        char (*names)[256] = NULL;
        int n = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (n == cap) {
                int nc = cap ? cap * 2 : 32;
                char (*nn)[256] = realloc(names, (size_t)nc * 256);
                if (!nn) break;
                names = nn;
                cap = nc;
            }
            sshc_cpy(names[n++], 256, e->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {                           /* insertion sort: directories are small and qsort's comparator would need a wrapper */
            for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
                char t[256];
                memcpy(t, names[j], 256); memcpy(names[j], names[j - 1], 256); memcpy(names[j - 1], t, 256);
            }
        }
        for (int i = 0; i < n && !sshc_sftp_dead(s); i++) {
            char l[1024], r[1024];
            sshc_join(l, sizeof l, local, names[i]);
            sshc_join(r, sizeof r, remote, names[i]);
            errors += sshc_xfer_upload(s, x, l, r);
        }
        free(names);
    } else if (S_ISREG(st.st_mode)) {
        int fd = open(local, O_RDONLY);
        if (fd < 0) { problem(x, local, strerror(errno)); return 1; }
        if (x->begin) x->begin(x->ctx, sshc_basename(local));
        int rc = sshc_sftp_put(s, fd, remote, st.st_mode & 0777, (uint64_t)st.st_size, x->progress, x->ctx);
        close(fd);
        if (x->end) x->end(x->ctx, rc == 0);
        if (rc != 0) { problem(x, remote, sshc_sftp_err(s)); return 1; }
    } else {
        problem(x, local, "not a regular file");
        return 1;
    }
    if (x->preserve) {
        SshcAttrs a;
        memset(&a, 0, sizeof a);
        a.flags = SSHC_ATTR_PERM | SSHC_ATTR_TIME;
        a.perm = st.st_mode & 0777;
        a.atime = (uint32_t)st.st_atime;
        a.mtime = (uint32_t)st.st_mtime;
        if (sshc_sftp_setstat(s, remote, &a) != 0) { problem(x, remote, sshc_sftp_err(s)); errors++; }
    }
    return errors;
}

/* ------------------------------------------------------------------------------------------------ download */

typedef struct { char (*names)[256]; int n, cap; } NameList;

static int collect(const SshcDirEnt *e, void *ctx) {
    NameList *l = ctx;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        char (*nn)[256] = realloc(l->names, (size_t)nc * 256);
        if (!nn) return -1;
        l->names = nn;
        l->cap = nc;
    }
    sshc_cpy(l->names[l->n++], 256, e->name);
    return 0;
}

static int by_name(const void *a, const void *b) { return strcmp(a, b); }

int sshc_xfer_download(SshcSftp *s, const SshcXfer *x, const char *remote, const char *local) {
    int errors = 0;
    SshcAttrs a;
    if (sshc_sftp_stat(s, remote, &a, true) != 0) { problem(x, remote, sshc_sftp_err(s)); return 1; }
    bool dir = (a.flags & SSHC_ATTR_PERM) && (a.perm & 0170000) == 0040000;
    if (dir) {
        if (!x->recursive) { problem(x, remote, "not a regular file"); return 1; }
        struct stat st;
        if (stat(local, &st) != 0) {
            if (mkdir(local, (a.flags & SSHC_ATTR_PERM) ? (mode_t)((a.perm & 0777) | 0700) : 0755) != 0) { problem(x, local, strerror(errno)); return 1; }
        } else if (!S_ISDIR(st.st_mode)) {
            problem(x, local, "Not a directory");
            return 1;
        }
        NameList l = { NULL, 0, 0 };
        if (sshc_sftp_readdir(s, remote, collect, &l) != 0) { problem(x, remote, sshc_sftp_err(s)); free(l.names); return 1; }
        qsort(l.names, (size_t)l.n, 256, by_name);
        for (int i = 0; i < l.n && !sshc_sftp_dead(s); i++) {
            char r[1024], lo[1024];
            sshc_join(r, sizeof r, remote, l.names[i]);
            sshc_join(lo, sizeof lo, local, l.names[i]);
            errors += sshc_xfer_download(s, x, r, lo);
        }
        free(l.names);
    } else {
        mode_t mode = (a.flags & SSHC_ATTR_PERM) ? (mode_t)(a.perm & 0777) : 0644;
        int fd = open(local, O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (fd < 0) { problem(x, local, strerror(errno)); return 1; }
        if (x->begin) x->begin(x->ctx, sshc_basename(remote));
        int rc = sshc_sftp_get(s, remote, fd, (a.flags & SSHC_ATTR_SIZE) ? a.size : 0, x->progress, x->ctx);
        close(fd);
        if (x->end) x->end(x->ctx, rc == 0);
        if (rc != 0) { problem(x, remote, sshc_sftp_err(s)); unlink(local); return 1; }
    }
    if (x->preserve && (a.flags & SSHC_ATTR_TIME)) {
        struct utimbuf t = { (time_t)a.atime, (time_t)a.mtime };
        utime(local, &t);
        if (a.flags & SSHC_ATTR_PERM) chmod(local, a.perm & 0777);
    }
    return errors;
}

/* ------------------------------------------------------------------------------------------------ globbing on the server */

int sshc_xfer_glob(SshcSftp *s, const char *path, char (**out)[1024], int *count) {
    const char *slash = strrchr(path, '/');
    const char *pat = slash ? slash + 1 : path;
    if (!strpbrk(pat, "*?")) {
        *out = malloc(1024);
        if (!*out) return -1;
        sshc_cpy((*out)[0], 1024, path);
        *count = 1;
        return 0;
    }
    char dir[1024];
    if (slash) {
        size_t dl = (size_t)(slash - path);
        if (dl == 0) dl = 1;
        if (dl >= sizeof dir) return -1;
        memcpy(dir, path, dl);
        dir[dl] = 0;
    } else {
        sshc_cpy(dir, sizeof dir, ".");
    }
    NameList l = { NULL, 0, 0 };
    if (sshc_sftp_readdir(s, dir, collect, &l) != 0) { free(l.names); return -1; }
    qsort(l.names, (size_t)l.n, 256, by_name);
    char (*res)[1024] = malloc(1024 * (size_t)(l.n ? l.n : 1));
    int n = 0;
    for (int i = 0; res && i < l.n; i++) {
        if (l.names[i][0] == '.' && pat[0] != '.') continue;                   /* a leading dot is matched only by a dot */
        if (!sshc_pattern_match(pat, l.names[i])) continue;
        if (slash) sshc_join(res[n++], 1024, dir, l.names[i]);
        else sshc_cpy(res[n++], 1024, l.names[i]);
    }
    free(l.names);
    *out = res;
    *count = n;
    return res ? 0 : -1;
}
