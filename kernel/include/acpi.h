/* ACPI: just enough to count the processors, power the machine off and reset it. */
#ifndef K_ACPI_H
#define K_ACPI_H

#include "types.h"

void acpi_init(void);                   /* reads the tables the loader found (BootInfo.rsdp_phys); safe to call without ACPI */
bool acpi_poweroff(void);               /* enters sleep state S5; returns (false) only if that is impossible or did not work */
bool acpi_reset(void);                  /* writes the FADT reset register; returns (false) if there is none or it did not work */
int  acpi_cpu_count(void);              /* enabled processors listed in the MADT, 0 if unknown */
bool acpi_pm_timer(u16 *port, bool *wide);   /* the PM timer's I/O port and width, false if the firmware has none */
int  acpi_ioapics(u32 *addrs, int max);      /* register window addresses of the I/O APICs in the MADT; returns how many */

#endif
