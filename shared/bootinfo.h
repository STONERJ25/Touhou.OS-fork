/*
 * Loader -> kernel handoff structure. Included by both the UEFI loader
 * (PE/COFF, MS ABI) and the kernel (ELF, SysV ABI), so it only uses
 * fixed-width integer types and has the same layout under both ABIs.
 */
#ifndef SHARED_BOOTINFO_H
#define SHARED_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_MAGIC      0x4F4B59534E474F54ULL
#define BOOTINFO_VERSION    6u      /* 2: boot_part; 3: report_phys, report_len; 4: efi_reset_system; 5: persist_*, wdt_*; 6: smbios_*, fw_*, gop_* (the hardware report) */

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

#define BOOT_SMBIOS_MAX     (128u * 1024u)  /* the largest structure table the loader copies */
#define BOOT_SMBIOS_BADSUM  0x01u           /* smbios_flags: the entry point's checksum did not add up (the table is used anyway, like Linux does for a bad ACPI sum) */
#define BOOT_SMBIOS_CUT     0x02u           /* smbios_flags: the table was longer than BOOT_SMBIOS_MAX and was cut */
#define BOOT_GOP_MAX        64              /* graphics modes listed in BootInfo.gop[] */
enum { BOOT_SB_UNKNOWN = 0, BOOT_SB_OFF, BOOT_SB_ON };      /* the "SecureBoot" variable; UNKNOWN: the variable does not exist or could not be read */

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

    /* version >= 6 (docs/HARDWARE_BREADTH.md, the hardware report): what the firmware says about itself. Written by boot/fwinfo.c; the table is
     * loader memory (BOOTMEM_BOOT_RECLAIMABLE), so the kernel parses it before pmm_reclaim_boot_memory(). */
    uint64_t smbios_phys;       /* a copy of the SMBIOS structure table, 0 = the firmware gave none */
    uint32_t smbios_len;        /* bytes copied (at most BOOT_SMBIOS_MAX) */
    uint16_t smbios_ver;        /* the entry point's version, major << 8 | minor: 0x0203 = 2.3, 0x0307 = 3.7 */
    uint8_t  smbios_kind;       /* 0 none, 2 the 32-bit "_SM_" entry point, 3 the 64-bit "_SM3_" one */
    uint8_t  smbios_flags;      /* BOOT_SMBIOS_* */
    uint32_t smbios_count;      /* structures the 2.x entry point announces (0 for 3.x: the table ends with a type 127 structure) */
    uint32_t uefi_revision;     /* EFI_SYSTEM_TABLE.Hdr.Revision: major << 16 | minor */
    uint32_t fw_revision;       /* EFI_SYSTEM_TABLE.FirmwareRevision (the vendor's own number) */
    uint8_t  secure_boot;       /* BOOT_SB_* */
    uint8_t  setup_mode;        /* the "SetupMode" variable: 1 = no platform key is enrolled yet, 0xFF = could not be read */
    uint16_t gop_count;         /* MaxMode: the modes the firmware offered */
    uint16_t gop_current;       /* the mode in use when the loader jumped */
    uint16_t gop_listed;        /* how many entries of gop[] are filled: the first BOOT_GOP_MAX modes */
    char     fw_vendor[48];     /* EFI_SYSTEM_TABLE.FirmwareVendor, narrowed to ASCII */
    struct { uint16_t w, h, index; uint8_t format, reserved; } gop[BOOT_GOP_MAX];    /* format: the UEFI pixel format (0 RGB, 1 BGR, 2 bit mask, 3 blit only) */
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
