/* `explorer --selftest`: tests of the Explorer's parts that need no window, run on the console: paths, formatting, file types,
 * the folder model (sliced scanning, sorting, filtering) and the whole job engine (copy, move inside and across volumes, name
 * conflicts, the Netherworld with its notes, delete, measure, cancel, the queue). Scratch files live in /tmp and the home folder
 * (two different volumes, so the move-by-copy path really runs) and in /Netherworld under names starting with "ex-selftest-". */
#include "explorer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static int g_checks, g_fail;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- helpers */

static void mkdirp(const char *path) {
    char buf[EX_PATH];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static void put(const char *path, const void *data, size_t len, unsigned mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { printf("cannot create %s: %s\n", path, strerror(errno)); return; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, (const char *)data + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
}

static void put_pattern(const char *path, size_t size, unsigned seed) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    unsigned char buf[4096];
    unsigned x = seed * 2654435761u + 12345;
    size_t done = 0;
    while (done < size) {
        size_t n = size - done < sizeof buf ? size - done : sizeof buf;
        for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; buf[i] = (unsigned char)(x >> 24); }
        if (write(fd, buf, n) != (ssize_t)n) break;
        done += n;
    }
    close(fd);
}

static bool same_file(const char *a, const char *b) {
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    bool same = fa >= 0 && fb >= 0;
    unsigned char x[4096], y[4096];
    while (same) {
        ssize_t n = read(fa, x, sizeof x);
        ssize_t m = 0, got = 0;
        while (got < n) { m = read(fb, y + got, (size_t)(n - got)); if (m <= 0) break; got += m; }
        if (n < 0 || got != (n > 0 ? n : 0)) { same = false; break; }
        if (n == 0) { m = read(fb, y, 1); same = m == 0; break; }
        if (memcmp(x, y, (size_t)n)) same = false;
    }
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return same;
}

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list_names(const char *dir, char ***out) {
    DIR *dp = opendir(dir);
    if (!dp) { *out = NULL; return -1; }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *d;
    while ((d = readdir(dp))) {
        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, (size_t)cap * sizeof *names); }
        names[n++] = strdup(d->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)n, sizeof *names, cmp_names);
    *out = names;
    return n;
}

static void free_names(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* Same names, types, contents, link targets and permission bits below a and b? */
static bool same_tree(const char *a, const char *b) {
    char **na, **nb;
    int ca = list_names(a, &na), cb = list_names(b, &nb);
    bool same = ca == cb && ca >= 0;
    for (int i = 0; same && i < ca; i++) {
        if (strcmp(na[i], nb[i])) { same = false; break; }
        char pa[EX_PATH], pb[EX_PATH];
        path_join(pa, sizeof pa, a, na[i]);
        path_join(pb, sizeof pb, b, nb[i]);
        struct stat sa, sb;
        if (lstat(pa, &sa) || lstat(pb, &sb) || (sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) { same = false; break; }
        if (S_ISDIR(sa.st_mode)) same = same_tree(pa, pb);
        else if (S_ISLNK(sa.st_mode)) {
            char ta[EX_PATH], tb[EX_PATH];
            ssize_t la = readlink(pa, ta, sizeof ta - 1), lb = readlink(pb, tb, sizeof tb - 1);
            same = la >= 0 && la == lb && !memcmp(ta, tb, (size_t)la);
        } else same = same_file(pa, pb) && (sa.st_mode & 0777) == (sb.st_mode & 0777) && sa.st_size == sb.st_size;
    }
    free_names(na, ca > 0 ? ca : 0);
    free_names(nb, cb > 0 ? cb : 0);
    return same;
}

static void rm_rf(const char *p) {
    struct stat st;
    if (lstat(p, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        chmod(p, 0755);
        char **names;
        int n = list_names(p, &names);
        for (int i = 0; i < n; i++) {
            char c[EX_PATH];
            path_join(c, sizeof c, p, names[i]);
            rm_rf(c);
        }
        free_names(names, n > 0 ? n : 0);
        rmdir(p);
    } else unlink(p);
}

static ConflictPolicy g_ans = CF_KEEP;
static bool g_ans_all;
static int g_asked;

static void drive(Job *j) {
    for (int guard = 0; guard < 400000; guard++) {
        if (job_run(j, 3)) return;
        JobInfo ji;
        job_info(j, &ji);
        if (ji.waiting) { g_asked++; job_resolve(j, g_ans, g_ans_all); }
    }
    CHECK(0, "a job did not finish");
}

static Job *mk(OpKind k, const char *dest, ConflictPolicy pol, int n, const char *const *srcs) {
    Job *j = job_new(k, dest);
    job_set_policy(j, pol);
    for (int i = 0; i < n; i++) job_add(j, srcs[i]);
    return j;
}

static int errors_of(Job *j) { JobInfo ji; job_info(j, &ji); return ji.nerr; }

/* ---------------------------------------------------------------- tests */

static void test_paths(void) {
    char out[EX_PATH], saved_home[EX_PATH];
    const char *h = getenv("HOME");
    snprintf(saved_home, sizeof saved_home, "%s", h ? h : "");
    CHECK(path_normalize("a/../b/./c//d", "/x/y", out, sizeof out) && !strcmp(out, "/x/y/b/c/d"), "normalize relative: %s", out);
    CHECK(path_normalize("/..", "/x", out, sizeof out) && !strcmp(out, "/"), "normalize /..: %s", out);
    CHECK(path_normalize("/a/b/../../..", "/", out, sizeof out) && !strcmp(out, "/"), "normalize beyond the root: %s", out);
    CHECK(path_normalize("/a//b/", "/", out, sizeof out) && !strcmp(out, "/a/b"), "normalize slashes: %s", out);
    setenv("HOME", "/HumanVillage/reimu", 1);
    CHECK(path_normalize("~/z", "/", out, sizeof out) && !strcmp(out, "/HumanVillage/reimu/z"), "normalize ~: %s", out);
    setenv("HOME", saved_home, 1);
    path_parent("/a/b", out, sizeof out); CHECK(!strcmp(out, "/a"), "parent of /a/b: %s", out);
    path_parent("/a", out, sizeof out); CHECK(!strcmp(out, "/"), "parent of /a: %s", out);
    path_parent("/", out, sizeof out); CHECK(!strcmp(out, "/"), "parent of /: %s", out);
    path_parent("/a/b/", out, sizeof out); CHECK(!strcmp(out, "/a"), "parent of /a/b/: %s", out);
    CHECK(!strcmp(path_base("/a/b.txt"), "b.txt") && !strcmp(path_base("/"), "/"), "basename");
    CHECK(path_inside("/a/b", "/a") && path_inside("/a", "/a") && !path_inside("/ab", "/a") && path_inside("/x", "/"), "path_inside");
    path_join(out, sizeof out, "/", "x"); CHECK(!strcmp(out, "/x"), "join root: %s", out);
    path_join(out, sizeof out, "/a", "x"); CHECK(!strcmp(out, "/a/x"), "join: %s", out);
}

static void test_format(void) {
    char b[64];
    fmt_size(b, sizeof b, 0); CHECK(!strcmp(b, "0 bytes"), "size 0: %s", b);
    fmt_size(b, sizeof b, 1); CHECK(!strcmp(b, "1 byte"), "size 1: %s", b);
    fmt_size(b, sizeof b, 1023); CHECK(!strcmp(b, "1023 bytes"), "size 1023: %s", b);
    fmt_size(b, sizeof b, 1536); CHECK(!strcmp(b, "1.5 KB"), "size 1536: %s", b);
    fmt_size(b, sizeof b, 5 * 1024 * 1024); CHECK(!strcmp(b, "5.0 MB"), "size 5M: %s", b);
    fmt_mode(b, 0100644); CHECK(!strcmp(b, "-rw-r--r--"), "mode: %s", b);
    fmt_mode(b, 040755); CHECK(!strcmp(b, "drwxr-xr-x"), "dir mode: %s", b);
    fmt_mode(b, 041777); CHECK(!strcmp(b, "drwxrwxrwt"), "sticky mode: %s", b);
    fmt_mode(b, 0104755); CHECK(!strcmp(b, "-rwsr-xr-x"), "setuid mode: %s", b);
    sh_quote(b, sizeof b, "it's"); CHECK(!strcmp(b, "'it'\\''s'"), "quote: %s", b);
    sh_quote(b, sizeof b, "a b"); CHECK(!strcmp(b, "'a b'"), "quote space: %s", b);
    const char *why = NULL;
    CHECK(!name_valid("", &why) && !name_valid("a/b", &why) && !name_valid("..", &why) && name_valid("ok name.txt", &why), "name_valid");
    CHECK(natcmp("file2", "file10") < 0 && natcmp("file10", "file2") > 0 && natcmp("a", "a") == 0 && natcmp("a1", "a01") != 0, "natcmp");
    CHECK(natcmp("img9.png", "img10.png") < 0 && natcmp("abc", "abd") < 0 && natcmp("ab", "abc") < 0, "natcmp more");
    CHECK(classify("a.ppm", 0100644, EK_FILE) == FT_IMAGE && classify("a.BMP", 0100644, EK_FILE) == FT_IMAGE, "classify image");
    CHECK(classify("a.c", 0100644, EK_FILE) == FT_TEXT && classify("Makefile", 0100644, EK_FILE) == FT_TEXT && classify(".gaprc", 0100644, EK_FILE) == FT_TEXT, "classify text");
    CHECK(classify("run", 0100755, EK_FILE) == FT_PROGRAM && classify("x.sh", 0100755, EK_FILE) == FT_PROGRAM && classify("x.txt", 0100755, EK_FILE) == FT_TEXT, "classify program");
    CHECK(classify("a.tar", 0100644, EK_FILE) == FT_ARCHIVE && classify("blob", 0100644, EK_FILE) == FT_GENERIC, "classify other");
    CHECK(classify("tty", 020666, EK_OTHER) == FT_DEVICE, "classify device");
}

static void test_names(const char *root) {
    char d[EX_PATH], out[EX_NAME], p[EX_PATH];
    path_join(d, sizeof d, root, "names");
    mkdirp(d);
    unique_name(d, "New Folder", out, sizeof out, "new");
    CHECK(!strcmp(out, "New Folder"), "unique new: %s", out);
    path_join(p, sizeof p, d, "New Folder"); mkdir(p, 0755);
    unique_name(d, "New Folder", out, sizeof out, "new");
    CHECK(!strcmp(out, "New Folder (2)"), "unique new 2: %s", out);
    path_join(p, sizeof p, d, "a.txt"); put(p, "x", 1, 0644);
    unique_name(d, "a.txt", out, sizeof out, "copy");
    CHECK(!strcmp(out, "a (copy).txt"), "unique copy: %s", out);
    path_join(p, sizeof p, d, "a (copy).txt"); put(p, "x", 1, 0644);
    unique_name(d, "a.txt", out, sizeof out, "copy");
    CHECK(!strcmp(out, "a (copy 2).txt"), "unique copy 2: %s", out);
    unique_name(d, "a.txt", out, sizeof out, "num");
    CHECK(!strcmp(out, "a (2).txt"), "unique num: %s", out);
    unique_name(d, "noext", out, sizeof out, "copy");
    CHECK(!strcmp(out, "noext (copy)"), "unique copy no ext: %s", out);
}

static void test_model(const char *root) {
    char d[EX_PATH], p[EX_PATH];
    path_join(d, sizeof d, root, "model");
    mkdirp(d);
    for (int i = 1; i <= 120; i++) {
        char n[64];
        snprintf(n, sizeof n, "file%d.txt", i);
        path_join(p, sizeof p, d, n);
        put_pattern(p, (size_t)(i * 7 % 50), (unsigned)i);
    }
    path_join(p, sizeof p, d, "Zeta"); mkdir(p, 0755);
    path_join(p, sizeof p, d, "alpha"); mkdir(p, 0755);
    path_join(p, sizeof p, d, ".hidden"); put(p, "h", 1, 0644);
    path_join(p, sizeof p, d, "prog"); put(p, "#!/bin/sh\n", 10, 0755);
    path_join(p, sizeof p, d, "link"); symlink("alpha", p);
    path_join(p, sizeof p, d, "dead"); symlink("nothing-here", p);
    Model m;
    model_init(&m);
    CHECK(model_open(&m, d), "model_open");
    int steps = 0;
    while (!model_scan_step(&m, 0)) { steps++; if (steps > 100000) break; }
    CHECK(steps >= 10, "the scan was sliced into %d steps", steps + 1);
    CHECK(m.nall == 120 + 2 + 1 + 1 + 2 && m.nshown == m.nall - 1 && m.nhidden == 1, "counts all=%d shown=%d hidden=%d", m.nall, m.nshown, m.nhidden);
    CHECK(m.shown[0]->is_dir && m.shown[1]->is_dir && m.shown[2]->is_dir, "folders (and the link to one) come first");
    CHECK(!strcmp(m.shown[0]->name, "alpha") && !strcmp(m.shown[1]->name, "link") && !strcmp(m.shown[2]->name, "Zeta"), "folder order: %s %s %s",
          m.shown[0]->name, m.shown[1]->name, m.shown[2]->name);
    int f1 = model_find(&m, "file1.txt"), f2 = model_find(&m, "file2.txt"), f10 = model_find(&m, "file10.txt");
    CHECK(f1 >= 0 && f1 < f2 && f2 < f10, "natural order file1 < file2 < file10 (%d %d %d)", f1, f2, f10);
    int pr = model_find(&m, "prog"), dd = model_find(&m, "dead");
    CHECK(pr >= 0 && m.shown[pr]->ftype == FT_PROGRAM, "prog is a program");
    CHECK(dd >= 0 && m.shown[dd]->broken && m.shown[dd]->kind == EK_LINK, "dead link is broken");
    CHECK(model_find(&m, "link") >= 0 && m.shown[model_find(&m, "link")]->is_dir, "link to a folder behaves as a folder");
    CHECK(model_find(&m, ".hidden") < 0, ".hidden is filtered out");
    m.show_hidden = true;
    model_refilter(&m);
    CHECK(model_find(&m, ".hidden") >= 0 && m.nshown == m.nall, "hidden files shown");
    m.show_hidden = false;
    model_refilter(&m);
    m.sort_col = COL_SIZE;
    model_sort(&m);
    int first_file = -1;
    for (int i = 0; i < m.nshown; i++) if (!m.shown[i]->is_dir) { first_file = i; break; }
    bool asc = true;
    for (int i = first_file + 1; i < m.nshown; i++) if (m.shown[i]->size < m.shown[i - 1]->size) asc = false;
    CHECK(first_file == 3 && asc, "sort by size ascending (first file at %d)", first_file);
    m.sort_desc = true;
    model_sort(&m);
    bool desc = true;
    for (int i = first_file + 1; i < m.nshown; i++) if (m.shown[i]->size > m.shown[i - 1]->size) desc = false;
    CHECK(desc && m.shown[0]->is_dir, "sort by size descending keeps folders first");
    m.sort_col = COL_NAME;
    m.sort_desc = false;
    model_sort(&m);
    int pf = model_prefix(&m, "file1", 0, true);
    CHECK(pf >= 0 && !strncmp(m.shown[pf]->name, "file1", 5), "type-ahead file1 -> %d", pf);
    int pz = model_prefix(&m, "z", 0, true);
    CHECK(pz >= 0 && !strcmp(m.shown[pz]->name, "Zeta"), "type-ahead is case-blind");
    model_select_all(&m);
    int64_t bytes = 0;
    CHECK(model_sel_count(&m, &bytes) == m.nshown && bytes > 0, "select all");
    model_select_range(&m, 2, 5, true, true);
    CHECK(model_sel_count(&m, NULL) == 4, "select range");
    model_select_none(&m);
    CHECK(model_sel_count(&m, NULL) == 0, "select none");
    model_clear(&m);
    CHECK(!model_open(&m, "/nonexistent-ex-folder") && m.err == ENOENT, "opening a missing folder fails with ENOENT");
}

static void test_copy(const char *root) {
    char src[EX_PATH], dst[EX_PATH], p[EX_PATH];
    path_join(src, sizeof src, root, "tree");
    path_join(dst, sizeof dst, root, "copies");
    mkdirp(src);
    mkdirp(dst);
    static const size_t sizes[] = { 0, 1, 4095, 65535, 65536, 65537, 200000 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        char n[32];
        snprintf(n, sizeof n, "f%u.bin", i);
        path_join(p, sizeof p, src, n);
        put_pattern(p, sizes[i], i + 1);
    }
    path_join(p, sizeof p, src, "sub/deeper/deepest"); mkdirp(p);
    path_join(p, sizeof p, src, "sub/deeper/deepest/x.txt"); put(p, "deep\n", 5, 0600);
    path_join(p, sizeof p, src, "sub/run.sh"); put(p, "#!/bin/sh\necho hi\n", 18, 0755);
    path_join(p, sizeof p, src, "sub/empty"); mkdirp(p);
    path_join(p, sizeof p, src, "sub/link"); symlink("../f1.bin", p);
    path_join(p, sizeof p, src, "name with spaces & 'quotes'.txt"); put(p, "q", 1, 0644);
    path_join(p, sizeof p, src, "f1.bin");
    struct utimbuf ub = { 1700000000, 1700000000 };
    utime(p, &ub);

    const char *one[1] = { src };
    Job *j = mk(OP_COPY, dst, CF_ASK, 1, one);
    int calls = 0;
    while (!job_run(j, 0)) { calls++; if (calls > 100000) break; }
    CHECK(calls >= 10, "a copy of a tree takes many small steps (%d)", calls);
    CHECK(errors_of(j) == 0 && job_ok_count(j) == 1, "copy of a tree: errors=%d ok=%d", errors_of(j), job_ok_count(j));
    JobInfo ji;
    job_info(j, &ji);
    CHECK(ji.files == 11 && ji.dirs == 5 && ji.bytes_done == 0 + 1 + 4095 + 65535 + 65536 + 65537 + 200000 + 5 + 18 + 1,
          "counters files=%lld dirs=%lld bytes=%lld", (long long)ji.files, (long long)ji.dirs, (long long)ji.bytes_done);
    job_free(j);
    char copy[EX_PATH];
    path_join(copy, sizeof copy, dst, "tree");
    CHECK(same_tree(src, copy), "the copy equals the original");
    struct stat st;
    path_join(p, sizeof p, copy, "f1.bin");
    CHECK(stat(p, &st) == 0 && st.st_mtime == 1700000000, "the copy keeps the modification time (%lld)", (long long)st.st_mtime);
    path_join(p, sizeof p, copy, "sub/run.sh");
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0755, "the copy keeps permissions (%o)", st.st_mode & 0777);

    /* a copy into the same folder gets a new name */
    path_join(p, sizeof p, src, "f2.bin");
    const char *f2[1] = { p };
    j = mk(OP_COPY, src, CF_ASK, 1, f2);
    drive(j);
    CHECK(errors_of(j) == 0 && g_asked == 0, "copy into the same folder: errors=%d asked=%d", errors_of(j), g_asked);
    job_free(j);
    path_join(p, sizeof p, src, "f2 (copy).bin");
    CHECK(stat(p, &st) == 0 && st.st_size == 4095, "f2 (copy).bin exists");

    /* a folder cannot be copied into itself */
    path_join(p, sizeof p, src, "sub");
    const char *sub[1] = { p };
    j = mk(OP_COPY, p, CF_ASK, 1, sub);
    drive(j);
    CHECK(errors_of(j) == 1, "copying a folder into itself is refused (errors=%d)", errors_of(j));
    job_free(j);
    char deep[EX_PATH];
    path_join(deep, sizeof deep, src, "sub/deeper");
    j = mk(OP_COPY, deep, CF_ASK, 1, sub);
    drive(j);
    CHECK(errors_of(j) == 1, "...or into something inside it (errors=%d)", errors_of(j));
    job_free(j);
    path_join(p, sizeof p, src, "sub/deeper/sub");
    CHECK(lstat(p, &st) != 0, "nothing was created by the refused copies");
}

static void test_conflicts(const char *root) {
    char a[EX_PATH], b[EX_PATH], p[EX_PATH], q[EX_PATH];
    path_join(a, sizeof a, root, "ca");
    path_join(b, sizeof b, root, "cb");
    mkdirp(a);
    mkdirp(b);
    path_join(p, sizeof p, a, "same.txt"); put(p, "from A", 6, 0644);
    path_join(q, sizeof q, b, "same.txt"); put(q, "from B (old)", 12, 0644);
    char buf[64];
    const char *one[1] = { p };
    /* skip */
    Job *j = mk(OP_COPY, b, CF_SKIP, 1, one);
    drive(j);
    JobInfo ji; job_info(j, &ji);
    CHECK(ji.nskipped == 1 && errors_of(j) == 0, "skip policy: skipped=%d", ji.nskipped);
    job_free(j);
    int fd = open(q, O_RDONLY); ssize_t n = read(fd, buf, sizeof buf); close(fd);
    CHECK(n == 12 && !memcmp(buf, "from B (old)", 12), "skip kept the old file");
    /* replace */
    j = mk(OP_COPY, b, CF_REPLACE, 1, one);
    drive(j);
    CHECK(errors_of(j) == 0, "replace policy: errors=%d", errors_of(j));
    job_free(j);
    fd = open(q, O_RDONLY); n = read(fd, buf, sizeof buf); close(fd);
    CHECK(n == 6 && !memcmp(buf, "from A", 6), "replace overwrote the file");
    /* keep both */
    j = mk(OP_COPY, b, CF_KEEP, 1, one);
    drive(j);
    job_free(j);
    path_join(q, sizeof q, b, "same (2).txt");
    CHECK(path_exists(q), "keep both made \"same (2).txt\"");
    /* ask: the job waits for an answer */
    j = mk(OP_COPY, b, CF_ASK, 1, one);
    CHECK(!job_run(j, 50), "job runs");
    job_info(j, &ji);
    CHECK(ji.waiting && ji.c_dst[0] && !ji.c_dst_dir, "the job asks and says what is in the way: %s", ji.c_dst);
    CHECK(!job_run(j, 20), "a waiting job makes no progress");
    job_resolve(j, CF_KEEP, false);
    drive(j);
    job_free(j);
    path_join(q, sizeof q, b, "same (3).txt");
    CHECK(path_exists(q), "ask + keep both made \"same (3).txt\"");
    /* apply to all */
    path_join(p, sizeof p, a, "m1.txt"); put(p, "1", 1, 0644);
    path_join(p, sizeof p, a, "m2.txt"); put(p, "2", 1, 0644);
    path_join(q, sizeof q, b, "m1.txt"); put(q, "old1", 4, 0644);
    path_join(q, sizeof q, b, "m2.txt"); put(q, "old2", 4, 0644);
    path_join(p, sizeof p, a, "m1.txt");
    char p2[EX_PATH];
    path_join(p2, sizeof p2, a, "m2.txt");
    const char *two[2] = { p, p2 };
    g_asked = 0;
    g_ans = CF_REPLACE;
    g_ans_all = true;
    j = mk(OP_COPY, b, CF_ASK, 2, two);
    drive(j);
    job_free(j);
    CHECK(g_asked == 1, "apply to all: asked only %d time(s)", g_asked);
    path_join(q, sizeof q, b, "m2.txt");
    fd = open(q, O_RDONLY); n = read(fd, buf, sizeof buf); close(fd);
    CHECK(n == 1 && buf[0] == '2', "apply to all replaced the second one too");
    g_ans = CF_KEEP;
    g_ans_all = false;
    /* merging folders */
    char fa[EX_PATH], fb[EX_PATH];
    path_join(fa, sizeof fa, a, "dir"); mkdirp(fa);
    path_join(fb, sizeof fb, b, "dir"); mkdirp(fb);
    path_join(p, sizeof p, fa, "only-a"); put(p, "a", 1, 0644);
    path_join(p, sizeof p, fa, "both"); put(p, "A", 1, 0644);
    path_join(p, sizeof p, fb, "only-b"); put(p, "b", 1, 0644);
    path_join(p, sizeof p, fb, "both"); put(p, "B", 1, 0644);
    const char *dirs[1] = { fa };
    g_asked = 0;
    j = mk(OP_COPY, b, CF_REPLACE, 1, dirs);
    drive(j);
    CHECK(errors_of(j) == 0 && g_asked == 0, "merge: errors=%d", errors_of(j));
    job_free(j);
    path_join(p, sizeof p, fb, "only-a");
    CHECK(path_exists(p), "merge brought only-a");
    path_join(p, sizeof p, fb, "only-b");
    CHECK(path_exists(p), "merge kept only-b");
    path_join(p, sizeof p, fb, "both");
    fd = open(p, O_RDONLY); n = read(fd, buf, sizeof buf); close(fd);
    CHECK(n == 1 && buf[0] == 'A', "merge replaced the clashing file");
    /* a folder cannot replace a file */
    path_join(p, sizeof p, b, "clash"); put(p, "file", 4, 0644);
    path_join(q, sizeof q, a, "clash"); mkdirp(q);
    const char *cl[1] = { q };
    j = mk(OP_COPY, b, CF_REPLACE, 1, cl);
    drive(j);
    CHECK(errors_of(j) == 1, "a folder cannot replace a file (errors=%d)", errors_of(j));
    job_free(j);
}

static void test_move(const char *root, const char *other_root) {
    char src[EX_PATH], keep[EX_PATH], dst[EX_PATH], p[EX_PATH];
    /* same volume: rename */
    path_join(src, sizeof src, root, "mv-src");
    path_join(keep, sizeof keep, root, "mv-keep");
    path_join(dst, sizeof dst, root, "mv-dst");
    mkdirp(src); mkdirp(dst);
    path_join(p, sizeof p, src, "a/b"); mkdirp(p);
    path_join(p, sizeof p, src, "a/b/data.bin"); put_pattern(p, 100000, 9);
    path_join(p, sizeof p, src, "a/top.txt"); put(p, "top", 3, 0644);
    mkdirp(keep);
    char kept[EX_PATH];
    path_join(kept, sizeof kept, keep, "a");
    char a_dir[EX_PATH];
    path_join(a_dir, sizeof a_dir, src, "a");
    /* make a reference copy first */
    const char *one[1] = { a_dir };
    Job *j = mk(OP_COPY, keep, CF_ASK, 1, one);
    drive(j);
    job_free(j);
    j = mk(OP_MOVE, dst, CF_ASK, 1, one);
    drive(j);
    CHECK(errors_of(j) == 0 && job_ok_count(j) == 1, "move in one volume: errors=%d", errors_of(j));
    job_free(j);
    char moved[EX_PATH];
    path_join(moved, sizeof moved, dst, "a");
    CHECK(!path_exists(a_dir) && same_tree(moved, kept), "moved tree is complete and the source is gone");
    /* moving into the folder it is in does nothing */
    const char *mv[1] = { moved };
    j = mk(OP_MOVE, dst, CF_ASK, 1, mv);
    drive(j);
    CHECK(errors_of(j) == 0 && path_exists(moved), "move into its own folder is a no-op");
    job_free(j);
    /* a folder cannot move into itself */
    char inner[EX_PATH];
    path_join(inner, sizeof inner, moved, "b");
    j = mk(OP_MOVE, inner, CF_ASK, 1, mv);
    drive(j);
    CHECK(errors_of(j) == 1 && path_exists(moved), "move into itself refused (errors=%d)", errors_of(j));
    job_free(j);

    /* another volume: copy, then remove each source (EXDEV) */
    struct stat sa, sb;
    bool cross = stat(root, &sa) == 0 && stat(other_root, &sb) == 0 && sa.st_dev != sb.st_dev;
    printf("  (%s and %s are %s volumes)\n", root, other_root, cross ? "different" : "the same");
    char far[EX_PATH];
    path_join(far, sizeof far, other_root, "mv-far");
    mkdirp(far);
    j = mk(OP_MOVE, far, CF_ASK, 1, mv);
    drive(j);
    CHECK(errors_of(j) == 0 && job_ok_count(j) == 1, "move across volumes: errors=%d", errors_of(j));
    job_free(j);
    char arrived[EX_PATH];
    path_join(arrived, sizeof arrived, far, "a");
    CHECK(!path_exists(moved) && same_tree(arrived, kept), "after the move across volumes the tree is complete and the source gone");
    /* and back, with a file too */
    char one_file[EX_PATH];
    path_join(one_file, sizeof one_file, arrived, "top.txt");
    const char *bf[1] = { one_file };
    j = mk(OP_MOVE, dst, CF_ASK, 1, bf);
    drive(j);
    job_free(j);
    path_join(p, sizeof p, dst, "top.txt");
    CHECK(path_exists(p) && !path_exists(one_file), "a single file moved back");
    rm_rf(far);
}

static void test_trash(const char *root, const char *other_root) {
    char p[EX_PATH], q[EX_PATH], orig[EX_PATH];
    char dir[EX_PATH];
    path_join(dir, sizeof dir, root, "tr");
    mkdirp(dir);
    char name[64];
    snprintf(name, sizeof name, "ex-selftest-%d.txt", (int)getpid());
    path_join(p, sizeof p, dir, name);
    put(p, "to the netherworld", 18, 0644);
    char tpath[EX_PATH];
    path_join(tpath, sizeof tpath, NETHERWORLD, name);
    const char *one[1] = { p };
    Job *j = mk(OP_TRASH, NULL, CF_ASK, 1, one);
    drive(j);
    CHECK(errors_of(j) == 0 && job_ok_count(j) == 1, "trash: errors=%d", errors_of(j));
    job_free(j);
    CHECK(!path_exists(p) && path_exists(tpath), "the file is in the Netherworld, not at home any more");
    int64_t when = 0;
    CHECK(trash_read_info(name, orig, sizeof orig, &when) && !strcmp(orig, p) && when > 1600000000, "the note says where it came from: %s", orig);
    /* a second file of the same name gets a new name in the Netherworld */
    put(p, "second", 6, 0644);
    j = mk(OP_TRASH, NULL, CF_ASK, 1, one);
    drive(j);
    job_free(j);
    char second[EX_NAME + 8];
    snprintf(second, sizeof second, "%s.1", name);
    path_join(q, sizeof q, NETHERWORLD, second);
    CHECK(path_exists(q), "the second one is called %s", second);
    CHECK(trash_read_info(second, orig, sizeof orig, NULL) && !strcmp(orig, p), "...and has its own note");
    /* restore both: the second finds its place taken */
    char t1[EX_PATH];
    path_join(t1, sizeof t1, NETHERWORLD, name);
    const char *both[2] = { t1, q };
    g_asked = 0;
    g_ans = CF_KEEP;
    g_ans_all = false;
    j = mk(OP_RESTORE, NULL, CF_ASK, 2, both);
    drive(j);
    CHECK(errors_of(j) == 0 && job_ok_count(j) == 2, "restore: errors=%d ok=%d", errors_of(j), job_ok_count(j));
    CHECK(g_asked == 1, "the second restore asked about the name clash (%d)", g_asked);
    job_free(j);
    CHECK(path_exists(p), "restored to the original place");
    char alt[EX_NAME + 16];
    snprintf(alt, sizeof alt, "ex-selftest-%d (2).txt", (int)getpid());
    path_join(q, sizeof q, dir, alt);
    CHECK(path_exists(q), "the clash was resolved as \"%s\"", alt);
    CHECK(!path_exists(t1) && !trash_read_info(name, orig, sizeof orig, NULL), "the Netherworld lost them and their notes");
    /* trash from another volume (copy and remove) */
    char far[EX_PATH];
    path_join(far, sizeof far, other_root, "tr-far");
    mkdirp(far);
    char fdir[EX_PATH];
    snprintf(name, sizeof name, "ex-selftest-dir-%d", (int)getpid());
    path_join(fdir, sizeof fdir, far, name);
    mkdirp(fdir);
    path_join(p, sizeof p, fdir, "inner.bin"); put_pattern(p, 70000, 5);
    path_join(p, sizeof p, fdir, "sub"); mkdirp(p);
    path_join(p, sizeof p, fdir, "sub/x"); put(p, "x", 1, 0644);
    char ref[EX_PATH];
    path_join(ref, sizeof ref, far, "reference");
    const char *fd1[1] = { fdir };
    mkdirp(ref);
    j = mk(OP_COPY, ref, CF_ASK, 1, fd1);
    drive(j);
    job_free(j);
    j = mk(OP_TRASH, NULL, CF_ASK, 1, fd1);
    drive(j);
    CHECK(errors_of(j) == 0, "trash a folder from another volume: errors=%d", errors_of(j));
    job_free(j);
    path_join(tpath, sizeof tpath, NETHERWORLD, name);
    char refcopy[EX_PATH];
    path_join(refcopy, sizeof refcopy, ref, name);
    CHECK(!path_exists(fdir) && same_tree(tpath, refcopy), "the folder arrived in the Netherworld complete");
    /* delete for good, and the note goes with it */
    const char *tp[1] = { tpath };
    j = mk(OP_DELETE, NULL, CF_ASK, 1, tp);
    drive(j);
    CHECK(errors_of(j) == 0, "delete from the Netherworld: errors=%d", errors_of(j));
    job_free(j);
    CHECK(!path_exists(tpath) && !trash_read_info(name, orig, sizeof orig, NULL), "gone, with its note");
    /* a thing without a note is restored to the home folder */
    const char *home = getenv("HOME");
    snprintf(name, sizeof name, "ex-selftest-nonote-%d", (int)getpid());
    path_join(tpath, sizeof tpath, NETHERWORLD, name);
    put(tpath, "no note", 7, 0644);
    const char *np[1] = { tpath };
    j = mk(OP_RESTORE, NULL, CF_ASK, 1, np);
    drive(j);
    CHECK(errors_of(j) == 0 && job_restored_home(j) == 1, "restore without a note: errors=%d home=%d", errors_of(j), job_restored_home(j));
    job_free(j);
    path_join(q, sizeof q, home && *home ? home : "/", name);
    CHECK(path_exists(q) && !path_exists(tpath), "it went to the home folder");
    unlink(q);
    /* things that must never be trashed or deleted */
    const char *bad1[1] = { "/" }, *bad2[1] = { NETHERWORLD };
    j = mk(OP_TRASH, NULL, CF_ASK, 1, bad1);
    drive(j);
    CHECK(errors_of(j) == 1, "the root cannot be trashed");
    job_free(j);
    j = mk(OP_DELETE, NULL, CF_ASK, 1, bad2);
    drive(j);
    CHECK(errors_of(j) == 1 && path_exists(NETHERWORLD), "the Netherworld cannot be deleted");
    job_free(j);
    rm_rf(far);
    /* the note round trip with awkward characters */
    snprintf(name, sizeof name, "ex-selftest-note-%d", (int)getpid());
    CHECK(trash_write_info(name, "/a b/it's \"here\"/caf\xc3\xa9") && trash_read_info(name, orig, sizeof orig, NULL) &&
          !strcmp(orig, "/a b/it's \"here\"/caf\xc3\xa9"), "note round trip");
    trash_remove_info(name);
}

static void test_delete_size(const char *root) {
    char d[EX_PATH], p[EX_PATH];
    path_join(d, sizeof d, root, "del");
    mkdirp(d);
    int files = 0;
    for (int i = 0; i < 12; i++) {
        char sub[64];
        snprintf(sub, sizeof sub, "d%d/e%d", i, i);
        path_join(p, sizeof p, d, sub);
        mkdirp(p);
        for (int k = 0; k < 40; k++) {
            char f[96];
            snprintf(f, sizeof f, "d%d/e%d/f%d", i, i, k);
            path_join(p, sizeof p, d, f);
            put(p, "12345", 5, 0644);
            files++;
        }
    }
    const char *one[1] = { d };
    Job *j = mk(OP_SIZE, NULL, CF_ASK, 1, one);
    drive(j);
    JobInfo ji; job_info(j, &ji);
    CHECK(ji.files == files && ji.dirs == 1 + 24 && ji.size_total == files * 5, "measure: files=%lld dirs=%lld bytes=%lld", (long long)ji.files, (long long)ji.dirs, (long long)ji.size_total);
    job_free(j);
    j = mk(OP_DELETE, NULL, CF_ASK, 1, one);
    int calls = 0;
    while (!job_run(j, 0)) { calls++; if (calls > 100000) break; }
    CHECK(calls > 100, "deleting %d files took %d slices", files, calls);
    CHECK(errors_of(j) == 0 && !path_exists(d), "delete tree: errors=%d, gone=%d", errors_of(j), !path_exists(d));
    job_free(j);
    if (geteuid() != 0) {
        mkdirp(d);
        path_join(p, sizeof p, d, "locked"); mkdirp(p);
        path_join(p, sizeof p, d, "locked/inside"); put(p, "x", 1, 0644);
        path_join(p, sizeof p, d, "free.txt"); put(p, "y", 1, 0644);
        path_join(p, sizeof p, d, "locked"); chmod(p, 0555);
        j = mk(OP_DELETE, NULL, CF_ASK, 1, one);
        drive(j);
        int ne;
        const JobErr *errs = job_errors(j, &ne);
        CHECK(errors_of(j) >= 1 && ne >= 1 && errs[0].path && errs[0].msg[0], "a protected folder is reported, not fatal (errors=%d)", errors_of(j));
        job_free(j);
        path_join(p, sizeof p, d, "free.txt");
        CHECK(!path_exists(p), "everything else was deleted anyway");
        path_join(p, sizeof p, d, "locked"); chmod(p, 0755);
    }
    rm_rf(d);
}

static void test_cancel_queue(const char *root, const char *other_root) {
    char big[EX_PATH], dst[EX_PATH], p[EX_PATH];
    path_join(big, sizeof big, root, "big.bin");
    put_pattern(big, 3 * 1024 * 1024, 77);
    path_join(dst, sizeof dst, other_root, "cancel-dst");
    mkdirp(dst);
    const char *one[1] = { big };
    Job *j = mk(OP_COPY, dst, CF_ASK, 1, one);
    for (int i = 0; i < 5; i++) job_run(j, 0);
    JobInfo ji; job_info(j, &ji);
    CHECK(ji.cur_done > 0 && ji.cur_done < ji.cur_size, "the copy is under way (%lld of %lld)", (long long)ji.cur_done, (long long)ji.cur_size);
    job_cancel(j);
    CHECK(job_run(j, 50), "a cancelled job finishes at once");
    CHECK(job_cancelled(j), "...as cancelled");
    job_free(j);
    path_join(p, sizeof p, dst, "big.bin");
    CHECK(!path_exists(p), "the half-written copy was removed");
    rm_rf(dst);
    /* the queue (ex_job_done does nothing without a window) */
    g_job_done_hook = NULL;
    path_join(dst, sizeof dst, root, "queue-dst");
    mkdirp(dst);
    j = mk(OP_COPY, dst, CF_ASK, 1, one);
    jobs_submit(j);
    Job *j2 = mk(OP_SIZE, NULL, CF_ASK, 1, one);
    jobs_submit(j2);
    CHECK(jobs_busy() && jobs_count() == 2, "two jobs queued");
    CHECK(jobs_front() == j && job_serial(j) != job_serial(j2), "the first is in front; serials differ");
    int guard = 0;
    while (jobs_busy() && guard++ < 100000) jobs_step(5);
    CHECK(!jobs_busy(), "the queue drained");
    path_join(p, sizeof p, dst, "big.bin");
    CHECK(same_file(big, p), "the queued copy is complete");
    rm_rf(dst);
    unlink(big);
}

int selftest_run(void) {
    char root[EX_PATH], other[EX_PATH];
    const char *home = getenv("HOME");
    snprintf(root, sizeof root, "/tmp/ex-selftest-%d", (int)getpid());
    snprintf(other, sizeof other, "%s/.ex-selftest-%d", home && *home && strcmp(home, "/") ? home : "/Netherworld", (int)getpid());
    mkdirp(root);
    mkdirp(other);
    printf("explorer selftest in %s and %s\n", root, other);
    test_paths();
    test_format();
    test_names(root);
    test_model(root);
    test_copy(root);
    test_conflicts(root);
    test_move(root, other);
    test_trash(root, other);
    test_delete_size(root);
    test_cancel_queue(root, other);
    rm_rf(root);
    rm_rf(other);
    if (g_fail) printf("EXPLORER SELFTEST FAIL %d of %d checks\n", g_fail, g_checks);
    else printf("EXPLORER SELFTEST PASS %d checks\n", g_checks);
    return g_fail ? 1 : 0;
}
