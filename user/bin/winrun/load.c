/* Mapping a PE32+ image (Microsoft's "PE Format" specification: an image is loaded by placing the headers and every section at ImageBase + its RVA, and
 * when it cannot sit at ImageBase, by adding the difference to every place the .reloc section lists).
 *
 * Where an image may go is a TouhouOS question, not a PE one. The native mmap with MAP_FIXED replaces whatever is already mapped there, so winrun asks for
 * a fixed placement only where nothing of its own can be: above the room its own program and its brk heap can ever take (the heap may grow to 1 GiB past
 * where it starts, kernel/sys/syscalls.c do_brk) and below the kernel's mmap area (USER_MMAP_BASE, 0x100000000000, kernel/include/process.h). The usual base of
 * a 64-bit image, 0x140000000, is in that window. Anything else is mapped where the kernel chooses and relocated. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "winrun.h"

#define NATIVE_MMAP_BASE 0x0000100000000000ull
#define BRK_ROOM         (1ull << 30)

/* The lowest address a fixed image may start at: past winrun's own image and everything its heap may grow into. */
static uint64_t fixed_floor(void) {
    uint64_t brk = (uint64_t)sbrk(0);
    return ((brk + 0xFFFF) & ~0xFFFFull) + BRK_ROOM + (64ull << 20);
}

uint64_t winrun_load(const PeImage *pe, char *err, size_t errlen) {
    uint64_t want = pe->image_base, size = pe->size_of_image;
    uint64_t base = 0;
    const char *why = NULL;
    if (want < fixed_floor()) why = "below the end of winrun's own memory";
    else if (want + size > NATIVE_MMAP_BASE) why = "inside the kernel's mmap area";
    if (!why) {
        void *p = mmap((void *)want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == (void *)want) base = want;
        else {
            if (p != MAP_FAILED) munmap(p, size);
            why = "the fixed mapping was refused";
        }
    }
    if (!base) {
        if (pe->characteristics & PE_FILE_RELOCS_STRIPPED) {
            snprintf(err, errlen, "ImageBase 0x%llx is not usable (%s) and the image has no relocations", (unsigned long long)want, why);
            return 0;
        }
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            snprintf(err, errlen, "no memory for an image of %llu bytes", (unsigned long long)size);
            return 0;
        }
        base = (uint64_t)p;
        if (winrun_verbose) fprintf(stderr, "winrun: ImageBase 0x%llx not used (%s): relocating\n", (unsigned long long)want, why);
    }
    uint8_t *img = (uint8_t *)base;
    pe_build(pe, img);
    unsigned n = 0;
    if (pe_relocate(pe, img, base - want, &n, err, errlen)) {
        munmap(img, size);
        return 0;
    }
    if (winrun_verbose) {
        fprintf(stderr, "winrun: image at 0x%llx (ImageBase 0x%llx, %llu bytes, %u relocations applied)\n", (unsigned long long)base,
                (unsigned long long)want, (unsigned long long)size, base != want ? n : 0);
        for (unsigned i = 0; i < pe->nsections; i++) {
            const PeSection *s = &pe->sec[i];
            fprintf(stderr, "winrun:   %-8s 0x%llx-0x%llx %c%c%c\n", s->name, (unsigned long long)(base + s->vaddr), (unsigned long long)(base + s->vaddr + s->mem_size),
                    s->chars & PE_SCN_MEM_READ ? 'r' : '-', s->chars & PE_SCN_MEM_WRITE ? 'w' : '-', s->chars & PE_SCN_MEM_EXECUTE ? 'x' : '-');
        }
    }
    return base;
}

void winrun_protect(const PeImage *pe, uint64_t base) {
    uint64_t hdr = ((uint64_t)pe->size_of_headers + 0xFFF) & ~0xFFFull;
    mprotect((void *)base, hdr, PROT_READ);
    for (unsigned i = 0; i < pe->nsections; i++) {
        const PeSection *s = &pe->sec[i];
        int prot = (s->chars & PE_SCN_MEM_READ ? PROT_READ : 0) | (s->chars & PE_SCN_MEM_WRITE ? PROT_WRITE : 0) | (s->chars & PE_SCN_MEM_EXECUTE ? PROT_EXEC : 0);
        if (mprotect((void *)(base + s->vaddr), s->mem_size, prot) && winrun_verbose)
            fprintf(stderr, "winrun: mprotect of %s failed: it stays read-write\n", s->name);
    }
}
