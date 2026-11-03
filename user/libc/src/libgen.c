#include <libgen.h>
#include <string.h>

char *basename(char *path) {
    if (!path || !*path) return ".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    if (n == 1 && path[0] == '/') return path;
    char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

char *dirname(char *path) {
    if (!path || !*path) return ".";
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    char *s = strrchr(path, '/');
    if (!s) return ".";
    while (s > path && s[-1] == '/') s--;
    if (s == path) { path[1] = 0; return path; }
    *s = 0;
    return path;
}
