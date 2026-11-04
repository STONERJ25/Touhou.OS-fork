/*
 * The differential probe of the Linux personality: it makes a few dozen system calls, many of them on purpose wrong, and prints what each returned. The expected
 * output is what a real Linux prints for the same program (tests/linux/golden/lnx_probe.txt, made by tools/linux-oracle.py in WSL); tests/case_linux.py runs the
 * program under the personality and compares line by line. A line that differs is a place where TouhouOS does not behave like Linux, and the manual page of the
 * call (section 2) decides which of the two is right. Only facts the manual pages promise are printed: no addresses, no process ids, no times.
 */
#include "lnx.h"

struct kstat { u64 st_dev, st_ino, st_nlink; u32 st_mode, st_uid, st_gid, pad0; u64 st_rdev; s64 st_size, st_blksize, st_blocks; s64 times[6]; s64 unused[3]; };
struct kts { s64 sec, nsec; };
struct kutsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
struct krlimit { u64 cur, max; };
struct kiovec { const void *base; u64 len; };
struct kpollfd { int fd; short events, revents; };

static char tcb[64];

void lnx_main(long *sp) {
    (void)sp;
    char buf[64];
    struct kstat st;

    /* the plainest errors */
    lnx_line("write to a bad descriptor", lnx_sys3(L_write, 999, "x", 1));
    lnx_line("write from a bad pointer", lnx_sys3(L_write, 1, 1, 1));
    lnx_line("unknown system call 9999", lnx_sys0(9999));
    lnx_line("system call number -1", lnx_sys0(-1));
    lnx_line("close a bad descriptor", lnx_sys1(L_close, 999));
    lnx_line("dup a bad descriptor", lnx_sys1(L_dup, 999));
    lnx_line("lseek a bad descriptor", lnx_sys3(L_lseek, 999, 0, 0));
    lnx_line("read a bad descriptor", lnx_sys3(L_read, 999, buf, 1));

    /* the program break */
    long b0 = lnx_sys1(L_brk, 0);
    lnx_line("brk(0) is stable", lnx_sys1(L_brk, 0) == b0);
    long b1 = lnx_sys1(L_brk, b0 + 3 * 4096);
    lnx_line("brk grows by three pages", b1 == b0 + 3 * 4096);
    *(volatile char *)(b0 + 100) = 7;
    lnx_line("the new heap is writable", *(volatile char *)(b0 + 100) == 7);
    lnx_line("brk(0) returns the new break", lnx_sys1(L_brk, 0) == b1);
    lnx_line("brk below the start is refused", lnx_sys1(L_brk, 4096) == b1);
    lnx_line("brk shrinks back", lnx_sys1(L_brk, b0) == b0);

    /* anonymous memory */
    long m = lnx_sys6(L_mmap, 0, 8192, 3, 0x22, -1, 0);                   /* PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS */
    lnx_line("mmap anonymous succeeds", m > 0 && !(m & 4095));
    int zero = 1;
    for (int i = 0; i < 8192; i += 512) if (((char *)m)[i]) zero = 0;
    lnx_line("anonymous memory is zero", zero);
    ((char *)m)[8191] = 5;
    lnx_line("anonymous memory is writable", ((char *)m)[8191] == 5);
    lnx_line("mprotect read-only", lnx_sys3(L_mprotect, m, 4096, 1));
    lnx_line("mprotect at an unaligned address", lnx_sys3(L_mprotect, m + 1, 4096, 1));
    lnx_line("munmap at an unaligned address", lnx_sys2(L_munmap, m + 1, 4096));
    lnx_line("munmap", lnx_sys2(L_munmap, m, 8192));
    lnx_line("mmap of length 0", lnx_sys6(L_mmap, 0, 0, 3, 0x22, -1, 0));
    lnx_line("mmap of a bad descriptor", lnx_sys6(L_mmap, 0, 4096, 1, 0x2, 999, 0));
    lnx_line("mmap fixed at an unaligned address", lnx_sys6(L_mmap, 1, 4096, 3, 0x32, -1, 0));

    /* the thread pointer */
    u64 fs = 0;
    lnx_line("arch_prctl ARCH_SET_FS", lnx_sys2(L_arch_prctl, 0x1002, tcb));
    lnx_line("arch_prctl ARCH_GET_FS", lnx_sys2(L_arch_prctl, 0x1003, &fs));
    lnx_line("the FS base reads back", fs == (u64)tcb);
    lnx_line("arch_prctl with an unknown code", lnx_sys2(L_arch_prctl, 0x9999, 0));
    lnx_line("ARCH_SET_FS with a kernel address", lnx_sys2(L_arch_prctl, 0x1002, 0xffff800000000000UL));

    /* identity */
    long pid = lnx_sys0(L_getpid), tid = lnx_sys0(L_gettid);
    int tidword = 0;
    lnx_line("getpid is positive", pid > 0);
    lnx_line("gettid equals getpid in a one-thread program", pid == tid);
    lnx_line("set_tid_address returns the thread id", lnx_sys1(L_set_tid_address, &tidword) == tid);
    lnx_line("getppid is positive", lnx_sys0(L_getppid) > 0);
    lnx_line("getuid equals geteuid", lnx_sys0(L_getuid) == lnx_sys0(L_geteuid));
    lnx_line("getgid equals getegid", lnx_sys0(L_getgid) == lnx_sys0(L_getegid));
    lnx_line("kill with signal 0 on itself", lnx_sys2(L_kill, pid, 0));
    lnx_line("kill of a process that does not exist", lnx_sys2(L_kill, 0x7ffffff0, 0));

    /* uname and limits */
    struct kutsname u;
    lnx_line("uname", lnx_sys1(L_uname, &u));
    lnx_puts("uname sysname: "); lnx_puts(u.sysname); lnx_puts("\n");
    lnx_puts("uname machine: "); lnx_puts(u.machine); lnx_puts("\n");
    lnx_line("uname into a bad pointer", lnx_sys1(L_uname, 1));
    struct krlimit rl;
    lnx_line("getrlimit NOFILE", lnx_sys2(L_getrlimit, 7, &rl));
    lnx_line("the NOFILE soft limit is at least 64", rl.cur >= 64);
    lnx_line("getrlimit STACK", lnx_sys2(L_getrlimit, 3, &rl));
    lnx_line("the STACK soft limit is at least 1 MiB", rl.cur >= (1 << 20));
    lnx_line("getrlimit of a resource that does not exist", lnx_sys2(L_getrlimit, 99, &rl));
    lnx_line("prlimit64 of ourselves", lnx_sys4(L_prlimit64, 0, 7, 0, &rl));
    lnx_line("prlimit64 of a process that does not exist", lnx_sys4(L_prlimit64, 0x7ffffff0, 7, 0, &rl));

    /* stat */
    lnx_line("stat of /", lnx_sys4(L_newfstatat, -100, "/", &st, 0));
    lnx_line("/ is a directory", (st.st_mode & 0170000) == 0040000);
    lnx_line("the old stat call", lnx_sys2(L_stat, "/", &st));
    lnx_line("stat of a missing path", lnx_sys4(L_newfstatat, -100, "/no/such/path", &st, 0));
    lnx_line("stat of an empty path", lnx_sys4(L_newfstatat, -100, "", &st, 0));
    lnx_line("stat of stdout by descriptor", lnx_sys4(L_newfstatat, 1, "", &st, 0x1000));
    lnx_line("stat with unknown flags", lnx_sys4(L_newfstatat, -100, "/", &st, 4));
    lnx_line("stat into a bad pointer", lnx_sys4(L_newfstatat, -100, "/", 1, 0));
    lnx_line("fstat of a bad descriptor", lnx_sys2(L_fstat, 999, &st));

    /* open, read, access */
    long fd = lnx_sys3(L_open, "/", 0, 0);
    lnx_line("open a directory for reading", fd >= 0);
    lnx_line("read from a directory", lnx_sys3(L_read, fd, buf, 16));
    lnx_line("close it", lnx_sys1(L_close, fd));
    lnx_line("close it again", lnx_sys1(L_close, fd));
    lnx_line("open a missing file", lnx_sys3(L_open, "/no/such/file", 0, 0));
    lnx_line("open with a bad pointer", lnx_sys3(L_open, 1, 0, 0));
    lnx_line("access of /", lnx_sys2(L_access, "/", 0));
    lnx_line("access of a missing path", lnx_sys2(L_access, "/no/such/file", 0));

    /* pipes, poll, vectored write */
    int p[2];
    lnx_line("pipe", lnx_sys1(L_pipe, p));
    lnx_line("lseek on a pipe", lnx_sys3(L_lseek, p[0], 0, 0));
    struct kpollfd pf = { p[0], 1, 0 };
    lnx_line("poll of an empty pipe without waiting", lnx_sys3(L_poll, &pf, 1, 0));
    lnx_line("write into the pipe", lnx_write(p[1], "hello", 5));
    lnx_line("poll of a pipe with data", lnx_sys3(L_poll, &pf, 1, 0));
    lnx_line("revents is POLLIN", pf.revents);
    lnx_line("read it back", lnx_sys3(L_read, p[0], buf, 64));
    lnx_line("the bytes are hello", buf[0] == 'h' && buf[1] == 'e' && buf[4] == 'o');
    struct kiovec iov[2] = { { "wr", 2 }, { "itev\n", 5 } };
    long w = lnx_sys3(L_writev, 1, iov, 2);
    lnx_line("writev returned", w);
    lnx_line("dup2 onto 20", lnx_sys2(L_dup2, 1, 20));
    lnx_line("close 20", lnx_sys1(L_close, 20));

    /* random numbers and clocks */
    lnx_line("getrandom", lnx_sys3(L_getrandom, buf, 16, 0));
    int any = 0;
    for (int i = 0; i < 16; i++) any |= buf[i];
    lnx_line("getrandom is not all zero", any != 0);
    struct kts t1, t2;
    lnx_line("clock_gettime MONOTONIC", lnx_sys2(L_clock_gettime, 1, &t1));
    lnx_line("clock_gettime REALTIME", lnx_sys2(L_clock_gettime, 0, &t2));
    lnx_line("the real time is after 2020", t2.sec > 1577836800);
    lnx_line("clock_gettime with a bad clock", lnx_sys2(L_clock_gettime, 99, &t1));
    lnx_line("clock_gettime into a bad pointer", lnx_sys2(L_clock_gettime, 1, 1));
    struct kts nap = { 0, 2000000 };
    lnx_line("nanosleep for 2 ms", lnx_sys2(L_nanosleep, &nap, 0));

    /* fork and wait */
    long child = lnx_sys0(L_fork);
    if (child == 0) lnx_exit(7);
    int status = 0;
    lnx_line("wait4 returns the child", lnx_sys4(L_wait4, child, &status, 0, 0) == child);
    lnx_line("the child's status word", status);
    lnx_line("wait4 with no children left", lnx_sys4(L_wait4, -1, &status, 0, 0));

    lnx_puts("probe done\n");
    lnx_exit(0);
}
