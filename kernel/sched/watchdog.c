/*
 * The soft-lockup detector (docs/HARDENING.md, "The lockup detector"; docs/RELIABILITY.md says how it shares the work with the TCO hardware watchdog).
 *
 * What it looks for: a task that has been running in the kernel for a long time without ever giving the CPU up of its own accord. The kernel is preemptive (the timer
 * ends a time slice wherever the code is, so a task that spins with interrupts on does not stop the others from running), which is why a count of context switches would
 * say nothing: a spinning kernel thread is switched out and in again all the time. What it never does is sleep, block, yield or come back to user mode. The timer tick
 * charges itself to whatever it interrupted: if that was the kernel (not user mode, not the idle task), the task's Task.kspin goes up by one. Anything that shows the
 * task is going on normally sets it back to zero: sleeping or blocking (schedule() with the task no longer runnable), task_yield, a tick that finds the task in user
 * mode, and lockup_touch() from the loops that are known to be long and are alive (the block layer calls it for every request, which covers the volume scans and repairs).
 * When kspin reaches the threshold (20 s of the task's own time by default; "lockup.thresh=SECONDS" changes it, "lockup=off" switches the detector off) it reports.
 *
 * The report (tag Momiji, the watchdog's): the task's name and ids, how long, the registers of the interrupted context and a backtrace of it with the panic code's
 * symbolised walk, so that the line of code that loops is named. It is counted in /proc/kstats (watchdog_reports). A task that goes on spinning is reported again after another
 * threshold. With "panic_on_lockup" on the command line the report is a panic instead (panic_frame): the usual incident text, the record in RAM and the restart that
 * "panic=SECONDS" asks for, so that a machine nobody watches comes back by itself and says why.
 *
 * What it does not see: a CPU that runs with interrupts off for good (a hard lockup). The timer interrupt does not arrive there either, and QEMU has no NMI source to
 * break in with; the TCO hardware watchdog is what ends that (kernel/drivers/tcowdt.c: the machine restarts when nobody feeds it, with no word on why but the record's last
 * sign of life). The next step would be an NMI from a performance counter, which a PC has and QEMU's TCG does not emulate.
 *
 * The detector is armed when the boot is complete (lockup_arm, called from kmain just before init starts): the boot thread does long work in the kernel (the check and
 * the repair of the volume under hvfsck=safe, unpacking the initrd) that the hardware watchdog is fed through separately (tcowdt_boot_feed). A hang before that is the
 * hardware watchdog's.
 */
#include "kernel.h"
#include "task.h"
#include "irq.h"
#include "cmdline.h"
#include "cpu.h"
#include "ktime.h"
#include "process.h"

#define LOCKUP_THRESH_DEFAULT 20            /* seconds */

static u64 g_reports;
static bool g_armed, g_off, g_panic;
static u32 g_thresh_ticks = LOCKUP_THRESH_DEFAULT * TIMER_HZ;
static u32 g_thresh_secs = LOCKUP_THRESH_DEFAULT;

u64 watchdog_report_count(void) { return g_reports; }

/* For loops that are long on purpose and still alive (a scan that makes request after request to a device): the running task has not locked up. */
void lockup_touch(void) {
    if (current) current->kspin = 0;
}

void lockup_arm(void) {
    char v[8];
    g_off = cmdline_value("lockup", v, sizeof v) && !strcmp(v, "off");
    g_panic = cmdline_flag("panic_on_lockup");
    long s = cmdline_int("lockup.thresh", LOCKUP_THRESH_DEFAULT);
    if (s < 1) s = 1;
    if (s > 3600) s = 3600;
    g_thresh_secs = (u32)s;
    g_thresh_ticks = (u32)s * TIMER_HZ;
    if (current) current->kspin = 0;                    /* the boot thread has been in the kernel since the first instruction */
    g_armed = true;
    if (g_off) klog("Momiji", "soft lockup detector: off (lockup=off)");
    else klog("Momiji", "soft lockup detector: a task that runs in the kernel for %u s without sleeping is reported%s (the hardware watchdog covers interrupts off for good)",
              g_thresh_secs, g_panic ? " and the machine panics (panic_on_lockup)" : "");
}

/* Called from the timer tick's return path (task_on_trap_return) with the frame of what the tick interrupted. */
void lockup_tick(Task *c, const TrapFrame *tf, bool idle) {
    if ((tf->cs & 3) == 3 || idle) { c->kspin = 0; return; }            /* user mode, or nothing to do: not a lockup */
    c->kspin++;
    if (!g_armed || g_off || c->kspin < g_thresh_ticks) return;
    c->kspin = 0;                                                       /* the next report, if it is still at it, comes after another threshold */
    g_reports++;
    char ids[40];
    if (c->proc) snprintf(ids, sizeof ids, "tid %d, pid %d", c->tid, c->proc->pid);
    else snprintf(ids, sizeof ids, "kernel thread, tid %d", c->tid);
    if (g_panic)
        panic_frame(tf, "soft lockup: '%s' (%s) has run in the kernel for %u s without sleeping, yielding or returning to user mode (panic_on_lockup)", c->name, ids, g_thresh_secs);
    klog("Momiji", "soft lockup: '%s' (%s) has run in the kernel for %u s without sleeping, yielding or returning to user mode", c->name, ids, g_thresh_secs);
    dump_trapframe(tf);
    dump_backtrace(tf->rip, tf->rbp);
}
