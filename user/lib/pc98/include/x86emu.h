/*
 * x86emu: an 8086 / NEC V30 / 80286 (real mode) CPU interpreter, the heart of the future PC-98 emulator
 * (docs/PC98.md). Freestanding: it needs <stdint.h>, <stddef.h> and memset/memcpy, nothing else, so it links into an
 * OS application through libgap and builds on a host compiler for the tests in tests/host/.
 *
 * The CPU talks to the world only through callbacks (X86Bus): memory is byte-wide reads and writes of a 20-bit
 * (24-bit on the 286 with A20 open) physical address, I/O is 8-bit (16-bit optional) port reads and writes, and an
 * interrupt controller answers "is INTR asserted, and if so which vector?" when the CPU is ready to take it.
 *
 *      X86Cpu cpu;
 *      X86Bus bus = { .ctx = me, .read8 = my_read, .write8 = my_write, .in8 = my_in, .out8 = my_out };
 *      x86_init(&cpu, X86_MODEL_8086, &bus);
 *      x86_reset(&cpu);                      // CS:IP = FFFF:0000, like the real chip
 *      cpu.s[X86_CS] = 0x1FC0; cpu.ip = 0;   // ...or start wherever you loaded code
 *      while (!cpu.halted) x86_step(&cpu);   // one instruction (or one iteration of a REP string op) per call
 *
 * What is modelled (the long list, with the quirks, is in docs/PC98.md, "CPU core"):
 *  - every documented 8086 instruction, the undocumented ones programs rely on (SALC, POP CS, the 60-6F/C0/C1/C8/C9
 *    aliases, SETMO, 8C/8E with reg 4-7), exact flags including AF, OF and PF, and the 8086's documented answers for
 *    flags Intel calls "undefined" where the SingleStepTests vectors pin them down;
 *  - prefixes: segment overrides, LOCK (ignored), REP/REPE/REPNE (and the V30's REPC/REPNC); a REP string instruction
 *    runs one iteration per x86_step() and leaves IP on its first prefix until it is done, so interrupts are taken
 *    between iterations exactly where the hardware takes them;
 *  - interrupts: INT n, INT3, INTO, IRET, divide error, single step (TF), NMI, INTR through bus.intr_ack, the
 *    one-instruction interrupt shadow after MOV/POP to a segment register and after STI, HLT;
 *  - the 80186 additions on the V30 and 286 models (PUSHA, POPA, BOUND, PUSH imm, IMUL imm, INS/OUTS, ENTER, LEAVE,
 *    shifts by an immediate, the 5-bit shift count mask);
 *  - V30 extras: TEST1/CLR1/SET1/NOT1, ROL4/ROR4, REPC/REPNC, AAM/AAD ignore their immediate; 286 real mode: SMSW,
 *    LMSW (setting PE stops the CPU with X86_STOP_PROTECTED), the invalid opcode trap (INT 6), FLAGS bits 12-15 reading 0,
 *    PUSH SP pushing the old value, divide error pointing at the faulting instruction.
 *  - a cycle counter: approximate 8086 clock counts (data sheet table plus the effective-address cost), no prefetch
 *    queue, no wait states, no bus width penalty. Good enough to pace timers and VSYNC, not for cycle-exact tricks.
 *
 * Not modelled: protected mode, the 8087, the V30's 8080 emulation mode (BRKEM), its packed-BCD string instructions
 * (ADD4S, SUB4S, CMP4S) and bit-field instructions (INS, EXT), the 8086 bug that drops all but the last prefix when a
 * REP instruction is interrupted, and exact cycle timing.
 */
#ifndef GAP_X86EMU_H
#define GAP_X86EMU_H

#include <stddef.h>
#include <stdint.h>

/* General registers, in the order of the instruction encoding (r[] index). */
enum { X86_AX, X86_CX, X86_DX, X86_BX, X86_SP, X86_BP, X86_SI, X86_DI };
/* Segment registers, in the order of the instruction encoding (s[] index). */
enum { X86_ES, X86_CS, X86_SS, X86_DS };

/* FLAGS bits. */
#define X86_CF 0x0001u
#define X86_PF 0x0004u
#define X86_AF 0x0010u
#define X86_ZF 0x0040u
#define X86_SF 0x0080u
#define X86_TF 0x0100u
#define X86_IF 0x0200u
#define X86_DF 0x0400u
#define X86_OF 0x0800u

typedef enum {
    X86_MODEL_8086 = 0,     /* also the 8088: the instruction set is the same, the bus width only changes timing */
    X86_MODEL_V30 = 1,      /* NEC V30 (and V20): 80186 instruction set plus NEC extras, 8086-style FLAGS */
    X86_MODEL_80286 = 2,    /* 80286 in real mode */
} X86Model;

/* Why x86_step() stopped the CPU for good (cpu->stop). */
enum {
    X86_STOP_NONE = 0,
    X86_STOP_PROTECTED = 1, /* LMSW tried to set PE: protected mode is not emulated */
    X86_STOP_PREFIXES = 2,  /* an endless run of prefix bytes (a whole segment of them) */
    X86_STOP_UNIMPLEMENTED = 3, /* an instruction this core does not implement (cpu->bad_opcode says which) */
};

struct X86Cpu;

typedef struct X86Bus {
    void *ctx;                                              /* passed back to every callback */
    uint8_t (*read8)(void *ctx, uint32_t addr);             /* required */
    void (*write8)(void *ctx, uint32_t addr, uint8_t v);    /* required */
    uint8_t (*in8)(void *ctx, uint16_t port);               /* NULL: reads 0xFF */
    void (*out8)(void *ctx, uint16_t port, uint8_t v);      /* NULL: ignored */
    uint16_t (*in16)(void *ctx, uint16_t port);             /* NULL: in8(port) | in8(port + 1) << 8 */
    void (*out16)(void *ctx, uint16_t port, uint16_t v);    /* NULL: out8(port, lo), out8(port + 1, hi) */
    /* INTR: called before an instruction when IF is set and no interrupt shadow is active. Return the vector of the
     * interrupt being acknowledged (the controller marks it in service), or -1 when INTR is not asserted. */
    int (*intr_ack)(void *ctx);
    /* High-level emulation hook for software interrupts (INT n, INT3, INTO): return 1 when the host handled the call
     * itself (registers already updated), 0 to dispatch through the interrupt vector table as usual. */
    int (*soft_int)(void *ctx, struct X86Cpu *cpu, uint8_t vector);
    /* Optional tracing: called before every instruction with CS:IP pointing at it. */
    void (*trace)(void *ctx, struct X86Cpu *cpu);
} X86Bus;

typedef struct X86Cpu {
    uint16_t r[8];          /* AX CX DX BX SP BP SI DI */
    uint16_t s[4];          /* ES CS SS DS */
    uint16_t ip;
    uint16_t flags;
    uint16_t msw;           /* 286 machine status word (SMSW/LMSW); PE is never set */
    X86Model model;
    X86Bus bus;
    uint32_t addr_mask;     /* 0xFFFFF: 1 MiB wrap-around (8086, or a 286 with A20 closed); 0xFFFFFF: A20 open */
    uint64_t cycles;        /* approximate clock count since x86_init */
    uint64_t instructions;  /* instructions (and REP iterations) executed */
    uint8_t halted;         /* HLT executed, waiting for an interrupt */
    uint8_t shadow;         /* interrupts are held off until the next instruction completes */
    uint8_t nmi_pending;
    uint8_t in_rep;         /* a REP string instruction is part-way: IP points at its first prefix */
    uint8_t stop;           /* X86_STOP_*: the CPU refuses to run until reset */
    uint8_t bad_opcode;     /* the opcode behind X86_STOP_UNIMPLEMENTED */
    /* Decoder scratch, valid during one instruction. */
    uint16_t op_ip;         /* offset of the first byte (prefix included) of the current instruction */
    int8_t seg_ovr;         /* segment override prefix or -1 */
    uint8_t rep;            /* 0, or the REP prefix byte (F2, F3, and on the V30 64 or 65) */
    uint8_t mod, reg, rm;   /* ModR/M fields */
    uint8_t ea_seg;         /* segment of the memory operand */
    uint16_t ea;            /* offset of the memory operand */
} X86Cpu;

/* Sets up a CPU of the given model on the given bus (copied). Registers are zero; call x86_reset() for the
 * power-on state. */
void x86_init(X86Cpu *cpu, X86Model model, const X86Bus *bus);
/* The RESET pin: CS=FFFF IP=0000 (8086, V30) or CS=F000 IP=FFF0 (286, which really fetches from FFFFF0 the first time;
 * here it is just F000:FFF0), FLAGS cleared, DS=ES=SS=0, halt and stop states cleared. Keeps the cycle counter. */
void x86_reset(X86Cpu *cpu);
/* Executes one instruction, one iteration of a REP string instruction, or the entry into a pending interrupt.
 * Returns the cycles it took (at least 1, also while halted). */
int x86_step(X86Cpu *cpu);
/* Runs until at least `cycles` clocks have passed, the CPU halts, or it stops. Returns the clocks used. A halted CPU
 * with an interrupt it may take resumes; a halted CPU with nothing to wake it uses up the budget (idle time). */
uint64_t x86_run(X86Cpu *cpu, uint64_t cycles);
/* Raises the NMI line (edge): vector 2 is taken before the next instruction, even from HLT. */
void x86_nmi(X86Cpu *cpu);
/* Enters interrupt `vector` now, the way INT n does (pushes FLAGS, CS, IP; clears IF and TF). */
void x86_interrupt(X86Cpu *cpu, uint8_t vector);

/* Physical address of seg:off for this CPU (wrap-around per addr_mask). */
static inline uint32_t x86_linear(const X86Cpu *cpu, uint16_t seg, uint16_t off) {
    return (((uint32_t)seg << 4) + off) & cpu->addr_mask;
}

/* "8086", "V30", "80286". */
const char *x86_model_name(X86Model model);

#endif
