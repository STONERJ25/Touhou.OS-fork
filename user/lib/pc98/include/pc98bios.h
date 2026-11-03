/*
 * pc98bios: a high-level-emulation ("HLE") BIOS for the pc98 machine (docs/PC98_BIOS.md).
 *
 * There is no BIOS ROM in this project and there never will be one (docs/ASSETS.md). Instead the interrupt vectors point
 * at tiny stubs in a ROM page of our own (FD800-FFFFF, segment FD80h, the place the real BIOS lives): each stub is an
 * `INT n` instruction followed by `IRET`. The machine's soft_int hook sees an INT executed *inside that page* and hands
 * the call to C (pc98_bios_soft_int), which reads and writes the CPU registers, the caller's FLAGS image on the stack
 * and guest memory, and returns; the stub's IRET then goes back to the caller. Because the stub is reached through the
 * vector, the BIOS answers every way a program can get there: `INT 1Bh`, `PUSHF; CALL FAR [saved vector]` from a
 * driver that hooked the vector, or a hardware interrupt. A call that has to wait (a key read with an empty buffer)
 * points IP back at the stub's INT, sets IF and halts the CPU: the next interrupt (the keyboard) runs, and its IRET
 * re-executes the INT, which asks again.
 *
 * What the BIOS provides is what the owner's real boot chain was seen to need (the trace-driven list is in
 * docs/PC98_BIOS.md): the BIOS work area at 0000:0400-05FF, the memory switches, INT 1Bh (disk) for hard disks
 * (SASI-style, 80h-83h, and the same units with relative sector numbers, 00h-03h) and 1 MB floppies (90h-93h),
 * INT 18h (keyboard and CRT), INT 1Ch (calendar and interval timer), INT 09h (keyboard interrupt), default handlers
 * for the other IRQs, and the boot itself: the first sector of the boot device at 1FC0:0000, entered by a far call with
 * DS = 0, the device's DA/UA in 0000:0584. A call the BIOS does not know returns an error status (CF set) and is reported
 * through the machine's event hook (PC98_EV_BIOS_UNKNOWN): the trace is the to-do list.
 *
 *      Pc98 *m = calloc(1, sizeof *m);
 *      pc98_init(m, X86_MODEL_80286, 10000000);
 *      pc98_bios_init(m);                              // vectors, work area, devices as the BIOS leaves them
 *      pc98_bios_attach(m, 0x80, &disk, image_bytes);  // first hard disk; writes go into image_bytes (or NULL)
 *      pc98_bios_boot(m, 0x80);                        // IPL to 1FC0:0000, far call
 *      while (!pc98_dead(m)) pc98_run(m, 100000);
 */
#ifndef GAP_PC98BIOS_H
#define GAP_PC98BIOS_H

#include <stddef.h>
#include <stdint.h>
#include <pc98disk.h>

struct Pc98;

/* ---- the HLE ROM page */
#define PC98_BIOS_SEG           0xFD80u
#define PC98_BIOS_ROM_BASE      0xFD800u
#define PC98_BIOS_ROM_SIZE      0x2800u     /* FD800-FFFFF */
#define PC98_BIOS_VEC_STUBS     0x0000u     /* the stub for vector n is at 4 * n: CD n CF 90 (INT n; IRET; NOP) */
#define PC98_BIOS_SVC_STUBS     0x0400u     /* service stubs (boot return, ...), 4 bytes each */
#define PC98_BIOS_INT1E_ENTRY   0x27ECu     /* FD80:27EC, the INT 1Eh entry IO.SYS writes into the vector itself */
#define PC98_BIOS_RESET_ENTRY   0x27F0u     /* FFFF0, where the CPU starts after a reset */

/* Where the boot program goes and the stack the BIOS calls it on (found by observation, docs/PC98_BIOS.md). */
#define PC98_IPL_SEG            0x1FC0u
#define PC98_BIOS_STACK_SEG     0x0000u
#define PC98_BIOS_STACK_TOP     0x7C00u

/* Machine-defined reasons for X86Cpu.stop (the CPU core's own codes are small numbers). */
#define PC98_STOP_BOOT_FAILED   0x80        /* the boot program returned to the BIOS: nothing bootable */
#define PC98_STOP_ROM_ENTRY     0x81        /* execution reached the HLE ROM page outside a known entry point */
#define PC98_STOP_FAULT_LOOP    0x82        /* the same CPU exception at the same place, again and again */
#define PC98_STOP_RESET         0x83        /* a reboot was requested (INT 1Eh, the reset vector) and is not modelled */

/* Notes the BIOS reports through the machine's event hook (PC98_EV_NOTE, a = code, b = detail). */
enum {
    PC98_NOTE_BOOT_LOADED = 1,  /* b = DA/UA: the first sector was loaded to 1FC0:0000 and is being entered */
    PC98_NOTE_BOOT_RETURNED,    /* b = CX the boot program returned with */
    PC98_NOTE_ROM_ENTRY,        /* b = offset in the ROM page of an unknown entry */
    PC98_NOTE_EXCEPTION,        /* b = vector: a CPU exception vector (00h-07h) reached the BIOS's default handler */
    PC98_NOTE_KEY_WAIT,         /* b = function: INT 18h is waiting for a key (the buffer is empty) */
    PC98_NOTE_DISK_RANGE,       /* b = DA/UA: a disk call addressed a sector beyond the image */
    PC98_NOTE_STOP_KEY,         /* b = 0: the STOP key raised INT 06h */
    PC98_NOTE_RESET,            /* b = 0: a reboot was requested */
};

typedef struct {
    const Pc98Disk *disk;       /* NULL: no drive */
    uint8_t *writable;          /* the image bytes again, writable (writes land there); NULL: write protected */
} Pc98BiosUnit;

typedef struct {
    uint8_t enabled;            /* pc98_bios_init() ran: the ROM page is mapped and the vectors point into it */
    uint8_t boot_device;        /* DA/UA of the last boot */
    Pc98BiosUnit hd[4];         /* DA/UA 80h-83h (and 00h-03h, relative sector numbers) */
    Pc98BiosUnit fd[4];         /* DA/UA 90h-93h: 1 MB (2HD) drives */
    /* Calendar: the date and time (BCD: year, month << 4 | weekday, day, hour, minute, second) that was true at
     * CPU cycle clock_cycles; the clock runs with the machine's own time. */
    uint8_t clock_bcd[6];
    uint64_t clock_cycles;
    /* Interval timer (INT 1Ch AH=02h): ticks of 10 ms left, and the routine to call through INT 07h's vector. */
    uint16_t timer_ticks;
    uint8_t timer_active;
    /* Text cursor, CRT mode and graphics mode (INT 18h AH=42h, CH) as the CRT BIOS last set them. */
    uint8_t cursor_shown, cursor_blink, crt_mode, gfx_mode;
    uint16_t cursor_addr;       /* byte offset in text VRAM */
    /* Bookkeeping for tests and the harness. */
    uint32_t calls;             /* calls answered */
    uint32_t unknown;           /* calls answered with "not implemented" */
    uint8_t last_unknown_vector, last_unknown_ah;
    uint32_t key_waits;         /* times a key read found the buffer empty and waited */
    uint32_t kb_polls;          /* guest reads of the key count (0000:0528) since the last key arrived: DOS's console
                                 * driver reads the buffer itself, so a large number means "idle at a prompt" */
    uint32_t disk_reads, disk_writes;   /* sectors */
    uint32_t fault_repeats;     /* identical CPU exceptions in a row (PC98_STOP_FAULT_LOOP after many) */
    uint32_t fault_last;        /* CS:IP (linear) and vector of the last one */
} Pc98Bios;

/* Puts the machine into the state the BIOS leaves before it boots: the ROM page with its stubs, every interrupt vector
 * pointing at the BIOS, the work area (640 KiB, the attached drives, the memory switches), both interrupt controllers
 * initialised (vectors 08h and 10h, the keyboard and the slave unmasked), the beeper tone, the text screen cleared.
 * Call after pc98_init() and again after attaching drives (pc98_bios_attach updates the drive bits itself). */
void pc98_bios_init(struct Pc98 *m);
/* Attaches a disk image as DA/UA 80h-83h (hard disk) or 90h-93h (1 MB floppy). writable: the same bytes as disk->data
 * if the guest may write to them (the caller decides whether to keep the result), or NULL for a write-protected drive.
 * disk = NULL detaches. Returns 0, or -1 for a unit number the BIOS does not have. */
int pc98_bios_attach(struct Pc98 *m, uint8_t da_ua, const Pc98Disk *disk, uint8_t *writable);
/* Boots from DA/UA the way the BIOS does: the first sector to 1FC0:0000, [0000:0584] = DA/UA, then a far call to it
 * with DS = 0, AL = DA/UA, the BIOS stack (0000:7C00) and interrupts off; a return lands in the BIOS, which stops the
 * CPU with PC98_STOP_BOOT_FAILED. Returns 0, or -1 when the device has no readable first sector. */
int pc98_bios_boot(struct Pc98 *m, uint8_t da_ua);
/* The machine's soft_int hook calls this for every INT n: returns 1 when the instruction is one of the BIOS's stubs and
 * the call was answered, 0 to let the CPU dispatch it through the vector table. */
int pc98_bios_soft_int(struct Pc98 *m, uint8_t vector);
/* Sets the calendar (BCD: year 00-99, month << 4 | weekday, day, hour, minute, second) as of now. */
void pc98_bios_set_clock(struct Pc98 *m, const uint8_t bcd[6]);
/* Reads the calendar as it is now (same layout). */
void pc98_bios_clock_now(struct Pc98 *m, uint8_t bcd[6]);
/* The key number (00h-7Fh) that types character ch with the BIOS's tables, or -1; *mods gets 01h when SHIFT is
 * needed and 10h for CTRL (the KB_SHIFT_STS bits). For scripted input in tools and tests. */
int pc98_key_for_char(uint8_t ch, uint8_t *mods);
/* Name of a PC98_NOTE_* code, for tracing tools. */
const char *pc98_bios_note_name(int code);
/* Name of a PC98_STOP_* or X86_STOP_* code. */
const char *pc98_stop_name(int code);

/* ---- BIOS work area addresses (0000:xxxx) used by this BIOS; docs/PC98_BIOS.md says what each holds and why */
#define PC98_WA_EXPMMSZ         0x0401u     /* extended memory, 128 KiB units */
#define PC98_WA_SCSI_EQUIP      0x0482u     /* SCSI units present (bit n = ID n) */
#define PC98_WA_BIOS_FLAG0      0x0500u
#define PC98_WA_BIOS_FLAG1      0x0501u     /* bits 0-2 memory size, bit 3 high resolution, 4-6 model, 7 8 MHz clock */
#define PC98_WA_KB_BUF          0x0502u     /* 16 words: scan code << 8 | character */
#define PC98_WA_KB_BUF_END      0x0522u
#define PC98_WA_KB_SHIFT_TBL    0x0522u
#define PC98_WA_KB_HEAD         0x0524u
#define PC98_WA_KB_TAIL         0x0526u
#define PC98_WA_KB_COUNT        0x0528u
#define PC98_WA_KB_RETRY        0x0529u
#define PC98_WA_KB_KEY_STS      0x052Au     /* 16 bytes: one bit per key that is down */
#define PC98_WA_KB_SHIFT_STS    0x053Au     /* bit 0 SHIFT, 1 CAPS, 2 KANA, 3 GRPH, 4 CTRL */
#define PC98_WA_CRT_ROW         0x053Bu     /* rasters per text row minus 1 (0Fh: 16-raster rows); the games' CSRFORM routine ORs 80h into it */
#define PC98_WA_CRT_STS         0x053Cu     /* the CRT mode INT 18h AH=0Ah set */
#define PC98_WA_DISK_EQUIP      0x055Cu     /* low byte bits 0-3: 1 MB floppies; high byte bits 0-3: hard disks */
#define PC98_WA_DISK_BOOT       0x0584u     /* DA/UA of the boot device */

/* Memory switches 1-8 live in the text attribute RAM at A3FE2, A3FE6, ... A3FFE. */
#define PC98_MEMSW(n)           (0xA3FE2u + 4u * ((n) - 1u))

#endif
