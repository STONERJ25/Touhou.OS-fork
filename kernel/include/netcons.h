/* The netconsole (kernel/drivers/netcons.c, docs/RELIABILITY.md): the kernel log as UDP datagrams, "netconsole=IP[:PORT]". */
#ifndef K_NETCONS_H
#define K_NETCONS_H

#include "types.h"

/* The panic path (panic.c): sends what has not been sent yet, through the card's polled transmit (NetDev.netpoll_tx), with interrupts off,
 * without a lock and without allocating. Does nothing without "netconsole=" or before the network was ever up. */
void netcons_panic_flush(void);

#endif
