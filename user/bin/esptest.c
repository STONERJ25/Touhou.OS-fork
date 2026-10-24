/*
 * esptest ESP OTHERPART [RODEV]...: checks the rules of the ESP write grant (BLKESPGRANT, shared/blkabi.h) from user space, for
 * tests/case_espgrant.py. ESP is the partition the system started from (an installed disk under blk.ro, so it is read-only), OTHERPART
 * another partition (the installed HUMANVILLAGE), each RODEV a node that must stay read-only meanwhile (the whole disk, another disk and
 * its partition): a write of 512 bytes at byte 51200 is tried on each, which the host then must not find. Run as root. Prints one line per
 * check and "ESPTEST PASS" or "ESPTEST FAIL (n)". Leaves the files grant1.txt and grant3.txt on the ESP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <gensokyo.h>
#include <blkabi.h>

#define MNT "/BambooForest/esptest"
#define MNT2 "/BambooForest/esptest2"

static int g_fail;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok) g_fail++;
}

static int ask(int fd, const char *name) {
    struct gblk_unlock u;
    memset(&u, 0, sizeof u);
    strncpy(u.name, name, sizeof u.name - 1);
    return ioctl(fd, BLKESPGRANT, &u) < 0 ? -errno : 0;
}

static int giveback(int fd) {
    struct gblk_unlock u;
    memset(&u, 0, sizeof u);
    return ioctl(fd, BLKWRLOCK, &u) < 0 ? -errno : 0;
}

static int state(int fd) {
    struct gblk_unlock u;
    memset(&u, 0, sizeof u);
    return ioctl(fd, BLKWRSTATE, &u);
}

static int put(int fd, char fill) {
    char buf[512];
    memset(buf, fill, sizeof buf);
    if (lseek(fd, 51200, SEEK_SET) < 0) return -errno;
    ssize_t n = write(fd, buf, sizeof buf);
    return n == (ssize_t)sizeof buf ? 0 : n < 0 ? -errno : -EIO;
}

static int mnt(const char *dev, const char *dir, const char *opts) {
    return gap_mount_opts(dev, dir, "fat32", 0, opts) < 0 ? -errno : 0;
}

static int umnt(const char *dir) { return gap_umount(dir) < 0 ? -errno : 0; }

/* 0 when a file can be created and written below dir, else -errno */
static int touch(const char *dir, const char *name, const char *text) {
    char p[160];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    ssize_t n = write(fd, text, strlen(text));
    int e = n == (ssize_t)strlen(text) ? 0 : n < 0 ? -errno : -EIO;
    if (close(fd) != 0 && !e) e = -errno;
    return e;
}

static int holds(const char *dir, const char *name, const char *text) {
    char p[160], buf[128];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n < 0) return 0;
    buf[n] = 0;
    return !strcmp(buf, text);
}

static int is_ro(const char *dir) {
    struct statfs sf;
    return statfs(dir, &sf) == 0 && (sf.f_flags & 1);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: esptest ESP OTHERPART [RODEV]...\n"); return 2; }
    const char *esp = argv[1], *other = argv[2];
    char dev[64], odev[64];
    snprintf(dev, sizeof dev, "/dev/%s", esp);
    snprintf(odev, sizeof odev, "/dev/%s", other);
    mkdir(MNT, 0700);
    mkdir(MNT2, 0700);

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 2; }
    check(state(fd) == (BLKWR_READONLY | BLKWR_POLICY), "the ESP starts read-only because of blk.ro, no grant");

    /* without the grant */
    check(mnt(dev, MNT, "rw,espgrant") == -EPERM, "mount -o espgrant without a grant is refused (EPERM)");
    int rc = mnt(dev, MNT, "rw");
    check(rc == 0 && is_ro(MNT), "a plain read-write mount comes up read-only, as before");
    check(touch(MNT, "nogrant.txt", "x") == -EROFS, "and writing a file there fails with EROFS");
    if (!rc) umnt(MNT);
    check(put(fd, 'a') == -EROFS, "the partition node is read-only");

    /* the refusals */
    check(ask(fd, "sdz1") == -EINVAL, "the grant wants the node's own name (EINVAL)");
    int rfd = open(dev, O_RDONLY);
    check(rfd >= 0 && ask(rfd, esp) == -EBADF, "a file opened read-only cannot ask (EBADF)");
    if (rfd >= 0) close(rfd);
    int ofd = open(odev, O_RDWR);
    check(ofd >= 0 && ask(ofd, other) == -EACCES, "another partition is not the ESP the system started from (EACCES)");
    if (ofd >= 0) close(ofd);
    for (int i = 3; i < argc; i++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/%s", argv[i]);
        int xfd = open(p, O_RDWR);
        char what[128];
        snprintf(what, sizeof what, "%s: no grant there (whole disk EINVAL, another disk's partition EACCES)", p);
        int r = xfd >= 0 ? ask(xfd, argv[i]) : -ENOENT;
        check(r == -EINVAL || r == -EACCES, what);
        if (xfd >= 0) close(xfd);
    }
    pid_t pid = fork();
    if (pid == 0) {                                      /* the open file is inherited, the right to ask is not */
        if (setuid(1000) != 0) _exit(3);
        _exit(ask(fd, esp) == -EPERM ? 0 : 1);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "a process that is not root gets EPERM, even holding the open file");
    rc = mnt(dev, MNT, "ro");
    check(rc == 0 && ask(fd, esp) == -EBUSY, "the ESP cannot be granted while it is mounted (EBUSY)");
    if (!rc) umnt(MNT);

    /* granted */
    check(ask(fd, esp) == 0, "root gets the grant for the ESP");
    check(state(fd) == (BLKWR_READONLY | BLKWR_POLICY | BLKWR_ESPGRANT), "the partition stays registered read-only; this file holds the grant");
    check(ask(fd, esp) == 0, "asking twice is harmless");
    int fd2 = open(dev, O_RDWR);
    check(fd2 >= 0 && ask(fd2, esp) == -EBUSY, "a second open file cannot get a second grant (EBUSY)");
    check(fd2 >= 0 && put(fd2, 'b') == -EROFS && put(fd, 'c') == -EROFS, "the grant does not open the partition node for writing");
    if (fd2 >= 0) close(fd2);
    check(mnt(dev, MNT, "ro,espgrant") == -EINVAL, "espgrant with ro makes no sense (EINVAL)");
    rc = mnt(dev, MNT, "rw,espgrant");
    check(rc == 0 && !is_ro(MNT), "mount -o rw,espgrant takes the grant: mounted read-write");
    check(state(fd) == (BLKWR_READONLY | BLKWR_POLICY | BLKWR_ESPGRANT | BLKWR_ESPMOUNTED), "BLKWRSTATE shows the grant taken by a mount");
    check(touch(MNT, "grant1.txt", "first grant\n") == 0 && holds(MNT, "grant1.txt", "first grant\n"), "a file is written and reads back");
    check(mnt(dev, MNT2, "rw") == -EBUSY, "the same partition cannot be mounted a second time");
    fd2 = open(dev, O_RDWR);
    check(fd2 >= 0 && put(fd2, 'd') == -EROFS && put(fd, 'e') == -EROFS, "the partition node stays read-only while it is mounted with the grant");
    if (fd2 >= 0) close(fd2);
    for (int i = 3; i < argc; i++) {
        char p[64], what[128];
        snprintf(p, sizeof p, "/dev/%s", argv[i]);
        int xfd = open(p, O_RDWR);
        snprintf(what, sizeof what, "%s stays read-only meanwhile (EROFS)", p);
        check(xfd >= 0 && put(xfd, 'f') == -EROFS, what);
        if (xfd >= 0) close(xfd);
    }

    /* the unmount ends it */
    sync();
    check(umnt(MNT) == 0 && !(state(fd) & BLKWR_ESPGRANT), "the unmount ends the grant");
    check(mnt(dev, MNT, "rw,espgrant") == -EPERM, "so a second mount with espgrant needs a new grant");
    rc = mnt(dev, MNT, "rw");
    check(rc == 0 && is_ro(MNT) && holds(MNT, "grant1.txt", "first grant\n"), "a plain mount is read-only again, the file is there");
    check(touch(MNT, "after.txt", "x") == -EROFS, "and cannot be written");
    if (!rc) umnt(MNT);

    /* closing the file ends it: the volume stays mounted, read-only */
    check(ask(fd, esp) == 0 && mnt(dev, MNT, "rw,espgrant") == 0, "granted and mounted again");
    check(touch(MNT, "grant2.txt", "second grant\n") == 0, "written through the second grant");
    close(fd);
    check(is_ro(MNT), "closing the grant's file turns the mounted volume read-only");
    check(touch(MNT, "late.txt", "x") == -EROFS, "writing fails afterwards (EROFS)");
    check(holds(MNT, "grant2.txt", "second grant\n"), "what was written before is there");
    check(umnt(MNT) == 0, "and it can be unmounted");

    /* a process that ends while it holds the grant: the same */
    pid = fork();
    if (pid == 0) {
        int cfd = open(dev, O_RDWR);
        if (cfd < 0 || ask(cfd, esp) || mnt(dev, MNT, "rw,espgrant")) _exit(1);
        _exit(touch(MNT, "grant3.txt", "third grant\n") == 0 ? 0 : 2);      /* exits holding it, without unmounting */
    }
    waitpid(pid, &ws, 0);
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "a child process takes the grant, mounts and writes");
    check(is_ro(MNT) && touch(MNT, "orphan.txt", "x") == -EROFS, "its exit turned the volume read-only");
    check(holds(MNT, "grant3.txt", "third grant\n"), "its file was written back first");
    check(umnt(MNT) == 0, "unmounted");

    /* BLKWRLOCK hands it back early */
    fd = open(dev, O_RDWR);
    check(fd >= 0 && ask(fd, esp) == 0 && mnt(dev, MNT, "rw,espgrant") == 0, "granted and mounted a fourth time");
    check(giveback(fd) == 0 && is_ro(MNT) && state(fd) == (BLKWR_READONLY | BLKWR_POLICY), "BLKWRLOCK hands the grant back: read-only");
    check(umnt(MNT) == 0, "unmounted");
    check(unlink(MNT "/grant2.txt") != 0, "nothing mounted any more");
    close(fd);

    rmdir(MNT);
    rmdir(MNT2);
    if (g_fail) printf("ESPTEST FAIL (%d)\n", g_fail);
    else printf("ESPTEST PASS\n");
    return g_fail ? 1 : 0;
}
