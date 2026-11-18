/* acflag: a program that sets EFLAGS.AC in its own flags and then lives with the kernel for a while (docs/HARDENING.md, "SMAP").
 *
 * With SMAP on, AC is the switch that lets the kernel touch user memory, and any ring may set it (POPF changes AC at every privilege level). The kernel clears it on
 * every entry: SFMASK does it for SYSCALL, and the CLAC at the top of isr_common does it for interrupts and exceptions, which the CPU enters with the program's AC as it
 * was. If the interrupt path forgot, every timer tick that hits this loop would run its handler with SMAP effectively off, and trap_handler logs a warning.
 *
 * Run by tests/case_hardening2.py as /usr/lib/kernel-tests/acflag: it makes system calls with AC set (including ones that fail on a bad user pointer, which go through
 * the copy routines' fault path) while the timer ticks, then clears AC and prints ACFLAG PASS. The loop uses aligned accesses only, so it also survives a CPU whose CR0.AM
 * turns AC into an alignment check in user mode. The kernel's own side (a warning in the log) is what the test looks at. */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define AC_FLAG 0x40000UL

static inline void set_ac(void) { __asm__ volatile ("pushfq\n\torq %0, (%%rsp)\n\tpopfq" : : "i"(AC_FLAG) : "memory", "cc"); }
static inline void clear_ac(void) { __asm__ volatile ("pushfq\n\tandq %0, (%%rsp)\n\tpopfq" : : "i"(~AC_FLAG) : "memory", "cc"); }
static inline unsigned long flags(void) { unsigned long f; __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(f)); return f; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(1, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
    int fails = 0;
    int zero = open("/dev/zero", O_RDONLY);                                     /* a read that never blocks, so that the failure is the copy to the buffer */
    if (zero < 0) { printf("  FAIL: no /dev/zero\n"); return 1; }
    long t0 = now_ms();
    set_ac();
    if (!(flags() & AC_FLAG)) { clear_ac(); printf("  FAIL: AC did not stay set\n"); return 1; }
    volatile unsigned long spin = 0;
    long calls = 0, efaults = 0;
    while (now_ms() - t0 < 1500) {
        for (int i = 0; i < 20000; i++) spin += (unsigned long)i;               /* user-mode time, with the ticks arriving in it */
        (void)getpid();
        char *bad = (char *)(0x7e0000000000UL + (unsigned long)(calls & 7) * 4096);
        errno = 0;
        if (read(zero, bad, 16) < 0 && errno == EFAULT) efaults++;                 /* a copy_to_user that faults inside the window and leaves through the fixup */
        if (open(bad, O_RDONLY) < 0 && errno == EFAULT) efaults++;              /* strncpy_from_user on an unmapped page */
        calls++;
    }
    int still = (flags() & AC_FLAG) != 0;                                       /* the kernel hands AC back as the program had it */
    clear_ac();
    if (!still) { printf("  FAIL: AC was lost across system calls or interrupts\n"); fails++; }
    if (efaults < calls * 2) { printf("  FAIL: %ld of %ld bad-pointer calls did not give EFAULT\n", calls * 2 - efaults, calls * 2); fails++; }
    printf("acflag: %ld rounds with AC set, %ld EFAULTs\n", calls, efaults);
    printf(fails ? "ACFLAG FAIL\n" : "ACFLAG PASS\n");
    return fails ? 1 : 0;
}
