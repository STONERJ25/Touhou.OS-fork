/* winrun's own pieces (docs/WINDOWS.md): the loader (load.c), the import resolver (imports.c) and the Win32 layer (win32.c). The Windows types are our own
 * minimal declarations from the Win32 documentation on Microsoft Learn (sizes from the x64 calling convention: BOOL and DWORD are 32 bits, HANDLE is a pointer). */
#ifndef WINRUN_H
#define WINRUN_H
#include <stdint.h>
#include "pe.h"

#define WINAPI __attribute__((ms_abi))

typedef struct {
    const char *name;
    void *fn;
} Win32Export;

typedef struct {
    const char *dll;                    /* as the import table names it, compared without case */
    const Win32Export *exports;
} Win32Dll;

extern const Win32Dll win32_dlls[];
extern int winrun_verbose;

/* load.c: maps the image (at ImageBase when it can, otherwise elsewhere and relocated). Returns the base or 0 with a reason in err. */
uint64_t winrun_load(const PeImage *pe, char *err, size_t errlen);
/* imports.c: resolves every import of the mapped image; 0, or -1 with a reason (a DLL that is not built in). */
int winrun_resolve(const PeImage *pe, uint8_t *img, char *err, size_t errlen);
/* imports.c: sets the protections the sections ask for, after the imports are written. */
void winrun_protect(const PeImage *pe, uint64_t base);
/* win32.c */
void win32_init(uint64_t image_base, const char *image_path, const char *cmdline, uint64_t stack_lo, uint64_t stack_hi);
void *win32_teb(void);
/* main.c: calls fn (an ms_abi function without arguments) on the stack whose top is top; returns what it returns */
uint32_t winrun_call_on_stack(void *fn, void *top);

#endif
