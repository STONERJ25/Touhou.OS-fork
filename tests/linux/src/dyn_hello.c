/*
 * What a program built in one of the three dynamic-linking styles sees of its own loading (L2 of docs/COMPAT.md): the same source is built as a dynamic position-independent
 * executable (gcc's default), as a dynamic non-PIE executable (-no-pie: fixed addresses plus an interpreter) and as a static-PIE (-static-pie: no interpreter, relocates
 * itself). The kernel maps the program, the interpreter hands over after relocating itself and the libraries, and everything below is something that is only right when the
 * loader got it right: relocated pointers in data, a thread-local template, the program headers found through the auxiliary vector, the objects the dynamic linker loaded.
 * Every line is a fact the System V ABI and the manual pages promise, so a real Linux prints the same lines; tests/case_linux_dyn.py compares the output under TouhouOS with
 * the stored output of a real Linux (tools/linux-oracle.py).
 *
 * Run as:  FOO=bar dyn_hello alpha beta     (the status is 3)
 */
#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef KIND
#define KIND "unknown"
#endif

extern const ElfW(Ehdr) __ehdr_start;

static int counter = 5;
static int *ptr_to_counter = &counter;                  /* a pointer in writable data: a relative relocation in a PIE */
static const char *const names[] = { "alpha", "beta", "gamma" };      /* pointers in read-only data (relocated, then made read-only: RELRO) */
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
static int (*const table[])(int) = { twice, thrice };
static __thread int tls_var = 42;                       /* a thread-local variable with an initial value: the PT_TLS template */
static __thread char tls_buf[300];

static int collect(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    int *n = data;
    if (strstr(info->dlpi_name, "vdso")) return 0;      /* a real Linux maps a vDSO and lists it; this kernel has none */
    printf("object %d: \"%s\" with %d segments\n", (*n)++, info->dlpi_name, info->dlpi_phnum);
    return 0;
}

int main(int argc, char **argv) {
    printf("hello from a %s program\n", KIND);
    printf("argc=%d\n", argc);
    for (int i = 1; i < argc; i++) printf("argv[%d]=%s\n", i, argv[i]);
    const char *foo = getenv("FOO");
    printf("FOO=%s\n", foo ? foo : "(unset)");

    printf("executable file type: %s\n", __ehdr_start.e_type == ET_DYN ? "ET_DYN" : "ET_EXEC");
    const ElfW(Phdr) *ph = (const ElfW(Phdr) *)getauxval(AT_PHDR);
    unsigned long phnum = getauxval(AT_PHNUM);
    int loads = 0, interp = 0, dynamic = 0, tls = 0;
    unsigned long bias = (unsigned long)-1;
    for (unsigned long i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_LOAD) { loads++; if (bias == (unsigned long)-1) bias = (unsigned long)&__ehdr_start - ph[i].p_vaddr; }
        if (ph[i].p_type == PT_INTERP) interp = 1;
        if (ph[i].p_type == PT_DYNAMIC) dynamic = 1;
        if (ph[i].p_type == PT_TLS) tls = 1;
    }
    printf("program headers through AT_PHDR: %lu entries of %lu bytes, %d loadable, interpreter %s, dynamic section %s, TLS %s\n", phnum, getauxval(AT_PHENT),
           loads, interp ? "named" : "none", dynamic ? "present" : "absent", tls ? "present" : "absent");
    printf("AT_PHDR lies inside the loaded image: %s\n", (unsigned long)ph >= (unsigned long)&__ehdr_start ? "yes" : "no");
    printf("AT_ENTRY is the entry point of the file (plus the load bias): %s\n", getauxval(AT_ENTRY) == __ehdr_start.e_entry + (__ehdr_start.e_type == ET_DYN ? bias : 0) ? "yes" : "no");
    printf("AT_BASE %s\n", getauxval(AT_BASE) ? "names the interpreter" : "is zero");
    printf("AT_PAGESZ=%lu AT_CLKTCK=%lu AT_SECURE=%lu AT_PLATFORM=%s\n", getauxval(AT_PAGESZ), getauxval(AT_CLKTCK), getauxval(AT_SECURE), (const char *)getauxval(AT_PLATFORM));
    const char *execfn = (const char *)getauxval(AT_EXECFN);
    const char *slash = execfn ? strrchr(execfn, '/') : NULL;
    printf("AT_EXECFN ends in: %s\n", slash ? slash + 1 : execfn);
    const unsigned char *rnd = (const unsigned char *)getauxval(AT_RANDOM);
    int nonzero = 0;
    for (int i = 0; rnd && i < 16; i++) nonzero |= rnd[i];
    printf("AT_RANDOM has bytes: %s\n", nonzero ? "yes" : "no");

    printf("relocated pointer: %d\n", *ptr_to_counter);
    *ptr_to_counter = 9;
    printf("through the variable: %d\n", counter);
    printf("relocated strings: %s %s %s\n", names[0], names[1], names[2]);
    printf("function table: %d %d\n", table[0](7), table[1](7));
    printf("thread-local variable: %d", tls_var);
    tls_var = 7;
    memset(tls_buf, 'x', sizeof tls_buf - 1);
    printf(" then %d, buffer of %zu\n", tls_var, strlen(tls_buf));

    int n = 0;
    dl_iterate_phdr(collect, &n);
    char *block = malloc(1 << 20);
    memset(block, 1, 1 << 20);
    printf("malloc of 1 MiB: %d\n", block[(1 << 20) - 1]);
    free(block);
    printf("float: %.3f %g\n", 3.14159, 0.5);
    return 3;
}
