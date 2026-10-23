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
#include "yinyang.h"
#include "input.h"
#include "tty.h"
#include "vfs.h"
#include "serial.h"
#include "ktime.h"
#include "random.h"
#include "process.h"

u64 g_hhdm;
u64 g_kernel_phys, g_kernel_virt;
BootInfo g_boot;

extern char __kernel_start[], __kernel_end[];
void kshell_main(void *arg);

#define SHRINE_RED   0xC8102E
#define CONSOLE_BG   0x0B0B14

/* Yin-yang orb on the left, title block on the right; the log starts below. */
static void print_banner(void) {
    static u32 orb[64 * 64];
    yinyang_render(orb, 64, 64, 0xF2F2F2, SHRINE_RED, CONSOLE_BG);
    fb_blit(10, 8, 64, 64, orb, 64);
    printk("\x1b[1;12H\x1b[1m\x1b[38;2;200;16;46mTouhou\x1b[38;2;242;242;242mOS\x1b[0m");
    printk("\x1b[2;12H\x1b[96mGensokyo kernel %s\x1b[0m  \"%s\"", KERNEL_VERSION, KERNEL_CODENAME);
    printk("\x1b[3;12H\x1b[90man unofficial Touhou Project fan OS -- the shrine gate is open\x1b[0m");
    printk("\x1b[4;12H\x1b[90m%s\x1b[0m", "────────────────────────────────────────");
    printk("\x1b[6;1H");
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
    tty_console_init();
    input_init();
    sti();
    klog("Sakuya", "interrupts enabled, scheduler running");
    rtc_init();
    random_init();

    if (cmdline_value("crash", crash, sizeof crash) && strncmp(crash, "early-", 6)) crash_test(crash);
    if (cmdline_flag("selftest")) selftest_run();

    vfs_init();
    layout_create();
    keyboard_init();
    serial_rx_init();
    if (g_boot.initrd_phys && g_boot.initrd_size) {
        initrd_unpack(phys_to_virt(g_boot.initrd_phys), g_boot.initrd_size);
        pmm_add_region(g_boot.initrd_phys, ALIGN_UP(g_boot.initrd_size, PAGE_SIZE));    /* contents now live in tmpfs */
    }

    process_init();
    klog("Gensokyo", "boot complete");
    printk("\n\x1b[1;35m Welcome to Gensokyo. \x1b[0m\n");

    char initpath[64] = "/sbin/init";
    cmdline_value("init", initpath, sizeof initpath);
    struct gstat st;
    if (strcmp(initpath, "kshell") && vfs_stat(initpath, NULL, NULL, 0, &st) == 0) {
        int rc = process_spawn_init(initpath, NULL);
        if (rc) klog("Gensokyo", "cannot start %s (%d)", initpath, rc);
        else klog("Alice", "init: %s", initpath);
    } else {
        klog("Alice", "no init program (%s): starting the kernel shell instead", initpath);
        task_create_detached("kshell", kshell_main, NULL);
    }
    for (;;) task_sleep_ms(1000);
}
