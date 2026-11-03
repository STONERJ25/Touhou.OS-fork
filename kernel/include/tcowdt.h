/* The hardware watchdog (kernel/drivers/tcowdt.c, docs/RELIABILITY.md): the Intel TCO timer, fed by the kernel until user space opens /dev/watchdog. */
#ifndef K_TCOWDT_H
#define K_TCOWDT_H

#include "types.h"

enum { TCOWDT_OFF = 0, TCOWDT_KERNEL, TCOWDT_USER, TCOWDT_NOBODY };     /* who feeds it */

/* A restart or a poweroff that somebody asked for begins (sys_reboot): the timer gets the shutdown's time budget and nobody feeds it any more,
 * so that a slow shutdown is not cut short and a shutdown that hangs still ends in a restart. Also tells the panic record that this start ends
 * on purpose. */
void tcowdt_shutdown(void);
void tcowdt_panic_restart(void);    /* the panic path restarts the machine: the same long budget, port I/O only, no log line */

int  tcowdt_feeder(void);           /* TCOWDT_*: TCOWDT_OFF when there is no watchdog or it is not armed */
u32  tcowdt_timeout(void);          /* seconds from the last feed to the restart, 0 when not armed */
u32  tcowdt_seconds_left(void);     /* until the restart, if nobody feeds it from now on (0 when not armed) */

#endif
