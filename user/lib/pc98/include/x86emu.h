/*
 * x86emu: an 8086 / NEC V30 / 80286 / 80386 / 80486 (real mode) CPU interpreter, the heart of the future PC-98 emulator
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
 *  - 80386 and 80486 real mode (src/x86cpu32.c, a separate interpreter, so the older models are untouched): operand- and
 *    address-size prefixes, 32-bit registers (the upper halves live in rh[] and survive 16-bit operations), 32-bit
 *    addressing with SIB bytes, FS and GS, the 0F page (Jcc near, SETcc, MOVZX/MOVSX, SHLD/SHRD, BT*, BSF/BSR, IMUL,
 *    LSS/LFS/LGS), the real-mode faults (segment limits of FFFFh: a word at FFFFh raises #GP or #SS instead of wrapping;
 *    invalid opcodes and LOCK misuse raise #UD; the 15-byte instruction limit), control, debug and test registers,
 *    LGDT/LIDT/SGDT/SIDT (real-mode interrupts use the IDTR), CPU shutdown; the 486 adds BSWAP, XADD, CMPXCHG, INVD,
 *    WBINVD, INVLPG and the AC flag. No CPUID (the ID flag stays clear) and no FPU on either.
 *
 * Not modelled: protected mode (setting PE stops the CPU), the 8087/80387, the V30's 8080 emulation mode (BRKEM), its
 * packed-BCD string instructions (ADD4S, SUB4S, CMP4S) and bit-field instructions (INS, EXT), the 8086 bug that drops all
 * but the last prefix when a REP instruction is interrupted, prefetch queues, debug breakpoints, and exact cycle timing.
 */
#ifndef GAP_X86EMU_H
#define GAP_X86EMU_H

#include <stddef.h>
#include <stdint.h>

/* General registers, in the order of the instruction encoding (r[] index). */
enum { X86_AX, X86_CX, X86_DX, X86_BX, X86_SP, X86_BP, X86_SI, X86_DI };
/* Segment registers, in the order of the instruction encoding (s[] index); FS and GS exist on the 386 and 486 only. */
enum { X86_ES, X86_CS, X86_SS, X86_DS, X86_FS, X86_GS };

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
#define X86_IOPL 0x3000u        /* 386/486: I/O privilege level, writable in real mode (always 0 on the 286) */
#define X86_NT 0x4000u          /* 386/486: nested task, writable in real mode */
#define X86_EFLAGS_AC 0x40000u  /* 486: alignment check, EFLAGS bit 18 (flags_hi bit 2); the 386 cannot set it */

/* CR0 bits (386/486; the low four are the 286's MSW). */
#define X86_CR0_PE 0x00000001u
#define X86_CR0_MP 0x00000002u
#define X86_CR0_EM 0x00000004u
#define X86_CR0_TS 0x00000008u
#define X86_CR0_ET 0x00000010u
#define X86_CR0_NE 0x00000020u  /* 486 */
#define X86_CR0_WP 0x00010000u  /* 486 */
#define X86_CR0_AM 0x00040000u  /* 486 */
#define X86_CR0_NW 0x20000000u  /* 486 */
#define X86_CR0_CD 0x40000000u  /* 486 */
#define X86_CR0_PG 0x80000000u

typedef enum {
    X86_MODEL_8086 = 0,     /* also the 8088: the instruction set is the same, the bus width only changes timing */
    X86_MODEL_V30 = 1,      /* NEC V30 (and V20): 80186 instruction set plus NEC extras, 8086-style FLAGS */
    X86_MODEL_80286 = 2,    /* 80286 in real mode */
    X86_MODEL_80386 = 3,    /* 80386 in real mode: the 32-bit instruction set, FS/GS, the 0F page; no 387 */
    X86_MODEL_80486 = 4,    /* 80486 in real mode (as a 486SX: no FPU): the 386 plus BSWAP/XADD/CMPXCHG and the AC flag */
} X86Model;

/* Why x86_step() stopped the CPU for good (cpu->stop). */
enum {
    X86_STOP_NONE = 0,
    X86_STOP_PROTECTED = 1, /* LMSW (or, on the 386/486, MOV CR0) tried to set PE: protected mode is not emulated */
    X86_STOP_PREFIXES = 2,  /* an endless run of prefix bytes (a whole segment of them) */
    X86_STOP_UNIMPLEMENTED = 3, /* an instruction this core does not implement (cpu->bad_opcode says which) */
    X86_STOP_SHUTDOWN = 4,  /* 386/486: a fault while entering a fault handler (an odd SP below 6, an IDT limit too small
                             * even for the double fault): the processor shuts down until reset, like the real chip */
};

struct X86Cpu;

typedef struct X86Bus {
    void *ctx;                                              /* passed back to every callback */
    uint8_t (*read8)(void *ctx, uint32_t addr);             /* required */
    void (*write8)(void *ctx, uint32_t addr, uint8_t v);    /* required */
    /* Optional word access for operands the CPU reads or writes as 16 bits (never across the end of the address space):
     * a device that treats a word differently from two bytes (the PC-98's EGC) needs them. NULL: two read8/write8, low
     * byte first. A 32-bit operand of the 386/486 models is two word accesses, low word first. */
    uint16_t (*read16)(void *ctx, uint32_t addr);
    void (*write16)(void *ctx, uint32_t addr, uint16_t v);
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
    uint16_t r[8];          /* AX CX DX BX SP BP SI DI (on the 386/486 the low halves of EAX..EDI) */
    uint16_t s[6];          /* ES CS SS DS, then FS GS (386/486) */
    uint16_t ip;
    uint16_t flags;         /* FLAGS (on the 386/486 the low half of EFLAGS) */
    uint16_t msw;           /* 286 machine status word (SMSW/LMSW); PE is never set. The 386/486 use cr0 instead */
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

    /* ---- 80386 / 80486 only (zero, and ignored, on the older models) */
    uint16_t rh[8];         /* the upper halves of EAX..EDI: EAX = r[0] | rh[0] << 16 (see x86_reg32()) */
    uint16_t flags_hi;      /* EFLAGS bits 16-31: only AC (bit 18) on the 486; RF, VM and ID stay 0 in real mode */
    uint32_t cr0, cr2, cr3; /* control registers as stored (reads add the bits the chip forces, see x86cpu32.c) */
    uint32_t dr[8];         /* debug registers DR0-DR7: stored, breakpoints are not modelled */
    uint32_t tr[8];         /* test registers TR3-TR7 (only TR6 and TR7 on the 386) */
    uint32_t gdt_base, idt_base; /* GDTR and IDTR; real-mode interrupts read their vectors through the IDTR */
    uint16_t gdt_limit, idt_limit;
    uint8_t bad_opcode2;    /* with bad_opcode 0Fh: the second opcode byte */
    /* Decoder scratch of the 386/486 interpreter, valid during one instruction. */
    uint8_t osz, asz;       /* operand size and address size in bytes (2 or 4) */
    uint8_t lock;           /* a LOCK prefix was seen */
    uint8_t fault;          /* the exception the instruction raised, as vector + 1; 0 = none */
    uint8_t ilen;           /* bytes fetched so far (more than 15 is a #GP) */
    uint8_t fetch_wrap;     /* the fetch went past offset FFFFh, the CS limit */
    uint8_t ea_esp;         /* the memory operand's base register was ESP (POP r/m adjusts) */
    uint8_t keep;           /* general registers (bit i = register i) a fault does not roll back (POPA part-way) */
    uint32_t ea32;          /* offset of the memory operand (32-bit addressing reaches past FFFFh, which faults) */
    /* A REP string instruction part-way (in_rep): the 386 runs the next iteration from what it decoded, not from memory
     * (a REP MOVS that overwrites its own bytes carries on). */
    uint8_t rep_op, rep_prefix, rep_osz, rep_asz, rep_wrap;
    int8_t rep_seg;
    uint16_t rep_ip, rep_next;  /* where the instruction starts, and the IP after it */
} X86Cpu;

/* Sets up a CPU of the given model on the given bus (copied). Registers are zero; call x86_reset() for the
 * power-on state. */
void x86_init(X86Cpu *cpu, X86Model model, const X86Bus *bus);
/* The RESET pin: CS=FFFF IP=0000 (8086, V30) or CS=F000 IP=FFF0 (286, 386, 486, which really fetch from the top of their
 * address space the first time; here it is just F000:FFF0), FLAGS cleared, DS=ES=SS=0, halt and stop states cleared.
 * The 386/486 also reset CR0, the IDTR (base 0, limit 3FFh), DR6/DR7, and put their component ID in DX. Keeps the cycle
 * counter. */
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

/* 386/486: a 32-bit general register (X86_AX..X86_DI) and its setter, which also updates the 16-bit view r[]. */
static inline uint32_t x86_reg32(const X86Cpu *cpu, int i) { return cpu->r[i] | (uint32_t)cpu->rh[i] << 16; }
static inline void x86_set_reg32(X86Cpu *cpu, int i, uint32_t v) {
    cpu->r[i] = (uint16_t)v;
    cpu->rh[i] = (uint16_t)(v >> 16);
}
/* 386/486: all of EFLAGS. */
static inline uint32_t x86_eflags(const X86Cpu *cpu) { return cpu->flags | (uint32_t)cpu->flags_hi << 16; }

/* "8086", "V30", "80286", "80386", "80486". */
const char *x86_model_name(X86Model model);

#endif
