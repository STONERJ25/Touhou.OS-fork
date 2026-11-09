/*
 * Loading a Linux program (docs/COMPAT.md, L2). Written from the ELF specification ("Program Loading", "Dynamic Linking": PT_LOAD, PT_INTERP, the load bias of a
 * position-independent object) and the System V x86-64 psABI ("Process Initialization": what the interpreter finds on the stack and in the auxiliary vector). No implementation
 * of a kernel or of a dynamic linker was read; the behaviour that matters (which addresses the interpreter expects, which auxiliary entries a C library needs) was observed
 * through the differential tests of tests/case_linux_dyn.py.
 *
 * What the kernel does and what it leaves to the interpreter: it maps the program's segments, maps the interpreter's segments, builds the stack (kernel/linux/stack.c) and starts
 * at the interpreter's first instruction with AT_PHDR/AT_ENTRY describing the program and AT_BASE the interpreter. The interpreter relocates itself, finds the program through
 * AT_PHDR, maps the libraries with mmap, relocates everything and jumps to AT_ENTRY. A program without an interpreter (a static executable, a static-PIE) starts at its own entry
 * and, for a static-PIE, relocates itself.
 */
#include "linux.h"
#include "kernel.h"

/* Where a position-independent executable goes (the address Linux uses when it does not randomize), and a deterministic placement in the mmap area for the interpreter. */
#define PIE_BASE 0x555555554000UL

static u64 round_up_to(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

/* The path in PT_INTERP, as a kmalloc'd string; *out is NULL when the program has no interpreter. */
static int read_interp_path(File *f, const ElfFile *e, char **out) {
    *out = NULL;
    for (int i = 0; i < e->eh.e_phnum; i++) {
        const Elf64_Phdr *ph = &e->ph[i];
        if (ph->p_type != PT_INTERP) continue;
        if (ph->p_filesz < 2 || ph->p_filesz >= PATH_MAX) return -ENOEXEC;
        char *s = kzalloc(ph->p_filesz + 1);
        if (!s) return -ENOMEM;
        int rc = exec_read_exact(f, ph->p_offset, s, ph->p_filesz);
        if (rc || s[ph->p_filesz - 1] != 0) { kfree(s); return rc ? rc : -ENOEXEC; }      /* the name is a C string; the segment holds its NUL */
        *out = s;
        return 0;
    }
    return 0;
}

int linux_load_elf(File *f, AddrSpace *as, LinuxImage *img, u64 *start, VNode *root, VNode *cwd, const Cred *cred) {
    ElfFile e, ie;
    memset(&ie, 0, sizeof ie);
    int rc = elf_read_headers(f, true, &e);
    if (rc) return rc;

    char *interp = NULL;
    File *ifile = NULL;
    ElfMapped m, im;
    u64 ibase = 0;
    rc = read_interp_path(f, &e, &interp);
    u64 bias = 0;                                                       /* a fixed-address program sits where it says; a position-independent one where we put it */
    if (!rc && e.eh.e_type == ET_DYN) bias = round_up_to(PIE_BASE, e.align) - e.lo;
    if (!rc) rc = elf_map(f, as, &e, bias, &m);

    if (!rc && interp) {
        rc = vfs_open_root(root, interp, O_RDONLY, 0, cred, cwd, &ifile);        /* a missing interpreter is ENOENT, as execve(2) says */
        if (!rc && !S_ISREG(ifile->vn->mode)) rc = -EACCES;
        if (!rc) rc = elf_read_headers(ifile, true, &ie);
        u64 ibias = 0;
        if (!rc && ie.eh.e_type == ET_DYN) {                            /* the interpreter goes into the mmap area, below whatever the libraries will get */
            u64 base = uvm_find_free(as, ie.hi - ie.lo + ie.align);
            if (!base) rc = -ENOMEM;
            else ibias = round_up_to(base, ie.align) - ie.lo;
        }
        if (!rc) rc = elf_map(ifile, as, &ie, ibias, &im);
        if (!rc) ibase = ibias + ie.lo;
    }

    if (!rc) {
        as->brk_start = as->brk = m.hi;                                 /* the heap starts right after the program, not after the interpreter */
        img->entry = m.entry;                                           /* AT_ENTRY is the program's entry point even when the interpreter runs first */
        img->phdr = m.phdr;
        img->phent = e.eh.e_phentsize;
        img->phnum = e.eh.e_phnum;
        img->interp_base = ibase;
        *start = interp ? im.entry : m.entry;
    }
    if (ifile) file_put(ifile);
    kfree(interp);
    elf_free_headers(&ie);
    elf_free_headers(&e);
    return rc;
}
