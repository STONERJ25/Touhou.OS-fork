/* The PE32+ parser of winrun: written from Microsoft's "PE Format" specification (see pe.h). It trusts nothing in the file: every offset and size is checked
 * against the file's length, against SizeOfImage and against each other before anything is read through it, and the arithmetic is done in 64 bits so that no
 * sum of two 32-bit fields can wrap. Pure C (stdio for the error text, string.h): compiled into winrun and, for the host tests, on its own. */
#include <stdio.h>
#include <string.h>
#include "pe.h"

#define FAIL(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int pow2(uint32_t v) { return v && !(v & (v - 1)); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* COFF file header (20 bytes after "PE\0\0") and the PE32+ optional header: offsets from the specification's tables. */
enum { COFF_MACHINE = 0, COFF_NSECT = 2, COFF_OPTSIZE = 16, COFF_CHARS = 18, COFF_SIZE = 20 };
enum { OPT_MAGIC = 0, OPT_ENTRY = 16, OPT_IMAGEBASE = 24, OPT_SECTALIGN = 32, OPT_FILEALIGN = 36, OPT_SIZEIMAGE = 56, OPT_SIZEHDRS = 60,
       OPT_SUBSYS = 68, OPT_DLLCHARS = 70, OPT_NRVA = 108, OPT_DIRS = 112 };
enum { SEC_VSIZE = 8, SEC_VADDR = 12, SEC_RAWSIZE = 16, SEC_RAWPTR = 20, SEC_CHARS = 36, SEC_SIZE = 40 };

int pe_parse(PeImage *pe, const uint8_t *f, size_t len, char *err, size_t errlen) {
    memset(pe, 0, sizeof *pe);
    pe->file = f;
    pe->file_len = len;
    if (len < 64 || f[0] != 'M' || f[1] != 'Z') FAIL("not a PE file: no MZ header");
    if (len > PE_MAX_IMAGE) FAIL("file too large (%zu bytes)", len);
    uint64_t lfanew = rd32(f + 0x3C);
    if (lfanew < 64 || lfanew + 4 + COFF_SIZE > len) FAIL("e_lfanew 0x%llx is outside the file", (unsigned long long)lfanew);
    if (memcmp(f + lfanew, "PE\0\0", 4)) FAIL("no PE signature at 0x%llx", (unsigned long long)lfanew);
    const uint8_t *coff = f + lfanew + 4;
    pe->machine = rd16(coff + COFF_MACHINE);
    pe->nsections = rd16(coff + COFF_NSECT);
    pe->characteristics = rd16(coff + COFF_CHARS);
    uint32_t optsize = rd16(coff + COFF_OPTSIZE);
    if (pe->machine != PE_MACHINE_AMD64) FAIL("machine 0x%x is not x86-64 (only PE32+ x86-64 images run)", pe->machine);
    if (!(pe->characteristics & PE_FILE_EXECUTABLE)) FAIL("not an executable image");
    if (pe->characteristics & PE_FILE_DLL) FAIL("a DLL, not a program");
    if (pe->nsections == 0 || pe->nsections > PE_MAX_SECTIONS) FAIL("%u sections (1 to %u allowed)", pe->nsections, PE_MAX_SECTIONS);
    if (optsize < OPT_DIRS) FAIL("optional header of %u bytes is too small for PE32+", optsize);
    uint64_t opt_off = lfanew + 4 + COFF_SIZE;
    uint64_t sect_off = opt_off + optsize;
    uint64_t sect_end = sect_off + (uint64_t)pe->nsections * SEC_SIZE;
    if (sect_end > len) FAIL("the section table runs past the end of the file");
    const uint8_t *opt = f + opt_off;
    if (rd16(opt + OPT_MAGIC) != PE_OPT_MAGIC_PE32PLUS) FAIL("optional header magic 0x%x is not PE32+ (0x20b)", rd16(opt + OPT_MAGIC));
    pe->entry_rva = rd32(opt + OPT_ENTRY);
    pe->image_base = rd64(opt + OPT_IMAGEBASE);
    pe->section_align = rd32(opt + OPT_SECTALIGN);
    pe->file_align = rd32(opt + OPT_FILEALIGN);
    pe->size_of_image = rd32(opt + OPT_SIZEIMAGE);
    pe->size_of_headers = rd32(opt + OPT_SIZEHDRS);
    pe->subsystem = rd16(opt + OPT_SUBSYS);
    pe->dll_characteristics = rd16(opt + OPT_DLLCHARS);
    pe->ndirs = rd32(opt + OPT_NRVA);
    if (pe->ndirs > PE_NDIRS) FAIL("%u data directories (at most 16)", pe->ndirs);
    if (OPT_DIRS + (uint64_t)pe->ndirs * 8 > optsize) FAIL("the data directories run past the optional header");
    if (!pow2(pe->file_align) || pe->file_align < 512 || pe->file_align > 65536) FAIL("FileAlignment %u is not a power of two from 512 to 64K", pe->file_align);
    if (!pow2(pe->section_align) || pe->section_align < pe->file_align) FAIL("SectionAlignment %u is not a power of two at least FileAlignment", pe->section_align);
    if (pe->section_align < 4096) FAIL("SectionAlignment %u is below the page size (not supported)", pe->section_align);
    if (pe->image_base & 0xFFFF) FAIL("ImageBase 0x%llx is not a multiple of 64K", (unsigned long long)pe->image_base);
    if (pe->size_of_image == 0 || pe->size_of_image > PE_MAX_IMAGE || pe->size_of_image % pe->section_align)
        FAIL("SizeOfImage 0x%x is zero, too big or not a multiple of SectionAlignment", pe->size_of_image);
    if (pe->image_base + pe->size_of_image > PE_USER_TOP) FAIL("ImageBase 0x%llx is outside the user address space", (unsigned long long)pe->image_base);
    if (pe->size_of_headers < sect_end || pe->size_of_headers > len || pe->size_of_headers % pe->file_align)
        FAIL("SizeOfHeaders 0x%x does not cover the headers, is past the file or is not aligned", pe->size_of_headers);
    if (pe->subsystem == PE_SUBSYSTEM_GUI) FAIL("a GUI program (subsystem 2): only console programs run");
    if (pe->subsystem != PE_SUBSYSTEM_CONSOLE) FAIL("subsystem %u is not a console program", pe->subsystem);
    for (uint32_t i = 0; i < pe->ndirs; i++) {
        pe->dir_rva[i] = rd32(opt + OPT_DIRS + 8 * i);
        pe->dir_size[i] = rd32(opt + OPT_DIRS + 8 * i + 4);
    }

    /* The sections: ascending, page-aligned, no overlap with each other or with the headers, all inside SizeOfImage, raw data inside the file. */
    uint64_t next = align_up(pe->size_of_headers, pe->section_align);
    for (unsigned i = 0; i < pe->nsections; i++) {
        const uint8_t *s = f + sect_off + (uint64_t)i * SEC_SIZE;
        PeSection *o = &pe->sec[i];
        memcpy(o->name, s, 8);
        o->name[8] = 0;
        for (int k = 0; k < 8; k++) if (o->name[k] && (o->name[k] < 0x20 || o->name[k] > 0x7E)) o->name[k] = '?';
        o->vsize = rd32(s + SEC_VSIZE);
        o->vaddr = rd32(s + SEC_VADDR);
        o->raw_size = rd32(s + SEC_RAWSIZE);
        o->raw_off = rd32(s + SEC_RAWPTR);
        o->chars = rd32(s + SEC_CHARS);
        uint64_t span = o->vsize ? o->vsize : o->raw_size;      /* raw data rounded up past VirtualSize is not loaded (pe_build copies only VirtualSize) */
        if (span == 0) FAIL("section %u (%s) is empty", i, o->name);
        if (o->vaddr % pe->section_align) FAIL("section %u (%s) at RVA 0x%x is not aligned", i, o->name, o->vaddr);
        if (o->vaddr < next) FAIL("section %u (%s) at RVA 0x%x overlaps the headers or the section before it", i, o->name, o->vaddr);
        uint64_t mem = align_up(span, pe->section_align);
        if ((uint64_t)o->vaddr + mem > pe->size_of_image) FAIL("section %u (%s) runs past SizeOfImage", i, o->name);
        if (o->raw_size) {
            if (o->raw_off % pe->file_align) FAIL("section %u (%s): raw data at 0x%x is not aligned", i, o->name, o->raw_off);
            if (o->raw_off < pe->size_of_headers) FAIL("section %u (%s): raw data overlaps the headers", i, o->name);
            if ((uint64_t)o->raw_off + o->raw_size > len) FAIL("section %u (%s): raw data runs past the end of the file", i, o->name);
        }
        o->mem_size = (uint32_t)mem;
        next = (uint64_t)o->vaddr + mem;
    }
    if (next != pe->size_of_image) FAIL("SizeOfImage 0x%x does not match the sections' end 0x%llx", pe->size_of_image, (unsigned long long)next);

    /* The entry point lies in an executable section. */
    int entry_ok = 0;
    for (unsigned i = 0; i < pe->nsections; i++) {
        const PeSection *s = &pe->sec[i];
        if (pe->entry_rva >= s->vaddr && pe->entry_rva < (uint64_t)s->vaddr + s->mem_size && (s->chars & PE_SCN_MEM_EXECUTE)) entry_ok = 1;
    }
    if (!entry_ok) FAIL("entry point RVA 0x%x is not in an executable section", pe->entry_rva);

    /* Every data directory that is used lies inside the image (the certificate table, 4, is a file offset, not an RVA). */
    for (uint32_t i = 0; i < pe->ndirs; i++) {
        if (i == 4 || (!pe->dir_rva[i] && !pe->dir_size[i])) continue;
        if ((uint64_t)pe->dir_rva[i] + pe->dir_size[i] > pe->size_of_image || pe->dir_rva[i] < pe->sec[0].vaddr)
            FAIL("data directory %u (RVA 0x%x, %u bytes) is outside the sections", i, pe->dir_rva[i], pe->dir_size[i]);
    }
    if (pe->ndirs > PE_DIR_TLS && pe->dir_size[PE_DIR_TLS]) FAIL("the image has thread-local storage (TLS directory): not supported before W2");
    return 0;
}

void pe_build(const PeImage *pe, uint8_t *img) {
    memcpy(img, pe->file, pe->size_of_headers);
    for (unsigned i = 0; i < pe->nsections; i++) {
        const PeSection *s = &pe->sec[i];
        uint32_t n = s->raw_size;
        if (s->vsize && n > s->vsize) n = s->vsize;              /* the rest of a section beyond its raw data stays zero */
        if (n) memcpy(img + s->vaddr, pe->file + s->raw_off, n);
    }
}

/* A NUL-terminated string at rva inside the image, or NULL. */
static const char *img_str(const PeImage *pe, const uint8_t *img, uint64_t rva, size_t max) {
    if (rva == 0 || rva >= pe->size_of_image) return NULL;
    size_t room = pe->size_of_image - rva;
    if (room > max) room = max;
    const void *z = memchr(img + rva, 0, room);
    return z ? (const char *)(img + rva) : NULL;
}

int pe_imports(const PeImage *pe, const uint8_t *img, pe_import_cb cb, void *ctx, char *err, size_t errlen) {
    if (pe->ndirs <= PE_DIR_IMPORT || !pe->dir_rva[PE_DIR_IMPORT]) return 0;
    uint64_t at = pe->dir_rva[PE_DIR_IMPORT];
    for (unsigned d = 0;; d++, at += 20) {
        if (at + 20 > pe->size_of_image) FAIL("the import directory has no terminating entry");
        if (d > 4096) FAIL("more than 4096 imported DLLs");
        const uint8_t *e = img + at;
        uint32_t ilt = rd32(e), name_rva = rd32(e + 12), iat = rd32(e + 16);
        if (!ilt && !rd32(e + 4) && !rd32(e + 8) && !name_rva && !iat) return 0;
        const char *dll = img_str(pe, img, name_rva, 256);
        if (!dll || !dll[0]) FAIL("import %u: the DLL name at RVA 0x%x is outside the image or not terminated", d, name_rva);
        if (!iat) FAIL("import %u (%s): no import address table", d, dll);
        uint64_t look = ilt ? ilt : iat;
        for (unsigned k = 0;; k++) {
            uint64_t lo = look + 8ull * k, ia = (uint64_t)iat + 8ull * k;
            if (lo + 8 > pe->size_of_image || ia + 8 > pe->size_of_image) FAIL("import %u (%s): the lookup table runs off the image", d, dll);
            if (k > 65536) FAIL("import %u (%s): more than 65536 names", d, dll);
            uint64_t v = rd64(img + lo);
            if (!v) break;
            PeImport imp = { .dll = dll, .iat_rva = (uint32_t)ia };
            if (v >> 63) {
                if (v & 0x7FFFFFFFFFFF0000ull) FAIL("import %u (%s): ordinal entry with reserved bits set", d, dll);
                imp.ordinal = (uint16_t)v;
            } else {
                if (v >> 31) FAIL("import %u (%s): name entry with reserved bits set", d, dll);
                if (v + 2 >= pe->size_of_image) FAIL("import %u (%s): hint/name at RVA 0x%llx is outside the image", d, dll, (unsigned long long)v);
                imp.hint = rd16(img + v);
                imp.name = img_str(pe, img, v + 2, 4096);
                if (!imp.name || !imp.name[0]) FAIL("import %u (%s): the name at RVA 0x%llx runs off the image", d, dll, (unsigned long long)v + 2);
            }
            if (cb) {
                int r = cb(ctx, &imp);
                if (r) return r;
            }
        }
    }
}

int pe_check(const PeImage *pe, const uint8_t *img, char *err, size_t errlen) {
    return pe_imports(pe, img, NULL, NULL, err, errlen);
}

int pe_relocate(const PeImage *pe, uint8_t *img, uint64_t delta, unsigned *count, char *err, size_t errlen) {
    *count = 0;
    uint32_t rva = pe->ndirs > PE_DIR_BASERELOC ? pe->dir_rva[PE_DIR_BASERELOC] : 0;
    uint32_t size = pe->ndirs > PE_DIR_BASERELOC ? pe->dir_size[PE_DIR_BASERELOC] : 0;
    if (!size) {
        if (delta) FAIL("the image has no base relocations and cannot be moved from 0x%llx", (unsigned long long)pe->image_base);
        return 0;
    }
    /* Two passes: the first validates every block and entry, the second writes; a bad table leaves the image untouched. */
    for (int pass = 0; pass < 2; pass++) {
        uint64_t off = 0;
        unsigned n = 0;
        while (off < size) {
            if (off + 8 > size) FAIL("a base relocation block header runs past the directory");
            uint32_t page = rd32(img + rva + off), bsize = rd32(img + rva + off + 4);
            if (bsize < 8 || bsize % 2 || off + bsize > size) FAIL("base relocation block at +0x%llx has a bad size %u", (unsigned long long)off, bsize);
            if (page % 4096 || page >= pe->size_of_image) FAIL("base relocation block at +0x%llx: page RVA 0x%x is outside the image", (unsigned long long)off, page);
            for (uint32_t e = 8; e < bsize; e += 2) {
                uint16_t ent = rd16(img + rva + off + e);
                unsigned type = ent >> 12, where = ent & 0xFFF;
                if (type == PE_REL_ABSOLUTE) continue;
                if (type != PE_REL_DIR64) FAIL("base relocation type %u is not supported (only DIR64 and ABSOLUTE)", type);
                uint64_t at = (uint64_t)page + where;
                if (at + 8 > pe->size_of_image) FAIL("a relocation at RVA 0x%llx is outside the image", (unsigned long long)at);
                if (at < pe->size_of_headers) FAIL("a relocation at RVA 0x%llx points into the headers", (unsigned long long)at);
                if (pass) wr64(img + at, rd64(img + at) + delta);
                n++;
            }
            off += bsize;
        }
        *count = n;
    }
    return 0;
}
