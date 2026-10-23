/* The Netherworld: where deleted things go. Deleting renames (or copies, across volumes) into /Netherworld under a name that is
 * free there (NAME, NAME.1, NAME.2, ... like the `trash` command) and writes a small sidecar file
 * /Netherworld/.trashinfo/<name>.info with the original absolute path, so Restore knows where the thing came from.
 * Items put there by the shell's `trash` command have no sidecar; they are restored to the home folder. */
#include "explorer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool trash_ensure(void) {
    struct stat st;
    if (stat(NETHERWORLD, &st) != 0) {
        if (mkdir(NETHERWORLD, 0777) != 0 && errno != EEXIST) return false;
        chmod(NETHERWORLD, 01777);
    }
    if (stat(TRASHINFO, &st) != 0) {
        if (mkdir(TRASHINFO, 0777) != 0 && errno != EEXIST) return false;
        chmod(TRASHINFO, 01777);
    }
    return true;
}

void trash_unique(const char *base, char *out, size_t cap) {
    path_join(out, cap, NETHERWORLD, base);
    for (int i = 1; path_exists(out) && i < 100000; i++) {
        char n[EX_NAME + 16];
        snprintf(n, sizeof n, "%s.%d", base, i);
        path_join(out, cap, NETHERWORLD, n);
    }
}

static void info_path(const char *trashed_name, char *out, size_t cap) {
    char n[EX_NAME + 8];
    snprintf(n, sizeof n, "%s.info", trashed_name);
    path_join(out, cap, TRASHINFO, n);
}

bool trash_write_info(const char *trashed_name, const char *orig_path) {
    if (strchr(orig_path, '\n') || !trash_ensure()) return false;
    char p[EX_PATH * 2];
    info_path(trashed_name, p, sizeof p);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    char buf[EX_PATH + 64];
    int n = snprintf(buf, sizeof buf, "path=%s\ntime=%lld\n", orig_path, (long long)time(NULL));
    bool ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    if (!ok) unlink(p);
    return ok;
}

bool trash_read_info(const char *trashed_name, char *orig, size_t cap, int64_t *when) {
    char p[EX_PATH * 2];
    info_path(trashed_name, p, sizeof p);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return false;
    char buf[EX_PATH + 128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    bool got = false;
    if (when) *when = 0;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!strncmp(line, "path=", 5) && line[5] == '/') { strlcpy(orig, line + 5, cap); got = true; }
        else if (!strncmp(line, "time=", 5) && when) *when = strtoll(line + 5, NULL, 10);
        line = nl ? nl + 1 : NULL;
    }
    return got;
}

void trash_remove_info(const char *trashed_name) {
    char p[EX_PATH * 2];
    info_path(trashed_name, p, sizeof p);
    unlink(p);
}

int trash_count(void) {
    DIR *dp = opendir(NETHERWORLD);
    if (!dp) return 0;
    int n = 0;
    struct dirent *d;
    while ((d = readdir(dp))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..") || !strcmp(d->d_name, ".trashinfo")) continue;
        n++;
    }
    closedir(dp);
    return n;
}
