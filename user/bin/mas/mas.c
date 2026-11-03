/* mas -- Marisa's assembler.  A small x86-64 assembler for the NASM-syntax files of the kernel and the C library (the nine .asm files of the
 * system), writing the same ELF64 object file NASM does for them, so that the system can assemble its own startup code and interrupt
 * stubs.  Written from the Intel manuals and the NASM manual's description of its syntax and output; no NASM source was used.
 *
 *   mas [-f elf64] [-I dir] [-MD depfile] [-o out.o] [-g] [-F dwarf] source.asm
 *
 * What it understands: sections with attributes, global/extern, labels (with .local names), equ, db dw dd dq, resb..resq, incbin,
 * align, times, default rel, the preprocessor subset of pp.inc (%macro %rep %assign %define %if %include), and the integer
 * instructions of insn.inc.  Everything else is an error that names the line.  -g and -F are accepted and ignored: no debug sections.
 * The file is plain C that mcc compiles, so the system can build its own assembler too. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAS_VERSION "0.1"

#include "util.inc"
#include "lex.inc"
#include "pp.inc"
#include "asm.inc"
#include "insn.inc"
#include "elf.inc"

static char *out_name_for(const char *src) {
    const char *slash = strrchr(src, '/');
    const char *bslash = strrchr(src, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    const char *base = slash ? slash + 1 : src;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    return xfmt("%.*s.o", (int)n, base);
}

int main(int argc, char **argv) {
    const char *src = NULL, *out = NULL, *depfile = NULL;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-f") && i + 1 < argc) {
            if (strcmp(argv[++i], "elf64")) fatal("only -f elf64 is supported");
        } else if (!strcmp(a, "-F") && i + 1 < argc) {
            i++;
        } else if (!strcmp(a, "-g") || !strncmp(a, "-O", 2) || !strncmp(a, "-w", 2) || !strncmp(a, "-W", 2)) {
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out = argv[++i];
        } else if (!strncmp(a, "-I", 2)) {
            const char *d = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (!d) fatal("-I needs a directory");
            if (ninc_dirs >= 32) fatal("too many -I directories");
            inc_dirs[ninc_dirs++] = (char *)d;
        } else if (!strcmp(a, "-MD") && i + 1 < argc) {
            depfile = argv[++i];
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version") || !strcmp(a, "-V")) {
            printf("mas %s -- Marisa Kirisame's assembler, NASM syntax for x86-64 TouhouOS (ze!)\n", MAS_VERSION);
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: mas [-f elf64] [-I dir] [-MD depfile] [-o out.o] [-g] [-F dwarf] source.asm\n");
            printf("mas assembles the NASM-syntax subset the system's own .asm files use (docs/SELFHOST.md).\n");
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fatal("unknown option '%s'", a);
        } else {
            if (src) fatal("one source file at a time");
            src = a;
        }
    }
    if (!src) fatal("no input file");
    char *dflt = out ? NULL : out_name_for(src);
    if (!out) out = dflt;

    cur_file = src;
    read_source_file(src, src);
    if (npconds) fatal("%%if without %%endif");
    if (collecting) fatal("%%macro or %%rep without its end");
    assemble_all();
    write_object(out, src);

    if (depfile) {
        FILE *f = fopen(depfile, "w");
        if (!f) fatal("cannot write '%s': %s", depfile, strerror(errno));
        fprintf(f, "%s: %s", out, src);
        for (int i = 0; i < ndep_files; i++) fprintf(f, " %s", dep_files[i]);
        fprintf(f, "\n");
        fclose(f);
    }
    return 0;
}
