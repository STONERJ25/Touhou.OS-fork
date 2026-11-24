/* Resolving the import table of a mapped image (Microsoft's "PE Format" specification, "The .idata Section": one directory entry per DLL, an import lookup
 * table of names or ordinals, and the import address table the loader overwrites with the addresses).
 *
 * A DLL that winrun does not build in is an error at load time: nothing of the program could work without it. A function that is not built in is not: it
 * gets a stub of its own that names it and ends the process with status 127 if, and only if, the program calls it. Programs import more than any one run
 * uses, so this keeps them running as far as the layer reaches. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "winrun.h"

static const char *g_stub_dll[100], *g_stub_name[100];
static uint16_t g_stub_ord[100];

static void stub_hit(int i) {
    fflush(NULL);
    if (g_stub_name[i]) fprintf(stderr, "winrun: unimplemented %s!%s\n", g_stub_dll[i], g_stub_name[i]);
    else fprintf(stderr, "winrun: unimplemented %s!#%u\n", g_stub_dll[i], g_stub_ord[i]);
    fflush(stderr);
    _exit(127);
}
static WINAPI void stub_overflow(void) {
    fflush(NULL);
    fprintf(stderr, "winrun: unimplemented function (one of more than 90 missing imports)\n");
    fflush(stderr);
    _exit(127);
}

/* One function per missing import, numbered 10 to 99: the number says which name to print. */
#define S(n) static WINAPI void stub_##n(void) { stub_hit(n); }
#define S10(a) S(a##0) S(a##1) S(a##2) S(a##3) S(a##4) S(a##5) S(a##6) S(a##7) S(a##8) S(a##9)
S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9)
#define P(n) { n, (void *)stub_##n },
#define P10(a) P(a##0) P(a##1) P(a##2) P(a##3) P(a##4) P(a##5) P(a##6) P(a##7) P(a##8) P(a##9)
static const struct { int n; void *fn; } g_stubs[] = { P10(1) P10(2) P10(3) P10(4) P10(5) P10(6) P10(7) P10(8) P10(9) };
#define NSTUBS ((int)(sizeof g_stubs / sizeof g_stubs[0]))

typedef struct {
    uint8_t *img;
    int used;
    char *err;
    size_t errlen;
} Ctx;

static int resolve_one(void *vctx, const PeImport *imp) {
    Ctx *c = vctx;
    const Win32Dll *dll = NULL;
    for (const Win32Dll *d = win32_dlls; d->dll; d++)
        if (!strcasecmp(d->dll, imp->dll)) dll = d;
    if (!dll) {
        snprintf(c->err, c->errlen, "imports %s, which is not built in (only KERNEL32.dll)", imp->dll);
        return -1;
    }
    void *fn = NULL;
    int stub = 0;
    if (imp->name)
        for (const Win32Export *e = dll->exports; e->name && !fn; e++)
            if (!strcmp(e->name, imp->name)) fn = e->fn;
    if (!fn) {
        stub = 1;
        if (c->used < NSTUBS) {
            int n = g_stubs[c->used].n;
            g_stub_dll[n] = imp->dll;
            g_stub_name[n] = imp->name;
            g_stub_ord[n] = imp->ordinal;
            fn = g_stubs[c->used++].fn;
        } else fn = (void *)stub_overflow;
    }
    memcpy(c->img + imp->iat_rva, &fn, 8);
    if (winrun_verbose) {
        if (imp->name) fprintf(stderr, "winrun: import %s!%s -> %s\n", imp->dll, imp->name, stub ? "unimplemented (stub)" : "built in");
        else fprintf(stderr, "winrun: import %s!#%u -> unimplemented (stub)\n", imp->dll, imp->ordinal);
    }
    return 0;
}

int winrun_resolve(const PeImage *pe, uint8_t *img, char *err, size_t errlen) {
    Ctx c = { img, 0, err, errlen };
    return pe_imports(pe, img, resolve_one, &c, err, errlen) ? -1 : 0;
}
