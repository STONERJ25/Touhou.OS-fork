/*
 * Internal to libpc98: the 80386/80486 interpreter (x86cpu32.c). The public entry points in x86cpu.c hand the
 * 386-class models to these functions; the 8086, V30 and 286 never reach them.
 */
#ifndef GAP_X86CPU386_H
#define GAP_X86CPU386_H

#include <x86emu.h>

#define X86_IS386(cpu) ((cpu)->model >= X86_MODEL_80386)

int x86_386_step(X86Cpu *cpu);
void x86_386_init(X86Cpu *cpu);         /* the system registers' power-on values (x86_init) */
void x86_386_reset(X86Cpu *cpu);
void x86_386_interrupt(X86Cpu *cpu, uint8_t vector);

#endif
