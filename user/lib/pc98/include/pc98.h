/*
 * pc98: a NEC PC-9801 machine skeleton around the x86emu CPU core ("Hakurei emulator" groundwork, docs/PC98.md).
 *
 * What is here: the memory map (640 KiB of conventional RAM, text VRAM with its attribute plane, two pages of four
 * graphics planes, an empty BIOS/ROM area), an I/O port dispatcher, and the chips a boot sector meets first: two
 * cascaded 8259 interrupt controllers, the 8253 interval timer, the two uPD7220 display controllers (status, VSYNC,
 * cursor and parameter bookkeeping; no drawing), the uPD8251 keyboard interface, the 8255 system port with the beeper
 * bit, the palette and display/draw page registers, the A20 gate. Disk images come from pc98disk.h.
 *
 * What is NOT here, by design: any ROM. The repository never contains a BIOS, a font ROM or game data (docs/ASSETS.md);
 * the ROM area reads as FFh and the machine starts a program the way the BIOS would hand over to a boot sector
 * (pc98_boot()). A future OS application supplies its own font (the OS's 8x16 font is enough for ASCII) and, if it
 * ever wants BIOS services, high-level emulation through X86Bus.soft_int.
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

/* ---- memory map (physical addresses) */
#define PC98_RAM_SIZE       0xA0000u    /* 00000-9FFFF conventional RAM */
#define PC98_TVRAM          0xA0000u    /* A0000-A1FFF character codes, one 16-bit word per cell */
#define PC98_TVRAM_ATTR     0xA2000u    /* A2000-A3FFF attributes, the even byte of each word */
#define PC98_TVRAM_END      0xA4000u
#define PC98_CGWIN          0xA4000u    /* A4000-A4FFF character generator window: no font ROM, reads FFh */
#define PC98_GVRAM_B        0xA8000u    /* A8000-AFFFF blue plane */
#define PC98_GVRAM_R        0xB0000u    /* B0000-B7FFF red plane */
#define PC98_GVRAM_G        0xB8000u    /* B8000-BFFFF green plane */
#define PC98_GVRAM_I        0xE0000u    /* E0000-E7FFF intensity plane (16-colour mode) */
#define PC98_ROM            0xE8000u    /* E8000-FFFFF BIOS and BASIC ROM: left empty, reads FFh */
#define PC98_PLANE_SIZE     0x8000u     /* 640 x 400 / 8 */
#define PC98_TEXT_COLS      80
#define PC98_TEXT_ROWS      25

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

/* ---- uPD7220 graphic display controller: the bookkeeping part */
typedef struct {
    uint8_t cmd;            /* the command the following parameters belong to */
    uint8_t nparam;
    uint8_t params[16];
    uint8_t pram[16];       /* parameter RAM: scroll areas (start address and line count), pattern */
    uint8_t display_on;
    uint8_t sync[8];        /* last SYNC parameters */
    uint8_t pitch, zoom;
    uint8_t csrform[3];
    uint8_t mask[2];
    uint32_t ead;           /* cursor / execute address (CSRW) */
    uint8_t fifo[16];       /* read FIFO (CSRR answers) */
    uint8_t fifo_len, fifo_pos;
    uint32_t commands;      /* commands received */
    uint32_t ignored;       /* drawing and read commands that are accepted but not carried out */
} Pc98Gdc;

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
    uint64_t frames;
    uint8_t vsync;          /* GDC status bit 5 */
    uint8_t vsync_irq_armed;/* a write to port 64h arms one CRTV interrupt (IRQ 2) */

    uint8_t ram[PC98_RAM_SIZE];
    uint8_t tvram[PC98_TVRAM_END - PC98_TVRAM];
    uint8_t gvram[2][4][PC98_PLANE_SIZE];   /* [page][B, R, G, I] */

    Pc98Pic pic[2];         /* master (ports 00h/02h), slave (08h/0Ah) on master IR7 */
    Pc98PitCounter pit[3];  /* 71h, 73h (also 3FDBh), 75h; control 77h (also 3FDFh) */
    Pc98Gdc gdc[2];         /* text (60h/62h), graphics (A0h/A2h) */
    Pc98Kbd kbd;            /* 41h/43h */
    uint8_t sys_a, sys_b, sys_c;   /* 8255 system port (31h, 33h, 35h; control 37h) */
    uint8_t nmi_enabled;    /* 50h / 52h */
    uint8_t mode_ff1;       /* mode flip-flop 1 (68h): bit n = flip-flop n */
    uint8_t mode_ff2_analog;/* mode flip-flop 2 (6Ah): 16-colour analog palette */
    uint8_t border;         /* 6Ch */
    uint8_t disp_page, draw_page;  /* A4h, A6h */
    uint8_t pal_digital[4]; /* A8h AAh ACh AEh in digital mode: two GRB nibbles each */
    uint8_t pal_index;      /* analog mode: A8h */
    uint8_t pal_analog[16][3];  /* G, R, B, 4 bits each */
    uint8_t a20;            /* F2h / F6h */
    uint32_t cpu_resets;    /* writes to F0h (the 286 way back from protected mode): counted, not carried out */

    /* Beeper (8255 port C bit 3, active low, tone from PIT counter 1). */
    uint8_t beep_on;
    uint32_t beep_changes;

    /* Ports nobody answers: reads give FFh, writes are dropped; the last ones are kept for debugging. */
    uint32_t unhandled;
    uint16_t last_unhandled_port;

    /* Optional host hooks for ports the skeleton does not know (return -1 from in to say "not mine"). */
    void *host;
    int (*port_in)(void *host, uint16_t port);
    int (*port_out)(void *host, uint16_t port, uint8_t v);
} Pc98;

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

/* Memory and ports as the CPU sees them (also for tests and debuggers). */
uint8_t pc98_read8(Pc98 *m, uint32_t addr);
void pc98_write8(Pc98 *m, uint32_t addr, uint8_t v);
uint8_t pc98_in8(Pc98 *m, uint16_t port);
void pc98_out8(Pc98 *m, uint16_t port, uint8_t v);

/* Devices, for the host side of the machine. */
void pc98_irq(Pc98 *m, int irq, int level);             /* 0-7 master, 8-15 slave */
int pc98_intr_ack(Pc98 *m);                             /* the INTA cycle: vector or -1 */
void pc98_key(Pc98 *m, uint8_t scancode, int down);     /* a PC-98 key code; release sends code | 80h */
void pc98_advance(Pc98 *m, uint64_t cpu_cycles);         /* let the devices see time pass */
uint32_t pc98_beep_hz(const Pc98 *m);                   /* tone of the beeper (0 when PIT counter 1 is unset) */

/* The text screen as plain text: 25 lines of 80 characters plus '\n' (2025 bytes and a NUL; buf needs 2026).
 * ASCII codes are copied, cells hidden by their attribute and control codes become spaces, anything else (kanji,
 * kana, graphics characters) becomes '?'. */
void pc98_text_dump(const Pc98 *m, char *buf);
/* Draws the displayed graphics page (and the text layer over it when font8x16 is given: 256 glyphs of 16 bytes, MSB is
 * the left pixel) into a 640x400 0x00RRGGBB buffer with `stride` pixels per row. */
void pc98_render(const Pc98 *m, uint32_t *rgb, int stride, const uint8_t *font8x16);

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

void pc98_gdc_reset(Pc98Gdc *g);
void pc98_gdc_command(Pc98Gdc *g, uint8_t cmd);
void pc98_gdc_param(Pc98Gdc *g, uint8_t v);
uint8_t pc98_gdc_read_data(Pc98Gdc *g);
uint8_t pc98_gdc_status(const Pc98Gdc *g, int vsync);

#endif
