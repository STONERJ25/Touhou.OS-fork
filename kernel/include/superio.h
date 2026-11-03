/*
 * Looking for the chip that has the fan (docs/SENSORS.md, "The fan").
 *
 * The CPU does not know how fast the fan turns. On a PC that is the Super I/O chip on the LPC bus (Nuvoton/Winbond, ITE, SMSC/Microchip, Fintek),
 * which has a "hardware monitor" block with fan tachometer inputs, or the embedded controller. Reaching the chip means writing a key to
 * its configuration port (0x2E or 0x4E) -- standard practice, lm-sensors' sensors-detect does it -- which is why this runs ONLY when root asks
 * (`sensors --probe-lpc`), never at boot and never by itself.
 *
 * What the probe may do, and nothing else:
 *   - write the unlock key sequence of one chip family to the INDEX port (0x2E or 0x4E), read the two ID registers (an index write, a data read),
 *     and leave configuration mode again (the family's exit);
 *   - for a chip it knows, select the logical device of the hardware monitor (the one write to a data port, register 0x07), read its activation
 *     and base address registers, and leave again.
 * It writes no other register of the chip. superio_probe() reaches the ports through SioPorts, so tests/host/test_superio.py can run it against a
 * model of the chips and check every byte that goes out.
 */
#ifndef K_SUPERIO_H
#define K_SUPERIO_H

#include "types.h"

typedef struct {
    void (*out)(void *ctx, u16 port, u8 v);
    u8   (*in)(void *ctx, u16 port);
    void *ctx;
} SioPorts;

enum { SIO_NONE, SIO_NUVOTON, SIO_ITE, SIO_SMSC };      /* the key sequence that unlocked it; Winbond and Fintek chips answer to Nuvoton's */

typedef struct {
    u16  port;                          /* 0x2E or 0x4E: the index port; the data port is the next one */
    int  family;                        /* SIO_* , SIO_NONE: nothing answered */
    u16  id;                            /* registers 0x20 (high) and 0x21 (low); for SMSC: device id and revision */
    char chip[24];                      /* the table's name for the id, or "" */
    bool hwm_known;                     /* the table says which logical device holds the hardware monitor */
    int  hwm_ldn;
    bool hwm_active;                    /* register 0x30 of that logical device says it is switched on */
    u16  hwm_base;                      /* registers 0x60/0x61: where its I/O ports are (0 when it has none) */
} SioResult;

int    superio_probe(const SioPorts *io, SioResult res[2]);          /* looks at 0x2E and 0x4E; returns how many of them answered */
size_t superio_format(const SioResult res[2], int answered, char *buf, size_t cap);    /* the text of /proc/superio after a probe */
const char *superio_family_name(int family);
bool   superio_lookup(int family, u16 id, const char **chip, int *hwm_ldn);   /* the table: false when the id is not in it */

#endif
