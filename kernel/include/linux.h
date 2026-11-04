/*
 * The Linux personality (docs/COMPAT.md): a process whose Process.abi is ABI_LINUX makes its system calls with the numbers, structures and conventions of
 * x86-64 Linux, and starts with the System V x86-64 initial stack (argc, argv, envp, auxiliary vector). Written from the psABI and the Linux manual pages and
 * checked against the behaviour of a real Linux by differential tests (tests/case_linux.py); no implementation of Linux, glibc or musl was read.
 */
#ifndef K_LINUX_H
#define K_LINUX_H

#include "process.h"

/* What the ELF loader learned about the program: the auxiliary vector is built from it. */
typedef struct LinuxImage {
    u64  entry;                 /* AT_ENTRY */
    u64  phdr;                  /* AT_PHDR: user address of the program headers (0 when no segment holds them) */
    u16  phent, phnum;          /* AT_PHENT, AT_PHNUM */
    u64  interp_base;           /* AT_BASE: where the dynamic linker was loaded (0: the program is static) */
    bool secure;                /* AT_SECURE: the program is set-user-id or set-group-id */
    u32  euid, egid;            /* the credentials the program will run with (AT_EUID, AT_EGID) */
} LinuxImage;

/* kernel/linux/syscall.c: dispatches tf->rax through the Linux table and returns the value for rax (a negative errno on failure). */
long linux_syscall(TrapFrame *tf);

/* kernel/linux/stack.c: maps the stack and lays out argc, argv, envp and the auxiliary vector as the psABI describes; *sp_out is the initial rsp. */
int  linux_build_stack(AddrSpace *as, const LinuxImage *img, char *const argv[], char *const envp[], const char *execfn, u64 *sp_out);

/* kernel/linux/signal.c: builds the handler's frame (an x86-64 Linux rt_sigframe) for signal_deliver; 0 or a negative errno. oldmask is the mask the handler returns to. */
int  linux_signal_frame(TrapFrame *tf, int sig, struct gsigaction *act, u32 oldmask);

/* kernel/sys/futex.c: what the Linux futex call needs beyond the native operations */
long futex_wait_deadline(u64 uaddr, u32 val, u64 deadline);
long futex_wake_addr(u64 uaddr, int n);
long futex_requeue_addr(u64 uaddr, int nwake, u64 nrequeue, u64 uaddr2, bool check, u32 expected);

/* kernel/sys/syscalls.c: the native handler of a native call number, for the Linux calls that mean exactly what a native call means. */
SyscallFn syscall_native(int nr);

#endif
