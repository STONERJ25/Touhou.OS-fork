/*
 * The memory-mapping calls a dynamic linker and a C library make (L2 of docs/COMPAT.md), one at a time with their edge cases: file-backed private mappings at offsets, a
 * reservation made PROT_NONE and filled with MAP_FIXED, partial munmap, mprotect, the flags a loader passes (MAP_DENYWRITE, MAP_NORESERVE, MAP_POPULATE, MAP_STACK), madvise,
 * mremap and the errors of bad arguments. A byte of an unmapped or protected page is probed with a system call (write() to a pipe fails with EFAULT), so that nothing crashes.
 * Every line is a fact the manual pages promise, so a real Linux prints the same lines (tools/linux-oracle.py); the program is linked statically.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096

static int probe_pipe[2];

static void drain(void) {
    char c;
    while (read(probe_pipe[0], &c, 1) == 1) {}
}

/* Can the byte at p be read? write() from it into a pipe tells without a signal. */
static int readable(const void *p) {
    drain();
    int ok = write(probe_pipe[1], p, 1) == 1;
    drain();
    return ok;
}

/* What a program may do with the byte at p: the byte goes into a pipe (it must be readable) and comes back out into the same place (it must be writable). Nothing changes. */
static const char *state(void *p) {
    drain();
    if (write(probe_pipe[1], p, 1) != 1) return "unmapped or no access";
    int ok = read(probe_pipe[0], p, 1) == 1;
    drain();
    return ok ? "read and write" : "read only";
}

int main(void) {
    if (pipe2(probe_pipe, O_NONBLOCK)) return 1;
    char path[] = "/tmp/lnx_map_tour_XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    char page[PAGE];
    for (int i = 0; i < 3; i++) {
        memset(page, 'A' + i, PAGE);
        if (write(fd, page, PAGE) != PAGE) return 1;
    }

    printf("--- anonymous memory\n");
    char *a = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("mapped: %s, first byte %d, last byte %d\n", a != MAP_FAILED ? "yes" : "no", a[0], a[4 * PAGE - 1]);
    for (int i = 0; i < 4; i++) memset(a + i * PAGE, 'a' + i, PAGE);
    printf("pages hold: %c %c %c %c\n", a[0], a[PAGE], a[2 * PAGE], a[3 * PAGE]);
    printf("aligned to a page: %s\n", ((unsigned long)a & (PAGE - 1)) ? "no" : "yes");

    printf("--- partial munmap\n");
    printf("unmap the middle two pages: %d\n", munmap(a + PAGE, 2 * PAGE));
    printf("page 0: %s; page 1: %s; page 2: %s; page 3: %s\n", state(a), state(a + PAGE), state(a + 2 * PAGE), state(a + 3 * PAGE));
    printf("page 0 still holds %c, page 3 still holds %c\n", a[0], a[3 * PAGE]);
    printf("unmap the first page: %d, the last: %d\n", munmap(a, PAGE), munmap(a + 3 * PAGE, PAGE));
    printf("page 0: %s; page 3: %s\n", state(a), state(a + 3 * PAGE));

    printf("--- file-backed private mappings\n");
    char *f0 = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    char *f1 = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    char *f2 = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 2 * PAGE);
    printf("offset 0: %c, offset 4096: %c, offset 8192: %c\n", f0[0], f1[0], f2[0]);
    f2[0] = 'Z';
    f2[PAGE - 1] = 'Y';
    char back[2];
    pread(fd, back, 1, 2 * PAGE);
    printf("private write: mapping shows %c...%c, the file still has %c\n", f2[0], f2[PAGE - 1], back[0]);
    printf("read-only mapping: %s\n", state(f0));
    printf("read-write private mapping: %s\n", state(f2));
    char *two = mmap(NULL, 2 * PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
    printf("two pages from offset 4096: %c then %c\n", two[0], two[PAGE]);
    char *odd = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 100);
    printf("an offset that is not a multiple of the page size: %s (%s)\n", odd == MAP_FAILED ? "refused" : "accepted", odd == MAP_FAILED ? strerror(errno) : "-");
    printf("a descriptor that is not open: %s\n", mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, 77, 0) == MAP_FAILED ? strerror(errno) : "accepted");
    printf("length zero: %s\n", mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ? strerror(errno) : "accepted");

    printf("--- a reservation filled with MAP_FIXED (what the dynamic linker does for a library)\n");
    char *res = mmap(NULL, 8 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    printf("reserved eight pages: %s; page 0 is %s\n", res != MAP_FAILED ? "yes" : "no", state(res));
    char *seg = mmap(res + PAGE, 2 * PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_DENYWRITE, fd, 0);
    printf("file at page 1 with MAP_FIXED: %s, at the address asked for: %s\n", seg != MAP_FAILED ? "yes" : "no", seg == res + PAGE ? "yes" : "no");
    char *data = mmap(res + 3 * PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 2 * PAGE);
    char before = data[0];
    data[0] = 'W';
    printf("data page at page 3: %c, written: %s\n", before, data[0] == 'W' ? "yes" : "no");
    char *bss = mmap(res + 4 * PAGE, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    printf("zero pages at page 4: %s, first byte %d\n", bss != MAP_FAILED ? "yes" : "no", bss[0]);
    printf("page 0: %s; page 1: %s ('%c'); page 2: %s ('%c'); page 3: %s; page 4: %s; page 5: %s; page 6: %s\n", state(res), state(res + PAGE), res[PAGE], state(res + 2 * PAGE), res[2 * PAGE],
           state(res + 3 * PAGE), state(res + 4 * PAGE), state(res + 5 * PAGE), state(res + 6 * PAGE));
    int rc = mprotect(res + 6 * PAGE, 2 * PAGE, PROT_NONE);
    printf("mprotect the gap pages 6 and 7 to PROT_NONE: %d; page 6: %s\n", rc, state(res + 6 * PAGE));
    printf("the page before the fixed mapping is untouched: %s\n", state(res));
    printf("MAP_FIXED at an address that is not page aligned: %s\n", mmap(res + 1, PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED ? strerror(errno) : "accepted");

    printf("--- mprotect (what RELRO does)\n");
    char *r = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(r, 'r', 3 * PAGE);
    printf("before: %s\n", state(r + PAGE));
    printf("mprotect the middle page read-only: %d\n", mprotect(r + PAGE, PAGE, PROT_READ));
    printf("page 0: %s; page 1: %s; page 2: %s\n", state(r), state(r + PAGE), state(r + 2 * PAGE));
    printf("page 1 still holds %c\n", r[PAGE]);
    rc = mprotect(r + PAGE, PAGE, PROT_READ | PROT_WRITE);
    printf("mprotect it back: %d; page 1: %s\n", rc, state(r + PAGE));
    printf("mprotect with an address that is not page aligned: %s\n", mprotect(r + 1, PAGE, PROT_READ) ? strerror(errno) : "accepted");

    printf("--- hints that change nothing a program can see\n");
    char *h = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_POPULATE, -1, 0);
    printf("MAP_NORESERVE | MAP_POPULATE: %s\n", h != MAP_FAILED ? "accepted" : strerror(errno));
    char *stk = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    printf("MAP_STACK: %s\n", stk != MAP_FAILED ? "accepted" : strerror(errno));
    printf("madvise MADV_WILLNEED: %d, MADV_SEQUENTIAL: %d\n", madvise(h, PAGE, MADV_WILLNEED), madvise(h, PAGE, MADV_SEQUENTIAL));
    memset(h, 'm', PAGE);
    rc = madvise(h, PAGE, MADV_DONTNEED);
    printf("madvise MADV_DONTNEED on anonymous memory: %d, and the page reads as zero: %s\n", rc, h[0] == 0 && h[PAGE - 1] == 0 ? "yes" : "no");

    printf("--- mremap\n");
    char *g = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(g, 'g', 2 * PAGE);
    char *g2 = mremap(g, 2 * PAGE, 6 * PAGE, MREMAP_MAYMOVE);
    printf("grow from two pages to six: %s; the old content is kept: %s; the new pages are zero: %s\n", g2 != MAP_FAILED ? "yes" : strerror(errno),
           g2 != MAP_FAILED && g2[0] == 'g' && g2[2 * PAGE - 1] == 'g' ? "yes" : "no", g2 != MAP_FAILED && g2[2 * PAGE] == 0 && g2[6 * PAGE - 1] == 0 ? "yes" : "no");
    char *g3 = g2 == MAP_FAILED ? g2 : mremap(g2, 6 * PAGE, 3 * PAGE, 0);
    printf("shrink to three pages in place: %s; the fourth page is gone: %s\n", g3 == g2 ? "yes" : "no", g3 != MAP_FAILED && !readable(g3 + 3 * PAGE) ? "yes" : "no");

    printf("--- a big anonymous block\n");
    size_t big = 64UL << 20;
    char *b = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    printf("64 MiB reserved: %s\n", b != MAP_FAILED ? "yes" : strerror(errno));
    if (b != MAP_FAILED) { b[0] = 1; b[big - 1] = 2; printf("first and last byte written: %d %d, middle reads %d\n", b[0], b[big - 1], b[big / 2]); munmap(b, big); }
    close(fd);
    return 0;
}
