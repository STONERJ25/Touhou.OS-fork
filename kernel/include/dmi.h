/*
 * DMI / SMBIOS: the strings a PC's firmware keeps about itself (BIOS vendor and date, system and board manufacturer, product names,
 * chassis type, processor sockets, memory modules). The loader copies the structure table out of firmware memory (boot/fwinfo.c,
 * BootInfo.smbios_*); the kernel parses that copy once, early (kernel/arch/x86_64/dmi.c), and shows the result as /proc/dmi.
 *
 * kernel/arch/x86_64/dmi_parse.c is the half that needs no machine: the structure walker (DMTF DSP0134) and the text formatter. It
 * includes nothing but types.h, so tests/host/test_dmi.py compiles it with gcc and checks it against Python's own reading of the
 * same bytes.
 */
#ifndef K_DMI_H
#define K_DMI_H

#include "types.h"

#define DMI_STR      64
#define DMI_MAX_CPU  8
#define DMI_MAX_MEM  32

typedef struct {
    char socket[24], manufacturer[32], version[DMI_STR];
    u16 max_mhz, cur_mhz;               /* 0 = not stated */
    u16 cores, enabled, threads;        /* 0 = not stated */
} DmiCpu;

typedef struct {
    char locator[32], bank[24], manufacturer[32], part[32], serial[32];
    u32 size_mib;                       /* 0: an empty slot */
    u16 speed, configured;              /* MT/s, 0 = not stated */
    u8  type, form;
} DmiMem;

typedef struct {
    bool ok;                            /* a table was walked */
    u16  ver;                           /* the entry point's version, major << 8 | minor */
    u8   kind, flags;                   /* 2 or 3; BOOT_SMBIOS_* */
    u32  structures, bytes;             /* walked / handed in */
    bool cut;                           /* the table ended in the middle of a structure, or without its type 127 terminator */

    char bios_vendor[DMI_STR], bios_version[DMI_STR], bios_date[24];
    u8   bios_major, bios_minor, ec_major, ec_minor;        /* 0xFF: not stated */

    char sys_manufacturer[DMI_STR], sys_product[DMI_STR], sys_version[DMI_STR], sys_serial[DMI_STR], sys_sku[DMI_STR], sys_family[DMI_STR];
    u8   uuid[16];
    bool have_uuid;

    char bb_manufacturer[DMI_STR], bb_product[DMI_STR], bb_version[DMI_STR], bb_serial[DMI_STR], bb_asset[DMI_STR];

    bool have_chassis;
    u8   chassis_type;
    char ch_manufacturer[DMI_STR], ch_version[DMI_STR], ch_serial[DMI_STR], ch_asset[DMI_STR];

    DmiCpu cpu[DMI_MAX_CPU];
    int    ncpu;                        /* structures seen (cpu[] holds the first DMI_MAX_CPU) */
    DmiMem mem[DMI_MAX_MEM];
    int    nmem;                        /* memory device structures seen, empty slots included (mem[] holds the first DMI_MAX_MEM) */
} DmiInfo;

/* Walks `len` bytes of SMBIOS structures (every length is checked; a broken table gives less, never a fault). `ver` is major << 8 | minor
 * of the entry point, which decides how the UUID is laid out (DSP0134: fields in little-endian order from 2.6 on). Zeroes *d first. */
void   dmi_parse(DmiInfo *d, const u8 *table, u32 len, u16 ver);

/* The "key: value" text of /proc/dmi; returns the length (always NUL terminated, never more than cap - 1). */
size_t dmi_format(const DmiInfo *d, char *buf, size_t cap);

const char *dmi_chassis_name(u8 type);          /* "Desktop", "Notebook" ... (NULL for a number the specification does not define) */
const char *dmi_memory_type_name(u8 type);      /* "DDR4" ... (NULL if unknown) */

void   dmi_init(void);                          /* dmi.c: parses the loader's copy of the table and registers /proc/dmi; before pmm_reclaim_boot_memory() */

#endif
