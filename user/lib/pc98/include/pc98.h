/*
 * pc98: a NEC PC-9801 machine around the x86emu CPU core ("Hakurei emulator", docs/PC98.md).
 *
 * What is here: the memory map (640 KiB of conventional RAM, the display memory of pc98gfx.h, an empty BIOS/ROM area), an
 * I/O port dispatcher, and the chips a program meets: two cascaded 8259 interrupt controllers, the 8253 interval timer,
 * the display hardware (pc98gfx.h: text VRAM and character generator, four graphics planes in two pages, palettes, the
 * GRCG and EGC, both uPD7220 controllers, a 640x400 renderer), the uPD8251 keyboard interface, the 8255 system port with
 * the beeper bit, the A20 gate. The FM sound board (opna.h) is the machine's too, once pc98_attach_opna() has been called.
 * Disk images come from pc98disk.h.
 *
 * What is NOT here, by design: any ROM. The repository never contains a BIOS, a font ROM or game data (docs/ASSETS.md);
 * the ROM area reads as FFh. There are two ways to start a program: pc98_boot() is the bare hand-over (the first
 * sector at 1FC0:0000 and nothing else: no vectors, no work area), and pc98bios.h is a high-level-emulation BIOS
 * whose interrupt vectors point at stubs in a ROM page of our own and are answered in C (X86Bus.soft_int); that one
 * boots real PC-98 disks. The application supplies the fonts (pc98gfx_set_font; the OS's 8x16 font is enough for ASCII).
 *
 * The structure is large (about 1 MiB with all the video memory): allocate it, do not put it on a stack.
 *
 *      Pc98 *m = malloc(sizeof *m);
 *      pc98_init(m, X86_MODEL_V30, 10000000);       // CPU model and clock
 *      pc98_boot(m, &disk, 0x90);                    // first sector to 1FC0:0000, AL = 90h (1 MB floppy drive 0)
 *      while (!pc98_dead(m)) pc98_run(m, 100000);    // a few thousand of these per second of machine time
 */
#ifndef GAP_PC98_H
#define GAP_PC98_H

#include <stddef.h>
#include <stdint.h>
#include <x86emu.h>
#include <pc98disk.h>
#include <pc98bios.h>
#include <pc98gfx.h>
#include <opna.h>

/* ---- memory map (physical addresses); the display memory belongs to pc98gfx.h */
#define PC98_RAM_SIZE       0xA0000u    /* 00000-9FFFF conventional RAM */
#define PC98_TVRAM          PC98GFX_TVRAM       /* A0000-A1FFF character codes, one 16-bit word per cell */
#define PC98_TVRAM_ATTR     PC98GFX_TVRAM_ATTR  /* A2000-A3FFF attributes, the even byte of each word */
#define PC98_TVRAM_END      PC98GFX_TVRAM_END
#define PC98_CGWIN          PC98GFX_CGWIN       /* A4000-A4FFF character generator window: not modelled, reads FFh */
#define PC98_GVRAM_B        PC98GFX_PLANE_B     /* A8000-AFFFF blue plane */
#define PC98_GVRAM_R        PC98GFX_PLANE_R     /* B0000-B7FFF red plane */
#define PC98_GVRAM_G        PC98GFX_PLANE_G     /* B8000-BFFFF green plane */
#define PC98_GVRAM_I        PC98GFX_PLANE_E     /* E0000-E7FFF intensity plane (16-colour mode) */
#define PC98_ROM            0xE8000u    /* E8000-FFFFF BIOS and BASIC ROM: left empty, reads FFh */
#define PC98_PLANE_SIZE     PC98GFX_PLANE_SIZE  /* 640 x 400 / 8 */
#define PC98_TEXT_COLS      PC98GFX_TEXT_COLS
#define PC98_TEXT_ROWS      PC98GFX_TEXT_ROWS

/* Boot hand-over: where the BIOS puts the first sector, and the register values we choose (docs/PC98.md). */
#define PC98_BOOT_SEG       0x1FC0u

/* Text attribute bits (the even byte at A2000 + 2 * cell). */
#define PC98_ATTR_VISIBLE   0x01        /* "ST": 0 hides the character (secret) */
#define PC98_ATTR_BLINK     0x02
#define PC98_ATTR_REVERSE   0x04
#define PC98_ATTR_UNDERLINE 0x08
#define PC98_ATTR_VLINE     0x10        /* vertical line, or semigraphics when mode flip-flop 1 says so */
#define PC98_ATTR_BLUE      0x20
#define PC98_ATTR_RED       0x40
#define PC98_ATTR_GREEN     0x80

/* ---- 8259A programmable interrupt controller */
typedef struct {
    uint8_t irr, isr, imr;
    uint8_t lines;          /* current level of the IR inputs (edge detection) */
    uint8_t icw1, icw2, icw3, icw4;
    uint8_t init_step;      /* 0 ready, 2/3/4: waiting for that ICW */
    uint8_t read_isr;       /* OCW3: a read of the even port returns ISR (1) or IRR (0) */
    uint8_t special_mask;   /* OCW3 special mask mode */
} Pc98Pic;

/* ---- 8253 programmable interval timer, one counter */
typedef struct {
    uint8_t mode, rw, bcd;
    uint8_t write_lsb_next; /* rw=3: the next write is the low byte */
    uint8_t read_lsb_next;
    uint8_t latched;        /* a latch command froze `latch` */
    uint8_t pending_lsb;
    uint8_t armed;          /* mode 0/4: the terminal count has not been reached since the last load */
    uint8_t loaded;         /* a count has been written since the mode was set */
    uint8_t out;
    uint16_t latch;
    uint32_t reload;        /* 1..65536 */
    uint32_t count;         /* 1..65536 */
    uint64_t edges;         /* rising edges of OUT so far (what IRQ 0 and the beeper see) */
} Pc98PitCounter;

/* ---- uPD8251A keyboard interface */
typedef struct {
    uint8_t mode_next;      /* the next write to 43h is a mode word (after reset or an internal reset) */
    uint8_t mode, command;
    uint8_t data;
    uint8_t rxrdy;
    uint8_t queue[32];
    uint8_t qhead, qlen;
    uint8_t overrun;
    uint32_t to_keyboard;   /* bytes the program sent to the keyboard (41h writes) */
} Pc98Kbd;

typedef struct Pc98 {
    X86Cpu cpu;
    uint32_t cpu_hz;
    uint32_t pit_hz;        /* 2457600 (5/10 MHz machines) or 1996800 (8 MHz machines) */
    uint64_t pit_frac;      /* PIT clock remainder, in CPU cycles * pit_hz */
    uint64_t frame_pos;     /* CPU cycles into the current video frame */
    uint32_t frame_cycles;  /* one frame: cpu_hz / 56.42 Hz */
    uint32_t vsync_cycles;  /* the VSYNC part of it */
    uint32_t line_cycles;   /* one scan line (440 per frame) and its blanked part: the HBLANK bit of the GDC status */
    uint32_t hblank_cycles;
    uint64_t dev_time;      /* CPU cycles the devices have been told about (pc98_advance)... */
    uint64_t cpu_mark;      /* ...and the CPU's own cycle count at that moment: dev_time + (cpu.cycles - cpu_mark) is "now" */
    uint64_t frames;
    uint8_t vsync;          /* GDC status bit 5 */
    uint8_t vsync_irq_armed;/* a write to port 64h arms one CRTV interrupt (IRQ 2) */

    uint8_t ram[PC98_RAM_SIZE];
    Pc98Gfx gfx;            /* text VRAM, the planes, palettes, GRCG, EGC, both GDCs, the character generator */

    Pc98Pic pic[2];         /* master (ports 00h/02h), slave (08h/0Ah) on master IR7 */
    Pc98PitCounter pit[3];  /* 71h, 73h (also 3FDBh), 75h; control 77h (also 3FDFh) */
    Pc98Kbd kbd;            /* 41h/43h */
    uint8_t sys_a, sys_b, sys_c;   /* 8255 system port (31h, 33h, 35h; control 37h) */
    uint8_t nmi_enabled;    /* 50h / 52h */
    uint8_t a20;            /* F2h / F6h */
    uint32_t cpu_resets;    /* writes to F0h (the 286 way back from protected mode): counted, not carried out */

    /* Beeper (8255 port C bit 3, active low, tone from PIT counter 1). */
    uint8_t beep_on;
    uint32_t beep_changes;

    /* The FM sound board (pc98_attach_opna): NULL when the machine has none (its ports then read FFh). The board is
     * brought up to the CPU's clock before every access to its ports and whenever the devices advance. */
    Opna *fm;
    uint64_t fm_time;           /* machine time (dev_time units) the board has been advanced to */

    /* Ports nobody answers: reads give FFh, writes are dropped; the last ones are kept for debugging. */
    uint32_t unhandled;
    uint16_t last_unhandled_port;

    /* Optional host hooks for ports the skeleton does not know (return -1 from in to say "not mine"). */
    void *host;
    int (*port_in)(void *host, uint16_t port);
    int (*port_out)(void *host, uint16_t port, uint8_t v);
    /* Optional event hook for tracing tools (pc98run): kind is a PC98_EV_* code, a and b depend on it. The CPU state
     * (m->cpu) is the one at the event. NULL: no events, no cost beyond a test. */
    void (*event)(void *host, int kind, uint32_t a, uint32_t b);
    uint8_t yield;              /* set by a hook: pc98_run() returns at the next slice boundary (and clears it) */
    uint32_t watch_lo, watch_hi;    /* a write watch for tools: every byte written into [watch_lo, watch_hi) is reported
                                 * (PC98_EV_MEM_WRITE; a word is two reports, low byte first); watch_hi = 0: off */
    uint32_t watch_plane_lo, watch_plane_hi;    /* the same for an offset range inside the graphics planes (a write to the
                                 * byte at that offset in any of the four, which is what the GRCG changes in all of them) */
    uint8_t watch_work_area;    /* report the first read of each byte of 0000:0400-05FF (PC98_EV_WORK_READ) */
    uint8_t work_seen[64];      /* bitmap of the bytes already reported */

    /* The high-level-emulation BIOS (pc98bios.h): its state and its ROM page at FD800-FFFFF (mapped once
     * pc98_bios_init() ran; until then the whole ROM area reads FFh). */
    Pc98Bios bios;
    uint8_t rom[PC98_BIOS_ROM_SIZE];
} Pc98;

/* Event kinds for Pc98.event. */
enum {
    PC98_EV_IN = 1,             /* a = port, b = value read (every port read) */
    PC98_EV_OUT,                /* a = port, b = value written */
    PC98_EV_UNHANDLED_IN,       /* a = port: nobody answers, FFh read */
    PC98_EV_UNHANDLED_OUT,      /* a = port, b = value: nobody answers, dropped */
    PC98_EV_SOFT_INT,           /* a = vector: guest code executed INT n (the instruction is at CS:cpu.op_ip) */
    PC98_EV_HW_INT,             /* a = vector: an interrupt request is being taken (CS:IP = where it interrupts) */
    PC98_EV_BIOS_CALL,          /* a = vector: the HLE BIOS is about to answer a call (registers as passed) */
    PC98_EV_BIOS_DONE,          /* a = vector, b = 1 if it is waiting (the call will be repeated): registers as returned */
    PC98_EV_BIOS_UNKNOWN,       /* a = vector, b = AH: a function the HLE BIOS does not implement (it returned CF=1) */
    PC98_EV_WORK_READ,          /* a = address, b = value: the first read of a BIOS work-area byte (watch_work_area) */
    PC98_EV_NOTE,               /* a = PC98_NOTE_* (pc98bios.h), b = detail */
    PC98_EV_MEM_WRITE,          /* a = address, b = byte: a write inside [watch_lo, watch_hi) (the CPU state is the writer's) */
};

/* Sets the machine up in its power-on state: RAM and VRAM cleared, both PICs masked, the timer stopped, the beeper off.
 * cpu_hz is the CPU clock (the PIT clock follows from it: 1.9968 MHz for multiples of 8 MHz, else 2.4576 MHz). */
void pc98_init(Pc98 *m, X86Model model, uint32_t cpu_hz);
/* Hands over to a boot program the way the BIOS does: loads sector 0 of `disk` (cylinder 0, head 0, sector 1) to
 * 1FC0:0000 and starts it with AL = da_ua (the device: 90h first 1 MB floppy, 80h first hard disk), CS = DS = ES = 1FC0,
 * SS:SP = 1FC0:0000, interrupts off. Returns 0, or -1 when the disk has no readable first sector. */
int pc98_boot(Pc98 *m, const Pc98Disk *disk, uint8_t da_ua);
/* Runs the machine for about `cycles` CPU clocks (devices advance in small slices). Returns the clocks used. */
uint64_t pc98_run(Pc98 *m, uint64_t cycles);
/* True when the CPU can never run again: stopped, or halted with interrupts off and no NMI. */
int pc98_dead(const Pc98 *m);

/* Memory and ports as the CPU sees them (also for tests and debuggers). The word accesses exist for the EGC, which
 * treats a 16-bit access at an even address in its graphics memory as one operation; elsewhere they are two bytes. */
uint8_t pc98_read8(Pc98 *m, uint32_t addr);
void pc98_write8(Pc98 *m, uint32_t addr, uint8_t v);
uint16_t pc98_read16(Pc98 *m, uint32_t addr);
void pc98_write16(Pc98 *m, uint32_t addr, uint16_t v);
uint8_t pc98_in8(Pc98 *m, uint16_t port);
void pc98_out8(Pc98 *m, uint16_t port, uint8_t v);

/* Puts an FM sound board on the machine: the PC-9801-86 (OPNA_BOARD_86, a YM2608) or the -26K (a YM2203) from opna.h on
 * ports 188h-18Eh (and A460h), its interrupt on the jumper's IRQ (12 is the factory setting), clocked from the CPU. `fm`
 * is the caller's (about 280 KiB: static or allocated); pass NULL to remove the board. Audio comes out of it with
 * opna_take(m->fm, ...) once the machine has run; the machine keeps synthesis on (set fm->synth = 0 beforehand for a
 * silent machine that only needs the timers). */
void pc98_attach_opna(Pc98 *m, Opna *fm, int board, int irq);

/* Devices, for the host side of the machine. */
void pc98_irq(Pc98 *m, int irq, int level);             /* 0-7 master, 8-15 slave */
int pc98_intr_ack(Pc98 *m);                             /* the INTA cycle: vector or -1 */
void pc98_key(Pc98 *m, uint8_t scancode, int down);     /* a PC-98 key code; release sends code | 80h */
void pc98_advance(Pc98 *m, uint64_t cpu_cycles);         /* let the devices see time pass */
uint32_t pc98_beep_hz(const Pc98 *m);                   /* tone of the beeper (0 when PIT counter 1 is unset) */

/* The text screen as plain text: 25 lines of 80 characters plus '\n' (2025 bytes and a NUL; buf needs 2026).
 * ASCII codes are copied, cells hidden by their attribute and control codes become spaces, anything else (kanji,
 * kana, graphics characters) becomes '?'. (pc98gfx_text_dump; it follows the text display's start address.) */
void pc98_text_dump(const Pc98 *m, char *buf);
/* Draws the displayed picture (the graphics page under the text layer, with the fonts given to pc98gfx_set_font(&m->gfx,
 * ...); without any the text shows only its attributes) into a 640x400 0x00RRGGBB buffer with `stride` pixels per row. */
void pc98_render(const Pc98 *m, uint32_t *rgb, int stride);

/* The device models, exposed for unit tests. */
void pc98_pic_reset(Pc98Pic *p);
void pc98_pic_write(Pc98Pic *p, int a0, uint8_t v);
uint8_t pc98_pic_read(Pc98Pic *p, int a0);
void pc98_pic_set_line(Pc98Pic *p, int ir, int level);
int pc98_pic_pending(const Pc98Pic *p);                 /* the IR that would be acknowledged now, or -1 */
int pc98_pic_ack(Pc98Pic *p);                           /* acknowledges it: IRR -> ISR, returns the IR or -1 */

void pc98_pit_control(Pc98PitCounter pit[3], uint8_t v);
void pc98_pit_write(Pc98PitCounter *c, uint8_t v);
uint8_t pc98_pit_read(Pc98PitCounter *c);
void pc98_pit_advance(Pc98PitCounter *c, uint64_t ticks);

#endif
