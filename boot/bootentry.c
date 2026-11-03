/*
 * bootentry: with "bootentry" on the command line (the installer writes the word into the cmdline.txt of a system it installs) the loader makes sure that
 * the firmware's boot menu has an entry for this system. A second EFI System Partition on an internal disk, next to Windows', is not always offered
 * by the firmware (the Dell's F12 menu lists what is in the boot variables and the removable media), so the first start from the installed ESP adds
 * one: a Boot#### variable ("TouhouOS", pointing at \EFI\BOOT\BOOTX64.EFI of the partition the loader was read from) and its number at the END of
 * BootOrder, so Windows stays the first choice and no existing entry is touched, replaced or reordered.
 *
 * Done once: a marker variable remembers which partition (its GPT GUID) got which entry, and the entry is checked to still exist. Every failure (a
 * full variable store, a firmware that refuses) is logged and the boot goes on.
 */
#include "loader.h"

typedef EFI_STATUS (EFIAPI *GetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attr, UINTN *size, void *data);
typedef EFI_STATUS (EFIAPI *SetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 attr, UINTN size, void *data);

static EFI_GUID globalGuid  = { 0x8BE4DF61, 0x93CA, 0x11D2, { 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C } };
static EFI_GUID markerGuid  = { 0x7e4d6a10, 0x5b3c, 0x4c1f, { 0x9a, 0x21, 0x6f, 0x0d, 0x2b, 0x8c, 0x31, 0x55 } };
static EFI_GUID devPathGuid2 = EFI_DEVICE_PATH_PROTOCOL_GUID;

#define VAR_ATTR  7u                            /* non-volatile, boot services, runtime services */
#define LOAD_OPTION_ACTIVE 1u
#define FIRST_NUMBER 0x0020u                    /* Windows and the firmware use the low numbers; ours start higher */
#define LAST_NUMBER  0x00FFu

static const CHAR16 g_file[] = { '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'B', 'O', 'O', 'T', 'X', '6', '4', '.', 'E', 'F', 'I', 0 };
static const CHAR16 g_desc[] = { 'T', 'o', 'u', 'h', 'o', 'u', 'O', 'S', 0 };

static void log_boot(UINT32 n) {
    static const char hex[] = "0123456789ABCDEF";
    char t[9] = { 'B', 'o', 'o', 't', hex[(n >> 12) & 15], hex[(n >> 8) & 15], hex[(n >> 4) & 15], hex[n & 15], 0 };
    log_str(t);
}

static void boot_name(CHAR16 *out, UINT32 n) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'B'; out[1] = 'o'; out[2] = 'o'; out[3] = 't';
    for (int i = 0; i < 4; i++) out[4 + i] = (CHAR16)hex[(n >> (12 - 4 * i)) & 0xF];
    out[8] = 0;
}

static EFI_STATUS get_var(const CHAR16 *name, EFI_GUID *vendor, UINT32 *attr, UINTN *size, void *data) {
    return ((GetVariableFn)g_st->RuntimeServices->GetVariable)((CHAR16 *)name, vendor, attr, size, data);
}

static EFI_STATUS set_var(const CHAR16 *name, EFI_GUID *vendor, UINT32 attr, UINTN size, void *data) {
    return ((SetVariableFn)g_st->RuntimeServices->SetVariable)((CHAR16 *)name, vendor, attr, size, data);
}

static int var_exists(const CHAR16 *name, EFI_GUID *vendor) {
    UINTN size = 0;
    EFI_STATUS s = get_var(name, vendor, 0, &size, 0);
    return s == EFI_BUFFER_TOO_SMALL || !EFI_ERROR(s);
}

typedef struct { UINT8 part_guid[16]; UINT16 number; UINT16 pad; } Marker;

static const CHAR16 g_marker_name[] = { 'T', 'o', 'u', 'h', 'o', 'u', 'B', 'o', 'o', 't', 'E', 'n', 't', 'r', 'y', 0 };
static const CHAR16 g_bootorder[]   = { 'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0 };

void bootentry_register(EFI_HANDLE device, const BootPartition *bp) {
    if (!bp->valid || bp->sig_type != 2) {
        log_str("loader: bootentry: the boot partition has no GPT identity; no boot menu entry is made\n");
        return;
    }
    Marker mk = {{0}, 0, 0};
    UINTN msize = sizeof mk;
    if (!EFI_ERROR(get_var(g_marker_name, &markerGuid, 0, &msize, &mk)) && msize == sizeof mk && !memcmp(mk.part_guid, bp->signature, 16)) {
        CHAR16 nm[9];
        boot_name(nm, mk.number);
        if (var_exists(nm, &globalGuid)) {
            log_str("loader: bootentry: "); log_boot(mk.number); log_str(" is this partition's entry already\n");
            return;
        }
    }

    /* the device path of the volume, without its end node, then the file */
    const UINT8 *node = 0;
    if (EFI_ERROR(g_bs->HandleProtocol(device, &devPathGuid2, (void **)&node)) || !node) {
        log_str("loader: bootentry: the boot volume has no device path\n");
        return;
    }
    UINTN dp_len = 0;
    for (int guard = 0; guard < 64; guard++) {
        UINT16 len = (UINT16)(node[dp_len + 2] | (node[dp_len + 3] << 8));
        if (node[dp_len] == 0x7F && node[dp_len + 1] == 0xFF) break;
        if (len < 4) { log_str("loader: bootentry: the device path is damaged\n"); return; }
        dp_len += len;
    }
    UINTN file_node = 4 + sizeof g_file;
    UINTN list_len = dp_len + file_node + 4;
    UINTN opt_len = 4 + 2 + sizeof g_desc + list_len;
    UINT8 *opt = 0;
    if (EFI_ERROR(g_bs->AllocatePool(EfiLoaderData, opt_len, (void **)&opt)) || !opt) { log_str("loader: bootentry: out of memory\n"); return; }
    UINT8 *p = opt;
    UINT32 attrs = LOAD_OPTION_ACTIVE;
    memcpy(p, &attrs, 4); p += 4;
    UINT16 fpl = (UINT16)list_len;
    memcpy(p, &fpl, 2); p += 2;
    memcpy(p, g_desc, sizeof g_desc); p += sizeof g_desc;
    memcpy(p, node, dp_len); p += dp_len;
    p[0] = 4; p[1] = 4; p[2] = (UINT8)file_node; p[3] = (UINT8)(file_node >> 8);    /* media device path, file path */
    memcpy(p + 4, g_file, sizeof g_file);
    p += file_node;
    p[0] = 0x7F; p[1] = 0xFF; p[2] = 4; p[3] = 0;                                  /* end of the device path */

    UINT32 num = FIRST_NUMBER;
    CHAR16 nm[9];
    for (; num <= LAST_NUMBER; num++) {
        boot_name(nm, num);
        if (!var_exists(nm, &globalGuid)) break;
    }
    if (num > LAST_NUMBER) { log_str("loader: bootentry: no free Boot#### number\n"); g_bs->FreePool(opt); return; }
    boot_name(nm, num);
    EFI_STATUS s = set_var(nm, &globalGuid, VAR_ATTR, opt_len, opt);
    g_bs->FreePool(opt);
    if (EFI_ERROR(s)) { log_str("loader: bootentry: the firmware refused to store "); log_boot(num); log_str(" (status "); log_hex(s); log_str(")\n"); return; }

    /* the number goes to the END of BootOrder: nothing existing moves */
    UINT16 order[64];
    UINTN osize = sizeof order;
    UINT32 oattr = VAR_ATTR;
    s = get_var(g_bootorder, &globalGuid, &oattr, &osize, order);
    if (EFI_ERROR(s)) osize = 0;
    UINTN n = osize / 2;
    int present = 0;
    for (UINTN i = 0; i < n; i++) if (order[i] == num) present = 1;
    if (!present) {
        if (n >= 63) { log_str("loader: bootentry: BootOrder is full; the entry exists but is not in the order\n"); return; }
        order[n++] = (UINT16)num;
        s = set_var(g_bootorder, &globalGuid, VAR_ATTR, n * 2, order);
        if (EFI_ERROR(s)) { log_str("loader: bootentry: the firmware refused to change BootOrder (status "); log_hex(s); log_str(")\n"); return; }
    }
    memcpy(mk.part_guid, bp->signature, 16);
    mk.number = (UINT16)num;
    set_var(g_marker_name, &markerGuid, VAR_ATTR, sizeof mk, &mk);

    /* read it back: the log line is the proof */
    UINT16 check[64];
    UINTN csize = sizeof check;
    int ok = !EFI_ERROR(get_var(g_bootorder, &globalGuid, 0, &csize, check)) && csize >= 2 && check[csize / 2 - 1] == (UINT16)num && var_exists(nm, &globalGuid);
    log_str("loader: bootentry: "); log_boot(num); log_str(" \"TouhouOS\" added at the end of BootOrder (");
    log_dec(n); log_str(" entries); read back: "); log_str(ok ? "ok" : "NOT CONFIRMED"); log_str("\n");
}

/* ---- "findinstalled": the stick's loader looks for a TouhouOS that was installed on an internal disk and makes its boot menu entry ---- */

static EFI_GUID sfsGuid2 = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
typedef EFI_STATUS (EFIAPI *LocateHandleBufferFn2)(UINT32 type, EFI_GUID *protocol, void *key, UINTN *count, EFI_HANDLE **buffer);

static int has_file(EFI_FILE_PROTOCOL *root, const char *name) {
    CHAR16 w[40];
    UINTN i = 0;
    for (; name[i] && i < 38; i++) w[i] = (CHAR16)(UINT8)name[i];
    w[i] = 0;
    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(root->Open(root, &f, w, EFI_FILE_MODE_READ, 0)) || !f) return 0;
    f->Close(f);
    return 1;
}

/* The partition identity (the HardDrive node of the device path) of a volume, quietly; *usb says whether a USB node is on the path. */
static int partition_of(EFI_HANDLE dev, BootPartition *bp, int *usb) {
    const UINT8 *node = 0;
    *usb = 0;
    if (EFI_ERROR(g_bs->HandleProtocol(dev, &devPathGuid2, (void **)&node)) || !node) return 0;
    for (int guard = 0; guard < 64; guard++) {
        UINT16 len = (UINT16)(node[2] | (node[3] << 8));
        if (node[0] == 0x7F && node[1] == 0xFF) break;
        if (len < 4) break;
        if (node[0] == 3 && (node[1] == 5 || node[1] == 15)) *usb = 1;          /* messaging: USB, USB class */
        if (node[0] == 4 && node[1] == 1 && len >= 42) {
            memcpy(&bp->part_number, node + 4, 4);
            memcpy(&bp->part_start, node + 8, 8);
            memcpy(&bp->part_size, node + 16, 8);
            memcpy(bp->signature, node + 24, 16);
            bp->table = node[40];
            bp->sig_type = node[41];
            bp->valid = 1;
        }
        node += len;
    }
    return bp->valid;
}

/* A firmware that boots fast only connects the devices it needs: the file systems of the other disks may not exist yet. Every PCI storage controller
 * (class 01: SATA, RAID, NVMe, IDE) is connected, which creates its disks, partitions and file systems; for a controller that is connected already this
 * does nothing. */
typedef EFI_STATUS (EFIAPI *ConnectControllerFn)(EFI_HANDLE controller, EFI_HANDLE *drivers, void *remaining, BOOLEAN recursive);
typedef EFI_STATUS (EFIAPI *PciReadFn)(void *pci_io, UINT32 width, UINT32 offset, UINTN count, void *buffer);
static EFI_GUID pciIoGuid2 = { 0x4cf5b200, 0x68b8, 0x4ca5, { 0x9e, 0xec, 0xb2, 0x3e, 0x3f, 0x50, 0x02, 0x9a } };

static void connect_storage(void) {
    UINTN n = 0;
    EFI_HANDLE *handles = 0;
    if (EFI_ERROR(((LocateHandleBufferFn2)g_bs->LocateHandleBuffer)(2 /* ByProtocol */, &pciIoGuid2, 0, &n, &handles)) || !handles) return;
    for (UINTN i = 0; i < n; i++) {
        void *pci_io = 0;
        if (EFI_ERROR(g_bs->HandleProtocol(handles[i], &pciIoGuid2, &pci_io)) || !pci_io) continue;
        PciReadFn read_cfg = (PciReadFn)((void **)pci_io)[6];                  /* PollMem, PollIo, Mem.Read, Mem.Write, Io.Read, Io.Write, Pci.Read */
        UINT8 cls[3] = { 0, 0, 0 };                                             /* programming interface, subclass, class (config bytes 9 to 11) */
        if (EFI_ERROR(read_cfg(pci_io, 0 /* bytes */, 9, 3, cls)) || cls[2] != 0x01) continue;
        ((ConnectControllerFn)g_bs->ConnectController)(handles[i], 0, 0, 1);
    }
    g_bs->FreePool(handles);
}

void bootentry_find_installed(EFI_HANDLE booted) {
    connect_storage();
    UINTN n = 0;
    EFI_HANDLE *handles = 0;
    if (EFI_ERROR(((LocateHandleBufferFn2)g_bs->LocateHandleBuffer)(2 /* ByProtocol */, &sfsGuid2, 0, &n, &handles)) || !handles) {
        log_str("loader: findinstalled: the firmware lists no file systems\n");
        return;
    }
    int found = 0;
    for (UINTN i = 0; i < n; i++) {
        if (handles[i] == booted) continue;                                   /* the volume this loader came from is not an installed system */
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        EFI_FILE_PROTOCOL *root = 0;
        if (EFI_ERROR(g_bs->HandleProtocol(handles[i], &sfsGuid2, (void **)&fs)) || !fs) continue;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root) continue;
        int ours = has_file(root, "kernel.elf") && has_file(root, "initrd.img") && has_file(root, "cmdline.txt") && has_file(root, "EFI\\BOOT\\BOOTX64.EFI");
        root->Close(root);
        if (!ours) continue;
        BootPartition bp = {0};
        int usb = 0;
        if (!partition_of(handles[i], &bp, &usb)) continue;
        if (usb) { log_str("loader: findinstalled: a TouhouOS volume on a USB device (partition "); log_dec(bp.part_number); log_str(") is not an installed system; skipped\n"); continue; }
        found++;
        log_str("loader: findinstalled: a TouhouOS boot partition on an internal disk (partition "); log_dec(bp.part_number); log_str(")\n");
        bootentry_register(handles[i], &bp);
    }
    g_bs->FreePool(handles);
    if (!found) log_str("loader: findinstalled: no installed TouhouOS found on the other volumes\n");
}
