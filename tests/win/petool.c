/* Host driver for winrun's PE parser (user/bin/winrun/pe.c), built by tests/host/test_winrun.py with gcc and the address and undefined-behaviour
 * sanitizers. It runs every stage the loader runs that does not need the target's memory: parse, build the image in a buffer, walk the imports, apply the
 * relocations for a move.
 *
 *     petool FILE                 prints what it found (or "refused: REASON"), in a form the test compares with llvm-readobj
 *     petool --fuzz FILE SEED N   N damaged copies of FILE (truncations, flipped bits, overwritten fields near the headers), every one parsed and loaded;
 *                                 prints "accepted A refused R": a crash or a sanitizer report is the failure */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pe.h"

static int print_import(void *ctx, const PeImport *imp) {
    (void)ctx;
    if (imp->name) printf("import %s %s 0x%x\n", imp->dll, imp->name, imp->iat_rva);
    else printf("import %s #%u 0x%x\n", imp->dll, imp->ordinal, imp->iat_rva);
    return 0;
}

static int count_import(void *ctx, const PeImport *imp) {
    size_t n = strlen(imp->dll) + (imp->name ? strlen(imp->name) : 0);
    *(size_t *)ctx += n;
    return 0;
}

/* All stages; 0 when the file is accepted, -1 when refused (err says why). */
static int load(const uint8_t *f, size_t len, int verbose, char *err, size_t errlen) {
    static PeImage pe;
    if (pe_parse(&pe, f, len, err, errlen)) return -1;
    uint8_t *img = calloc(1, pe.size_of_image);
    if (!img) { snprintf(err, errlen, "no memory"); return -1; }
    pe_build(&pe, img);
    int rc = 0;
    size_t sum = 0;
    if (pe_imports(&pe, img, verbose ? print_import : count_import, &sum, err, errlen)) rc = -1;
    unsigned n = 0;
    uint64_t delta = pe.ndirs > PE_DIR_BASERELOC && pe.dir_size[PE_DIR_BASERELOC] ? 0x7650000ull : 0;     /* a move, when the image can be moved */
    if (!rc && pe_relocate(&pe, img, delta, &n, err, errlen)) rc = -1;
    if (!rc && verbose) {
        printf("base 0x%llx entry 0x%x size 0x%x headers 0x%x subsystem %u\n", (unsigned long long)pe.image_base, pe.entry_rva, pe.size_of_image,
               pe.size_of_headers, pe.subsystem);
        for (unsigned i = 0; i < pe.nsections; i++)
            printf("section %s 0x%x 0x%x 0x%x 0x%x 0x%x\n", pe.sec[i].name, pe.sec[i].vaddr, pe.sec[i].vsize, pe.sec[i].raw_off, pe.sec[i].raw_size, pe.sec[i].chars);
        printf("relocs %u\n", n);
    }
    free(img);
    return rc;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); exit(2); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    if (fread(b, 1, n, fp) != (size_t)n) exit(2);
    fclose(fp);
    *len = (size_t)n;
    return b;
}

static uint64_t rng;
static uint32_t rnd(void) { rng = rng * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(rng >> 33); }

int main(int argc, char **argv) {
    char err[256];
    if (argc == 2) {
        size_t len;
        uint8_t *f = slurp(argv[1], &len);
        if (load(f, len, 1, err, sizeof err)) printf("refused: %s\n", err);
        free(f);
        return 0;
    }
    if (argc != 5 || strcmp(argv[1], "--fuzz")) { fprintf(stderr, "usage: petool FILE | petool --fuzz FILE SEED N\n"); return 2; }
    size_t len;
    uint8_t *orig = slurp(argv[2], &len);
    rng = strtoull(argv[3], NULL, 0);
    long count = strtol(argv[4], NULL, 0);
    long acc = 0, ref = 0;
    for (long i = 0; i < count; i++) {
        size_t n = len;
        uint8_t *f = malloc(len);                       /* exactly the damaged length: the sanitizer sees any read past it */
        memcpy(f, orig, len);
        switch (rnd() % 4) {
        case 0: n = rnd() % (len + 1); break;           /* truncated */
        case 1: for (int k = 1 + rnd() % 8; k--;) f[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); break;           /* bits flipped anywhere */
        case 2: for (int k = 1 + rnd() % 4; k--;) { size_t at = rnd() % (len < 1024 ? len : 1024); f[at] ^= (uint8_t)(1u << (rnd() % 8)); } break;   /* in the headers */
        case 3: { size_t at = rnd() % (len < 1024 ? len - 4 : 1020); uint32_t v = rnd() % 4 ? rnd() : rnd() % 0x4000; memcpy(f + at, &v, 4); break; }  /* a field overwritten */
        }
        uint8_t *g = malloc(n ? n : 1);
        memcpy(g, f, n);
        if (load(g, n, 0, err, sizeof err)) ref++; else acc++;
        free(g);
        free(f);
    }
    printf("accepted %ld refused %ld\n", acc, ref);
    free(orig);
    return 0;
}
