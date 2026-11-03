/* Machine exit paths: QEMU debug-exit, ACPI-style poweroff, reset. */
#include "arch.h"
#include "abi.h"
#include "cpu.h"
#include "kernel.h"
#include "acpi.h"
#include "fbcon.h"
#include "irq.h"
#include "cmdline.h"
#include "net.h"

NORETURN void qemu_exit(int code) {
    /* The test harness reads the serial port over a TCP connection and QEMU leaves through exit(): on a loaded Windows host the connection
     * can be reset before the harness has read the last lines, the verdict ("UTEST PASS") among them, and a passing run is reported as
     * "QEMU exited (code 1)" (about 1 run in 60 for the utest suite while the host was busy, on main as well). Give the reader a moment. */
    udelay(300000);
    outb(0xF4, (u8)code);               /* isa-debug-exit: QEMU exits with (code << 1) | 1 */
    for (;;) hlt();
}

/* Everything the hardware needs before the power goes. The network cards are armed so that a magic packet wakes the machine, and the chipset is told to listen for them (the
 * cards' power-management event is a wake source only when the chipset's event block says so); "e1000.wol=off" on the command line leaves it all alone. Interrupts are off while it
 * runs: the network thread polls the cards, and a poll in the middle of the card's register sequence would be a second user of it. Done once, so calling it twice is harmless:
 * the shutdown paths call it before the last log lines go to the disk (what it says is the only record if the machine does not wake), machine_poweroff() as the safety net. */
void machine_prepare_poweroff(void) {
    static bool done;
    if (done) return;
    done = true;
    u64 fl = irq_save();
    bool armed = netdev_shutdown_all();
    acpi_wake_arm(armed);
    irq_restore(fl);
}

NORETURN void machine_poweroff(void) {
    cli();
    machine_prepare_poweroff();
    netdev_shutdown_all();              /* the last steps: whatever a magic packet latched since the card was armed is cleared, or the machine would wake the moment it is off */
    acpi_wake_settle();
    acpi_poweroff();                    /* real hardware: the FADT's PM1 control block and the _S5 sleep type */
    outw(0x604, 0x2000);                /* QEMU q35 / ACPI PM1a_CNT shortcut */
    outw(0xB004, 0x2000);               /* older QEMU / Bochs */
    outw(0x4004, 0x3400);               /* VirtualBox */
    /* Still running: none of the ways to switch off worked. Whoever is looking at a frozen desktop deserves to be told. */
    fbcon_force_on();
    printk("\n\x1b[1;33m[Gensokyo] the machine did not switch itself off (ACPI S5 failed). It is safe to hold the power button now.\x1b[0m\n");
    for (;;) hlt();
}

/* ---- restart ---- */

/* A beep of the PC speaker (the PIT's channel 2 through the speaker gate): a restart that does not happen leaves the screen frozen and the network dead, and the
 * stage it got stuck at is worth knowing. */
static void speaker_beep(u32 hz, u32 ms) {
    u32 div = 1193182u / hz;
    outb(0x43, 0xB6);
    outb(0x42, (u8)div);
    outb(0x42, (u8)(div >> 8));
    u8 g = inb(0x61);
    outb(0x61, (u8)(g | 3));
    udelay(ms * 1000u);
    outb(0x61, (u8)(g & ~3u));
}

static void settle(void) { for (int i = 0; i < 200000; i++) inb(0x80); }

static const char *const g_way_name[RESET_LAST + 1] = {
    "auto", "the ACPI reset register", "the PCI reset port (0xCF9)", "the keyboard controller", "the firmware's ResetSystem", "a triple fault",
};

static void try_way(int way) {
    switch (way) {
    case RESET_ACPI: acpi_reset(); break;
    case RESET_PCI: outb(0xCF9, 0x02); udelay(50); outb(0xCF9, 0x06); settle(); break;
    case RESET_KBD:
        for (int i = 0; i < 100000; i++) { if (!(inb(0x64) & 2)) break; }
        outb(0x64, 0xFE);                   /* pulse the CPU reset line through the keyboard controller */
        settle();
        break;
    case RESET_EFI: efi_reset_system(); break;
    case RESET_TRIPLE: {
        struct { u16 limit; u64 base; } PACKED empty = { 0, 0 };
        __asm__ volatile ("lidt %0; int3" : : "m"(empty));
        break;
    }
    default: break;
    }
}

static int way_by_name(const char *s, size_t n) {
    static const struct { const char *name; int way; } names[] = {
        { "acpi", RESET_ACPI }, { "pci", RESET_PCI }, { "cf9", RESET_PCI }, { "kbd", RESET_KBD }, { "efi", RESET_EFI }, { "triple", RESET_TRIPLE },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++)
        if (strlen(names[i].name) == n && !memcmp(names[i].name, s, n)) return names[i].way;
    return 0;
}

/* The ways in the order they are tried: the one asked for, then "reboot=efi,pci" from the command line, then the defaults: the ACPI reset register, the PCI reset port, the
 * keyboard controller. Three ways are left out unless they are named, each for what it did on the Dell OptiPlex 7050 (2026-10-25):
 *  - the ACPI reset register when it is the SMI command port (io 0xb2 = 0x73 there): a software SMI that never came back and froze the machine before the PCI way, which does
 *    restart it, was reached;
 *  - the firmware's ResetSystem: a #GP at address 0x1000, inside the firmware's own reset code, in the physical mode that is all a loader that never calls
 *    SetVirtualAddressMap can offer it (`reboot efi` still tries it; QEMU's OVMF restarts through it);
 *  - a triple fault: where it does not restart the machine it halts the processor for good, and that is no place to power off from.
 * When no way restarts the machine it is switched off. */
static int restart_order(int via, int order[RESET_LAST + 1]) {
    int n = 0;
    bool used[RESET_LAST + 1] = { false };
    #define ADD(w) do { int w_ = (w); if (w_ > 0 && w_ <= RESET_LAST && !used[w_]) { used[w_] = true; order[n++] = w_; } } while (0)
    ADD(via);
    char list[64];
    if (cmdline_value("reboot", list, sizeof list)) {
        for (const char *p = list; *p;) {
            const char *e = p;
            while (*e && *e != ',') e++;
            if ((size_t)(e - p) == 4 && !memcmp(p, "none", 4)) return 0;       /* "reboot=none" (a test hook): no way is tried, the machine is switched off instead */
            ADD(way_by_name(p, (size_t)(e - p)));
            p = *e ? e + 1 : e;
        }
    }
    if (!acpi_reset_is_smi()) ADD(RESET_ACPI);
    ADD(RESET_PCI);
    ADD(RESET_KBD);
    #undef ADD
    return n;
}

/* "acpi, pci, kbd": what a plain restart tries, for /proc/acpi. */
void restart_order_text(char *out, size_t n) {
    static const char *const short_name[RESET_LAST + 1] = { "auto", "acpi", "pci", "kbd", "efi", "triple" };
    int order[RESET_LAST + 1];
    int k = restart_order(0, order);
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < k && o + 1 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? ", " : "", short_name[order[i]]);
    if (!k) snprintf(out, n, "none (reboot=none), power off");
    else if (o + 1 < n) snprintf(out + o, n - o, ", then power off");
}

NORETURN void machine_reboot(int via) {
    cli();
    fbcon_force_on();                                   /* the desktop would hide what follows */
    int order[RESET_LAST + 1];
    int n = restart_order(via, order);
    char rst[48];
    acpi_describe_reset(rst, sizeof rst);
    printk("\n[Gensokyo] restarting (ACPI reset register: %s; firmware ResetSystem: %s)\n", rst, efi_reset_known() ? "yes" : "no");
    for (int i = 0; i < n; i++) {
        printk("[Gensokyo] restart, way %d: %s\n", i + 1, g_way_name[order[i]]);
        for (int b = 0; b <= i; b++) {                  /* way N is announced by N beeps: the count of the last one heard is how far it got */
            speaker_beep(1000, 70);
            udelay(110000);
        }
        try_way(order[i]);
    }
    printk("\n\x1b[1;33m[Gensokyo] none of the ways to restart the machine worked");
    if (acpi_can_poweroff()) {
        printk(": switching it off instead.\x1b[0m\n");
        machine_poweroff();
    }
    printk(".\x1b[0m\n");
    try_way(RESET_TRIPLE);                              /* nothing else is left to try */
    for (;;) hlt();
}
