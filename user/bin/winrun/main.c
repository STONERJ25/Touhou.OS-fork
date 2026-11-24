/* winrun: runs a Windows console program (PE32+, x86-64) on TouhouOS (docs/WINDOWS.md).
 *
 *     winrun [-v] PROGRAM.EXE [ARGUMENTS...]
 *
 * Reads the file, checks it completely (pe.c), maps it (load.c), fills its import address table from the built-in Win32 layer (imports.c, win32.c) and calls
 * its entry point with the Windows x64 calling convention (Microsoft Learn, "x64 calling convention": the compiler does the conversion for an ms_abi
 * function pointer). The command line is built the way Windows programs expect to parse it (Microsoft Learn, "Parsing C++ command-line arguments": a
 * space separates arguments, an argument with a space, a tab or a quote is quoted, backslashes are doubled only in front of a quote).
 *
 * Exit status: what the program gives ExitProcess (or returns from its entry), 126 for a file that cannot be run, 127 for a call of a function that is not
 * built in, 2 for a usage error. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <gensokyo.h>
#include "winrun.h"

int winrun_verbose;

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        *cap = (*len + n + 1) * 2;
        *buf = realloc(*buf, *cap);
        if (!*buf) { fprintf(stderr, "winrun: out of memory\n"); exit(126); }
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void quote_arg(char **b, size_t *l, size_t *c, const char *a) {
    if (*a && !strpbrk(a, " \t\n\v\"")) { append(b, l, c, a, strlen(a)); return; }
    append(b, l, c, "\"", 1);
    for (const char *p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) { for (size_t i = 0; i < 2 * bs; i++) append(b, l, c, "\\", 1); break; }         /* before the closing quote: doubled */
        if (*p == '"') { for (size_t i = 0; i < 2 * bs + 1; i++) append(b, l, c, "\\", 1); }     /* before a quote: doubled, and one more escapes it */
        else for (size_t i = 0; i < bs; i++) append(b, l, c, "\\", 1);                            /* elsewhere: literal */
        append(b, l, c, p, 1);
    }
    append(b, l, c, "\"", 1);
}

int main(int argc, char **argv) {
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-v")) { winrun_verbose = 1; i++; }
    if (i >= argc) {
        fprintf(stderr, "usage: winrun [-v] PROGRAM.EXE [ARGUMENTS...]\n");
        return 2;
    }
    const char *path = argv[i];
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "winrun: %s: cannot open\n", path);
        return 126;
    }
    if (st.st_size > PE_MAX_IMAGE) { fprintf(stderr, "winrun: %s: file too large\n", path); return 126; }
    size_t len = (size_t)st.st_size;
    uint8_t *file = malloc(len ? len : 1);
    size_t got = 0;
    while (file && got < len) {
        long r = read(fd, file + got, len - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!file || got != len) { fprintf(stderr, "winrun: %s: read failed\n", path); return 126; }

    static PeImage pe;
    char err[256];
    if (pe_parse(&pe, file, len, err, sizeof err)) { fprintf(stderr, "winrun: %s: %s\n", path, err); return 126; }
    uint64_t base = winrun_load(&pe, err, sizeof err);
    if (!base) { fprintf(stderr, "winrun: %s: %s\n", path, err); return 126; }
    if (winrun_resolve(&pe, (uint8_t *)base, err, sizeof err)) { fprintf(stderr, "winrun: %s: %s\n", path, err); return 126; }
    winrun_protect(&pe, base);

    char *cmd = NULL;
    size_t cl = 0, cc = 0;
    append(&cmd, &cl, &cc, "", 0);
    for (int k = i; k < argc; k++) {
        if (k > i) append(&cmd, &cl, &cc, " ", 1);
        quote_arg(&cmd, &cl, &cc, argv[k]);
    }
    /* The program's own stack: 1 MiB (the default SizeOfStackReserve of the linkers), so that the TEB can name its exact bounds. The pages arrive on first
     * touch; a C runtime's __chkstk probes them in order from the top. */
    uint64_t stack_size = 1ull << 20;
    uint8_t *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) { fprintf(stderr, "winrun: no memory for the stack\n"); return 126; }
    win32_init(base, path, cmd, (uint64_t)stack, (uint64_t)stack + stack_size);
    free(file);                                     /* the image is in its own mapping now; pe.file is not used after this */

    /* gs: points at the TEB (SYS_gsbase, docs/WINDOWS.md). A kernel without the call answers -ENOSYS: programs with no C runtime still run. */
    void *teb = win32_teb();
    gap_set_gsbase(teb);
    int have_gs = gap_get_gsbase() == teb;
    void *entry = (void *)(base + pe.entry_rva);
    if (winrun_verbose) fprintf(stderr, "winrun: entry 0x%llx, TEB 0x%llx (gs %s), stack 0x%llx-0x%llx, command line [%s]\n", (unsigned long long)(base + pe.entry_rva),
                                (unsigned long long)teb, have_gs ? "set" : "NOT set: this kernel has no SYS_gsbase", (unsigned long long)stack,
                                (unsigned long long)stack + stack_size, cmd);
    fflush(NULL);
    uint32_t rc = winrun_call_on_stack(entry, stack + stack_size);
    fflush(NULL);
    _exit((int)rc);
}

/* winrun_call_on_stack(fn, top) in the System V convention (rdi, rsi): switch to the new stack, leave the 32 bytes of shadow space the Windows x64
 * convention gives every callee (rsp 16-byte aligned at the call), call fn, come back to the old stack. fn may clobber what the Windows convention lets
 * it (rax, rcx, rdx, r8-r11): rbx, rbp, r12-r15 are callee-saved in both conventions. */
__asm__(".text\n.globl winrun_call_on_stack\nwinrun_call_on_stack:\n"
        "push %rbp\n mov %rsp, %rbp\n"
        "mov %rsi, %rsp\n and $-16, %rsp\n sub $32, %rsp\n"
        "call *%rdi\n"
        "mov %rbp, %rsp\n pop %rbp\n ret\n");
