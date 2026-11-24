/* The PE32+ (x86-64) image format, as far as winrun needs it: our own declarations, written from Microsoft's published "PE Format" specification
 * (Microsoft Learn, the PE/COFF specification: "MS-DOS Stub", "Signature", "COFF File Header", "Optional Header (Image Only)", "Section Table",
 * "The .idata Section", "The .reloc Section"). No Windows SDK header was looked at; the numbers are the specification's.
 *
 * The parser in pe.c is pure (no system calls, no allocation): it reads a file that is already in memory, refuses anything that is not exactly valid with a
 * one-line reason, and builds the in-memory image into a caller's buffer. That keeps it testable on the host (tests/host/test_winrun.py fuzzes it). */
#ifndef WINRUN_PE_H
#define WINRUN_PE_H
#include <stddef.h>
#include <stdint.h>

#define PE_MACHINE_AMD64        0x8664
#define PE_OPT_MAGIC_PE32PLUS   0x20B
#define PE_FILE_RELOCS_STRIPPED 0x0001
#define PE_FILE_EXECUTABLE      0x0002
#define PE_FILE_DLL             0x2000
#define PE_SUBSYSTEM_GUI        2
#define PE_SUBSYSTEM_CONSOLE    3
#define PE_DLLCHAR_DYNAMIC_BASE 0x0040

#define PE_SCN_CNT_UNINIT       0x00000080
#define PE_SCN_MEM_EXECUTE      0x20000000
#define PE_SCN_MEM_READ         0x40000000
#define PE_SCN_MEM_WRITE        0x80000000

#define PE_DIR_EXPORT   0
#define PE_DIR_IMPORT   1
#define PE_DIR_BASERELOC 5
#define PE_DIR_TLS      9
#define PE_DIR_IAT      12
#define PE_NDIRS        16

#define PE_REL_ABSOLUTE 0
#define PE_REL_DIR64    10

#define PE_MAX_SECTIONS 96          /* the specification's limit for an image */
#define PE_MAX_IMAGE    (1u << 30)  /* our limit: 1 GiB of image is far beyond any console program */
#define PE_USER_TOP     0x0000800000000000ull

typedef struct {
    char name[9];                   /* NUL-terminated copy of the 8-byte name */
    uint32_t vaddr, vsize;          /* RVA and VirtualSize (0 means "SizeOfRawData") */
    uint32_t raw_off, raw_size;     /* PointerToRawData, SizeOfRawData */
    uint32_t chars;                 /* Characteristics (PE_SCN_*) */
    uint32_t mem_size;              /* what the section occupies in memory: max(vsize, raw_size) rounded up to SectionAlignment */
} PeSection;

typedef struct {
    const uint8_t *file;
    size_t file_len;
    uint16_t machine, nsections, characteristics, subsystem, dll_characteristics;
    uint64_t image_base;
    uint32_t entry_rva, section_align, file_align, size_of_image, size_of_headers;
    uint32_t ndirs;
    uint32_t dir_rva[PE_NDIRS], dir_size[PE_NDIRS];
    PeSection sec[PE_MAX_SECTIONS];
} PeImage;

/* One import, as pe_imports hands it out. name is NULL for an import by ordinal. */
typedef struct {
    const char *dll;
    const char *name;
    uint16_t hint, ordinal;
    uint32_t iat_rva;               /* where the resolved 64-bit address goes */
} PeImport;

/* Every function returns 0 on success or -1 with a one-line reason in err (no newline). */
int pe_parse(PeImage *pe, const uint8_t *file, size_t len, char *err, size_t errlen);
/* Copies the headers and the sections into img (size_of_image bytes, which the caller zeroed). */
void pe_build(const PeImage *pe, uint8_t *img);
/* Walks the import table of a built image. cb returns 0 to go on, nonzero to stop (that value is returned). */
typedef int (*pe_import_cb)(void *ctx, const PeImport *imp);
int pe_imports(const PeImage *pe, const uint8_t *img, pe_import_cb cb, void *ctx, char *err, size_t errlen);
/* Applies the base relocations for a move by delta (new base - ImageBase). Validates every block and entry before writing anything; *count gets the number of
 * DIR64 fix-ups. A delta of 0 still validates. */
int pe_relocate(const PeImage *pe, uint8_t *img, uint64_t delta, unsigned *count, char *err, size_t errlen);
/* The import walk with nothing resolved: validates the whole table. */
int pe_check(const PeImage *pe, const uint8_t *img, char *err, size_t errlen);

#endif
