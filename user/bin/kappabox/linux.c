/* linux, native, linuxroot: running programs built for Linux (docs/COMPAT.md). The kernel answers such a program's system calls with the Linux personality, and gives it the
 * tree under /linux as its root when one is mounted there. */
#include "kappabox.h"
#include <gensokyo.h>

#define LINUX_ROOT "/linux"

/* linux [-r] PROGRAM [ARGUMENT...]   (PROGRAM is a path: no search through the path variable, as with a program started by ./)
 * With a Linux root mounted on /linux the path is read in that tree ("linux /bin/ls" is /linux/usr/bin/ls); -r runs the program in the real root instead, which is where the
 * static test programs of the first levels live. Without /linux every path is a real one. */
APPLET(linux) {
    int first = 1, real_root = 0;
    if (argc > 1 && !strcmp(argv[1], "-r")) { real_root = 1; first = 2; }
    if (argc <= first) { kb_error("usage: linux [-r] PROGRAM [ARGUMENT...]"); return 2; }
    if (gap_setabi(ABI_LINUX | (real_root ? ABI_NOROOT : 0)) < 0) { kb_error("cannot select the Linux personality: %s", strerror(errno)); return 1; }
    execv(argv[first], &argv[first]);
    kb_error("cannot run '%s': %s", argv[first], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

/* native PROGRAM [ARGUMENT...]: the other way round, for a native program started from inside a Linux one (a Linux process keeps its personality across exec). */
APPLET(native) {
    if (argc < 2) { kb_error("usage: native PROGRAM [ARGUMENT...]"); return 2; }
    if (gap_setabi(ABI_NATIVE) < 0) { kb_error("cannot select the native personality: %s", strerror(errno)); return 1; }
    execvp(argv[1], &argv[1]);
    kb_error("cannot run '%s': %s", argv[1], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

/* Is something mounted on /linux? A mounted filesystem has a device number of its own. */
static int root_mounted(void) {
    struct stat a, b;
    if (stat(LINUX_ROOT, &a) < 0 || stat("/", &b) < 0) return 0;
    return a.st_dev != b.st_dev;
}

/* The archive's etc/resolv.conf is a placeholder: the real name servers are the ones this machine learned (DHCP writes /etc/resolv.conf). */
static void adopt_resolv_conf(void) {
    if (access("/etc/resolv.conf", R_OK) == 0 && access(LINUX_ROOT "/etc", W_OK) == 0) {
        unlink(LINUX_ROOT "/etc/resolv.conf");
        if (kb_copy_file("/etc/resolv.conf", LINUX_ROOT "/etc/resolv.conf", 0) < 0) kb_error("cannot copy /etc/resolv.conf into the Linux root: %s", strerror(errno));
    }
}

extern int applet_tar(int argc, char **argv);

/* Runs the tar applet on the archive in a child process and reads what it says on standard error. A device node or a pipe in the archive cannot be made (tar says so with a
 * warning and ends with status 2); here that is a line of ours and not a failure. Any other message is a real problem. Returns 0 if everything but those was fine. */
static int unpack(const char *archive) {
    int fds[2];
    if (pipe(fds) < 0) return 1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        close(fds[1]);
        char *targv[] = { "tar", "-xpf", (char *)archive, "-C", LINUX_ROOT, NULL };
        int rc = applet_tar(5, targv);
        fflush(stdout);
        _exit(rc);
    }
    close(fds[1]);
    char line[4200];
    size_t n = 0;
    int problems = 0, skipped = 0;
    for (;;) {
        char c;
        ssize_t r = read(fds[0], &c, 1);
        if (r == 0 || (r < 0 && errno != EINTR)) break;
        if (r < 0) continue;
        if (c != '\n' && n < sizeof line - 1) { line[n++] = c; continue; }
        line[n] = 0;
        n = 0;
        char *special = strstr(line, ": special files (devices, pipes) are not extracted");
        if (special) {
            *special = 0;
            const char *name = strchr(line, ':') ? strchr(line, ':') + 2 : line;
            printf("linuxroot: skipped %s: a device node or a pipe cannot be unpacked\n", name);
            skipped++;
        } else if (line[0]) {
            fprintf(stderr, "%s\n", line);
            problems++;
        }
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    (void)skipped;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return (rc == 0 || (rc == 2 && !problems)) ? 0 : 1;
}

/* linuxroot FILE.tar[.gz]   mounts a tmpfs on /linux unless one is there, and unpacks the archive into it: modes, symbolic links and times are kept, device nodes and
 *                           other special files are skipped with a line each (tar says which)
 * linuxroot -d              empties /linux and unmounts it, which gives the memory back; a Linux program that is still running keeps it busy
 * linuxroot                 says whether a Linux root is mounted
 * Mounting needs root. The archive is a plain tar or a gzip-compressed one (make-linux-root.py makes both). */
APPLET(linuxroot) {
    if (argc == 1) {
        printf(root_mounted() ? "a Linux root is mounted on " LINUX_ROOT "\n" : "no Linux root: " LINUX_ROOT " is not mounted (linuxroot FILE.tar unpacks one)\n");
        return root_mounted() ? 0 : 1;
    }
    if (geteuid() != 0) { kb_error("the Linux root is mounted and filled by root"); return 1; }

    if (!strcmp(argv[1], "-d")) {
        if (!root_mounted()) { kb_error("nothing is mounted on " LINUX_ROOT); return 1; }
        DIR *d = opendir(LINUX_ROOT);
        if (!d) { kb_error(LINUX_ROOT ": %s", strerror(errno)); return 1; }
        char **names = NULL;
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            names = realloc(names, (size_t)(n + 1) * sizeof *names);
            names[n++] = strdup(de->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char *path = kb_join(LINUX_ROOT, names[i]);
            kb_remove_tree(path, 1, 0);
            free(path);
            free(names[i]);
        }
        free(names);
        if (gap_umount(LINUX_ROOT) < 0) { printf("linuxroot: " LINUX_ROOT " is empty, but a Linux program is still running in it, so it stays mounted\n"); return 0; }
        printf("linuxroot: " LINUX_ROOT " is gone; Linux programs run in the real root again\n");
        return 0;
    }

    if (argc != 2) { kb_error("usage: linuxroot FILE.tar[.gz] | linuxroot -d | linuxroot"); return 2; }
    const char *archive = argv[1];
    if (root_mounted()) { kb_error(LINUX_ROOT " already holds a Linux root (linuxroot -d removes it first)"); return 1; }
    if (access(archive, R_OK) < 0) { kb_error("%s: %s", archive, strerror(errno)); return 1; }
    if (mkdir(LINUX_ROOT, 0755) < 0 && errno != EEXIST) { kb_error("cannot make " LINUX_ROOT ": %s", strerror(errno)); return 1; }
    if (gap_mount("linuxroot", LINUX_ROOT, "tmpfs", 0) < 0) { kb_error("cannot mount a tmpfs on " LINUX_ROOT ": %s", strerror(errno)); return 1; }

    int rc = unpack(archive);
    adopt_resolv_conf();
    if (rc) { kb_error("unpacking %s did not go cleanly; " LINUX_ROOT " holds what was unpacked", archive); return 1; }
    printf("linuxroot: %s unpacked into " LINUX_ROOT "\n", archive);
    return 0;
}
