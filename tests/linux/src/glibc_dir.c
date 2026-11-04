/*
 * Directories through a statically linked C library: mkdir, creating files, opendir/readdir/rewinddir/closedir (getdents64), stat of the entries, chdir and getcwd,
 * rename, rmdir of a non-empty directory, and the errors. The names are sorted before they are printed (a directory has no promised order). A real Linux prints
 * the same lines (tools/linux-oracle.py); tests/case_linux.py compares.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static const char *kind(int t) { return t == DT_DIR ? "dir" : t == DT_REG ? "file" : t == DT_LNK ? "link" : "other"; }

static int list(const char *path, int show) {
    DIR *d = opendir(path);
    if (!d) return -1;
    char *names[64];
    int types[64], n = 0, dots = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < 64) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { dots++; continue; }
        names[n] = strdup(e->d_name);
        types[n++] = e->d_type;
    }
    closedir(d);
    if (show) {
        printf("dot entries: %d\n", dots);
        int order[64];
        for (int i = 0; i < n; i++) order[i] = i;
        char *sorted[64];
        for (int i = 0; i < n; i++) sorted[i] = names[i];
        qsort(sorted, n, sizeof sorted[0], cmp);
        for (int i = 0; i < n; i++) {
            int t = 0;
            for (int j = 0; j < n; j++) if (names[j] == sorted[i]) t = types[j];
            printf("entry %s: %s\n", sorted[i], kind(t));
        }
    }
    for (int i = 0; i < n; i++) free(names[i]);
    return n;
}

static void put(const char *path, int bytes) {
    FILE *f = fopen(path, "w");
    for (int i = 0; i < bytes; i++) fputc('x', f);
    fclose(f);
}

int main(void) {
    const char *dir = "/tmp/lnx_dir_test";
    char p[128];
    const char *names[] = { "alpha", "beta", "gamma.txt", "delta" };
    for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); unlink(p); }
    snprintf(p, sizeof p, "%s/sub", dir);
    rmdir(p);
    rmdir(dir);

    printf("mkdir: %d\n", mkdir(dir, 0755));
    errno = 0;
    int r = mkdir(dir, 0755);
    printf("mkdir again: %d, errno %d\n", r, errno);
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); put(p, 10 * (i + 1)); }
    snprintf(p, sizeof p, "%s/sub", dir);
    printf("mkdir sub: %d\n", mkdir(p, 0755));

    printf("entries: %d\n", list(dir, 1));
    printf("again: %d\n", list(dir, 0));

    struct stat st;
    snprintf(p, sizeof p, "%s/gamma.txt", dir);
    r = stat(p, &st);
    printf("stat gamma.txt: %d, size %ld, regular %d\n", r, (long)st.st_size, S_ISREG(st.st_mode) ? 1 : 0);
    snprintf(p, sizeof p, "%s/sub", dir);
    r = stat(p, &st);
    printf("stat sub: %d, directory %d\n", r, S_ISDIR(st.st_mode) ? 1 : 0);

    printf("chdir: %d\n", chdir(dir));
    char cwd[128];
    printf("getcwd: %s\n", getcwd(cwd, sizeof cwd) ? cwd : "(failed)");
    printf("access alpha: %d\n", access("alpha", F_OK));
    printf("rename: %d\n", rename("alpha", "delta"));
    printf("access alpha: %d, delta: %d\n", access("alpha", F_OK), access("delta", F_OK));
    int fd = open("delta", O_RDONLY);
    char b[32];
    ssize_t n = read(fd, b, sizeof b);
    printf("read delta: %zd bytes\n", n);
    close(fd);
    printf("chdir back: %d\n", chdir("/"));

    errno = 0;
    r = rmdir(dir);
    printf("rmdir of a directory with files: %d, errno %d\n", r, errno);
    errno = 0;
    r = (opendir("/tmp/lnx_no_such_dir") == NULL);
    printf("opendir of a missing path: %d, errno %d\n", r, errno);
    errno = 0;
    snprintf(p, sizeof p, "%s/delta", dir);
    r = (opendir(p) == NULL);
    printf("opendir of a file: %d, errno %d\n", r, errno);

    printf("listing after the changes: %d\n", list(dir, 0));
    for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s/%s", dir, names[i]); unlink(p); }
    snprintf(p, sizeof p, "%s/sub", dir);
    printf("rmdir sub: %d\n", rmdir(p));
    printf("rmdir: %d\n", rmdir(dir));
    errno = 0;
    r = stat(dir, &st);
    printf("stat after rmdir: %d, errno %d\n", r, errno);
    return 0;
}
