/*
 * The Linux personality (docs/COMPAT.md): a process whose Process.abi is ABI_LINUX makes its system calls with the numbers, structures and conventions of
 * x86-64 Linux, and starts with the System V x86-64 initial stack (argc, argv, envp, auxiliary vector). Written from the psABI and the Linux manual pages and
 * checked against the behaviour of a real Linux by differential tests (tests/case_linux.py); no implementation of Linux, glibc or musl was read.
 */
#ifndef K_LINUX_H
#define K_LINUX_H

#include "process.h"
#include "elf.h"

/* kernel/sys/exec.c: ELF files. elf_read_headers reads and checks the ELF header and the program headers of f (an ET_DYN too when allow_dyn); elf_map maps every PT_LOAD
 * segment at (the address the file names + bias) and fills the pages from the file. The native loader uses them with bias 0, the Linux loader (kernel/linux/loader.c) also
 * for position-independent programs and for the interpreter. */
typedef struct ElfFile {
    Elf64_Ehdr  eh;
    Elf64_Phdr *ph;             /* the program headers (kmalloc'd: elf_free_headers) */
    u64         lo, hi;         /* the pages the PT_LOAD segments cover, at the addresses the file names */
    u64         align;          /* the strictest alignment a segment asks for (at least a page) */
} ElfFile;
typedef struct ElfMapped {
    u64 entry;                  /* e_entry + bias */
    u64 phdr;                   /* where the program headers are in memory (0: no segment holds them) */
    u64 hi;                     /* the end of the highest mapped page: the program break starts there */
} ElfMapped;
int  exec_read_exact(File *f, u64 off, void *buf, size_t n);        /* n bytes at off, or -ENOEXEC when the file ends first */
int  elf_read_headers(File *f, bool allow_dyn, ElfFile *e);
void elf_free_headers(ElfFile *e);
int  elf_map(File *f, AddrSpace *as, const ElfFile *e, u64 bias, ElfMapped *m);

/* What the ELF loader learned about the program: the auxiliary vector is built from it. */
typedef struct LinuxImage {
    u64  entry;                 /* AT_ENTRY */
    u64  phdr;                  /* AT_PHDR: user address of the program headers (0 when no segment holds them) */
    u16  phent, phnum;          /* AT_PHENT, AT_PHNUM */
    u64  interp_base;           /* AT_BASE: where the dynamic linker was loaded (0: the program is static) */
    bool secure;                /* AT_SECURE: the program is set-user-id or set-group-id */
    u32  euid, egid;            /* the credentials the program will run with (AT_EUID, AT_EGID) */
} LinuxImage;

/* kernel/linux/loader.c: loads the ELF file f for a Linux program: an ET_EXEC where it says, an ET_DYN (PIE, static-PIE) at a base of our choosing, and the interpreter
 * named by PT_INTERP (opened in root, a directory "/" means) beside it. *start gets the first instruction to run (the interpreter's, else the program's); img gets what the
 * auxiliary vector needs and the program break is placed after the program. 0 or a negative errno. */
int  linux_load_elf(File *f, AddrSpace *as, LinuxImage *img, u64 *start, VNode *root, VNode *cwd, const Cred *cred);

/* kernel/linux/root.c: the Linux root (docs/COMPAT.md, "The Linux root"). */
VNode *linux_root_get(void);                            /* a reference to the top of the tree mounted on /linux, or NULL when nothing is mounted there */
VNode *linux_exec_root(Process *p);                     /* the root the program p execs will have (a reference, or NULL for the real root); see the rules in root.c */
void   linux_root_adopt(Process *p, VNode *root);       /* execve's commit: p now runs in root (takes over the reference; NULL: the real root) and its cwd is made to fit */

/* kernel/linux/syscall.c: dispatches tf->rax through the Linux table and returns the value for rax (a negative errno on failure). */
long linux_syscall(TrapFrame *tf);

/* kernel/linux/stack.c: maps the stack and lays out argc, argv, envp and the auxiliary vector as the psABI describes; *sp_out is the initial rsp. */
int  linux_build_stack(AddrSpace *as, const LinuxImage *img, char *const argv[], char *const envp[], const char *execfn, u64 *sp_out);

/* kernel/linux/signal.c: builds the handler's frame (an x86-64 Linux rt_sigframe) for signal_deliver; 0 or a negative errno. oldmask is the mask the handler returns to
 * (a kernel signal set, siginfo.h); info is what the handler's siginfo argument shows. */
int  linux_signal_frame(TrapFrame *tf, int sig, struct gsigaction *act, u64 oldmask, const SigInfo *info);

/* kernel/sys/futex.c: what the Linux futex call needs beyond the native operations */
long futex_wait_deadline(u64 uaddr, u32 val, u64 deadline);
long futex_wake_addr(u64 uaddr, int n);
long futex_requeue_addr(u64 uaddr, int nwake, u64 nrequeue, u64 uaddr2, bool check, u32 expected);

/* kernel/sys/syscalls.c: the native handler of a native call number, for the Linux calls that mean exactly what a native call means. */
SyscallFn syscall_native(int nr);

#endif
