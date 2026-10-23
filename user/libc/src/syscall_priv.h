/* Raw system call stubs and errno plumbing (internal). */
#ifndef LIBGAP_SYSCALL_PRIV_H
#define LIBGAP_SYSCALL_PRIV_H

#include <syscall.h>
#include <errno.h>

static inline long __sc0(long n) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}
static inline long __sc1(long n, long a) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
    return r;
}
static inline long __sc2(long n, long a, long b) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
static inline long __sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static inline long __sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}
static inline long __sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

/* A thread that has been cancelled (pthread_cancel) and then blocks in the kernel gets -ECANCELED: every wrapper that goes
 * through __ret is therefore a cancellation point, and the thread exits here instead of returning. */
void __cancel_exit(void) __attribute__((noreturn));

/* Kernel returns -errno in [-4095, -1]. */
static inline long __ret(long r) {
    if ((unsigned long)r > (unsigned long)-4096L) {
        if (r == -ECANCELED) __cancel_exit();
        errno = (int)-r;
        return -1;
    }
    return r;
}

#endif
