/*
 * Gensokyo UEFI loader (BOOTX64.EFI).
 *
 * Reads kernel.elf, initrd.img and cmdline.txt from the boot volume, picks a framebuffer mode,
 * exits boot services, builds boot page tables and jumps to the higher-half kernel with a BootInfo.
 */
#include "loader.h"
#include "../shared/elf.h"

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

static void parse_video(const char *cmdline, UINT32 *w, UINT32 *h) {
    for (const char *p = cmdline; *p; p++) {
        if (p[0] == 'v' && p[1] == 'i' && p[2] == 'd' && p[3] == 'e' && p[4] == 'o' && p[5] == '=') {
            const char *q = p + 6;
            UINT32 a = 0, b = 0;
            while (*q >= '0' && *q <= '9') a = a * 10 + (UINT32)(*q++ - '0');
            if (*q == 'x') q++;
            while (*q >= '0' && *q <= '9') b = b * 10 + (UINT32)(*q++ - '0');
            if (a >= 640 && b >= 480) { *w = a; *h = b; }
            return;
        }
    }
}

static void setup_gop(BootFramebuffer *fb, UINT32 want_w, UINT32 want_h) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (EFI_ERROR(g_bs->LocateProtocol(&gopGuid, 0, (void **)&gop)) || !gop) {
        log_str("loader: no Graphics Output Protocol, continuing without a framebuffer\n");
        return;
    }

    /* Look for the requested mode, then a couple of fallbacks, among the 32-bit linear modes. */
    static const UINT32 fallback[][2] = { {1024, 768}, {1280, 800}, {800, 600} };
    UINT32 best = gop->Mode->Mode;
    int found = 0;
    for (int attempt = -1; attempt < 3 && !found; attempt++) {
        UINT32 w = attempt < 0 ? want_w : fallback[attempt][0];
        UINT32 h = attempt < 0 ? want_h : fallback[attempt][1];
        for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
            UINTN isz = 0;
            if (EFI_ERROR(gop->QueryMode(gop, i, &isz, &info)) || !info) continue;
            if ((info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
                 info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) &&
                info->HorizontalResolution == w && info->VerticalResolution == h) {
                best = i;
                found = 1;
                break;
            }
        }
    }
    if (found && best != gop->Mode->Mode) gop->SetMode(gop, best);

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    if (info->PixelFormat > PixelBlueGreenRedReserved8BitPerColor) {
        log_str("loader: framebuffer pixel format not supported\n");
        return;
    }
    fb->base   = gop->Mode->FrameBufferBase;
    fb->size   = gop->Mode->FrameBufferSize;
    fb->width  = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch  = info->PixelsPerScanLine * 4;
    fb->bpp    = 32;
    fb->format = info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ? BOOTFB_BGRX : BOOTFB_RGBX;

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
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:        return BOOTMEM_USABLE;
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
    k->phys_base = alloc_pages(EFI_MEMTYPE_KERNEL, k->size / PAGE_SIZE);
    memset((void *)k->phys_base, 0, k->size);

    for (UINT32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > file_size) { log_str("loader: truncated kernel.elf\n"); return 0; }
        memcpy((void *)(k->phys_base + (ph[i].p_vaddr - lo)), (const void *)(file_phys + ph[i].p_offset), ph[i].p_filesz);
    }
    return 1;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    g_st = st;
    g_bs = st->BootServices;

    serial_init();
    g_bs->SetWatchdogTimer(0, 0, 0, 0);
    log_str("Gensokyo loader 0.1\n");

    /* --- boot volume --- */
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    if (EFI_ERROR(g_bs->HandleProtocol(image, &loadedImageGuid, (void **)&li)) ||
        EFI_ERROR(g_bs->HandleProtocol(li->DeviceHandle, &sfsGuid, (void **)&fs)) ||
        EFI_ERROR(fs->OpenVolume(fs, &root))) {
        log_str("loader: cannot open the boot volume\n");
        return EFI_NOT_FOUND;
    }

    /* --- files --- */
    UINT64 kfile_phys = 0, kfile_size = 0, initrd_phys = 0, initrd_size = 0, cmd_phys = 0, cmd_size = 0;
    if (EFI_ERROR(read_file(root, "kernel.elf", EfiLoaderData, &kfile_phys, &kfile_size))) {
        log_str("loader: kernel.elf not found on the boot volume\n");
        return EFI_NOT_FOUND;
    }
    log_str("loader: kernel.elf "); log_dec(kfile_size); log_str(" bytes\n");

    if (!EFI_ERROR(read_file(root, "initrd.img", EFI_MEMTYPE_MODULE, &initrd_phys, &initrd_size))) {
        log_str("loader: initrd.img "); log_dec(initrd_size); log_str(" bytes\n");
    } else {
        initrd_phys = initrd_size = 0;
        log_str("loader: no initrd.img\n");
    }
    char cmdline[BOOTINFO_CMDLINE_MAX] = {0};
    if (!EFI_ERROR(read_file(root, "cmdline.txt", EfiLoaderData, &cmd_phys, &cmd_size))) {
        UINT64 n = cmd_size < BOOTINFO_CMDLINE_MAX - 1 ? cmd_size : BOOTINFO_CMDLINE_MAX - 1;
        memcpy(cmdline, (const void *)cmd_phys, n);
        while (n && (cmdline[n - 1] == '\n' || cmdline[n - 1] == '\r' || cmdline[n - 1] == ' ')) n--;
        cmdline[n] = 0;
    }
    log_str("loader: cmdline '"); log_str(cmdline); log_str("'\n");

    /* --- framebuffer --- */
    BootFramebuffer fb = {0};
    UINT32 want_w = 1024, want_h = 768;
    parse_video(cmdline, &want_w, &want_h);
    setup_gop(&fb, want_w, want_h);

    /* --- kernel image --- */
    KernelImage kimg = {0};
    if (!load_kernel(kfile_phys, kfile_size, &kimg)) return EFI_LOAD_ERROR;
    log_str("loader: kernel at phys "); log_hex(kimg.phys_base);
    log_str(" virt "); log_hex(kimg.virt_base);
    log_str(" entry "); log_hex(kimg.entry); log_str("\n");

    /* --- scratch arena, memory map buffer --- */
    UINT64 arena_phys = alloc_pages(EfiLoaderData, ARENA_PAGES);
    arena_init(arena_phys, ARENA_PAGES * PAGE_SIZE);
    BootInfo *bi = arena_alloc(sizeof(BootInfo), 4096);
    UINT64 stack_phys = (UINT64)arena_alloc(STACK_PAGES * PAGE_SIZE, 4096);
    UINT64 stack_top_virt = HHDM_BASE + stack_phys + STACK_PAGES * PAGE_SIZE;

    UINT64 rawmap_phys = alloc_pages(EfiLoaderData, MAP_BUF_PAGES);
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

    log_str("loader: leaving boot services\n");

    /* --- exit boot services: nothing may touch boot services between GetMemoryMap and the exit --- */
    UINTN mapSize = 0, descSize = 0;
    int exited = 0;
    for (int attempt = 0; attempt < 5 && !exited; attempt++) {
        UINTN mapKey = 0;
        UINT32 descVer = 0;
        mapSize = MAP_BUF_PAGES * PAGE_SIZE;
        EFI_STATUS s = g_bs->GetMemoryMap(&mapSize, rawmap, &mapKey, &descSize, &descVer);
        if (EFI_ERROR(s)) { log_str("loader: GetMemoryMap failed\n"); loader_halt(); }
        if (!EFI_ERROR(g_bs->ExitBootServices(image, mapKey))) exited = 1;
    }
    if (!exited) { log_str("loader: ExitBootServices failed\n"); loader_halt(); }
    g_bs = 0;                       /* console logging is serial-only from here on */

    /* --- convert the memory map: classify, sort, merge --- */
    UINT64 ndesc = mapSize / descSize;
    BootMemRegion *regions = arena_alloc(ndesc * sizeof(BootMemRegion), 8);
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
    log_str("loader: entering kernel\n");
    handoff(pml4, kimg.entry, stack_top_virt, HHDM_BASE + (UINT64)bi);
}
