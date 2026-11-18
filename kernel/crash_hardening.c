/*
 * Deliberate faults for the hardening features (docs/HARDENING.md), asked for with crash=KIND on the kernel command line like the ones in main.c.
 * main.c's crash_test hands every kind it does not know to hardening_crash() below; tests/case_hardening2.py boots each one and looks at the panic.
 *
 *   crash=smep    the kernel jumps to an executable user page: SMEP must stop it ("SMEP VIOLATION" with the address)
 *   crash=smap    the kernel reads a user page outside the copy routines' window: SMAP must stop it ("SMAP VIOLATION" with the address)
 *   crash=smap-window  every copy routine in every way it can end (a thread, once the fault fixup of process_init is installed), then a plain load: the window must be closed again
 *   crash=lockup  a kernel thread spins with interrupts on and never sleeps: the lockup detector (kernel/sched/watchdog.c) must report it; the boot goes on
 *
 * The user page is made in the kernel's own page tables (nothing but the kernel runs this early), at an address below USER_TOP, with the U/S bit set. Written
 * from the Intel SDM, volume 3A, 4.6.1: SMEP faults a supervisor-mode instruction fetch from a user page; SMAP faults a supervisor-mode data access to one
 * unless EFLAGS.AC is set, and nothing here sets it.
 */
#include "kernel.h"
#include "cpu.h"
#include "cpuinfo.h"
#include "mm.h"
#include "task.h"
#include "process.h"
#include "arch.h"

extern bool (*g_pagefault_hook)(TrapFrame *tf, u64 cr2);        /* kernel/arch/x86_64/trap.c: process_init() installs the fixup for faults inside the copy routines */

#define PROBE_ADDR 0x400000UL

/* A page the CPU takes for user memory: executable (and holding a RET) or not. */
static bool map_user_probe(bool exec) {
    paddr_t p = pmm_alloc_page_zeroed();
    if (!p) return false;
    if (exec) *(volatile u8 *)phys_to_virt(p) = 0xC3;                  /* ret */
    if (vmm_map(vmm_kernel_pml4(), PROBE_ADDR, p, PTE_U | PTE_W | (exec ? 0 : PTE_NX))) { pmm_free_page(p); return false; }
    return true;
}

static void spin_forever(void *arg) {
    (void)arg;
    for (;;) pause();                                                   /* interrupts stay on: the timer preempts it, and it never gives the CPU up by itself */
}

/* The copy routines in every way they end: a copy that works in both directions, a clear, and copies that fault and leave through __uaccess_fault. Each must close the
 * window again: the plain load that follows has to be refused (the caller reaches it as crash=smap-window; a window left open would let the load through). */
static bool windows_close(void) {
    char buf[16] = "SMAP";
    if (copy_to_user((void *)PROBE_ADDR, buf, sizeof buf)) return false;
    char back[16];
    if (copy_from_user(back, (const void *)PROBE_ADDR, sizeof back) || memcmp(buf, back, sizeof buf)) return false;
    if (clear_user((void *)PROBE_ADDR, sizeof buf)) return false;
    if (copy_from_user(back, (const void *)(PROBE_ADDR + 0x100000), sizeof back) != -EFAULT) return false;       /* nothing mapped: the fault is redirected */
    if (copy_to_user((void *)(PROBE_ADDR + 0x100000), buf, sizeof buf) != -EFAULT) return false;
    if (clear_user((void *)(PROBE_ADDR + 0x100000), sizeof buf) != -EFAULT) return false;
    char s[16];
    if (strncpy_from_user(s, (const char *)(PROBE_ADDR + 0x100000), sizeof s) != -EFAULT) return false;
    return true;
}

/* crash=smap-window needs the fixup that sends a fault inside the copy routines to __uaccess_fault, which process_init() installs well after the crash tests run: a thread
 * waits for it, then does the whole round. */
static void smap_window_thread(void *arg) {
    (void)arg;
    while (!g_pagefault_hook) task_sleep_ms(10);
    if (!map_user_probe(false)) { klog("Eiki", "crash=smap-window: no memory for the probe page"); return; }
    if (!windows_close()) { klog("Eiki", "crash=smap-window: a copy routine gave a wrong answer"); return; }
    klog("Eiki", "crash=smap-window: every copy routine worked; the next load must be refused");
    volatile u64 v = *(volatile u64 *)PROBE_ADDR;
    klog("Eiki", "crash=smap-window: the load went through (%lu): a window was left open", (unsigned long)v);
}

bool hardening_crash(const char *kind) {
    if (!strcmp(kind, "smap-window")) {
        if (!g_cpu.smap_on) { klog("Eiki", "crash=smap-window: SMAP is not on: nothing to test"); return true; }
        if (!task_create_detached("smap-window", smap_window_thread, NULL)) klog("Eiki", "crash=smap-window: could not start the thread");
        return true;
    }
    if (!strcmp(kind, "smap")) {
        if (!g_cpu.smap_on) { klog("Eiki", "crash=smap: SMAP is not on (%s): nothing to test", g_cpu.smap_off_asked ? "smap=off" : "not offered by this CPU"); return true; }
        if (!map_user_probe(false)) { klog("Eiki", "crash=smap: no memory for the probe page"); return true; }
        volatile u64 v = *(volatile u64 *)PROBE_ADDR;                   /* a plain load: AC is clear */
        klog("Eiki", "crash=smap: the load went through (%lu)", (unsigned long)v);
        return true;
    }
    if (!strcmp(kind, "smep")) {
        if (!g_cpu.smep_on) { klog("Eiki", "crash=smep: SMEP is not on: nothing to test"); return true; }
        if (!map_user_probe(true)) { klog("Eiki", "crash=smep: no memory for the probe page"); return true; }
        ((void (*)(void))PROBE_ADDR)();
        klog("Eiki", "crash=smep: the jump to the user page returned");
        return true;
    }
    if (!strcmp(kind, "lockup")) {
        if (!task_create_detached("lockup", spin_forever, NULL)) klog("Eiki", "crash=lockup: could not start the spinning thread");
        return true;
    }
    return false;
}
