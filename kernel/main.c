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
#include "pci.h"
#include "acpi.h"
#include "initcall.h"
#include "stagemark.h"

void virtio_blk_init(void);
void ahci_init(void);
void nvme_init(void);
void usb_init(void);
bool usb_wait_settled(u32 max_ms);
void storage_mount_boot_volumes(void);

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

/* crash=desktop: a panic while the window server owns the screen (init can no longer be killed by a signal, so the test that
 * checks the panic is drawn over the desktop needs another way). Waits for the window server to take the screen, then gives
 * the desktop time to paint its first frames. */
static void crash_on_desktop(void *arg) {
    (void)arg;
    while (fbcon_enabled()) task_sleep_ms(100);
    klog("Eiki", "crash=desktop: the window server has the screen; the verdict falls in 20 seconds");
    task_sleep_ms(20000);
    panic("requested panic while the window server owns the screen (crash=desktop)");
}

static void crash_test(const char *kind) {
    klog("Eiki", "deliberate crash requested: %s", kind);
    if (!strcmp(kind, "desktop")) {
        task_create_detached("crash-desktop", crash_on_desktop, NULL);
        return;
    }
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

/* Boot progress squares (shared/stagemark.h), drawn through the loader's direct map of the framebuffer, and "stagehold=N". */
static int g_stagehold;

static void stage(int n) {
    const BootFramebuffer *f = &g_boot.fb;
    u64 end = f->base + (u64)f->pitch * f->height;
    if (f->base && end <= (4ULL << 30))             /* the direct map reaches at least the first 4 GiB */
        stage_mark((volatile u8 *)(g_hhdm + f->base), f->pitch, f->width, f->height, n);
    if (n == 4 && cmdline_flag("stagebeep")) {      /* one long beep on the PC speaker: the kernel is running, whatever the screen shows */
        outb(0x43, 0xB6); outb(0x42, 0xA9); outb(0x42, 0x04);
        outb(0x61, (u8)(inb(0x61) | 3));
        for (u64 t = rdtsc(); rdtsc() - t < 1200000000ULL;) __asm__ volatile ("pause");
        outb(0x61, (u8)(inb(0x61) & ~3));
    }
    if (g_stagehold == n) for (;;) hlt();
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
    char hold[8];
    if (cmdline_value("stagehold", hold, sizeof hold)) for (const char *q = hold; *q >= '0' && *q <= '9'; q++) g_stagehold = g_stagehold * 10 + (*q - '0');
    heap_init_debug();                  /* "heapdebug": before the first allocation, so every object is poisoned or none */
    stage(4);

    cpu_init();
    stage(5);
    gdt_init();
    idt_init();
    stage(6);

    /* Early console on the loader's direct map. */
    fb_init(&g_boot.fb);
    stage(7);
    fbcon_init();
    stage(8);
    print_banner();
    klog("Gensokyo", "kernel %s \"%s\" starting", KERNEL_VERSION, KERNEL_CODENAME);
    klog("Gensokyo", "cpu: %s (%s)", g_cpu.brand, g_cpu.vendor);
    char cpudesc[160];
    cpu_describe(cpudesc, sizeof cpudesc);
    klog("Gensokyo", "cpu: %s", cpudesc);
    klog("Gensokyo", "image %p .. %p, cmdline '%s'", __kernel_start, __kernel_end, cmdline_string());

    char crash[24];
    if (cmdline_value("crash", crash, sizeof crash) && !strncmp(crash, "early-", 6)) crash_test(crash + 6);

    boot_mmap_save(bi);
    pmm_init();
    boot_mmap_report();
    vmm_init();
    fb_remap();                         /* the loader's direct-map alias of the framebuffer is gone now */
    klog("Kappa", "framebuffer %ux%u at %#lx, pitch %u bytes, %s; the firmware's MTRR for it: %s", g_boot.fb.width, g_boot.fb.height,
         (unsigned long)g_boot.fb.base, g_boot.fb.pitch, g_fb.ready ? fb_cache_name() : "none (no graphics output was found)",
         g_fb.ready ? cpu_mtrr_type_name(g_boot.fb.base, (u64)g_boot.fb.pitch * g_boot.fb.height) : "-");
    klog("Boundary", "paging online: %lu MiB RAM, %lu MiB free", (unsigned long)(pmm_total_pages() >> 8),
         (unsigned long)(pmm_free_pages_count() >> 8));
    acpi_init();

    sched_init();
    pic_init();
    lapic_virtual_wire();
    timer_init();
    fb_report(timer_tsc_mhz());         /* how fast the first full-screen fill was: tells a write-combining mapping from an uncached one */
    tty_console_init();
    input_init();
    sti();
    klog("Sakuya", "interrupts enabled, scheduler running");
    timer_verify();                     /* no tick from the PIT? the local APIC timer takes over (a PC gives no other hint why it froze) */
    rtc_init();
    random_init();

    if (cmdline_value("crash", crash, sizeof crash) && strncmp(crash, "early-", 6)) crash_test(crash);
    if (cmdline_flag("selftest")) selftest_run();

    vfs_init();
    layout_create();
    keyboard_init();
    serial_rx_init();

    /* Buses and device drivers. Each *_init is a weak no-op until a driver supplies it (see drivers/driver_stubs.c). */
    pci_init();
    pmm_reclaim_boot_memory();          /* only now: pci_init has switched off any DMA the firmware left running into that memory */
    fb_tune(timer_tsc_mhz());           /* a slow screen (no write-combining) gets a write-combining MTRR over its BAR, if that makes it faster: gfx/fbtune.c */
    u64 t_storage = uptime_ms();
    virtio_blk_init();
    ahci_init();
    nvme_init();
    storage_mount_boot_volumes();
    klog("Marisa", "storage probing took %lu ms", (unsigned long)(uptime_ms() - t_storage));      /* a spun-down disk can make this long: it is the usual reason for a slow boot */
    usb_init();
    run_initcalls();                    /* device nodes, sockets, ptys: everything registered with INITCALL() */

    if (g_boot.initrd_phys && g_boot.initrd_size) {
        initrd_unpack(phys_to_virt(g_boot.initrd_phys), g_boot.initrd_size);
        pmm_add_region(g_boot.initrd_phys, ALIGN_UP(g_boot.initrd_size, PAGE_SIZE));    /* contents now live in tmpfs */
    }

    usb_wait_settled(8000);             /* a USB stick's HUMANVILLAGE volume is mounted before the first session starts (bounded; returns at once without USB) */
    process_init();
    klog("Gensokyo", "boot complete after %lu ms", (unsigned long)uptime_ms());
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
