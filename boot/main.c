/*
 * Gensokyo UEFI loader (BOOTX64.EFI).
 *
 * Reads kernel.elf, initrd.img and cmdline.txt from the boot volume, picks a framebuffer mode,
 * exits boot services, builds boot page tables and jumps to the higher-half kernel with a BootInfo.
 */
#include "loader.h"
#include "../shared/elf.h"
#include "../shared/stagemark.h"

EFI_SYSTEM_TABLE  *g_st;
EFI_BOOT_SERVICES *g_bs;

#define PAGE_SIZE     4096ULL
#define ARENA_PAGES   512            /* 2 MiB of scratch: page tables, BootInfo, region list, boot stack */
#define MAP_BUF_PAGES 64             /* room for the raw UEFI memory map */
#define STACK_PAGES   8

static EFI_GUID gopGuid         = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID sfsGuid         = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID fileInfoGuid    = EFI_FILE_INFO_ID;
static EFI_GUID acpi20Guid      = EFI_ACPI_20_TABLE_GUID;
static EFI_GUID acpi10Guid      = EFI_ACPI_10_TABLE_GUID;

static UINT64 alloc_pages(UINT32 memtype, UINTN pages) {
    EFI_PHYSICAL_ADDRESS addr = 0;
    if (EFI_ERROR(g_bs->AllocatePages(AllocateAnyPages, memtype, pages, &addr))) {
        log_str("loader: AllocatePages failed\n");
        loader_halt();
    }
    return addr;
}

/* Reads a whole file into freshly allocated pages of the given memory type. */
static EFI_STATUS read_file(EFI_FILE_PROTOCOL *root, const char *ascii_name, UINT32 memtype,
                            UINT64 *out_phys, UINT64 *out_size) {
    CHAR16 name[64];
    UINTN i = 0;
    for (; ascii_name[i] && i < 62; i++) name[i] = (CHAR16)(UINT8)ascii_name[i];
    name[i] = 0;

    EFI_FILE_PROTOCOL *f = 0;
    EFI_STATUS s = root->Open(root, &f, name, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s)) return s;

    UINT64 infoBuf[96];                 /* 768 bytes, 8-byte aligned */
    UINTN infoSize = sizeof(infoBuf);
    s = f->GetInfo(f, &fileInfoGuid, &infoSize, infoBuf);
    if (EFI_ERROR(s)) { f->Close(f); return s; }

    UINT64 size = ((EFI_FILE_INFO *)infoBuf)->FileSize;
    UINT64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) pages = 1;
    UINT64 phys = alloc_pages(memtype, pages);
    memset((void *)phys, 0, pages * PAGE_SIZE);

    UINTN toRead = size;
    s = f->Read(f, &toRead, (void *)phys);
    f->Close(f);
    if (EFI_ERROR(s) || toRead != size) return EFI_LOAD_ERROR;

    *out_phys = phys;
    *out_size = size;
    return EFI_SUCCESS;
}

/*
 * cmdline.txt as the kernel gets it: plain ASCII, words separated by single spaces. The file is edited on another computer, and
 * what an editor writes is not always plain: Windows editors and PowerShell save a UTF-8 byte order mark or whole UTF-16, a
 * multi-line file leaves newlines inside words, and the kernel (which only splits on spaces) would then not find "blk.ro" and
 * leave the disks writable without a word. Everything unusual is turned into spaces (or '?'), UTF-16 is narrowed to its low bytes.
 */
static void normalize_cmdline(const UINT8 *raw, UINT64 size, char *out, UINTN cap) {
    UINT64 i = 0, step = 1, hi = 1;                         /* hi: which byte of a UTF-16 unit is the character (the other must be 0) */
    int wide = 0;
    if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) i = 3;                          /* UTF-8 BOM */
    else if (size >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) { wide = 1; i = 2; step = 2; hi = 1; }       /* UTF-16 little endian BOM */
    else if (size >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) { wide = 1; i = 2; step = 2; hi = 0; }       /* UTF-16 big endian BOM */
    else if (size >= 4 && raw[0] && !raw[1] && raw[2] && !raw[3]) { wide = 1; step = 2; hi = 1; }       /* UTF-16 little endian without a BOM */
    UINTN n = 0;
    int pending_space = 0;
    for (; i + (wide ? 1 : 0) < size && n + 1 < cap; i += step) {
        UINT8 c = raw[i + (wide ? (hi ? 0 : 1) : 0)];
        UINT8 other = wide ? raw[i + (hi ? 1 : 0)] : 0;
        if (wide && other) c = '?';
        if (c < 0x20 || c == 0x7F || c == ' ') { pending_space = 1; continue; }     /* NUL, CR, LF, TAB, ...: a word boundary */
        if (c >= 0x80) c = '?';
        if (pending_space && n) { if (n + 2 >= cap) break; out[n++] = ' '; }
        pending_space = 0;
        out[n++] = (char)c;
    }
    out[n] = 0;
}

/* video=WxH     that mode (default 1024x768, with fallbacks)
 * video=native   keep the mode the firmware already chose: on a PC that is normally the monitor's own resolution
 * video=max      the largest mode the display offers, up to 2560x1600 */
enum { VIDEO_EXACT, VIDEO_NATIVE, VIDEO_MAX };
static int g_video_policy = VIDEO_EXACT;

static void parse_video(const char *cmdline, UINT32 *w, UINT32 *h) {
    for (const char *p = cmdline; *p; p++) {
        if (p[0] == 'v' && p[1] == 'i' && p[2] == 'd' && p[3] == 'e' && p[4] == 'o' && p[5] == '=') {
            const char *q = p + 6;
            if (q[0] == 'n' && q[1] == 'a' && q[2] == 't') { g_video_policy = VIDEO_NATIVE; return; }
            if (q[0] == 'm' && q[1] == 'a' && q[2] == 'x') { g_video_policy = VIDEO_MAX; return; }
            UINT32 a = 0, b = 0;
            while (*q >= '0' && *q <= '9') a = a * 10 + (UINT32)(*q++ - '0');
            if (*q == 'x') q++;
            while (*q >= '0' && *q <= '9') b = b * 10 + (UINT32)(*q++ - '0');
            if (a >= 640 && b >= 480) { *w = a; *h = b; }
            return;
        }
    }
}

/* Boot progress squares (shared/stagemark.h) and "stagehold=N": stop right after square N. The squares are the only thing a PC without a
 * serial port can show once the loader's text stops, so each stage of the handoff draws one. */
static int g_stagehold;
static int g_stagebeep;             /* "stagebeep": also say it on the PC speaker (stage N beeps N times; the kernel adds one long beep) */
static int g_noexitmarks;           /* "noexitmarks": no framebuffer writes by the loader once the call that leaves the firmware has returned */
static int g_exitreset;             /* "exitreset": restart the machine about six seconds after that call returned (no screen needed to see it) */

static int has_word(const char *cmdline, const char *word) {
    for (const char *p = cmdline; *p; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        int k = 0;
        while (word[k] && p[k] == word[k]) k++;
        if (!word[k] && (p[k] == 0 || p[k] == ' ')) return 1;
    }
    return 0;
}

static int g_cliexit;               /* "cliexit": interrupts off right before the exit call */
static int g_quietexit;             /* "quietexit": no console text between the files and the exit call */
static int g_nomarks;               /* "nomarks": no squares from the loader at all */
static int g_stdmem;                /* "stdmem": kernel and initrd are ordinary loader data, retyped in the map we hand over (no OS-defined memory types) */

/* The number after "key" (key includes the '='); 1 when the key is there. */
static int find_number(const char *cmdline, const char *key, int *out) {
    for (const char *p = cmdline; *p; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        int k = 0;
        while (key[k] && p[k] == key[k]) k++;
        if (!key[k]) {
            int v = 0;
            for (const char *q = p + k; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
            *out = v;
            return 1;
        }
    }
    return 0;
}

static void append_words(char *cmdline, UINTN cap, const char *words) {
    UINTN n = 0;
    while (cmdline[n]) n++;
    if (n && n + 1 < cap) cmdline[n++] = ' ';
    for (; *words && n + 1 < cap; words++) cmdline[n++] = *words;
    cmdline[n] = 0;
}

static void parse_stagehold(const char *cmdline) {
    static const char key[] = "stagehold=";
    static const char beep[] = "stagebeep";
    g_cliexit = has_word(cmdline, "cliexit");
    g_quietexit = has_word(cmdline, "quietexit");
    g_nomarks = has_word(cmdline, "nomarks");
    g_stdmem = has_word(cmdline, "stdmem");
    g_noexitmarks = has_word(cmdline, "noexitmarks");
    g_exitreset = has_word(cmdline, "exitreset");
    for (const char *p = cmdline; *p; p++) {
        int b = 0;
        while (beep[b] && p[b] == beep[b]) b++;
        if (!beep[b]) g_stagebeep = 1;
    }
    for (const char *p = cmdline; *p; p++) {
        int k = 0;
        while (key[k] && p[k] == key[k]) k++;
        if (!key[k]) {
            int v = 0;
            for (const char *q = p + k; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
            g_stagehold = v;
            return;
        }
    }
}

static void stage(const BootFramebuffer *fb, int n) {
    if (fb->base && !g_nomarks && !(g_noexitmarks && (n == 2 || n == 3))) stage_mark((volatile UINT8 *)fb->base, fb->pitch, fb->width, fb->height, n);
    if (g_stagebeep && n >= 2) pc_beep(n);
    if (g_stagehold == n) loader_halt();
}

/* The marks of the exit from the firmware (rows 1-5 of the grid in shared/stagemark.h). Plain stores to the framebuffer: they do not
 * touch boot services, so they may sit between GetMemoryMap and ExitBootServices. */
static void exit_mark(const BootFramebuffer *fb, int row, int col) {
    if (fb->base && !g_nomarks) mark_square((volatile UINT8 *)fb->base, fb->pitch, fb->width, fb->height, row, col, 0x00FFFFFFu);
}

static void exit_status(const BootFramebuffer *fb, int row, UINT64 status) {
    if (fb->base && !g_nomarks) mark_byte((volatile UINT8 *)fb->base, fb->pitch, fb->width, fb->height, row, status);
}

/*
 * The ladder (command-line word "ladder"; "strategy=N" picks a rung by hand). The first boot of a PC is an experiment, and a machine
 * that stops in the firmware's exit call can be tried one hypothesis per flashed stick or, much faster, one per boot: every start of this
 * loader counts itself in a non-volatile variable named after the build, picks the rung with that number and adds its words to the
 * command line (so the kernel's log shows them too). The owner just keeps restarting the machine and says which rung reached the desktop.
 */
typedef struct { const char *what; const char *words; } Rung;
static const Rung g_rungs[] = {
    { "probe: the machine restarts by itself if the exit call comes back",               "exitreset stagebeep" },
    { "no screen writes by the loader after the exit call",                              "noexitmarks stagebeep" },
    { "... and ordinary memory types for the kernel and the initrd",                     "noexitmarks stdmem stagebeep" },
    { "... and interrupts off, no console text before the exit call",                    "noexitmarks stdmem cliexit quietexit stagebeep" },
    { "... and no squares from the loader at all",                                       "noexitmarks stdmem cliexit quietexit nomarks stagebeep" },
};
#define NRUNGS ((int)(sizeof g_rungs / sizeof g_rungs[0]))

typedef EFI_STATUS (EFIAPI *GetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 *attr, UINTN *size, void *data);
typedef EFI_STATUS (EFIAPI *SetVariableFn)(CHAR16 *name, EFI_GUID *vendor, UINT32 attr, UINTN size, void *data);
static EFI_GUID triesGuid = { 0x7e4d6a10, 0x5b3c, 0x4c1f, { 0x9a, 0x21, 0x6f, 0x0d, 0x2b, 0x8c, 0x31, 0x55 } };

/* How many times this build of the loader has started on this machine, this start included (1, 2, 3 ...); 0 if the variable store refuses. */
static UINT32 start_count(void) {
    static const char hex[] = "0123456789abcdef";
    CHAR16 name[24] = { 'T', 'o', 'u', 'h', 'o', 'u', 'T', 'r', 'y' };
    UINT32 h = 2166136261u;                                       /* the build's date and time make the name: a new image starts at rung 0 */
    for (const char *p = __DATE__ " " __TIME__; *p; p++) { h ^= (UINT8)*p; h *= 16777619u; }
    for (int i = 0; i < 8; i++) name[9 + i] = (CHAR16)hex[(h >> (28 - 4 * i)) & 0xF];
    name[17] = 0;
    EFI_RUNTIME_SERVICES *rt = g_st->RuntimeServices;
    UINT32 count = 0, attr = 0;
    UINTN size = sizeof count;
    if (EFI_ERROR(((GetVariableFn)rt->GetVariable)(name, &triesGuid, &attr, &size, &count)) || size != sizeof count) count = 0;
    count++;
    if (EFI_ERROR(((SetVariableFn)rt->SetVariable)(name, &triesGuid, 7 /* non-volatile, boot and runtime */, sizeof count, &count))) {
        log_str("loader: cannot count the starts (the variable store refuses): rung 0 every time\n");
        return 0;
    }
    return count;
}

static void pick_rung(char *cmdline, UINTN cap) {
    int rung = -1;
    if (!find_number(cmdline, "strategy=", &rung)) {
        if (!has_word(cmdline, "ladder")) return;
        UINT32 c = start_count();
        rung = c ? (int)((c - 1) % (UINT32)NRUNGS) : 0;
        append_words(cmdline, cap, "strategy=");                  /* the digit follows below */
        UINTN n = 0;
        while (cmdline[n]) n++;
        if (n + 2 < cap) { cmdline[n] = (char)('0' + rung); cmdline[n + 1] = 0; }
    }
    if (rung < 0 || rung >= NRUNGS) return;
    log_str("loader: rung "); log_dec((UINT64)rung); log_str(" of "); log_dec((UINT64)(NRUNGS - 1)); log_str(": "); log_str(g_rungs[rung].what); log_str("\n");
    append_words(cmdline, cap, g_rungs[rung].words);
}

/* Retype [base, base+len) in the sorted region list, splitting regions at its edges; returns the new count (the array has slack). */
static UINT32 retype_range(BootMemRegion *r, UINT32 n, UINT64 base, UINT64 len, UINT32 type) {
    UINT64 end = base + len;
    UINT32 count = n;
    for (UINT32 i = 0; i < n; i++) {
        UINT64 rb = r[i].base, re = rb + r[i].length;
        if (re <= base || rb >= end || r[i].type == type) continue;
        UINT32 old = r[i].type;
        UINT64 ob = rb > base ? rb : base, oe = re < end ? re : end;
        r[i].base = ob; r[i].length = oe - ob; r[i].type = type;
        if (rb < ob) { r[count].base = rb; r[count].length = ob - rb; r[count].type = old; r[count].reserved = 0; count++; }
        if (oe < re) { r[count].base = oe; r[count].length = re - oe; r[count].type = old; r[count].reserved = 0; count++; }
    }
    return count;
}

/* Insertion sort by base, then merge neighbours of one type that touch. */
static UINT32 sort_merge(BootMemRegion *r, UINT32 n) {
    for (UINT32 i = 1; i < n; i++) {
        BootMemRegion x = r[i];
        UINT32 j = i;
        while (j > 0 && r[j - 1].base > x.base) { r[j] = r[j - 1]; j--; }
        r[j] = x;
    }
    UINT32 m = 0;
    for (UINT32 i = 0; i < n; i++) {
        if (m && r[m - 1].type == r[i].type && r[m - 1].base + r[m - 1].length == r[i].base) r[m - 1].length += r[i].length;
        else r[m++] = r[i];
    }
    return m;
}

/* Which byte order a mode uses: BOOTFB_BGRX, BOOTFB_RGBX, or -1 when it is not a 32-bit linear framebuffer we can draw to.
 * Real firmware often describes its modes with PixelBitMask; the masks say which of the two layouts it really is. */
static int pixel_layout(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
    switch (info->PixelFormat) {
    case PixelBlueGreenRedReserved8BitPerColor: return BOOTFB_BGRX;
    case PixelRedGreenBlueReserved8BitPerColor: return BOOTFB_RGBX;
    case PixelBitMask: {
        const EFI_PIXEL_BITMASK *m = &info->PixelInformation;
        if (m->RedMask == 0x00FF0000 && m->GreenMask == 0x0000FF00 && m->BlueMask == 0x000000FF) return BOOTFB_BGRX;
        if (m->RedMask == 0x000000FF && m->GreenMask == 0x0000FF00 && m->BlueMask == 0x00FF0000) return BOOTFB_RGBX;
        return -1;
    }
    default: return -1;
    }
}

/* The usable mode with the most pixels that fits in max_w x max_h; returns its index or -1. */
static int largest_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 max_w, UINT32 max_h) {
    int best = -1;
    UINT64 best_area = 0;
    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN isz = 0;
        if (EFI_ERROR(gop->QueryMode(gop, i, &isz, &info)) || !info || pixel_layout(info) < 0) continue;
        if (info->HorizontalResolution > max_w || info->VerticalResolution > max_h) continue;
        UINT64 area = (UINT64)info->HorizontalResolution * info->VerticalResolution;
        if (area > best_area) { best_area = area; best = (int)i; }
    }
    return best;
}

static void setup_gop(BootFramebuffer *fb, UINT32 want_w, UINT32 want_h) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    /* The handle the firmware draws its own console on is the display that really has a monitor attached; on a machine with
     * two graphics adapters the first GOP instance can be the one with nothing plugged in. */
    if (EFI_ERROR(g_bs->HandleProtocol(g_st->ConsoleOutHandle, &gopGuid, (void **)&gop)) || !gop) {
        gop = 0;
        if (EFI_ERROR(g_bs->LocateProtocol(&gopGuid, 0, (void **)&gop)) || !gop) {
            log_str("loader: no Graphics Output Protocol, continuing without a framebuffer\n");
            return;
        }
    }

    UINT32 best = gop->Mode->Mode;
    int found = 0;
    if (g_video_policy == VIDEO_MAX) {
        int m = largest_mode(gop, 2560, 1600);
        if (m >= 0) { best = (UINT32)m; found = 1; }
    } else if (g_video_policy == VIDEO_EXACT) {
        /* Look for the requested mode, then a couple of fallbacks, among the 32-bit linear modes. */
        static const UINT32 fallback[][2] = { {1024, 768}, {1280, 800}, {800, 600} };
        for (int attempt = -1; attempt < 3 && !found; attempt++) {
            UINT32 w = attempt < 0 ? want_w : fallback[attempt][0];
            UINT32 h = attempt < 0 ? want_h : fallback[attempt][1];
            for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
                EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
                UINTN isz = 0;
                if (EFI_ERROR(gop->QueryMode(gop, i, &isz, &info)) || !info) continue;
                if (pixel_layout(info) >= 0 && info->HorizontalResolution == w && info->VerticalResolution == h) {
                    best = i;
                    found = 1;
                    break;
                }
            }
        }
    }
    if (found && best != gop->Mode->Mode) gop->SetMode(gop, best);

    /* Whatever the policy chose (native included) must be something we can draw on; otherwise take the best that is. */
    if (pixel_layout(gop->Mode->Info) < 0) {
        int m = largest_mode(gop, 1920, 1200);
        if (m >= 0) gop->SetMode(gop, (UINT32)m);
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    int layout = pixel_layout(info);
    if (layout < 0) {
        log_str("loader: framebuffer pixel format not supported\n");
        return;
    }
    fb->base   = gop->Mode->FrameBufferBase;
    fb->size   = gop->Mode->FrameBufferSize;
    fb->width  = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch  = info->PixelsPerScanLine * 4;
    fb->bpp    = 32;
    fb->format = (UINT32)layout;

    log_str("loader: framebuffer ");
    log_dec(fb->width); log_str("x"); log_dec(fb->height);
    log_str(" at "); log_hex(fb->base); log_str("\n");
}

static UINT64 find_rsdp(void) {
    UINT64 rsdp10 = 0;
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &g_st->ConfigurationTable[i];
        if (guid_equal(&t->VendorGuid, &acpi20Guid)) return (UINT64)t->VendorTable;
        if (guid_equal(&t->VendorGuid, &acpi10Guid)) rsdp10 = (UINT64)t->VendorTable;
    }
    return rsdp10;
}

static UINT32 classify(UINT32 t) {
    switch (t) {
    case EfiConventionalMemory:      return BOOTMEM_USABLE;
    /* The firmware's own memory (boot services code and data) is free by the specification the moment boot services are gone, but
     * hardware or other processors it left running may still be using it: a NIC or controller with a ring in there, or a core
     * parked in a loop in there. It is handed to the kernel as "reclaimable": the kernel gives it to the allocator only after
     * it has silenced every PCI DMA master (pci_init) and parked the other processors, which is what Linux does as well. */
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:              return BOOTMEM_BOOT_RECLAIMABLE;
    case EFI_MEMTYPE_KERNEL:         return BOOTMEM_KERNEL;
    case EFI_MEMTYPE_MODULE:         return BOOTMEM_MODULE;
    case EfiACPIReclaimMemory:       return BOOTMEM_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:           return BOOTMEM_ACPI_NVS;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace: return BOOTMEM_MMIO;
    default:                         return BOOTMEM_RESERVED;
    }
}

/* Loads the kernel ELF's PT_LOAD segments into one contiguous physical block. */
static int load_kernel(UINT64 file_phys, UINT64 file_size, KernelImage *k) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file_phys;
    if (file_size < sizeof(*eh) || !elf64_check(eh) || eh->e_type != ET_EXEC) {
        log_str("loader: kernel.elf is not a valid x86-64 ELF executable\n");
        return 0;
    }
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file_phys + eh->e_phoff);

    UINT64 lo = ~0ULL, hi = 0;
    for (UINT32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (k->nseg >= 8) { log_str("loader: too many kernel segments\n"); return 0; }
        UINT64 s = ph[i].p_vaddr & ~0xFFFULL;
        UINT64 e = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~0xFFFULL;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
        k->seg[k->nseg].vaddr = ph[i].p_vaddr;
        k->seg[k->nseg].memsz = ph[i].p_memsz;
        k->seg[k->nseg].flags = ph[i].p_flags;
        k->nseg++;
    }
    if (k->nseg == 0 || lo < KERNEL_VIRT_BASE) {
        log_str("loader: kernel has no loadable segments in the higher half\n");
        return 0;
    }

    k->virt_base = lo;
    k->size      = hi - lo;
    k->entry     = eh->e_entry;
    k->phys_base = alloc_pages(g_stdmem ? EfiLoaderData : EFI_MEMTYPE_KERNEL, k->size / PAGE_SIZE);
    memset((void *)k->phys_base, 0, k->size);

    for (UINT32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > file_size) { log_str("loader: truncated kernel.elf\n"); return 0; }
        memcpy((void *)(k->phys_base + (ph[i].p_vaddr - lo)), (const void *)(file_phys + ph[i].p_offset), ph[i].p_filesz);
    }
    return 1;
}

/* An error message on a PC would flash by as the firmware moves on to the next boot entry (and there is no serial port to read it
 * from): keep it on the screen for a while before handing control back. */
static EFI_STATUS fail_wait(EFI_STATUS code) {
    log_str("loader: stopping here; the firmware carries on in 30 seconds\n");
    g_bs->Stall(30UL * 1000 * 1000);
    return code;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    g_st = st;
    g_bs = st->BootServices;

    serial_init();
    g_bs->SetWatchdogTimer(0, 0, 0, 0);
    log_str("Hakurei torii (loader) 0.1 -- opening the gate to Gensokyo\n");

    /* --- boot volume --- */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    if (EFI_ERROR(g_bs->HandleProtocol(image, &loadedImageGuid, (void **)&li)) ||
        EFI_ERROR(g_bs->HandleProtocol(li->DeviceHandle, &sfsGuid, (void **)&fs)) ||
        EFI_ERROR(fs->OpenVolume(fs, &root))) {
        log_str("loader: cannot open the boot volume\n");
        return fail_wait(EFI_NOT_FOUND);
    }

    /* --- files --- */
    UINT64 kfile_phys = 0, kfile_size = 0, initrd_phys = 0, initrd_size = 0, cmd_phys = 0, cmd_size = 0;
    char cmdline[BOOTINFO_CMDLINE_MAX] = {0};          /* first: the words decide how the rest is loaded */
    if (!EFI_ERROR(read_file(root, "cmdline.txt", EfiLoaderData, &cmd_phys, &cmd_size)))
        normalize_cmdline((const UINT8 *)cmd_phys, cmd_size, cmdline, BOOTINFO_CMDLINE_MAX);
    else {
        /* No file at all (a stick prepared by hand): the safe defaults, which keep every SATA and NVMe disk read-only. */
        static const char defaults[] = "video=native blk.ro";
        memcpy(cmdline, defaults, sizeof defaults);
        log_str("loader: no cmdline.txt on the boot volume: using the safe default\n");
    }
    pick_rung(cmdline, BOOTINFO_CMDLINE_MAX);
    parse_stagehold(cmdline);
    log_str("loader: cmdline '"); log_str(cmdline); log_str("'\n");

    if (EFI_ERROR(read_file(root, "kernel.elf", EfiLoaderData, &kfile_phys, &kfile_size))) {
        log_str("loader: kernel.elf not found on the boot volume\n");
        return fail_wait(EFI_NOT_FOUND);
    }
    log_str("loader: kernel.elf "); log_dec(kfile_size); log_str(" bytes\n");

    if (!EFI_ERROR(read_file(root, "initrd.img", g_stdmem ? EfiLoaderData : EFI_MEMTYPE_MODULE, &initrd_phys, &initrd_size))) {
        log_str("loader: initrd.img "); log_dec(initrd_size); log_str(" bytes\n");
    } else {
        initrd_phys = initrd_size = 0;
        log_str("loader: no initrd.img\n");
    }
    root->Close(root);                  /* nothing of the boot volume stays open when the firmware shuts its drivers down */

    /* --- framebuffer --- */
    BootFramebuffer fb = {0};
    UINT32 want_w = 1024, want_h = 768;
    parse_video(cmdline, &want_w, &want_h);
    setup_gop(&fb, want_w, want_h);
    stage(&fb, 1);

    /* --- kernel image --- */
    KernelImage kimg = {0};
    if (!load_kernel(kfile_phys, kfile_size, &kimg)) return fail_wait(EFI_LOAD_ERROR);
    log_str("loader: kernel at phys "); log_hex(kimg.phys_base);
    log_str(" virt "); log_hex(kimg.virt_base);
    log_str(" entry "); log_hex(kimg.entry); log_str("\n");

    /* --- scratch arena, memory map buffer --- */
    UINT64 arena_phys = alloc_pages(EfiLoaderData, ARENA_PAGES);
    arena_init(arena_phys, ARENA_PAGES * PAGE_SIZE);
    BootInfo *bi = arena_alloc(sizeof(BootInfo), 4096);
    UINT64 stack_phys = (UINT64)arena_alloc(STACK_PAGES * PAGE_SIZE, 4096);
    UINT64 stack_top_virt = HHDM_BASE + stack_phys + STACK_PAGES * PAGE_SIZE;

    UINTN map_pages = MAP_BUF_PAGES;
    UINT64 rawmap_phys = alloc_pages(EfiLoaderData, map_pages);
    EFI_MEMORY_DESCRIPTOR *rawmap = (EFI_MEMORY_DESCRIPTOR *)rawmap_phys;

    bi->magic       = BOOTINFO_MAGIC;
    bi->version     = BOOTINFO_VERSION;
    bi->hhdm_base   = HHDM_BASE;
    bi->fb          = fb;
    bi->kernel_phys = kimg.phys_base;
    bi->kernel_virt = kimg.virt_base;
    bi->kernel_size = kimg.size;
    bi->initrd_phys = initrd_phys;
    bi->initrd_size = initrd_size;
    bi->rsdp_phys   = find_rsdp();
    bi->arena_phys  = arena_phys;
    bi->arena_size  = ARENA_PAGES * PAGE_SIZE;
    memcpy(bi->cmdline, cmdline, BOOTINFO_CMDLINE_MAX);

    if (!g_quietexit) log_str("loader: leaving boot services\n");

    /* --- exit boot services: nothing may touch boot services between GetMemoryMap and the exit --- */
    UINTN mapSize = 0, descSize = 0;
    int exited = 0;
    for (int attempt = 0; attempt < 16 && !exited; attempt++) {
        UINTN mapKey = 0;
        UINT32 descVer = 0;
        mapSize = map_pages * PAGE_SIZE;
        exit_mark(&fb, 1, attempt);                 /* attempt N begins (squares: see shared/stagemark.h) */
        EFI_STATUS s = g_bs->GetMemoryMap(&mapSize, rawmap, &mapKey, &descSize, &descVer);
        if (s == EFI_BUFFER_TOO_SMALL) {            /* a firmware with a huge map: a bigger buffer (the allocation changes the map, so ask again) */
            exit_status(&fb, 2, s);
            map_pages = (mapSize >> 12) + 16;
            rawmap_phys = alloc_pages(EfiLoaderData, map_pages);
            rawmap = (EFI_MEMORY_DESCRIPTOR *)rawmap_phys;
            continue;
        }
        if (EFI_ERROR(s)) { exit_status(&fb, 2, s); log_str("loader: GetMemoryMap failed\n"); loader_halt(); }
        /* A stale map key (something the firmware did while shutting down changed the map) is the normal reason for a failure: ask again. */
        exit_mark(&fb, 3, attempt);                 /* about to call ExitBootServices */
        if (g_cliexit) __asm__ volatile ("cli");
        EFI_STATUS es = g_bs->ExitBootServices(image, mapKey);
        if (g_exitreset) pc_reset();                /* never returns: the machine restarts, which shows that the call came back */
        if (!g_noexitmarks) exit_mark(&fb, 6, 0);   /* the call came back */
        if (g_stagebeep) pc_beep(1);
        if (!g_noexitmarks) exit_status(&fb, 4, es);
        if (!EFI_ERROR(es)) exited = 1;
    }
    if (!exited) { exit_mark(&fb, 5, 0); log_str("loader: ExitBootServices failed\n"); loader_halt(); }
    g_bs = 0;                       /* console logging is serial-only from here on */
    stage(&fb, 2);                  /* the firmware's own page tables are still active: the framebuffer is reachable at its physical address */

    /* --- convert the memory map: classify, sort, merge --- */
    UINT64 ndesc = mapSize / descSize;
    BootMemRegion *regions = arena_alloc((ndesc + 8) * sizeof(BootMemRegion), 8);      /* slack for the splits of the stdmem retyping */
    UINT32 n = 0;
    for (UINT64 i = 0; i < ndesc; i++) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)rawmap + i * descSize);
        if (d->NumberOfPages == 0) continue;
        BootMemRegion r = { d->PhysicalStart, d->NumberOfPages * PAGE_SIZE, classify(d->Type), 0 };
        UINT32 j = n++;
        while (j > 0 && regions[j - 1].base > r.base) { regions[j] = regions[j - 1]; j--; }
        regions[j] = r;
    }
    UINT32 m = 0;
    for (UINT32 i = 0; i < n; i++) {
        if (m && regions[m - 1].type == regions[i].type &&
            regions[m - 1].base + regions[m - 1].length == regions[i].base) {
            regions[m - 1].length += regions[i].length;
        } else {
            regions[m++] = regions[i];
        }
    }
    if (g_stdmem) {                 /* the kernel and the initrd were allocated as plain loader data: say what they really are */
        m = retype_range(regions, m, kimg.phys_base, kimg.size, BOOTMEM_KERNEL);
        if (initrd_size) m = retype_range(regions, m, initrd_phys, (initrd_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), BOOTMEM_MODULE);
        m = sort_merge(regions, m);
    }
    UINT64 max_ram_end = 0, usable = 0;
    for (UINT32 i = 0; i < m; i++) {
        UINT32 t = regions[i].type;
        if (t == BOOTMEM_USABLE) usable += regions[i].length;
        if (t == BOOTMEM_USABLE || t == BOOTMEM_BOOT_RECLAIMABLE || t == BOOTMEM_KERNEL || t == BOOTMEM_MODULE ||
            t == BOOTMEM_ACPI_RECLAIMABLE || t == BOOTMEM_ACPI_NVS) {
            UINT64 end = regions[i].base + regions[i].length;
            if (end > max_ram_end) max_ram_end = end;
        }
    }
    bi->mmap_phys  = (UINT64)regions;
    bi->mmap_count = m;
    log_str("loader: "); log_dec(m); log_str(" memory regions, ");
    log_dec(usable >> 20); log_str(" MiB usable, RAM ends at "); log_hex(max_ram_end); log_str("\n");

    /* --- page tables, then jump --- */
    enable_nxe();
    UINT64 pml4 = build_page_tables(&kimg, max_ram_end, (UINT64)(void *)handoff);
    stage(&fb, 3);
    log_str("loader: entering kernel\n");
    handoff(pml4, kimg.entry, stack_top_virt, HHDM_BASE + (UINT64)bi);
}
