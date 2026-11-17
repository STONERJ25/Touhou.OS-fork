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

/*
 * Which personality does an executable file ask for (docs/COMPAT.md, "The personality is chosen by the file")? Written from elf(5) (the ELF header's e_ident[EI_OSABI], program
 * headers, the note format: namesz, descsz, type, then the name and the descriptor each padded to a multiple of the note segment's alignment) and from the GNU ABI note
 * specification published with the Linux Standard Base ("ABI note tag": owner "GNU", type 1, a descriptor of four words whose first is the operating system, 0 for Linux).
 * Our own note (user/user.ld) has the owner "Gensokyo".
 *
 * A file is read here only as far as its program headers and its note segments; whatever is wrong with it is left for the real loader to report (a file this function
 * cannot parse simply carries no marker).
 */
#define EI_OSABI         7
#define ELFOSABI_GNU     3                       /* a.k.a. ELFOSABI_LINUX: what a static glibc program has (it uses IFUNC) */
#define NT_GNU_ABI_TAG   1
#define ELF_NOTE_OS_LINUX 0
#define NT_GENSOKYO_ABI  1
#define NOTE_SEGMENT_MAX 4096                    /* the notes of a real program are a few dozen bytes; a bigger segment is not looked at */
#define NOTE_SEGMENTS_MAX 8

static unsigned scan_note_segment(File *f, const Elf64_Phdr *ph) {
    if (ph->p_filesz < 12 || ph->p_filesz > NOTE_SEGMENT_MAX) return 0;
    u8 *buf = kmalloc(ph->p_filesz);
    if (!buf) return 0;
    unsigned mark = 0;
    if (exec_read_exact(f, ph->p_offset, buf, ph->p_filesz) == 0) {
        u64 pad = ph->p_align >= 8 ? 8 : 4;                                 /* a segment aligned to 8 (GNU properties) pads its names and descriptors to 8 */
        u64 len = ph->p_filesz;
        for (u64 at = 0; at + 12 <= len; ) {
            u32 namesz, descsz, type;
            memcpy(&namesz, buf + at, 4);
            memcpy(&descsz, buf + at + 4, 4);
            memcpy(&type, buf + at + 8, 4);
            u64 name = at + 12, desc = name + ((u64)namesz + pad - 1) / pad * pad, next = desc + ((u64)descsz + pad - 1) / pad * pad;
            if (namesz > len || descsz > len || desc > len || next < desc || desc + descsz > len) break;      /* a note that runs off the segment ends the list */
            if (type == NT_GNU_ABI_TAG && namesz == 4 && !memcmp(buf + name, "GNU", 4) && descsz >= 4) {
                u32 os;
                memcpy(&os, buf + desc, 4);
                if (os == ELF_NOTE_OS_LINUX) mark |= ELF_MARK_LINUX;
            } else if (type == NT_GENSOKYO_ABI && namesz == 9 && !memcmp(buf + name, "Gensokyo", 9) && descsz >= 4) {
                mark |= ELF_MARK_NATIVE;
            }
            at = next;
        }
    }
    kfree(buf);
    return mark;
}

unsigned linux_elf_mark(File *f) {
    ElfFile e;
    if (elf_read_headers(f, true, &e)) return 0;
    unsigned mark = 0;
    if (e.eh.e_ident[EI_OSABI] == ELFOSABI_GNU) mark |= ELF_MARK_LINUX;
    int segments = 0;
    for (int i = 0; i < e.eh.e_phnum; i++) {
        const Elf64_Phdr *ph = &e.ph[i];
        if (ph->p_type == PT_INTERP) mark |= ELF_MARK_LINUX;                /* native programs are static: whatever dynamic linker is named, the file is for the Linux loader */
        else if (ph->p_type == PT_NOTE && segments++ < NOTE_SEGMENTS_MAX) mark |= scan_note_segment(f, ph);
    }
    elf_free_headers(&e);
    return mark;
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
        if (!rc) {                                                      /* the interpreter's segments are named after it in /proc/<pid>/maps, as Linux names them (links resolved) */
            char canon[256];
            path_canonical(root, interp, canon, sizeof canon);
            file_set_path(ifile, canon);
        }
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
