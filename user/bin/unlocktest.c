/*
 * unlocktest DISK [WPDISK]: checks the rules of the per-disk write unlock (BLKWRUNLOCK, shared/blkabi.h) from user space, for
 * tests/case_install.py. DISK must be a whole disk that blk.ro made read-only; the test writes one 512-byte block at byte 51200 of it
 * (and nothing else), which the host then finds in the image. WPDISK, if given, is a disk whose medium is write-protected: its unlock
 * must fail with EROFS. Run as root. Prints one line per check and "UNLOCKTEST PASS" or "UNLOCKTEST FAIL (n)".
 * unlocktest --busy DISK: a partition of DISK is mounted, so the unlock must fail with EBUSY.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/wait.h>
#include <blkabi.h>

static int g_fail;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static int unlock(int fd, const char *name) {
    struct gblk_unlock u;
    memset(&u, 0, sizeof u);
    strncpy(u.name, name, sizeof u.name - 1);
    return ioctl(fd, BLKWRUNLOCK, &u) < 0 ? -errno : 0;
}

static int lock(int fd) {
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: unlocktest DISK [WPDISK] | --busy DISK\n"); return 2; }
    if (!strcmp(argv[1], "--busy") && argc > 2) {
        char p[64];
        snprintf(p, sizeof p, "/dev/%s", argv[2]);
        int bfd = open(p, O_RDWR);
        check(bfd >= 0 && unlock(bfd, argv[2]) == -EBUSY, "a disk with a mounted partition cannot be unlocked");
        printf("UNLOCKTEST %s\n", g_fail ? "FAIL (1)" : "PASS");
        return g_fail ? 1 : 0;
    }
    const char *disk = argv[1];
    char path[64];
    snprintf(path, sizeof path, "/dev/%s", disk);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror(path); return 2; }
    int st = state(fd);
    check(st == (BLKWR_READONLY | BLKWR_POLICY), "the disk starts read-only because of blk.ro, not unlocked");
    check(put(fd, 'a') == -EROFS, "a write is refused before the unlock");
    check(unlock(fd, "sdz") == -EINVAL, "the unlock wants the disk's own name");
    int rfd = open(path, O_RDONLY);
    check(rfd >= 0 && unlock(rfd, disk) == -EBADF, "a file opened read-only cannot be unlocked");
    close(rfd);

    pid_t pid = fork();
    if (pid == 0) {                                      /* the open file is inherited, the right to unlock is not */
        if (setuid(1000) != 0) _exit(3);
        _exit(unlock(fd, disk) == -EPERM && put(fd, 'b') == -EROFS ? 0 : 1);
    }
    int ws = 0;
    waitpid(pid, &ws, 0);
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "a process that is not root gets EPERM, even holding the open file");

    check(unlock(fd, disk) == 0, "root unlocks the disk");
    check(state(fd) == (BLKWR_READONLY | BLKWR_POLICY | BLKWR_UNLOCKED), "the disk stays registered read-only; this file holds the unlock");
    check(unlock(fd, disk) == 0, "unlocking twice is harmless");
    int fd2 = open(path, O_RDWR);
    check(fd2 >= 0 && !(state(fd2) & BLKWR_UNLOCKED) && put(fd2, 'c') == -EROFS, "a second open of the same disk is still read-only");
    close(fd2);
    char part[64];
    snprintf(part, sizeof part, "/dev/%sp1", disk);
    int pfd = open(part, O_RDWR);
    if (pfd < 0) { snprintf(part, sizeof part, "/dev/%s1", disk); pfd = open(part, O_RDWR); }
    if (pfd >= 0) {
        check(unlock(pfd, part + 5) == -EINVAL, "a partition node cannot be unlocked");
        check(put(pfd, 'd') == -EROFS, "the disk's partitions stay read-only while the disk is unlocked");
        close(pfd);
    }
    pid = fork();
    if (pid == 0) _exit(put(fd, 'E') == 0 ? 0 : 1);      /* the unlock belongs to the open file, which the child shares */
    waitpid(pid, &ws, 0);
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "a write through the unlocked file goes through");
    char back[512];
    check(lseek(fd, 51200, SEEK_SET) == 51200 && read(fd, back, sizeof back) == (ssize_t)sizeof back && back[0] == 'E' && back[511] == 'E',
          "and reads back");
    check(lock(fd) == 0 && put(fd, 'f') == -EROFS && state(fd) == (BLKWR_READONLY | BLKWR_POLICY), "BLKWRLOCK takes the unlock back");
    check(unlock(fd, disk) == 0, "unlocked once more");
    close(fd);
    fd = open(path, O_RDWR);
    check(fd >= 0 && state(fd) == (BLKWR_READONLY | BLKWR_POLICY) && put(fd, 'g') == -EROFS, "closing the file ended the unlock");
    close(fd);

    if (argc > 2) {
        snprintf(path, sizeof path, "/dev/%s", argv[2]);
        int wfd = open(path, O_RDWR);
        check(wfd >= 0 && state(wfd) == BLKWR_READONLY && unlock(wfd, argv[2]) == -EROFS, "a write-protected medium cannot be unlocked");
        if (wfd >= 0) close(wfd);
    }
    if (g_fail) printf("UNLOCKTEST FAIL (%d)\n", g_fail);
    else printf("UNLOCKTEST PASS\n");
    return g_fail ? 1 : 0;
}
