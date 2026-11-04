/*
 * A tiny kit for the programs in this folder: freestanding C for the Linux personality (docs/COMPAT.md), with no C library at all, only the x86-64 Linux system
 * call convention (number in rax, arguments in rdi, rsi, rdx, r10, r8, r9; the result is the value or a negative errno). A program defines lnx_main(); _start
 * hands it the initial stack pointer: argc at *sp, then argv, envp and the auxiliary vector, as the System V psABI lays them out.
 *
 * The very same source is built twice: by the project's compiler for TouhouOS (tools/build.py, kind "linuxtest") and by gcc inside WSL for a real Linux
 * (tools/linux-oracle.py). Programs that print results must print the same text on both; the Linux output is stored in tests/linux/golden/ and tests/case_linux.py
 * compares the guest's output with it. Print no addresses, process ids or times: only facts that the Linux manual pages promise.
 */
#ifndef LNX_H
#define LNX_H

typedef unsigned long u64;
typedef long          s64;
typedef unsigned int  u32;
typedef unsigned char u8;

/* the system calls these programs use (x86-64 Linux numbers; the kernel's own copy is kernel/linux/numbers.h) */
#define L_read          0
#define L_write         1
#define L_open          2
#define L_close         3
#define L_stat          4
#define L_fstat         5
#define L_poll          7
#define L_lseek         8
#define L_mmap          9
#define L_mprotect      10
#define L_munmap        11
#define L_brk           12
#define L_writev        20
#define L_access        21
#define L_pipe          22
#define L_dup           32
#define L_dup2          33
#define L_nanosleep     35
#define L_getpid        39
#define L_fork          57
#define L_execve        59
#define L_exit          60
#define L_wait4         61
#define L_kill          62
#define L_uname         63
#define L_getrlimit     97
#define L_getuid        102
#define L_getgid        104
#define L_geteuid       107
#define L_getegid       108
#define L_getppid       110
#define L_arch_prctl    158
#define L_gettid        186
#define L_set_tid_address 218
#define L_clock_gettime 228
#define L_exit_group    231
#define L_newfstatat    262
#define L_readlinkat    267
#define L_prlimit64     302
#define L_getrandom     318

static inline long lnx_sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}
#define lnx_sys0(n)                  lnx_sys6(n, 0, 0, 0, 0, 0, 0)
#define lnx_sys1(n, a)               lnx_sys6(n, (long)(a), 0, 0, 0, 0, 0)
#define lnx_sys2(n, a, b)            lnx_sys6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define lnx_sys3(n, a, b, c)         lnx_sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define lnx_sys4(n, a, b, c, d)      lnx_sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

/* the compiler calls these for block copies and clears, even without a library */
void *memset(void *d, int c, unsigned long n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
void *memcpy(void *d, const void *s, unsigned long n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); return r; }

static unsigned long lnx_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int lnx_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static long lnx_write(int fd, const void *p, unsigned long n) { return lnx_sys3(L_write, fd, p, n); }
static void lnx_puts(const char *s) { lnx_write(1, s, lnx_strlen(s)); }
static void lnx_putl(long v) {
    char b[24];
    int i = 23;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    lnx_puts(b + i);
}
static void lnx_puthex(u64 v) {
    char b[20];
    int i = 19;
    b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    lnx_puts("0x");
    lnx_puts(b + i);
}
static void lnx_line(const char *name, long v) { lnx_puts(name); lnx_puts(": "); lnx_putl(v); lnx_puts("\n"); }
static void lnx_exit(int code) { for (;;) lnx_sys1(L_exit_group, code); }

void lnx_main(long *sp);
__asm__(".text\n.globl _start\n_start:\n\txor %ebp, %ebp\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall lnx_main\n\tud2\n");

#endif
