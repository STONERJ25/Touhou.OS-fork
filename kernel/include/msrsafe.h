/*
 * Model-specific registers that may not be there, read without taking the machine down.
 *
 * rdmsr of a register the CPU does not have raises #GP, and a #GP in the kernel is a panic. Whether a register exists depends on the model,
 * on the firmware and on the hypervisor (QEMU's TCG answers some and faults on others; a CPUID bit that says "yes" is no promise), so the
 * thermal code reads through msr_read_safe(): the one rdmsr instruction it executes has its address in a small table (msrsafe.asm), the
 * #GP handler looks the faulting address up in that table, and a hit makes the function return -1 instead of the machine dying.
 *
 * The handler is put in front of the kernel's own #GP stub by patching vector 13 of the IDT at run time (msrsafe_init); every other #GP goes
 * on to the old stub untouched. Nothing is written to a register: this is for reading only.
 */
#ifndef K_MSRSAFE_H
#define K_MSRSAFE_H

#include "types.h"

void msrsafe_init(void);                    /* installs the handler (once; later calls do nothing) */
bool msrsafe_ready(void);
int  msr_read_safe(u32 msr, u64 *out);      /* 0 with *out set, or -1: the CPU raised #GP (or the handler is not installed, in which case rdmsr is not tried) */
int  msrsafe_selftest(void);                /* executes a deliberate #GP under the handler's protection: 0 when it was caught, -1 when the handler is not installed */

#endif
