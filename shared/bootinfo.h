/*
 * Loader -> kernel handoff structure. Included by both the UEFI loader
 * (PE/COFF, MS ABI) and the kernel (ELF, SysV ABI), so it only uses
 * fixed-width integer types and has the same layout under both ABIs.
 */
#ifndef SHARED_BOOTINFO_H
#define SHARED_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC      0x4F4B59534E474F54ULL
#define BOOTINFO_VERSION    5u      /* 2: boot_part; 3: report_phys, report_len; 4: efi_reset_system; 5: persist_*, wdt_* */

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

/* The partition the loader was read from: the HardDrive node of the boot volume's UEFI device path (UEFI 2.x, 10.3.5.1). valid is 0 when the
 * firmware gave none (a CD, a network boot). The kernel shows it in /proc/bootdisk; the installer uses it to find its source files and to
 * refuse the disk the system is running from. */
typedef struct {
    uint32_t valid;
    uint32_t part_number;       /* 1-based entry of the partition table */
    uint64_t part_start;        /* first logical block */
    uint64_t part_size;         /* logical blocks */
    uint8_t  signature[16];     /* GPT: the partition's unique GUID (on-disk byte order); MBR: the 32-bit disk signature in the first 4 bytes */
    uint8_t  table;             /* 1 MBR, 2 GPT */
    uint8_t  sig_type;          /* 0 none, 1 MBR signature, 2 GUID */
    uint8_t  reserved[6];
} BootPartition;

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

    BootPartition boot_part;    /* version >= 2 */

    uint64_t report_phys;       /* version >= 3: text the loader wants in the kernel log (the "netdiag" report), 0 if none; loader data, read it before the memory is reclaimed */
    uint64_t report_len;

    uint64_t efi_reset_system;  /* version >= 4: the address of the firmware's ResetSystem runtime service (physical: SetVirtualAddressMap is never called), 0 if there is none */

    /* version >= 5 (docs/RELIABILITY.md): the panic record region and the hardware watchdog */
    uint64_t persist_phys;      /* RAM the loader took out of the memory map (type BOOTMEM_RESERVED) for the panic record, which survives a warm reset; 0 = none */
    uint64_t persist_len;
    uint32_t persist_note;      /* BOOT_PERSIST_*: how the loader got it, or why not */
    uint32_t wdt_state;         /* BOOT_WDT_*: what the loader did with the TCO watchdog */
    uint32_t wdt_seconds;       /* the timeout it armed it with (seconds from now to the restart), 0 if it did not arm it */
    uint32_t wdt_tcobase;       /* the TCO I/O base it found, 0 if none */
} BootInfo;

/* The panic record region: where the loader puts it unless "persist=0xADDR" (page aligned, below 4 GiB) or "persist=off" says otherwise. Low
 * enough to be RAM on every PC, above what firmware uses while it starts (it allocates from the top of memory down, and so does the loader). */
#define BOOT_PERSIST_PHYS   0x03F00000ULL
#define BOOT_PERSIST_LEN    0x10000ULL      /* 64 KiB */

#define BOOT_PERSIST_MOR    0x100u          /* flag in persist_note: MemoryOverwriteRequestControl asks the firmware to clear memory at a reset (left by Windows): no record can survive */
enum {
    BOOT_PERSIST_NONE = 0,                  /* an older loader (version < 5) leaves it 0 too */
    BOOT_PERSIST_OK,                        /* allocated: persist_phys is the region */
    BOOT_PERSIST_OFF,                       /* "persist=off" */
    BOOT_PERSIST_BUSY,                      /* the firmware uses that memory in this start */
    BOOT_PERSIST_NOTRAM,                    /* not RAM on this machine */
};

enum {
    BOOT_WDT_NONE = 0,                      /* not looked at (an older loader leaves it 0 too) */
    BOOT_WDT_ARMED,                         /* running, NO_REBOOT clear: a kernel that does not feed it restarts the machine */
    BOOT_WDT_ARMED_NR,                      /* running, but NO_REBOOT stays set: it cannot restart the machine */
    BOOT_WDT_OFF,                           /* "wdt=off" */
    BOOT_WDT_NOFEED,                        /* the kernel image does not carry BOOT_WDT_MARK: it would not feed the timer, so it is not armed */
    BOOT_WDT_NOTFOUND,                      /* no TCO watchdog on this chipset */
};
#define BOOT_WDT_STATE(s)   ((s) & 0xFFu)
#define BOOT_WDT_NR_CLEARED 0x100u          /* flag in wdt_state: NO_REBOOT was set when the loader looked, and it cleared it */

/* A kernel that feeds the TCO watchdog from early in its start (kernel/drivers/tcowdt.c) carries this string; the loader arms the timer only for
 * such a kernel, so that falling back to an older kernel never turns into a restart every two minutes. */
#define BOOT_WDT_MARK       "Gensokyo:TCO-watchdog-feeder:1"

#endif /* SHARED_BOOTINFO_H */
