/*
 * The kernel log on the stick (kernel/fs/bootlog.c): once the HUMANVILLAGE volume is mounted read-write, TOUHOU.LOG at its root holds the
 * kernel log (what dmesg shows), TOUHOU.PRV the previous boot's. On a PC without a serial port this is how a failed first boot is read:
 * on another computer, from the stick. `bootlog=off` on the kernel command line switches it off.
 */
#ifndef K_BOOTLOG_H
#define K_BOOTLOG_H

#include "types.h"

/* The mount policy calls this after it mounted the volume. `rw` is false for a read-only (or dirty-mounted) volume: nothing is written then. */
void bootlog_volume_mounted(const char *mountpoint, bool rw);

/* Poweroff / reboot (before the final vfs_sync_all): writes what is pending plus a closing line. Does nothing when no log is being kept.
 * The panic path has no hook on purpose: every block driver here sleeps and waits for interrupts and for the USB thread, which a panic
 * (interrupts off, scheduler gone) cannot offer, so a write from there could only hang the machine in the middle of its last message.
 * The panic text is on the screen and the serial port; the file holds the log up to the last periodic write, at most about 5 s earlier. */
void bootlog_shutdown(void);

#endif
