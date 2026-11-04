/*
 * idle: the idle skip. The games wait for the next frame in a busy loop (the three instructions
 * `mov ax,[counter]; cmp ax,[bp+6]; jc again` that wait for the VSYNC interrupt's handler to count up, in TH01 and TH04 alike): in a
 * gameplay frame those loops are about 63 % of the machine's clocks, and an emulator that executes them one by one spends most of
 * the host's time on nothing. This module finds such a loop and moves the machine's clock forward to the next thing that can
 * change what the loop sees, without executing the iterations in between.
 *
 * It is exact in the sense that matters: the guest cannot tell. What it relies on, and checks every time before it skips:
 *   1. a probe: the machine runs a short stretch (one or two 256-clock slices) with the CPU's bus wrapped, which records the
 *      architectural state before every instruction and every effect that leaves the CPU: RAM writes, port accesses, software
 *      interrupts, reads of display memory, interrupts taken;
 *   2. a recurrence: the state at the end of the stretch (all registers, flags, IP) equals the state before some earlier
 *      instruction of it, and from there to the end nothing happened that could change the machine except the CPU running the
 *      same instructions again: every RAM write nets to zero (a push of the same value to the same place), no port is written,
 *      the only ports read are the keyboard status and the display controller's status byte, no interrupt was taken, no INT
 *      instruction ran, display memory was not read. The loop is then a fixed cycle of C clocks and n instructions;
 *   3. a horizon: the clocks until anything outside the CPU can change: the next frame boundary (the VSYNC interrupt), the
 *      timer's next output edge (a lower bound from its count), the FM board's next interrupt (opna_ticks_to_irq), a key waiting to be
 *      delivered, and, when the loop reads the display status, the next VSYNC or HBLANK edge. Keys and the other things the host
 *      does happen between the stretches the caller asks for, never inside one.
 * The skip is a whole number of iterations (a multiple of C clocks, short of the horizon by a margin): the CPU is left in the
 * very state it would be in had it run, the instruction and clock counters add up, and the devices are advanced by exactly that
 * many clocks with pc98_advance(), the call the machine makes itself between slices. Everything the guest does after that
 * happens as without the skip, up to the 256-clock slice grid its interrupts already live on.
 *
 * What it does not do: skip a loop that counts (`loop $` delays: the registers change), writes the display (drawing), calls the BIOS
 * or touches any port but the two named, and it never skips across anything the host has to look at: the caller passes
 * how much machine time it may move (`room`) and gets back how much did pass.
 *
 * Only the public fields of Pc98 are used (and its bus function pointers, replaced for the length of a probe); nothing in
 * libpc98 needed to change. Freestanding like the library; the host tests compile it as it is.
 */
#ifndef PC98APP_IDLE_H
#define PC98APP_IDLE_H

#include <pc98.h>

#define IDLE_MAX_RECORDS 1024
#define IDLE_MAX_WRITES 256
#define IDLE_MAX_PORTS 64

/* The architectural state of the CPU at an instruction boundary, packed so that two of them compare with memcmp. */
typedef struct {
    uint16_t r[8], rh[8], s[6], ip, flags, flags_hi;
    uint32_t cr0;
    uint8_t halted, in_rep, shadow, nmi;
} IdleState;

typedef struct {
    IdleState st;
    uint64_t cycles, instructions;
    uint16_t nw, np, ni, na, nh;        /* counts of the events logged before this instruction: writes, port accesses, INTs, interrupt acks, display reads */
} IdleRecord;

typedef struct {
    X86Bus orig;                        /* the machine's own bus, put back after the probe */
    Pc98 *m;
    int active;
    int overflow;                       /* a log filled up: the probe says nothing */
    int bad_write;                      /* a write outside RAM */
    uint16_t nrec;
    IdleRecord rec[IDLE_MAX_RECORDS];
    uint16_t nw;
    struct { uint32_t addr; uint8_t old; } wlog[IDLE_MAX_WRITES];
    uint16_t np;
    struct { uint16_t port; uint8_t out; } plog[IDLE_MAX_PORTS];
    uint16_t ni, na, nh;
} IdleProbe;

typedef struct {
    int enabled;
    /* statistics for the status line, the benchmark and the tests */
    uint64_t skipped_clocks, skipped_instrs, skips, probes, hits;     /* clocks and instructions that were not executed (they are counted in the CPU's totals all the same) */
    uint64_t near_event, room_limited, too_short;     /* pure loops found but not skipped (or cut short): an event was too near; the caller's time ran out; less than two turns fit */
    /* what decides when to probe */
    uint32_t last_lin;                  /* CS:IP (linear) at the end of the previous stretch */
    int still;                          /* stretches in a row that ended near each other */
    int cooldown, backoff;              /* stretches to run plainly after a probe that found nothing, and how long the next wait is */
    uint64_t last_period;               /* clocks per turn of the last loop that was found (0 = none): how long the next probe needs to be */
    uint32_t loop_lo, loop_hi;          /* the linear addresses of the last loop that was skipped (0 = none): a stretch that ends in it is probed at once */
    IdleProbe probe;                    /* about 90 KiB: the structure lives where the caller put it (not on a stack) */
} IdleSkip;

void idle_init(IdleSkip *s, int enabled);
/* Runs the machine for about `chunk` clocks (the executed part may overshoot by the last instruction, as pc98_run does), and
 * when the stretch ended in a pure loop moves the clock on by whole iterations, but never more than `room` clocks of machine time
 * pass in all (`chunk` is cut to `room`). Returns the clocks that passed, executed and skipped together. With the skip disabled
 * this is pc98_run(m, chunk). */
uint64_t idle_run(IdleSkip *s, Pc98 *m, uint64_t chunk, uint64_t room);
/* The horizon (lower bound, in machine clocks) from the machine's state as it is now, for a loop that reads the display status
 * when `status` is set. Exposed for the tests. */
uint64_t idle_horizon(const Pc98 *m, int status);

#endif
