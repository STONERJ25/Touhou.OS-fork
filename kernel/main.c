#include "kernel.h"
#include "serial.h"
#include "fb.h"
#include "fbcon.h"
#include "cpu.h"

u64 g_hhdm;

extern char __kernel_start[], __kernel_end[];

static const char *region_type_name(u32 t) {
    switch (t) {
    case BOOTMEM_USABLE:           return "usable";
    case BOOTMEM_BOOT_RECLAIMABLE: return "boot-reclaimable";
    case BOOTMEM_ACPI_RECLAIMABLE: return "acpi-reclaimable";
    case BOOTMEM_ACPI_NVS:         return "acpi-nvs";
    case BOOTMEM_RESERVED:         return "reserved";
    case BOOTMEM_MMIO:             return "mmio";
    case BOOTMEM_KERNEL:           return "kernel";
    case BOOTMEM_MODULE:           return "module";
    default:                       return "?";
    }
}

NORETURN void kmain(BootInfo *bi) {
    serial_init();
    if (bi->magic != BOOTINFO_MAGIC) {
        serial_write("kernel: bad BootInfo magic\n", 27);
        for (;;) hlt();
    }
    g_hhdm = bi->hhdm_base;

    fb_init(&bi->fb);
    fbcon_init();

    klog("Gensokyo", "kernel %s \"%s\" starting", KERNEL_VERSION, KERNEL_CODENAME);
    klog("Gensokyo", "image %p .. %p", __kernel_start, __kernel_end);
    klog("Kappa", "framebuffer %ux%u pitch %u at phys %#lx (%s)", bi->fb.width, bi->fb.height, bi->fb.pitch,
         (unsigned long)bi->fb.base, bi->fb.format == BOOTFB_BGRX ? "BGRX" : "RGBX");
    klog("Gensokyo", "cmdline: '%s'", bi->cmdline);

    const BootMemRegion *mm = phys_to_virt(bi->mmap_phys);
    u64 usable = 0;
    for (u32 i = 0; i < bi->mmap_count; i++) {
        klog("Boundary", "%#012lx-%#012lx %s", (unsigned long)mm[i].base,
             (unsigned long)(mm[i].base + mm[i].length), region_type_name(mm[i].type));
        if (mm[i].type == BOOTMEM_USABLE) usable += mm[i].length;
    }
    klog("Boundary", "%lu MiB usable", (unsigned long)(usable >> 20));

    printk("\n\x1b[1;35m Welcome to Gensokyo. \x1b[0m\n");
    printk("\x1b[36m┌────────┐\n│ box ok │\n└────────┘\x1b[0m\n");

    for (;;) hlt();
}
