/* The panic path and the record that survives a reset (kernel/panic.c, docs/RELIABILITY.md). */
#ifndef K_PANICREC_H
#define K_PANICREC_H

#include "types.h"

/* panic() and panic_frame() (trap.c) bracket what they print with these. panic_begin() turns interrupts off, gives the panic the screen and
 * returns how deep it is: 1 for a first panic, 2 for a panic while the first was being handled (print one line), more (print nothing).
 * panic_end() writes the record, flushes the netconsole and restarts the machine after "panic=SECONDS", or stops it. */
int  panic_begin(void);
NORETURN void panic_end(void);
bool panic_in_progress(void);

void panicrec_shutdown(void);       /* a restart or poweroff that was asked for: the record says that this start ended cleanly */

/* Kernel log text as the screen got it, made plain: cursor moves become line breaks, other escape sequences and control characters
 * (but tab and newline) go. In place; returns the new length. */
size_t klog_plain(char *s, size_t n);

#endif
