/* ACPI: just enough to count the processors, power the machine off and reset it. */
#ifndef K_ACPI_H
#define K_ACPI_H

#include "types.h"

void acpi_init(void);                   /* reads the tables the loader found (BootInfo.rsdp_phys); safe to call without ACPI */
bool acpi_poweroff(void);               /* enters sleep state S5; returns (false) only if that is impossible or did not work */
bool acpi_reset(void);                  /* writes the FADT reset register; returns (false) if there is none or it did not work */
bool acpi_can_poweroff(void);           /* S5 is known: acpi_poweroff() has a chance */
void acpi_wake_settle(void);            /* the last step before the power goes: clear the latched PME_B0 status again (a magic packet during the shutdown would wake the machine at once) */
void acpi_wake_arm(bool nic_armed);     /* before a power-off: enable the chipset's "PME_B0" wake event (Wake on LAN) when a network card has been armed to raise it */
bool acpi_reset_is_smi(void);           /* the reset register is the SMI command port (a software SMI that may never come back): left out of the default restart order */
void acpi_describe_reset(char *out, size_t n);   /* "io 0xcf9 = 0x06", "none" ... */
void restart_order_text(char *out, size_t n);    /* the default restart order for /proc/acpi: "acpi, pci, kbd" (power.c) */
int  acpi_cpu_count(void);              /* enabled processors listed in the MADT, 0 if unknown */
bool acpi_pm_timer(u16 *port, bool *wide);   /* the PM timer's I/O port and width, false if the firmware has none */
int  acpi_ioapics(u32 *addrs, int max);      /* register window addresses of the I/O APICs in the MADT; returns how many */

#endif
