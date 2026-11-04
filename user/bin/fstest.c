/*
 * fstest: filesystem semantics, checked at the system-call level (errno values included).
 *
 *   fstest DIR      creates DIR/fst-<pid>, runs every check inside it, removes it again
 *   fstest -churn SEED DIR [OPS]   a random write-heavy workload for the power-cut test (see churn() below)
 *
 * Written for the FAT driver (case-insensitive names, no symlinks, sparse files allocated, 4 GiB limit...) but the
 * checks are the ordinary POSIX ones, so tmpfs passes as well except where a line says otherwise. Prints one line per
 * failure and "FSTEST PASS" / "FSTEST FAIL n" at the end; the exit status is the number of failures.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>

static int g_checks, g_fail;
static char g_root[512];

#define CHECK(cond, ...)                                                                              \
    do {                                                                                              \
        g_checks++;                                                                                   \
        if (!(cond)) {                                                                                \
            int _e = errno;                                                                           \
            g_fail++;                                                                                 \
            printf("FAIL line %d: ", __LINE__);                                                       \
            printf(__VA_ARGS__);                                                                      \
            printf(" [errno %d: %s]\n", _e, strerror(_e));                                            \
        }                                                                                             \
    } while (0)

static char *P(const char *rel) {
    static char bufs[8][1024];
    static int k;
    char *b = bufs[k++ & 7];
    snprintf(b, 1024, "%s/%s", g_root, rel);
    return b;
}

static void fill(unsigned char *b, size_t n, unsigned seed, size_t base) {
    for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(((base + i) * 2654435761u >> 13) + seed);
}

static int check_pattern(const unsigned char *b, size_t n, unsigned seed, size_t base) {
    for (size_t i = 0; i < n; i++)
        if (b[i] != (unsigned char)(((base + i) * 2654435761u >> 13) + seed)) return 0;
    return 1;
}

static int write_file(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const unsigned char *p = data;
    while (n) {
        ssize_t w = write(fd, p, n > 4096 ? 4096 : n);
        if (w <= 0) { close(fd); return -1; }
        p += w;
        n -= (size_t)w;
    }
    return close(fd);
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* number of names in a directory (without . and ..), and 1 if `name` is among them (exact spelling) */
static int count_dir(const char *path, const char *name, int *found) {
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    if (found) *found = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        n++;
        if (name && found && !strcmp(e->d_name, name)) *found = 1;
    }
    closedir(d);
    return n;
}

static unsigned long free_blocks(void) {
    struct statfs sf;
    return statfs(g_root, &sf) == 0 ? sf.f_bfree : 0;
}

/* ---------------------------------------------------------------------------------------------- the checks */

static void t_basic(void) {
    unsigned char buf[10000], back[10000];
    fill(buf, sizeof buf, 7, 0);
    CHECK(write_file(P("basic.bin"), buf, sizeof buf) == 0, "create and write");
    CHECK(file_size(P("basic.bin")) == (long)sizeof buf, "size is %ld", file_size(P("basic.bin")));
    int fd = open(P("basic.bin"), O_RDONLY);
    CHECK(fd >= 0, "reopen");
    ssize_t r = read(fd, back, sizeof back);
    CHECK(r == (ssize_t)sizeof back && check_pattern(back, sizeof back, 7, 0), "read back %ld bytes", (long)r);
    CHECK(read(fd, back, 1) == 0, "EOF");
    CHECK(lseek(fd, 5000, SEEK_SET) == 5000 && read(fd, back, 100) == 100 && check_pattern(back, 100, 7, 5000), "seek and read");
    close(fd);
    struct stat st, dst;
    CHECK(stat(P("basic.bin"), &st) == 0 && stat(g_root, &dst) == 0, "stat");
    CHECK(S_ISREG(st.st_mode) && S_ISDIR(dst.st_mode), "types");
    CHECK(st.st_uid == dst.st_uid && st.st_gid == dst.st_gid, "a new file belongs to the owner of its directory (%u vs %u)", st.st_uid, dst.st_uid);
    CHECK((st.st_mode & 0777) == (dst.st_mode & 0666), "file mode %o vs directory %o", st.st_mode & 0777, dst.st_mode & 0777);
    CHECK(st.st_mtime > 1700000000, "mtime looks like a date (%ld)", (long)st.st_mtime);
    /* O_EXCL and case-insensitive lookup */
    CHECK(open(P("basic.bin"), O_WRONLY | O_CREAT | O_EXCL, 0644) < 0 && errno == EEXIST, "O_EXCL on an existing file");
    CHECK(open(P("BASIC.BIN"), O_RDONLY) >= 0, "case-insensitive open");
    int before = count_dir(g_root, NULL, NULL);
    fd = open(P("Basic.Bin"), O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "O_CREAT with another spelling opens the existing file");
    if (fd >= 0) close(fd);
    int found;
    CHECK(count_dir(g_root, "basic.bin", &found) == before && found, "no second entry and the stored spelling is kept");
    CHECK(open(P("nosuchfile"), O_RDONLY) < 0 && errno == ENOENT, "ENOENT");
    CHECK(open(P("basic.bin/x"), O_RDONLY) < 0 && errno == ENOTDIR, "path through a file");
    CHECK(open(g_root, O_WRONLY) < 0 && errno == EISDIR, "open a directory for writing");
    CHECK(unlink(P("basic.bin")) == 0 && !exists(P("basic.bin")), "unlink");
}

static void t_sparse_append_truncate(void) {
    unsigned char b[300];
    int fd = open(P("sparse.bin"), O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create");
    CHECK(write(fd, "AB", 2) == 2, "write 2 bytes");
    CHECK(lseek(fd, 20000, SEEK_SET) == 20000 && write(fd, "Z", 1) == 1, "write past the end");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 20001, "size after the hole is %ld", (long)st.st_size);
    CHECK(lseek(fd, 100, SEEK_SET) == 100 && read(fd, b, 300) == 300, "read from the hole");
    int zero = 1;
    for (int i = 0; i < 300; i++) if (b[i]) zero = 0;
    CHECK(zero, "the hole reads as zeros");
    CHECK(lseek(fd, 19999, SEEK_SET) == 19999 && read(fd, b, 10) == 2 && b[0] == 0 && b[1] == 'Z', "tail of the file");
    CHECK(ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5, "shrink");
    CHECK(ftruncate(fd, 9000) == 0 && fstat(fd, &st) == 0 && st.st_size == 9000, "grow");
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, b, 9) == 9 && b[0] == 'A' && b[1] == 'B' && b[2] == 0 && b[8] == 0,
          "grown part is zero, old data kept");
    CHECK(lseek(fd, 4090, SEEK_SET) == 4090 && read(fd, b, 20) == 20, "read across a cluster/page boundary");
    zero = 1;
    for (int i = 0; i < 20; i++) if (b[i]) zero = 0;
    CHECK(zero, "no stale bytes after shrink+grow");
    close(fd);
    CHECK(truncate(P("sparse.bin"), 0) == 0 && file_size(P("sparse.bin")) == 0, "truncate to 0");
    CHECK(truncate(P("sparse.bin"), 70000) == 0 && file_size(P("sparse.bin")) == 70000, "truncate to 70000");
    fd = open(P("sparse.bin"), O_RDONLY);
    unsigned char *big = malloc(70000);
    CHECK(read(fd, big, 70000) == 70000, "read it all");
    zero = 1;
    for (int i = 0; i < 70000; i++) if (big[i]) { zero = 0; break; }
    CHECK(zero, "70000 zeros after truncate(0) + truncate(70000)");
    free(big);
    close(fd);
    /* O_APPEND from two descriptors */
    int a = open(P("app.txt"), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    int c = open(P("app.txt"), O_WRONLY | O_APPEND);
    CHECK(a >= 0 && c >= 0, "two append descriptors");
    for (int i = 0; i < 50; i++) {
        CHECK(write(a, "aa", 2) == 2 && write(c, "cc", 2) == 2, "append %d", i);
    }
    close(a);
    close(c);
    CHECK(file_size(P("app.txt")) == 200, "appended size %ld", file_size(P("app.txt")));
    fd = open(P("app.txt"), O_RDWR | O_TRUNC);
    CHECK(fd >= 0 && file_size(P("app.txt")) == 0, "O_TRUNC");
    close(fd);
    unlink(P("app.txt"));
    unlink(P("sparse.bin"));
}

static void t_boundaries(void) {
    struct statfs sf;
    CHECK(statfs(g_root, &sf) == 0 && sf.f_bsize >= 512, "statfs");
    size_t cb = sf.f_bsize ? sf.f_bsize : 4096;
    size_t sizes[] = { 0, 1, cb - 1, cb, cb + 1, 2 * cb - 1, 2 * cb, 2 * cb + 1, 3 * cb, 4095, 4096, 4097, 8191, 8192, 8193, 65536, 65537 };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        size_t n = sizes[k];
        unsigned char *buf = malloc(n + 1), *back = malloc(n + 1);
        fill(buf, n, (unsigned)k, 0);
        char name[64];
        snprintf(name, sizeof name, "bound-%zu.bin", n);
        CHECK(write_file(P(name), buf, n) == 0, "write %zu bytes", n);
        CHECK(file_size(P(name)) == (long)n, "size of %zu-byte file", n);
        int fd = open(P(name), O_RDONLY);
        ssize_t r = n ? read(fd, back, n) : 0;
        CHECK(fd >= 0 && r == (ssize_t)n && (n == 0 || check_pattern(back, n, (unsigned)k, 0)), "read back %zu bytes", n);
        close(fd);
        /* append one byte and check the old data and the new byte */
        fd = open(P(name), O_WRONLY | O_APPEND);
        CHECK(fd >= 0 && write(fd, "!", 1) == 1, "append to %zu bytes", n);
        close(fd);
        fd = open(P(name), O_RDONLY);
        r = read(fd, back, n + 1);
        CHECK(r == (ssize_t)n + 1 && back[n] == '!' && (n == 0 || check_pattern(back, n, (unsigned)k, 0)), "after append to %zu", n);
        close(fd);
        unlink(P(name));
        free(buf);
        free(back);
    }
    /* statfs follows allocation: a 100000-byte file takes ceil(100000 / cluster) clusters */
    unsigned long f0 = free_blocks();
    unsigned char *buf = malloc(100000);
    fill(buf, 100000, 3, 0);
    CHECK(write_file(P("space.bin"), buf, 100000) == 0, "write space.bin");
    free(buf);
    sync();
    unsigned long f1 = free_blocks();
    unsigned long want = (100000 + cb - 1) / cb;
    CHECK(f0 - f1 == want || sf.f_type != 0x4d44, "free blocks fell by %lu, expected %lu", f0 - f1, want);
    CHECK(unlink(P("space.bin")) == 0, "unlink space.bin");
    CHECK(free_blocks() == f0, "free blocks come back after unlink (%lu vs %lu)", free_blocks(), f0);
}

static void t_unlink_rename_while_open(void) {
    unsigned char buf[30000], back[30000];
    fill(buf, sizeof buf, 9, 0);
    unsigned long f_base = free_blocks();
    CHECK(write_file(P("open.bin"), buf, sizeof buf) == 0, "create");
    unsigned long f0 = free_blocks();
    CHECK(f0 < f_base, "the file occupies clusters");
    int fd = open(P("open.bin"), O_RDWR);
    CHECK(fd >= 0, "open");
    CHECK(unlink(P("open.bin")) == 0 && !exists(P("open.bin")), "unlink while open");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_nlink == 0, "nlink is 0 (%u)", (unsigned)st.st_nlink);
    CHECK(free_blocks() == f0, "the clusters stay allocated while the file is open");
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, back, sizeof back) == (ssize_t)sizeof back && check_pattern(back, sizeof back, 9, 0), "still readable");
    CHECK(lseek(fd, 100, SEEK_SET) == 100 && write(fd, "ghost", 5) == 5, "still writable");
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)sizeof back, "size");
    CHECK(write_file(P("open.bin"), "new", 3) == 0 && file_size(P("open.bin")) == 3, "the name can be reused meanwhile");
    close(fd);
    CHECK(unlink(P("open.bin")) == 0, "remove the new one");
    sync();
    CHECK(free_blocks() == f_base, "closing the last descriptor frees the clusters (%lu vs %lu)", free_blocks(), f_base);

    /* rename while open */
    CHECK(write_file(P("before.txt"), "move me", 7) == 0, "create before.txt");
    fd = open(P("before.txt"), O_RDWR);
    CHECK(rename(P("before.txt"), P("After Rename.txt")) == 0, "rename while open");
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, back, 7) == 7 && !memcmp(back, "move me", 7), "descriptor still reads the file");
    CHECK(lseek(fd, 0, SEEK_END) == 7 && write(fd, "!", 1) == 1, "append through the old descriptor");
    close(fd);
    CHECK(file_size(P("After Rename.txt")) == 8 && !exists(P("before.txt")), "new name has the data");
    unlink(P("After Rename.txt"));
}

static void t_dirs_and_renames(void) {
    CHECK(mkdir(P("d1"), 0755) == 0, "mkdir");
    CHECK(mkdir(P("d1"), 0755) < 0 && errno == EEXIST, "mkdir existing");
    CHECK(mkdir(P("D1"), 0755) < 0 && errno == EEXIST, "mkdir with another case");
    CHECK(mkdir(P("nodir/sub"), 0755) < 0 && errno == ENOENT, "mkdir below a missing parent");
    CHECK(write_file(P("d1/f.txt"), "x", 1) == 0, "file in d1");
    CHECK(rmdir(P("d1")) < 0 && errno == ENOTEMPTY, "rmdir non-empty");
    CHECK(rmdir(P("d1/f.txt")) < 0 && errno == ENOTDIR, "rmdir a file");
    CHECK(unlink(P("d1")) < 0 && errno == EISDIR, "unlink a directory");
    CHECK(mkdir(P("d2"), 0755) == 0 && mkdir(P("d3"), 0755) == 0 && write_file(P("d3/g.txt"), "g", 1) == 0, "more dirs");
    CHECK(rename(P("d1"), P("d3")) < 0 && errno == ENOTEMPTY, "rename a directory over a non-empty one");
    CHECK(rename(P("d2"), P("d1")) < 0 && errno == ENOTEMPTY, "rename an empty directory over a non-empty one");
    CHECK(rename(P("d1/f.txt"), P("d3")) < 0 && errno == EISDIR, "rename a file over a directory");
    CHECK(write_file(P("plain.txt"), "p", 1) == 0, "plain file");
    CHECK(rename(P("d2"), P("plain.txt")) < 0 && errno == ENOTDIR, "rename a directory over a file");
    CHECK(rename(P("d1"), P("d1/inner")) < 0 && errno == EINVAL, "rename a directory into itself");
    CHECK(mkdir(P("d1/in"), 0755) == 0 && rename(P("d1"), P("d1/in/deeper")) < 0 && errno == EINVAL, "rename a directory below itself");
    CHECK(rename(P("d1"), P("d1")) == 0 && exists(P("d1")), "rename onto itself");
    CHECK(rename(P("d2"), P("d3x")) == 0 && !exists(P("d2")) && exists(P("d3x")), "rename an empty directory");
    CHECK(rename(P("d3x"), P("d1/in/d3")) == 0, "move a directory deeper");
    CHECK(write_file(P("d1/in/d3/leaf.txt"), "leaf", 4) == 0 && file_size(P("d1/in/d3/leaf.txt")) == 4, "write inside the moved directory");
    CHECK(rename(P("d1/in/d3"), P("d3-back")) == 0 && file_size(P("d3-back/leaf.txt")) == 4, "move it out again with its content");
    CHECK(rename(P("d3-back"), P("D3-BACK")) == 0, "case-only rename of a directory");
    int found;
    CHECK(count_dir(g_root, "D3-BACK", &found) >= 1 && found, "new spelling listed");
    CHECK(rename(P("d3"), P("D3-BACK")) < 0, "replace a non-empty directory");   /* d3 holds g.txt; target holds leaf.txt */
    /* replace a file */
    CHECK(write_file(P("one.txt"), "one", 3) == 0 && write_file(P("two.txt"), "twotwo", 6) == 0, "two files");
    CHECK(rename(P("one.txt"), P("two.txt")) == 0 && file_size(P("two.txt")) == 3 && !exists(P("one.txt")), "rename over an existing file");
    CHECK(rename(P("two.txt"), P("d3/two.txt")) == 0 && file_size(P("d3/two.txt")) == 3, "move across directories");
    CHECK(rename(P("missing"), P("x")) < 0 && errno == ENOENT, "rename a missing file");
    /* clean up */
    unlink(P("d3/two.txt")); unlink(P("d3/g.txt")); unlink(P("D3-BACK/leaf.txt")); unlink(P("plain.txt")); unlink(P("d1/f.txt"));
    CHECK(rmdir(P("d1/in")) == 0 && rmdir(P("d1")) == 0 && rmdir(P("d3")) == 0 && rmdir(P("D3-BACK")) == 0, "rmdir them all");
    CHECK(count_dir(g_root, NULL, NULL) == 0, "clean directory (%d left)", count_dir(g_root, NULL, NULL));
}

static void t_names(void) {
    static const char *bad[] = { "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\\b", "trailing.", "trailing ", "..." , "ctl\x01x", "bad\xff\xfe.txt", "bad\xc0\xaf.txt" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int fd = open(P(bad[i]), O_WRONLY | O_CREAT, 0644);
        CHECK(fd < 0 && (errno == EINVAL || errno == ENOENT), "name #%u must be refused, got fd %d", i, fd);
        if (fd >= 0) { close(fd); unlink(P(bad[i])); }
    }
    char longname[300];
    memset(longname, 'n', 256);
    longname[256] = 0;
    CHECK(open(P(longname), O_WRONLY | O_CREAT, 0644) < 0 && errno == ENAMETOOLONG, "256-character name");
    memset(longname, 'n', 255);
    longname[255] = 0;
    int fd = open(P(longname), O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "255-character name");
    if (fd >= 0) { close(fd); int f; CHECK(count_dir(g_root, longname, &f) == 1 && f, "listed with all 255 characters"); CHECK(unlink(P(longname)) == 0, "remove it"); }
    /* lengths around the 13-character long-name slot size, and the 8.3 limits */
    static const int lens[] = { 1, 7, 8, 9, 11, 12, 13, 14, 25, 26, 27, 38, 39, 40, 100, 200 };
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        char nm[256];
        for (int k = 0; k < lens[i]; k++) nm[k] = (char)('a' + (k * 7 + i) % 26);
        nm[lens[i]] = 0;
        char up[256];
        strcpy(up, nm);
        up[0] = (char)(up[0] - 32);                              /* mixed case forces a long name */
        CHECK(write_file(P(up), up, (size_t)lens[i]) == 0, "create a %d-character name", lens[i]);
        int f;
        CHECK(count_dir(g_root, up, &f) >= 1 && f, "%d-character name listed as created", lens[i]);
        CHECK(file_size(P(nm)) == lens[i], "found by its lower-case spelling");
    }
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        char nm[256];
        for (int k = 0; k < lens[i]; k++) nm[k] = (char)('a' + (k * 7 + i) % 26);
        nm[lens[i]] = 0;
        CHECK(unlink(P(nm)) == 0, "remove %d-character name through another spelling", lens[i]);
    }
    /* UTF-8: 2-, 3- and 4-byte sequences (the apple needs a surrogate pair on disk) */
    static const char *uni[] = { "caf\xc3\xa9.txt", "\xe6\x9d\xb1\xe6\x96\xb9.txt", "apple-\xf0\x9f\x8d\x8e.txt", "\xce\xa3\xce\xb9\xce\xb3\xce\xbc\xce\xb1.dat",
                                 "\xd0\x9c\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0" };
    for (unsigned i = 0; i < sizeof uni / sizeof uni[0]; i++) {
        CHECK(write_file(P(uni[i]), "u", 1) == 0, "create unicode name #%u", i);
        int f;
        CHECK(count_dir(g_root, uni[i], &f) >= 1 && f, "unicode name #%u round-trips", i);
    }
    CHECK(file_size(P("CAF\xc3\x89.TXT")) == 1, "case-insensitive for Latin-1 (U+00E9 / U+00C9)");
    CHECK(file_size(P("\xce\xa3\xce\x99\xce\x93\xce\x9c\xce\x91.DAT")) == 1, "case-insensitive for Greek");
    CHECK(file_size(P("\xd0\xbc\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0")) == 1, "case-insensitive for Cyrillic");
    for (unsigned i = 0; i < sizeof uni / sizeof uni[0]; i++) unlink(P(uni[i]));
    /* special spellings that are fine */
    static const char *good[] = { ".hidden", "a..b", "a b", " leading", "UPPER", "MiXeD.TxT", "a.b.c.d.e", "no_ext", "~tilde", "dollar$", "x(1).txt", "semi;colon", "plus+plus", "comma,comma", "eq=eq", "[brackets]" };
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
        CHECK(write_file(P(good[i]), "g", 1) == 0, "create '%s'", good[i]);
        int f;
        CHECK(count_dir(g_root, good[i], &f) >= 1 && f, "'%s' listed with its own spelling", good[i]);
    }
    CHECK(count_dir(g_root, NULL, NULL) == (int)(sizeof good / sizeof good[0]), "every name is a separate entry");
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) CHECK(unlink(P(good[i])) == 0, "remove '%s'", good[i]);
}

static void t_many(void) {
    char name[160];
    const int N = 700;
    CHECK(mkdir(P("m"), 0755) == 0, "mkdir m");
    for (int i = 0; i < N; i++) {
        snprintf(name, sizeof name, "m/Entry number %04d with a name long enough to take several directory slots.txt", i);
        int fd = open(P(name), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) { CHECK(0, "create entry %d", i); break; }
        char t[16];
        int tn = snprintf(t, sizeof t, "%d", i);
        write(fd, t, (size_t)tn);
        close(fd);
    }
    CHECK(count_dir(P("m"), NULL, NULL) == N, "all %d entries listed (%d)", N, count_dir(P("m"), NULL, NULL));
    for (int i = 0; i < N; i += 2) {
        snprintf(name, sizeof name, "m/Entry number %04d with a name long enough to take several directory slots.txt", i);
        if (unlink(P(name)) != 0) { CHECK(0, "unlink entry %d", i); break; }
    }
    CHECK(count_dir(P("m"), NULL, NULL) == N / 2, "half are gone (%d)", count_dir(P("m"), NULL, NULL));
    for (int i = 0; i < N; i += 2) {                             /* refill the holes with different names */
        snprintf(name, sizeof name, "m/refill %04d.txt", i);
        if (write_file(P(name), "r", 1) != 0) { CHECK(0, "refill %d", i); break; }
    }
    CHECK(count_dir(P("m"), NULL, NULL) == N, "refilled (%d)", count_dir(P("m"), NULL, NULL));
    /* every name appears exactly once and can be stat'ed */
    DIR *d = opendir(P("m"));
    int seen = 0, bad = 0, dup = 0;
    char prev[300] = "";
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        seen++;
        char full[600];
        snprintf(full, sizeof full, "%s/m/%s", g_root, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) bad++;
        if (!strcmp(prev, e->d_name)) dup++;
        strlcpy(prev, e->d_name, sizeof prev);
    }
    if (d) closedir(d);
    CHECK(seen == N && !bad && !dup, "readdir: %d seen, %d unstatable, %d duplicates", seen, bad, dup);
    for (int i = 1; i < N; i += 2) {
        snprintf(name, sizeof name, "m/Entry number %04d with a name long enough to take several directory slots.txt", i);
        if (unlink(P(name)) != 0) { CHECK(0, "unlink odd entry %d", i); break; }
    }
    for (int i = 0; i < N; i += 2) {
        snprintf(name, sizeof name, "m/refill %04d.txt", i);
        if (unlink(P(name)) != 0) { CHECK(0, "unlink refill %d", i); break; }
    }
    CHECK(rmdir(P("m")) == 0, "rmdir m (%d left)", count_dir(P("m"), NULL, NULL));
}

static void t_modes(void) {
    CHECK(write_file(P("mode.txt"), "m", 1) == 0, "create");
    struct stat st;
    CHECK(stat(P("mode.txt"), &st) == 0, "stat");
    mode_t cur = st.st_mode & 07777;
    uid_t uid = st.st_uid;
    gid_t gid = st.st_gid;
    CHECK(chmod(P("mode.txt"), cur) == 0, "chmod to the current mode succeeds");
    if (getuid() != 0) {
        CHECK(chown(P("mode.txt"), uid, gid) < 0 && errno == EPERM, "only root may chown");
    } else {
        CHECK(chown(P("mode.txt"), uid, gid) == 0, "chown to the current owner succeeds");
        CHECK(chown(P("mode.txt"), (uid_t)-1, (gid_t)-1) == 0, "chown -1 -1 succeeds");
        CHECK(chown(P("mode.txt"), uid + 1, gid) < 0 && errno == EPERM, "chown to someone else is refused");
        CHECK(chmod(P("mode.txt"), cur | 0111) < 0 && errno == EPERM, "adding execute bits is refused");
        CHECK(chmod(P("mode.txt"), 04755) < 0 && errno == EPERM, "setuid is refused");
        mode_t ro = cur & ~0222;
        CHECK(chmod(P("mode.txt"), ro) == 0 && stat(P("mode.txt"), &st) == 0 && (st.st_mode & 0777) == (ro & 0777), "read-only attribute via chmod");
        CHECK(chmod(P("mode.txt"), cur) == 0 && stat(P("mode.txt"), &st) == 0 && (st.st_mode & 07777) == cur, "and back");
    }
    CHECK(unlink(P("mode.txt")) == 0, "remove");
}

/* ---------------------------------------------------------------------------------------------------------------------
 * fstest -churn SEED DIR [OPS]: a write-heavy random workload for the power-cut test (tests/case_storage.py,
 * storage_crash_*), which kills the VM at a random moment and checks the image. It runs forever (or OPS operations) in
 * DIR/churn: creates, appends, overwrites, truncates, renames (to a new name, to another directory, over a file of the
 * same identity spelled alike or in another case, whole directories), deletes, mkdir/rmdir, bursts of small files and
 * one big file. Every name starts with "k<id>" (files) or "d<id>" (directories), and every byte of file k<id> is either 0
 * or churn_byte(id, offset) - also after renames - so the checker can tell a file that shows stale bytes (another file's,
 * or a directory's) from one that is merely shorter or longer than it was. Prints "churn N" after every 20 operations,
 * "CHURNFAIL ..." when a call fails for any reason but a full volume. */

#define CH_FILES 400
#define CH_DIRS  24
#define CH_BIG   (8u << 20)
#define CH_PATH  600

static unsigned char churn_byte(unsigned id, unsigned off) {
    unsigned x = off * 2654435761u + id * 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return (unsigned char)x;
}

static unsigned g_rng;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static unsigned rn(unsigned n) { return n ? rnd() % n : 0; }

typedef struct { char path[CH_PATH]; unsigned id, size; } ChFile;
static ChFile g_f[CH_FILES];
static int g_nf;
static char g_d[CH_DIRS][CH_PATH];                      /* g_d[0] is the churn root itself */
static int g_nd;
static unsigned g_next_id = 2, g_ops;
static unsigned char g_buf[65536];
static const char *g_spells[] = { "Marisa borrowed this", "Reimu's donation box ledger", "Sakuya time stop",
                                  "Patchouli's grimoire, volume", "Cirno is the strongest", "Remilia Scarlet" };

static void ch_fail(const char *what, const char *path) {
    printf("CHURNFAIL op %u: %s %s: %s\n", g_ops, what, path, strerror(errno));
}

/* A name for id in directory dir: 8.3 upper, 8.3 lower, or a long one (2 to 9 directory slots). */
static void ch_name(char *out, const char *dir, char kind, unsigned id, const char *ext) {
    switch (rn(3)) {
    case 0: snprintf(out, CH_PATH, "%s/%c%u.%s", dir, kind == 'k' ? 'K' : 'D', id, ext[0] == 'b' ? "BIN" : "DAT"); break;
    case 1: snprintf(out, CH_PATH, "%s/%c%u.%s", dir, kind, id, ext); break;
    default: {
        char pad[80];
        unsigned n = rn(70);
        for (unsigned i = 0; i < n; i++) pad[i] = (char)('a' + rn(26));
        pad[n] = 0;
        snprintf(out, CH_PATH, "%s/%c%u %s %s.%s", dir, kind, id, g_spells[rn(6)], pad, ext);
    }
    }
}

/* Writes churn bytes of `id` for [off, off+n) through fd, in pieces of random size. 1 = done, 0 = volume full, -1 = error. */
static int ch_put(int fd, unsigned id, unsigned off, unsigned n, const char *path) {
    if (lseek(fd, off, SEEK_SET) != (off_t)off) { ch_fail("lseek", path); return -1; }
    while (n) {
        unsigned take = 1 + rn(sizeof g_buf);
        if (take > n) take = n;
        for (unsigned i = 0; i < take; i++) g_buf[i] = churn_byte(id, off + i);
        ssize_t w = write(fd, g_buf, take);
        if (w < 0 && errno == ENOSPC) return 0;
        if (w <= 0) { ch_fail("write", path); return -1; }
        off += (unsigned)w;
        n -= (unsigned)w;
    }
    return 1;
}

static int ch_find_file(unsigned id_skip) {
    if (!g_nf) return -1;
    for (int tries = 0; tries < 8; tries++) {
        int k = (int)rn((unsigned)g_nf);
        if (g_f[k].id != id_skip) return k;
    }
    return -1;
}

static void ch_drop_file(int k) { g_f[k] = g_f[--g_nf]; }

static void ch_unlink(int k) {
    if (unlink(g_f[k].path) != 0) ch_fail("unlink", g_f[k].path);
    ch_drop_file(k);
}

static void ch_make_room(void) {                        /* volume full: delete a few files, the big one first */
    for (int k = 0; k < g_nf; k++)
        if (g_f[k].id == 1) { ch_unlink(k); return; }
    for (int i = 0; i < 6 && g_nf; i++) ch_unlink((int)rn((unsigned)g_nf));
}

static void ch_create(const char *dir, unsigned size, unsigned id) {
    if (g_nf >= CH_FILES) { ch_unlink((int)rn((unsigned)g_nf)); return; }
    ChFile *f = &g_f[g_nf];
    ch_name(f->path, dir, 'k', id, id == 1 ? "bin" : "txt");
    int fd = open(f->path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { if (errno == ENOSPC) ch_make_room(); else if (errno != EEXIST) ch_fail("create", f->path); return; }
    f->id = id;
    f->size = 0;
    g_nf++;
    int r = ch_put(fd, id, 0, size, f->path);
    close(fd);
    struct stat st;
    if (stat(f->path, &st) == 0) f->size = (unsigned)st.st_size;
    if (r == 0) ch_make_room();
}

static unsigned ch_size(void) {
    switch (rn(6)) {
    case 0: return 0;
    case 1: return 1 + rn(600);
    case 2: return 4096 * (1 + rn(4)) + rn(3) - 1;                 /* cluster boundaries */
    case 3: return 1 + rn(40000);
    default: return 1 + rn(6000);
    }
}

/* Replaces the prefix `from` of every model path with `to` (a directory moved). */
static void ch_reprefix(const char *from, const char *to) {
    size_t fl = strlen(from);
    char tmp[CH_PATH];
    for (int k = 0; k < g_nf; k++)
        if (!strncmp(g_f[k].path, from, fl) && g_f[k].path[fl] == '/') {
            snprintf(tmp, sizeof tmp, "%s%s", to, g_f[k].path + fl);
            strcpy(g_f[k].path, tmp);
        }
    for (int k = 1; k < g_nd; k++)
        if (!strncmp(g_d[k], from, fl) && (g_d[k][fl] == '/' || !g_d[k][fl])) {
            snprintf(tmp, sizeof tmp, "%s%s", to, g_d[k] + fl);
            strcpy(g_d[k], tmp);
        }
}

/* Would every path below `from` still fit (with room for a file name) once it lives below `to`? */
static int ch_fits(const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to);
    for (int k = 1; k < g_nd; k++)
        if (!strncmp(g_d[k], from, fl) && (g_d[k][fl] == '/' || !g_d[k][fl]) && tl + strlen(g_d[k]) - fl > CH_PATH - 200) return 0;
    return 1;
}

static int ch_dir_empty(const char *d) {
    for (int k = 0; k < g_nf; k++)
        if (!strncmp(g_f[k].path, d, strlen(d)) && g_f[k].path[strlen(d)] == '/') return 0;
    for (int k = 1; k < g_nd; k++)
        if (!strncmp(g_d[k], d, strlen(d)) && g_d[k][strlen(d)] == '/') return 0;
    return 1;
}

static void ch_step(void) {
    unsigned r = rn(100);
    const char *dir = g_d[rn((unsigned)g_nd)];
    char np[CH_PATH];
    int k;
    if (r < 22 || g_nf < 4) {                                       /* create */
        ch_create(dir, ch_size(), g_next_id++);
    } else if (r < 34) {                                            /* append */
        if ((k = ch_find_file(1)) < 0) return;
        int fd = open(g_f[k].path, O_WRONLY);
        if (fd < 0) { ch_fail("open", g_f[k].path); return; }
        int res = ch_put(fd, g_f[k].id, g_f[k].size, 1 + rn(20000), g_f[k].path);
        close(fd);
        struct stat st;
        if (stat(g_f[k].path, &st) == 0) g_f[k].size = (unsigned)st.st_size;
        if (res == 0) ch_make_room();
    } else if (r < 42) {                                            /* overwrite inside (or across the end of) a file */
        if ((k = ch_find_file(0)) < 0 || !g_f[k].size) return;
        unsigned off = rn(g_f[k].size), n = 1 + rn(9000);
        int fd = open(g_f[k].path, O_WRONLY);
        if (fd < 0) { ch_fail("open", g_f[k].path); return; }
        int res = ch_put(fd, g_f[k].id, off, n, g_f[k].path);
        close(fd);
        struct stat st;
        if (stat(g_f[k].path, &st) == 0) g_f[k].size = (unsigned)st.st_size;
        if (res == 0) ch_make_room();
    } else if (r < 48) {                                            /* truncate: shrink or grow */
        if ((k = ch_find_file(0)) < 0) return;
        unsigned ns = rn(2) ? rn(g_f[k].size + 1) : g_f[k].size + rn(20000);
        if (truncate(g_f[k].path, ns) != 0) { if (errno == ENOSPC) ch_make_room(); else ch_fail("truncate", g_f[k].path); return; }
        g_f[k].size = ns;
    } else if (r < 59) {                                            /* delete */
        if ((k = ch_find_file(0)) >= 0) ch_unlink(k);
    } else if (r < 65) {                                            /* rename in place, or move to another directory */
        if ((k = ch_find_file(0)) < 0) return;
        ch_name(np, rn(2) ? dir : g_d[0], 'k', g_f[k].id, g_f[k].id == 1 ? "bin" : "txt");
        if (rename(g_f[k].path, np) != 0) { if (errno != EEXIST && errno != ENOSPC) ch_fail("rename", np); return; }
        for (int j = 0; j < g_nf; j++)                              /* the name may have replaced a file of the same identity */
            if (j != k && !strcasecmp(g_f[j].path, np)) { ch_drop_file(j); if (k == g_nf) k = j; break; }
        strcpy(g_f[k].path, np);
    } else if (r < 75) {                                            /* the editor's save: a new copy, renamed over the old one */
        if ((k = ch_find_file(1)) < 0) return;
        unsigned id = g_f[k].id;
        char target[CH_PATH];
        strcpy(target, g_f[k].path);
        if (rn(3) == 0) {                                           /* the same name in another case: a different spelling */
            char *base = strrchr(target, '/') + 1;
            for (char *p = base; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32); else if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        }
        char tmp[CH_PATH];
        snprintf(tmp, sizeof tmp, "%.*s/k%u.tmp", (int)(strrchr(g_f[k].path, '/') - g_f[k].path), g_f[k].path, id);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { if (errno == ENOSPC) ch_make_room(); else ch_fail("create", tmp); return; }
        int res = ch_put(fd, id, 0, ch_size(), tmp);
        close(fd);
        if (res <= 0) { unlink(tmp); if (!res) ch_make_room(); return; }
        if (rename(tmp, target) != 0) { ch_fail("rename over", target); unlink(tmp); return; }
        strcpy(g_f[k].path, target);
        struct stat st;
        if (stat(target, &st) == 0) g_f[k].size = (unsigned)st.st_size;
    } else if (r < 79) {                                            /* mkdir */
        if (g_nd >= CH_DIRS) return;
        ch_name(np, dir, 'd', g_next_id++, "dir");
        if (strlen(np) > CH_PATH - 200) return;
        if (mkdir(np, 0755) != 0) { if (errno != ENOSPC) ch_fail("mkdir", np); return; }
        strcpy(g_d[g_nd++], np);
    } else if (r < 82) {                                            /* rmdir */
        if (g_nd < 2) return;
        k = 1 + (int)rn((unsigned)g_nd - 1);
        if (!ch_dir_empty(g_d[k])) return;
        if (rmdir(g_d[k]) != 0) { ch_fail("rmdir", g_d[k]); return; }
        strcpy(g_d[k], g_d[--g_nd]);
    } else if (r < 85) {                                            /* move or rename a whole directory */
        if (g_nd < 2) return;
        k = 1 + (int)rn((unsigned)g_nd - 1);
        size_t kl = strlen(g_d[k]);
        if (!strncmp(dir, g_d[k], kl) && (dir[kl] == '/' || !dir[kl])) return;     /* not into itself */
        ch_name(np, dir, 'd', g_next_id++, "dir");
        if (strlen(np) > CH_PATH - 200 || !ch_fits(g_d[k], np)) return;
        if (rename(g_d[k], np) != 0) { if (errno != ENOSPC) ch_fail("move dir", np); return; }
        char from[CH_PATH];
        strcpy(from, g_d[k]);
        ch_reprefix(from, np);
    } else if (r < 91) {                                            /* the big file grows by 64..256 KiB, or goes */
        int b = -1;
        for (int j = 0; j < g_nf; j++) if (g_f[j].id == 1) b = j;
        if (b < 0) { ch_create(g_d[0], 65536, 1); return; }
        if (g_f[b].size >= CH_BIG) {
            if (rn(2)) ch_unlink(b);
            else if (truncate(g_f[b].path, rn(300000)) == 0) { struct stat st; if (stat(g_f[b].path, &st) == 0) g_f[b].size = (unsigned)st.st_size; }
            else ch_fail("truncate big", g_f[b].path);
            return;
        }
        int fd = open(g_f[b].path, O_WRONLY);
        if (fd < 0) { ch_fail("open", g_f[b].path); return; }
        int res = ch_put(fd, 1, g_f[b].size, 65536 + rn(196608), g_f[b].path);
        close(fd);
        struct stat st;
        if (stat(g_f[b].path, &st) == 0) g_f[b].size = (unsigned)st.st_size;
        if (res == 0) ch_make_room();
    } else if (r < 96) {                                            /* a burst of small files */
        unsigned n = 10 + rn(20);
        for (unsigned i = 0; i < n; i++) ch_create(dir, 1 + rn(2000), g_next_id++);
    } else if (r < 98) {
        sync();
    } else {                                                        /* a burst of deletes */
        for (int i = 0; i < 10 && g_nf > 4; i++) ch_unlink((int)rn((unsigned)g_nf));
    }
}

static int churn(unsigned seed, const char *dir, unsigned ops) {
    g_rng = seed ? seed : 1;
    for (int i = 0; i < 10; i++) rnd();
    snprintf(g_d[0], CH_PATH, "%s/churn", dir);
    if (mkdir(g_d[0], 0755) != 0 && errno != EEXIST) { ch_fail("mkdir", g_d[0]); return 1; }
    g_nd = 1;
    printf("churn start seed %u\n", seed);
    fflush(stdout);
    for (g_ops = 1; !ops || g_ops <= ops; g_ops++) {
        ch_step();
        if (g_ops % 20 == 0) { printf("churn %u\n", g_ops); fflush(stdout); }
    }
    printf("churn done\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "-churn"))
        return churn((unsigned)strtoul(argv[2], NULL, 0), argv[3], argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 0);
    if (argc != 2) { fprintf(stderr, "usage: fstest DIR | fstest -churn SEED DIR [OPS]\n"); return 2; }
    snprintf(g_root, sizeof g_root, "%s/fst-%d", argv[1], (int)getpid());
    if (mkdir(g_root, 0755) != 0) { printf("FAIL: cannot create %s: %s\n", g_root, strerror(errno)); return 1; }
    t_basic();
    t_sparse_append_truncate();
    t_boundaries();
    t_unlink_rename_while_open();
    t_dirs_and_renames();
    t_names();
    t_many();
    t_modes();
    int left = count_dir(g_root, NULL, NULL);
    CHECK(left == 0, "%d entries left behind", left);
    sync();
    rmdir(g_root);
    if (g_fail) printf("FSTEST FAIL %d of %d checks\n", g_fail, g_checks);
    else printf("FSTEST PASS (%d checks)\n", g_checks);
    return g_fail > 100 ? 100 : g_fail;
}
