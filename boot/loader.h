#ifndef LOADER_H
#define LOADER_H

#include "uefi.h"
#include "../shared/bootinfo.h"

extern EFI_SYSTEM_TABLE *g_st;
extern EFI_BOOT_SERVICES *g_bs;     /* NULL once ExitBootServices has succeeded */

/* util.c */
void  serial_init(void);
void  log_str(const char *s);
void  log_hex(UINT64 v);
void  log_dec(UINT64 v);
void  enable_nxe(void);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
int   guid_equal(const EFI_GUID *a, const EFI_GUID *b);
__attribute__((noreturn)) void loader_halt(void);
void  pc_beep(int n);               /* n short beeps on the PC speaker: a second channel when the screen shows nothing (needs "stagebeep") */
__attribute__((noreturn)) void pc_reset(void);   /* about six seconds from now: restart the machine (the PCH reset register, then a triple fault) */

/* paging.c */
typedef struct {
    UINT64 vaddr;
    UINT64 memsz;
    UINT32 flags;       /* PF_* */
} KernelSegment;

typedef struct {
    UINT64        phys_base;    /* physical address the image was copied to */
    UINT64        virt_base;    /* lowest virtual address (page aligned) */
    UINT64        size;         /* bytes, page aligned */
    UINT64        entry;
    UINT32        nseg;
    KernelSegment seg[8];
} KernelImage;

void  arena_init(UINT64 base, UINT64 size);
void *arena_alloc(UINT64 size, UINT64 align);        /* zeroed */
UINT64 build_page_tables(const KernelImage *k, UINT64 max_ram_end, UINT64 handoff_code_addr);
__attribute__((noreturn)) void handoff(UINT64 pml4_phys, UINT64 entry, UINT64 stack_top, UINT64 bootinfo_virt);

#endif /* LOADER_H */
