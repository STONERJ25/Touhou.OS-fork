/*
 * /proc/dmi: what the firmware's SMBIOS table says about this machine (kernel/include/dmi.h). The loader copied the structure table
 * (BootInfo.smbios_*); it sits in memory that is handed to the page allocator after the PCI scan, so it is parsed once, early, from
 * hwinfo_early() in hwinfo.c. The walker and the text are in dmi_parse.c (host-tested).
 */
#include "kernel.h"
#include "dmi.h"
#include "mm.h"
#include "vfs.h"

static DmiInfo g_dmi;

static size_t gen_dmi(char *buf, size_t cap) { return dmi_format(&g_dmi, buf, cap); }

void dmi_init(void) {
    if (g_boot.version >= 6 && g_boot.smbios_kind && g_boot.smbios_phys && g_boot.smbios_len) {
        dmi_parse(&g_dmi, (const u8 *)phys_to_virt(g_boot.smbios_phys), g_boot.smbios_len, g_boot.smbios_ver);
        g_dmi.kind = g_boot.smbios_kind;
        g_dmi.flags = g_boot.smbios_flags;
        klog("Gensokyo", "dmi: SMBIOS %u.%u, %u structures: %s %s, BIOS %s %s (%s)", g_dmi.ver >> 8, g_dmi.ver & 0xFF, g_dmi.structures,
             g_dmi.sys_manufacturer[0] ? g_dmi.sys_manufacturer : g_dmi.bb_manufacturer, g_dmi.sys_product[0] ? g_dmi.sys_product : g_dmi.bb_product,
             g_dmi.bios_vendor, g_dmi.bios_version, g_dmi.bios_date);
    } else {
        klog("Gensokyo", "dmi: the firmware gave no SMBIOS table%s", g_boot.version >= 6 ? "" : " (an older loader does not pass it on)");
        dmi_parse(&g_dmi, NULL, 0, 0);
    }
    procfs_register("dmi", gen_dmi);
}
