/*
 * Loader -> kernel handoff structure. Included by both the UEFI loader
 * (PE/COFF, MS ABI) and the kernel (ELF, SysV ABI), so it only uses
 * fixed-width integer types and has the same layout under both ABIs.
 */
#ifndef SHARED_BOOTINFO_H
#define SHARED_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC      0x4F4B59534E474F54ULL
#define BOOTINFO_VERSION    1u

/* Virtual address layout the loader sets up before jumping to the kernel. */
#define HHDM_BASE           0xFFFF800000000000ULL   /* phys 0 is mapped here, RAM only is guaranteed */
#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL   /* kernel image is linked here */

/* OS-defined UEFI memory types (0x80000000..0xFFFFFFFF are reserved for loaders). */
#define EFI_MEMTYPE_KERNEL  0x80000001u
#define EFI_MEMTYPE_MODULE  0x80000002u

enum {
    BOOTMEM_USABLE = 1,         /* free RAM */
    BOOTMEM_BOOT_RECLAIMABLE,   /* loader code/data, BootInfo, boot page tables: free after boot */
    BOOTMEM_ACPI_RECLAIMABLE,
    BOOTMEM_ACPI_NVS,
    BOOTMEM_RESERVED,
    BOOTMEM_MMIO,
    BOOTMEM_KERNEL,             /* kernel image */
    BOOTMEM_MODULE,             /* initrd and other loaded files */
};

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} BootMemRegion;

enum {
    BOOTFB_BGRX = 0,            /* bytes in memory: B, G, R, X  (u32 == 0x00RRGGBB) */
    BOOTFB_RGBX = 1,            /* bytes in memory: R, G, B, X  (u32 == 0x00BBGGRR) */
};

typedef struct {
    uint64_t base;              /* physical address, 0 if no framebuffer was found */
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per scanline */
    uint32_t bpp;               /* always 32 */
    uint32_t format;            /* BOOTFB_* */
    uint32_t reserved;
} BootFramebuffer;

#define BOOTINFO_CMDLINE_MAX 256

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved0;

    uint64_t hhdm_base;

    BootFramebuffer fb;

    uint64_t mmap_phys;         /* physical address of BootMemRegion[mmap_count], sorted, non-overlapping */
    uint32_t mmap_count;
    uint32_t reserved1;

    uint64_t kernel_phys;       /* where the kernel image was copied */
    uint64_t kernel_virt;       /* lowest virtual address of the image */
    uint64_t kernel_size;       /* bytes, page aligned */

    uint64_t initrd_phys;       /* 0 if none */
    uint64_t initrd_size;

    uint64_t rsdp_phys;         /* ACPI RSDP, 0 if none */

    uint64_t arena_phys;        /* loader scratch memory (page tables, this struct); free it when done */
    uint64_t arena_size;

    char cmdline[BOOTINFO_CMDLINE_MAX];
} BootInfo;

#endif /* SHARED_BOOTINFO_H */
