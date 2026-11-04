/*
 * What the firmware says about itself, for the hardware report (docs/HARDWARE_BREADTH.md, the `hwreport` command): the SMBIOS
 * structure table, the firmware's vendor and revision, whether Secure Boot is on, and the graphics modes it offered.
 *
 * Follows the UEFI specification (2.x, section 4.6 "EFI Configuration Table": the vendor GUIDs of the SMBIOS and SMBIOS 3 entry
 * points; section 3.3 "Globally Defined Variables": SecureBoot and SetupMode; section 12.9: Graphics Output) and the DMTF's SMBIOS
 * Reference Specification (DSP0134): the 2.x entry point is the 31-byte "_SM_" structure with its "_DMI_" intermediate anchor, the 3.x
 * one the 24-byte "_SM3_" structure. The structure table itself is copied into loader memory: the firmware may keep it in boot
 * services memory that the kernel gets back, so the kernel parses its copy early (kernel/arch/x86_64/dmi.c).
 *
 * Everything here only reads. Nothing in the firmware's state is changed, nothing is written to the screen except one log line, and
 * a machine without any of this (no SMBIOS, no GOP, no Secure Boot variable) simply leaves the fields at zero.
 */
#include "loader.h"

static EFI_GUID smbios3Guid   = { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } };
static EFI_GUID smbios2Guid   = { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static EFI_GUID globalVarGuid = { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };
static EFI_GUID gopGuid2      = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

typedef EFI_STATUS (EFIAPI *GetVarFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attr, UINTN *size, void *data);

static UINT8 sum8(const UINT8 *p, UINTN n) {
    UINT8 s = 0;
    while (n--) s = (UINT8)(s + *p++);
    return s;
}

/* The two entry points, 3.x first (the specification says a table of 3.0 or later is reachable through "_SM3_" and a 2.x reader may
 * find the older anchor beside it): the address and length of the structure table and the version. Returns 1 if there is one. */
static int find_smbios(BootInfo *bi, UINT64 *table, UINT32 *len) {
    const UINT8 *ep3 = 0, *ep2 = 0;
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &g_st->ConfigurationTable[i];
        if (guid_equal(&t->VendorGuid, &smbios3Guid)) ep3 = (const UINT8 *)t->VendorTable;
        else if (guid_equal(&t->VendorGuid, &smbios2Guid)) ep2 = (const UINT8 *)t->VendorTable;
    }
    if (ep3 && !memcmp(ep3, "_SM3_", 5) && ep3[6] >= 0x18 && ep3[6] <= 0x40) {
        UINT64 addr;
        UINT32 max;
        memcpy(&max, ep3 + 0x0C, 4);                    /* structure table maximum size */
        memcpy(&addr, ep3 + 0x10, 8);                   /* structure table address */
        if (addr && max >= 4) {
            bi->smbios_kind = 3;
            bi->smbios_ver = (UINT16)(ep3[7] << 8 | ep3[8]);
            if (sum8(ep3, ep3[6])) bi->smbios_flags |= BOOT_SMBIOS_BADSUM;
            *table = addr;
            *len = max;
            return 1;
        }
    }
    if (ep2 && !memcmp(ep2, "_SM_", 4) && ep2[5] >= 0x1F && ep2[5] <= 0x40 && !memcmp(ep2 + 0x10, "_DMI_", 5)) {
        UINT16 tlen, count;
        UINT32 addr;
        memcpy(&tlen, ep2 + 0x16, 2);                   /* structure table length */
        memcpy(&addr, ep2 + 0x18, 4);                   /* structure table address */
        memcpy(&count, ep2 + 0x1C, 2);                  /* number of structures */
        if (addr && tlen >= 4) {
            bi->smbios_kind = 2;
            bi->smbios_ver = (UINT16)(ep2[6] << 8 | ep2[7]);
            bi->smbios_count = count;
            if (sum8(ep2, ep2[5]) || sum8(ep2 + 0x10, 15)) bi->smbios_flags |= BOOT_SMBIOS_BADSUM;
            *table = addr;
            *len = tlen;
            return 1;
        }
    }
    return 0;
}

static void copy_smbios(BootInfo *bi) {
    UINT64 table = 0;
    UINT32 len = 0;
    if (!find_smbios(bi, &table, &len) || table >= (1ULL << 44)) {
        bi->smbios_kind = 0;
        log_str("loader: the firmware provides no SMBIOS table\n");
        return;
    }
    if (len > BOOT_SMBIOS_MAX) { len = BOOT_SMBIOS_MAX; bi->smbios_flags |= BOOT_SMBIOS_CUT; }
    UINT64 pages = (len + 4095ULL) / 4096ULL;
    EFI_PHYSICAL_ADDRESS copy = 0;
    if (EFI_ERROR(g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &copy))) {
        bi->smbios_kind = 0;
        log_str("loader: no memory for a copy of the SMBIOS table\n");
        return;
    }
    memcpy((void *)copy, (const void *)table, len);
    bi->smbios_phys = copy;
    bi->smbios_len = len;
    log_str("loader: SMBIOS "); log_dec(bi->smbios_ver >> 8); log_str("."); log_dec(bi->smbios_ver & 0xFF);
    log_str(bi->smbios_kind == 3 ? " (64-bit entry point), " : " (32-bit entry point), ");
    log_dec(len); log_str(" bytes of structures copied\n");
}

static void firmware_facts(BootInfo *bi) {
    bi->uefi_revision = g_st->Hdr.Revision;
    bi->fw_revision = g_st->FirmwareRevision;
    if (g_st->FirmwareVendor) {
        UINTN n = 0;
        for (; n < sizeof bi->fw_vendor - 1 && g_st->FirmwareVendor[n]; n++) {
            CHAR16 c = g_st->FirmwareVendor[n];
            bi->fw_vendor[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        bi->fw_vendor[n] = 0;
    }

    /* Secure Boot (the loader is unsigned: a machine that has it on will not even have started this code, but a firmware that shows the
     * variable as on while letting an unsigned loader run is worth knowing about) */
    CHAR16 sb[] = { 'S', 'e', 'c', 'u', 'r', 'e', 'B', 'o', 'o', 't', 0 };
    CHAR16 sm[] = { 'S', 'e', 't', 'u', 'p', 'M', 'o', 'd', 'e', 0 };
    UINT8 v = 0;
    UINTN size = 1;
    UINT32 attr = 0;
    GetVarFn get = (GetVarFn)g_st->RuntimeServices->GetVariable;
    bi->secure_boot = BOOT_SB_UNKNOWN;
    if (!EFI_ERROR(get(sb, &globalVarGuid, &attr, &size, &v)) && size == 1) bi->secure_boot = v ? BOOT_SB_ON : BOOT_SB_OFF;
    size = 1;
    bi->setup_mode = 0xFF;
    if (!EFI_ERROR(get(sm, &globalVarGuid, &attr, &size, &v)) && size == 1) bi->setup_mode = v;
}

/* The same display the framebuffer comes from (main.c setup_gop: the console's own handle first, then any). */
static void gop_modes(BootInfo *bi) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (EFI_ERROR(g_bs->HandleProtocol(g_st->ConsoleOutHandle, &gopGuid2, (void **)&gop)) || !gop) {
        gop = 0;
        if (EFI_ERROR(g_bs->LocateProtocol(&gopGuid2, 0, (void **)&gop)) || !gop) return;
    }
    if (!gop->Mode) return;
    bi->gop_count = (UINT16)(gop->Mode->MaxMode > 0xFFFF ? 0xFFFF : gop->Mode->MaxMode);
    bi->gop_current = (UINT16)gop->Mode->Mode;
    UINT16 n = 0;
    for (UINT32 i = 0; i < gop->Mode->MaxMode && n < BOOT_GOP_MAX; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN isz = 0;
        if (EFI_ERROR(gop->QueryMode(gop, i, &isz, &info)) || !info) continue;
        bi->gop[n].w = (UINT16)info->HorizontalResolution;
        bi->gop[n].h = (UINT16)info->VerticalResolution;
        bi->gop[n].index = (UINT16)i;
        bi->gop[n].format = (UINT8)info->PixelFormat;
        n++;
        g_bs->FreePool(info);
    }
    bi->gop_listed = n;
}

/* With boot services still alive, before the exit. */
void fwinfo_collect(BootInfo *bi) {
    copy_smbios(bi);
    firmware_facts(bi);
    gop_modes(bi);
}
