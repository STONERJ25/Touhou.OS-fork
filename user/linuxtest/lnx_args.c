/*
 * Checks the initial stack the kernel builds for a Linux program (System V psABI, "Process Initialization"): alignment, argc/argv/envp, and the auxiliary vector
 * entries a static C library relies on. Prints only facts that hold on every Linux, so the output of this program under a real Linux is the expected output.
 *
 * Run it as:  FOO=bar linux lnx_args alpha beta     (on a real Linux: env -i FOO=bar ./lnx_args alpha beta)
 */
#include "lnx.h"

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

extern char _start[];

static int ends_with(const char *s, const char *tail) {
    unsigned long n = lnx_strlen(s), m = lnx_strlen(tail);
    return n >= m && lnx_streq(s + n - m, tail);
}

void lnx_main(long *sp) {
    long argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    lnx_line("stack pointer is 16-byte aligned", ((u64)sp & 15) == 0);
    lnx_line("argc", argc);
    lnx_line("argv[0] is this program", argc > 0 && ends_with(argv[0], "lnx_args"));
    lnx_line("argv ends with a null", argv[argc] == 0);
    for (long i = 1; i < argc; i++) { lnx_puts("argv: "); lnx_puts(argv[i]); lnx_puts("\n"); }
    long nenv = 0;
    int foo = 0;
    while (envp[nenv]) { if (lnx_streq(envp[nenv], "FOO=bar")) foo = 1; nenv++; }
    lnx_line("environment has FOO=bar", foo);

    u64 *aux = (u64 *)(envp + nenv + 1);
    u64 phdr = 0, phent = 0, phnum = 0, pagesz = 0, base = 1, entry = 0, uid = 99, euid = 99, gid = 99, egid = 99, platform = 0, hwcap = 0, clktck = 0, secure = 9, rnd = 0, execfn = 0;
    int ended = 0;
    for (; aux[0]; aux += 2) {
        switch (aux[0]) {
        case AT_PHDR: phdr = aux[1]; break;
        case AT_PHENT: phent = aux[1]; break;
        case AT_PHNUM: phnum = aux[1]; break;
        case AT_PAGESZ: pagesz = aux[1]; break;
        case AT_BASE: base = aux[1]; break;
        case AT_ENTRY: entry = aux[1]; break;
        case AT_UID: uid = aux[1]; break;
        case AT_EUID: euid = aux[1]; break;
        case AT_GID: gid = aux[1]; break;
        case AT_EGID: egid = aux[1]; break;
        case AT_PLATFORM: platform = aux[1]; break;
        case AT_HWCAP: hwcap = aux[1]; break;
        case AT_CLKTCK: clktck = aux[1]; break;
        case AT_SECURE: secure = aux[1]; break;
        case AT_RANDOM: rnd = aux[1]; break;
        case AT_EXECFN: execfn = aux[1]; break;
        }
    }
    ended = aux[0] == AT_NULL;
    lnx_line("auxv ends with AT_NULL", ended);
    lnx_line("AT_PAGESZ", (long)pagesz);
    lnx_line("AT_PHENT", (long)phent);
    lnx_line("AT_PHNUM is at least 1", phnum >= 1);
    lnx_line("AT_PHDR follows an ELF header", phdr != 0 && *(u32 *)(phdr - 64) == 0x464c457f);
    lnx_line("AT_BASE is 0 for a static program", base == 0);
    lnx_line("AT_ENTRY is _start", entry == (u64)_start);
    lnx_line("AT_UID is getuid", (long)uid == lnx_sys0(L_getuid));
    lnx_line("AT_EUID is geteuid", (long)euid == lnx_sys0(L_geteuid));
    lnx_line("AT_GID is getgid", (long)gid == lnx_sys0(L_getgid));
    lnx_line("AT_EGID is getegid", (long)egid == lnx_sys0(L_getegid));
    lnx_line("AT_PLATFORM", platform ? 0 : -1);
    if (platform) { lnx_puts("AT_PLATFORM string: "); lnx_puts((const char *)platform); lnx_puts("\n"); }
    lnx_line("AT_HWCAP is not 0", hwcap != 0);
    lnx_line("AT_CLKTCK", (long)clktck);
    lnx_line("AT_SECURE", (long)secure);
    int nonzero = 0;
    if (rnd) for (int i = 0; i < 16; i++) nonzero |= ((u8 *)rnd)[i];
    lnx_line("AT_RANDOM points at 16 random bytes", rnd != 0 && nonzero);
    lnx_line("AT_EXECFN names this program", execfn != 0 && ends_with((const char *)execfn, "lnx_args"));
    lnx_exit(0);
}
