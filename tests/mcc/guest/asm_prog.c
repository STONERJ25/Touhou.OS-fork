/* compiled by mcc ON the guest: inline assembly that talks to the kernel (the same constraints libgap's system call stubs use) */
#include <stdio.h>
#include <string.h>

#define SYS_exit 0
#define SYS_write 2
#define SYS_getpid 22

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static long sc0(long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory");
    return r;
}

static long sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

static void *thread_pointer(void) {
    void *tp;
    __asm__("movq %%fs:0, %0" : "=r"(tp));
    return tp;
}

int main(void) {
    const char *msg = "raw write system call\n";
    long n = sc3(SYS_write, 1, (long)msg, (long)strlen(msg));
    printf("wrote %ld bytes\n", n);
    long pid = sc0(SYS_getpid);
    printf("getpid matches libc: %d\n", pid > 0);
    printf("sc4 with a register variable: %ld\n", sc4(SYS_write, 1, (long)"four\n", 5, 0));
    printf("thread pointer is its own address: %d\n", *(void **)thread_pointer() == thread_pointer());
    char dst[8] = "........";
    char *d = dst;
    const char *s = "xyz";
    unsigned long cnt = 3;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(cnt) : : "memory");
    printf("%.8s %d\n", dst, (int)(d - dst));
    return 0;
}
