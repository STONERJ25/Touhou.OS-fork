/*
 * What a Linux program sees of the file system when it runs in the Linux root of TouhouOS (docs/COMPAT.md, "The Linux root"): its own tree is "/", the machine's /proc, /dev and
 * /tmp are in it, ".." does not lead out of it, an absolute symlink is read inside it, and the native directories (/HumanVillage, /Hakurei ...) do not exist. A real Linux has no
 * such root, so this program has no stored output: tests/case_linux_dyn.py compares what it prints with the lines the rules of the root promise. Linked statically, so that it
 * runs from /tmp before and without any library of the root.
 *
 * Run from /tmp as:  root_paths      (it leaves /tmp/rp_marker behind, which the test looks for with a native program)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static void same(const char *a, const char *b) {
    struct stat x, y;
    int ra = stat(a, &x), rb = stat(b, &y);
    if (ra || rb) { printf("%s and %s: %s\n", a, b, strerror(errno)); return; }
    printf("%s is %s %s\n", a, x.st_ino == y.st_ino && x.st_dev == y.st_dev ? "the same directory as" : "a different directory from", b);
}

static void absent(const char *path) {
    struct stat st;
    printf("%s: %s\n", path, stat(path, &st) ? strerror(errno) : "exists");
}

static void first_line(const char *path, char *out, size_t n) {
    out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(out, n, "(%s)", strerror(errno)); return; }
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r < 0) r = 0;
    out[r] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void) {
    char buf[512], line[256];
    printf("--- where '..' leads\n");
    same("/..", "/");
    same("/tmp/..", "/");
    same("/dev/..", "/");
    same("/proc/..", "/");
    same("/etc/../..", "/");
    same("/usr/bin/../..", "/");
    same("/tmp/../etc", "/etc");

    printf("--- what exists\n");
    absent("/etc/os-release");
    absent("/usr/bin/ls");
    absent("/lib64/ld-linux-x86-64.so.2");
    absent("/HumanVillage");
    absent("/Hakurei");
    absent("/ScarletDevilMansion");
    absent("/BambooForest");
    absent("/linux");
    absent("/dev/null");
    absent("/proc/self");
    DIR *d = opendir("/");
    char *names[64];
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64) if (e->d_name[0] != '.') names[n++] = strdup(e->d_name);
    if (d) closedir(d);
    qsort(names, n, sizeof names[0], cmp);
    printf("the top directory holds:");
    for (int i = 0; i < n; i++) printf(" %s", names[i]);
    printf("\n");
    ssize_t l = readlink("/bin", buf, sizeof buf - 1);
    printf("/bin is a symbolic link to: %.*s\n", (int)(l < 0 ? 0 : l), buf);

    printf("--- the working directory\n");
    printf("getcwd at the start: %s\n", getcwd(buf, sizeof buf) ? buf : strerror(errno));
    l = readlink("/proc/self/cwd", buf, sizeof buf - 1);
    printf("/proc/self/cwd: %.*s\n", (int)(l < 0 ? 0 : l), buf);
    chdir("/etc");
    printf("after chdir(/etc): %s\n", getcwd(buf, sizeof buf) ? buf : strerror(errno));
    chdir("..");
    printf("after chdir(..): %s\n", getcwd(buf, sizeof buf) ? buf : strerror(errno));
    chdir("../../..");
    printf("after chdir(../../..): %s\n", getcwd(buf, sizeof buf) ? buf : strerror(errno));
    chdir("/tmp");
    first_line("../etc/os-release", line, sizeof line);
    printf("from /tmp, ../etc/os-release starts with: %.12s\n", line);
    chdir("/usr/lib/x86_64-linux-gnu");
    printf("a relative path in the root: %s\n", access("libc.so.6", R_OK) == 0 ? "libc.so.6 is here" : strerror(errno));
    chdir("/tmp");

    printf("--- symbolic links are read inside the root\n");
    unlink("/tmp/rp_link");
    if (symlink("/etc/passwd", "/tmp/rp_link")) printf("symlink: %s\n", strerror(errno));
    first_line("/tmp/rp_link", line, sizeof line);
    printf("/tmp/rp_link -> /etc/passwd reads: %.31s\n", line);
    int fd = open("/tmp/rp_link", O_RDONLY);
    ssize_t r = fd >= 0 ? read(fd, buf, sizeof buf - 1) : 0;
    if (r < 0) r = 0;
    buf[r] = 0;
    printf("it is the root's passwd file (Reimu Hakurei is in it): %s\n", strstr(buf, "Reimu Hakurei") ? "yes" : "no");
    if (fd >= 0) close(fd);
    unlink("/tmp/rp_link");

    printf("--- the machine's own directories\n");
    fd = open("/tmp/rp_marker", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    printf("created /tmp/rp_marker: %s\n", fd >= 0 ? "yes" : strerror(errno));
    if (fd >= 0) { dprintf(fd, "made by a Linux program\n"); close(fd); }
    fd = open("/dev/zero", O_RDONLY);
    r = fd >= 0 ? read(fd, buf, 4) : -1;
    printf("/dev/zero gives %zd bytes\n", r);
    if (fd >= 0) close(fd);
    l = readlink("/proc/self/exe", buf, sizeof buf - 1);
    printf("/proc/self/exe: %.*s\n", (int)(l < 0 ? 0 : l), buf);
    first_line("/proc/uptime", line, sizeof line);
    printf("/proc/uptime can be read: %s\n", line[0] >= '0' && line[0] <= '9' ? "yes" : "no");
    struct utsname u;
    uname(&u);
    printf("uname: %s %s\n", u.sysname, u.machine);
    return 0;
}
