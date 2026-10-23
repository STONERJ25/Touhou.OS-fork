/* mcc -- Marisa's compiler for TouhouOS.  Shared declarations.
 *
 * mcc is built as one translation unit: main.c includes every *.inc file after this header.  That keeps the
 * program free of symbol clashes with libgap and makes it trivial for mcc to compile itself (stage 2).
 * The source is plain C11 without VLAs, so it builds with the host clang and with mcc.
 */
#ifndef MCC_H
#define MCC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;

#define MCC_VERSION "0.1"
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) < (y) ? (y) : (x))

/* ------------------------------------------------------------------ util.inc */

void *xmalloc(size_t n);
void *xcalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xformat(const char *fmt, ...);
char *xvformat(const char *fmt, va_list ap);
void fatal(const char *fmt, ...) __attribute__((noreturn));

typedef struct {
    u8 *p;
    size_t len, cap;
} Buf;

typedef struct {
    char *key;
    int keylen;
    void *val;
} MapEntry;

typedef struct {
    MapEntry *buckets;
    int capacity;
    int used;
} Map;

/* ------------------------------------------------------------------ ELF */

#define EI_NIDENT 16
typedef struct {
    u8 e_ident[EI_NIDENT];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
    u32 sh_name, sh_type;
    u64 sh_flags, sh_addr, sh_offset, sh_size;
    u32 sh_link, sh_info;
    u64 sh_addralign, sh_entsize;
} Shdr;

typedef struct {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

typedef struct {
    u32 st_name;
    u8 st_info, st_other;
    u16 st_shndx;
    u64 st_value, st_size;
} ElfSym;

typedef struct {
    u64 r_offset, r_info;
    i64 r_addend;
} Rela;

enum { ET_REL = 1, ET_EXEC = 2 };
enum { SHT_NULL = 0, SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8, SHT_GROUP = 17 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4, SHF_MERGE = 0x10, SHF_STRINGS = 0x20, SHF_INFO_LINK = 0x40, SHF_TLS = 0x400 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4, STT_COMMON = 5, STT_TLS = 6 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2 };
enum { PT_LOAD = 1, PT_TLS = 7, PT_GNU_STACK = 0x6474e551 };
enum { PF_X = 1, PF_W = 2, PF_R = 4 };
enum {
    R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_GOT32 = 3, R_X86_64_PLT32 = 4,
    R_X86_64_GOTPCREL = 9, R_X86_64_32 = 10, R_X86_64_32S = 11, R_X86_64_16 = 12, R_X86_64_PC16 = 13,
    R_X86_64_8 = 14, R_X86_64_PC8 = 15, R_X86_64_TPOFF64 = 18, R_X86_64_GOTTPOFF = 22, R_X86_64_TPOFF32 = 23,
    R_X86_64_PC64 = 24, R_X86_64_GOTPCRELX = 41, R_X86_64_REX_GOTPCRELX = 42
};

#define ELF64_R_SYM(i) ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)((i) & 0xffffffffu))
#define ELF64_R_INFO(s, t) (((u64)(s) << 32) | (u64)(t))
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_INFO(b, t) (((b) << 4) | ((t) & 0xf))

#endif
