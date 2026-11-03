/*
 * Recovery without a person, loader side (docs/RELIABILITY.md).
 *
 *   The panic record region: a fixed range of RAM (BOOT_PERSIST_PHYS, 64 KiB; "persist=0xADDR" or "persist=off" on the command line) taken
 *   from the firmware with AllocatePages(AllocateAddress) and handed to the kernel as BOOTMEM_RESERVED, so that no kernel (an older one
 *   included) ever gives it out. Its contents are not touched here: what the previous start left there (kernel/panic.c writes how it ended,
 *   and a panic's text) is still there at the next start on a PC that keeps DRAM through a warm reset. The firmware's memory map is
 *   consulted first, so that the log can say why a region could not be had (the firmware uses it in this start, or it is not RAM).
 *   MemoryOverwriteRequestControl (the TCG "platform reset attack mitigation" variable, which Windows sets) asks the firmware to clear memory
 *   at a reset; the loader only reports it.
 *
 *   The hardware watchdog: just before the jump into the kernel (after ExitBootServices: no firmware code runs in between, and none can stop
 *   it) the Intel TCO timer (shared/tcohw.h) is armed with "wdt=SECONDS" (default 120; "wdt=off": not at all), but only for a kernel image
 *   that carries BOOT_WDT_MARK, a kernel that feeds the timer from early in its start. A kernel that hangs at its first instruction then
 *   restarts the machine, the loader counts that start (update.c), and after the second failed start of an update on trial the previous
 *   kernel returns. An older kernel without the mark (a fallback to one) is never armed for, and a timer still counting is stopped for it:
 *   it would be restarted every two minutes. What was done is passed on in BootInfo.wdt_*, since nothing the loader prints after
 *   ExitBootServices is seen on a PC without a serial port.
 */
#include "loader.h"
#include "../shared/tcohw.h"

typedef EFI_STATUS (EFIAPI *GetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attr, UINTN *size, void *data);

static EFI_GUID morGuid = { 0xe20939be, 0x32d4, 0x41be, { 0xa1, 0x50, 0x89, 0x7f, 0x85, 0xd4, 0x98, 0x29 } };    /* MEMORY_ONLY_RESET_CONTROL_GUID */

/* The value of "key" (key includes the '='), copied into out; 1 when the word is there. */
static int word_value(const char *cmdline, const char *key, char *out, UINTN n) {
    for (const char *p = cmdline; *p; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        UINTN k = 0;
        while (key[k] && p[k] == key[k]) k++;
        if (key[k]) continue;
        UINTN i = 0;
        for (const char *q = p + k; *q && *q != ' ' && i + 1 < n; q++) out[i++] = *q;
        out[i] = 0;
        return 1;
    }
    return 0;
}

static int str_is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int parse_u64(const char *s, UINT64 *out) {
    UINT64 v = 0;
    int base = 10, digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; s++, digits++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : base == 16 && *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : base == 16 && *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v * (UINT64)base + (UINT64)d;
    }
    *out = v;
    return digits > 0;
}

/* The firmware's memory type for [base, base + len): -1 when no single descriptor holds the whole range. */
static int map_type_of(UINT64 base, UINT64 len) {
    UINTN size = 0, key = 0, dsize = 0;
    UINT32 dver = 0;
    g_bs->GetMemoryMap(&size, 0, &key, &dsize, &dver);
    size += 8 * 64;                                      /* the pool allocation below adds descriptors of its own */
    void *buf = 0;
    if (EFI_ERROR(g_bs->AllocatePool(EfiLoaderData, size, &buf)) || !buf) return -1;
    int type = -1;
    if (!EFI_ERROR(g_bs->GetMemoryMap(&size, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsize, &dver)) && dsize) {
        for (UINTN off = 0; off + dsize <= size; off += dsize) {
            const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)buf + off);
            UINT64 s = d->PhysicalStart, e = s + d->NumberOfPages * 4096ULL;
            if (base >= s && base + len <= e) { type = (int)d->Type; break; }
        }
    }
    g_bs->FreePool(buf);
    return type;
}

static const char *type_name(int t) {
    switch (t) {
    case EfiConventionalMemory: return "free memory";
    case EfiLoaderCode: case EfiLoaderData: return "loader memory";
    case EfiBootServicesCode: case EfiBootServicesData: return "firmware memory (boot services)";
    case EfiRuntimeServicesCode: case EfiRuntimeServicesData: return "firmware runtime memory";
    case EfiACPIReclaimMemory: case EfiACPIMemoryNVS: return "ACPI memory";
    case EfiReservedMemoryType: return "reserved memory";
    case EfiMemoryMappedIO: case EfiMemoryMappedIOPortSpace: return "device memory";
    case -1: return "no single region of the memory map";
    default: return "memory of another type";
    }
}

void recovery_reserve(BootInfo *bi, const char *cmdline) {
    bi->persist_phys = bi->persist_len = 0;
    bi->persist_note = BOOT_PERSIST_NONE;
    UINT64 base = BOOT_PERSIST_PHYS, len = BOOT_PERSIST_LEN;
    char v[24];
    if (word_value(cmdline, "persist=", v, sizeof v)) {
        UINT64 want = 0;
        if (str_is(v, "off") || str_is(v, "0") || str_is(v, "no")) {
            bi->persist_note = BOOT_PERSIST_OFF;
            log_str("loader: persist=off: no panic record region\n");
            return;
        }
        if (parse_u64(v, &want) && want && !(want & 0xFFF) && want + len <= (4ULL << 30)) base = want;
        else { log_str("loader: persist="); log_str(v); log_str(" is not a page-aligned address below 4 GiB: using the default\n"); }
    }

    UINT8 mor = 0;
    UINT32 attr = 0;
    UINTN msize = sizeof mor;
    CHAR16 morName[] = { 'M','e','m','o','r','y','O','v','e','r','w','r','i','t','e','R','e','q','u','e','s','t','C','o','n','t','r','o','l',0 };
    if (!EFI_ERROR(((GetVariableFn)g_st->RuntimeServices->GetVariable)(morName, &morGuid, &attr, &msize, &mor)) && msize >= 1 && (mor & 1)) {
        bi->persist_note |= BOOT_PERSIST_MOR;
        log_str("loader: MemoryOverwriteRequestControl is set: the firmware clears memory at a restart, so no panic record survives one\n");
    }

    int type = map_type_of(base, len);
    EFI_PHYSICAL_ADDRESS a = base;
    if (!EFI_ERROR(g_bs->AllocatePages(AllocateAddress, EfiLoaderData, (UINTN)(len / 4096), &a)) && a == base) {
        bi->persist_phys = base;
        bi->persist_len = len;
        bi->persist_note = (bi->persist_note & BOOT_PERSIST_MOR) | BOOT_PERSIST_OK;
        log_str("loader: panic record region "); log_hex(base); log_str(" ("); log_dec(len >> 10); log_str(" KiB, was ");
        log_str(type_name(type)); log_str("), kept out of the kernel's memory map\n");
        return;
    }
    int ram = type == EfiLoaderCode || type == EfiLoaderData || type == EfiBootServicesCode || type == EfiBootServicesData ||
              type == EfiRuntimeServicesCode || type == EfiRuntimeServicesData || type == EfiACPIReclaimMemory || type == EfiACPIMemoryNVS;
    bi->persist_note = (bi->persist_note & BOOT_PERSIST_MOR) | (ram ? BOOT_PERSIST_BUSY : BOOT_PERSIST_NOTRAM);
    log_str("loader: no panic record region in this start: "); log_hex(base); log_str(" is "); log_str(type_name(type));
    log_str(" (persist=0xADDR picks another place)\n");
}

/* Does the kernel image carry the mark of a kernel that feeds the watchdog? */
static int has_mark(const UINT8 *p, UINT64 n) {
    static const char mark[] = BOOT_WDT_MARK;
    const UINT64 m = sizeof mark - 1;
    for (UINT64 i = 0; i + m <= n; i++) {
        if (p[i] != (UINT8)mark[0]) continue;
        UINT64 k = 1;
        while (k < m && p[i + k] == (UINT8)mark[k]) k++;
        if (k == m) return 1;
    }
    return 0;
}

void recovery_arm(BootInfo *bi, const char *cmdline, UINT64 kfile_phys, UINT64 kfile_size) {
    bi->wdt_state = BOOT_WDT_NONE;
    bi->wdt_seconds = 0;
    bi->wdt_tcobase = 0;
    UINT64 secs = 120;
    int off = 0;
    char v[16];
    if (word_value(cmdline, "wdt=", v, sizeof v)) {
        off = str_is(v, "off") || str_is(v, "0") || str_is(v, "no");
        if (!off && (!parse_u64(v, &secs) || secs < 2 || secs > 600)) secs = 120;
    }
    TcoHw t;
    if (tcohw_locate(&t) == TCO_KIND_NONE) {
        bi->wdt_state = off ? BOOT_WDT_OFF : BOOT_WDT_NOTFOUND;
        log_str("loader: no TCO watchdog: "); log_str(t.why); log_str("\n");
        return;
    }
    /* Not arming it means stopping it: a timer still counting from the previous start (QEMU keeps it through a reset) or left running by the
     * firmware would restart a kernel that does not feed it. */
    int running = tcohw_running(&t);
    if (off) {
        if (running) tcohw_halt(&t);
        bi->wdt_state = BOOT_WDT_OFF;
        log_str("loader: wdt=off: the watchdog is not armed\n");
        return;
    }
    if (!has_mark((const UINT8 *)(UINTN)kfile_phys, kfile_size)) {
        if (running) tcohw_halt(&t);
        bi->wdt_state = BOOT_WDT_NOFEED;
        log_str("loader: this kernel does not feed the TCO watchdog: it is not armed\n");
        return;
    }
    t.nr_reg = t.nr_phys ? (volatile UINT32 *)(UINTN)t.nr_phys : 0;        /* the firmware's page tables map physical addresses one to one */
    int before = tcohw_noreboot(&t);
    int nr = tcohw_noreboot_clear(&t);
    tcohw_start(&t, (UINT32)secs);
    bi->wdt_state = (nr == 1 ? BOOT_WDT_ARMED_NR : BOOT_WDT_ARMED) | (before == 1 && nr == 0 ? BOOT_WDT_NR_CLEARED : 0);
    bi->wdt_seconds = (UINT32)secs;
    bi->wdt_tcobase = t.tcobase;
    log_str("loader: the TCO watchdog is armed: "); log_dec(secs); log_str(" s (TCO at io "); log_hex(t.tcobase);
    log_str(nr == 1 ? "; NO_REBOOT stays set: it cannot restart the machine)\n" : ")\n");
}
