#include "kernel.h"
#include "serial.h"
#include "fb.h"
#include "fbcon.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "cmdline.h"
#include "arch.h"
#include "mm.h"
#include "irq.h"
#include "task.h"

u64 g_hhdm;
u64 g_kernel_phys, g_kernel_virt;
BootInfo g_boot;

extern char __kernel_start[], __kernel_end[];

static void print_banner(void) {
    printk("\x1b[1;35m");
    printk("  _____                _                ___  ____\n");
    printk(" |_   _|__  _   _ | |__   ___  _   _ / _ \\/ ___|\n");
    printk("   | |/ _ \\| | | || '_ \\ / _ \\| | | | | | \\___ \\\n");
    printk("   | | (_) | |_| || | | | (_) | |_| | |_| |___) |\n");
    printk("   |_|\\___/ \\__,_||_| |_|\\___/ \\__,_|\\___/|____/\n");
    printk("\x1b[0m\n");
}

static void crash_test(const char *kind) {
    klog("Eiki", "deliberate crash requested: %s", kind);
    if (!strcmp(kind, "div0")) {
        volatile int num = 100, den = 0;        /* both volatile: "1 / x" would be folded without trapping */
        volatile int q = num / den;
        (void)q;
    } else if (!strcmp(kind, "pf")) {
        *(volatile u64 *)0xdead0000UL = 1;
    } else if (!strcmp(kind, "null")) {
        *(volatile u64 *)8 = 1;
    } else if (!strcmp(kind, "gp")) {
        __asm__ volatile ("movw $0x100, %%ax; movw %%ax, %%ds" : : : "ax");
    } else if (!strcmp(kind, "ud")) {
        __asm__ volatile ("ud2");
    } else if (!strcmp(kind, "panic")) {
        panic("requested panic");
    } else if (!strcmp(kind, "nx")) {
        void (*fn)(void) = (void (*)(void))(g_hhdm + 0x200000);     /* direct map is NX */
        fn();
    } else if (!strcmp(kind, "ro")) {
        *(volatile char *)__kernel_start = 0;                       /* text is read-only */
    }
}

NORETURN void kmain(BootInfo *bi) {
    serial_init();
    if (bi->magic != BOOTINFO_MAGIC) {
        serial_write("kernel: bad BootInfo magic\n", 27);
        for (;;) hlt();
    }
    memcpy(&g_boot, bi, sizeof g_boot);
    g_hhdm = bi->hhdm_base;
    g_kernel_phys = bi->kernel_phys;
    g_kernel_virt = bi->kernel_virt;
    cmdline_init(bi->cmdline);

    cpu_init();
    gdt_init();
    idt_init();

    /* Early console on the loader's direct map. */
    fb_init(&g_boot.fb);
    fbcon_init();
    print_banner();
    klog("Gensokyo", "kernel %s \"%s\" starting", KERNEL_VERSION, KERNEL_CODENAME);
    klog("Gensokyo", "cpu: %s (%s)", g_cpu.brand, g_cpu.vendor);
    klog("Gensokyo", "image %p .. %p, cmdline '%s'", __kernel_start, __kernel_end, cmdline_string());

    char crash[24];
    if (cmdline_value("crash", crash, sizeof crash) && !strncmp(crash, "early-", 6)) crash_test(crash + 6);

    boot_mmap_save(bi);
    pmm_init();
    vmm_init();
    fb_remap();                         /* the loader's direct-map alias of the framebuffer is gone now */
    klog("Kappa", "framebuffer %ux%u at %#lx, write-combining", g_boot.fb.width, g_boot.fb.height,
         (unsigned long)g_boot.fb.base);
    pmm_reclaim_boot_memory();
    klog("Boundary", "paging online: %lu MiB RAM, %lu MiB free", (unsigned long)(pmm_total_pages() >> 8),
         (unsigned long)(pmm_free_pages_count() >> 8));

    sched_init();
    pic_init();
    timer_init();
    sti();
    klog("Sakuya", "interrupts enabled, scheduler running");

    if (cmdline_value("crash", crash, sizeof crash) && strncmp(crash, "early-", 6)) crash_test(crash);
    if (cmdline_flag("selftest")) selftest_run();

    klog("Gensokyo", "boot complete");
    printk("\n\x1b[1;35m Welcome to Gensokyo. \x1b[0m\n");
    for (;;) hlt();
}
