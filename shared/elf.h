/* Minimal ELF64 definitions, shared by the loader, the kernel and host-side notes. */
#ifndef SHARED_ELF_H
#define SHARED_ELF_H

#include <stdint.h>

#define EI_NIDENT   16
#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS64  2
#define ELFDATA2LSB 1

#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62

#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_PHDR     6
#define PT_TLS      7

#define PF_X        1
#define PF_W        2
#define PF_R        4

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

static inline int elf64_check(const Elf64_Ehdr *h) {
    return h->e_ident[0] == 0x7f && h->e_ident[1] == 'E' && h->e_ident[2] == 'L' && h->e_ident[3] == 'F'
        && h->e_ident[EI_CLASS] == ELFCLASS64 && h->e_ident[EI_DATA] == ELFDATA2LSB
        && h->e_machine == EM_X86_64 && h->e_phentsize == sizeof(Elf64_Phdr);
}

#endif /* SHARED_ELF_H */
