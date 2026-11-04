/*
 * emu: the PC-98 machine as the application runs it, everything but the window: the library's machine (CPU model, HLE BIOS,
 * display, FM board), the disks, the font, the clock that keeps it in step with real time, the idle skip, the sound pipe and the
 * keyboard. The desktop program (main.c, ui.c) is a skin on this; the host tests and the benchmark drive the very same code
 * with a fake clock and no window, which is how the integration is tested without a desktop.
 *
 *     Emu *e = emu_new(&cfg);
 *     emu_insert(e, SLOT_HD1, "game.hdi", err, sizeof err);
 *     emu_boot(e, -1, err, sizeof err);
 *     for (;;) {
 *         unsigned what = emu_step(e, now_ns);            // runs the machine for the time that is owed; EMU_PICTURE: e->pixels is new
 *         emu_key(e, 0x29, 1);  ...                       // keys go in between steps
 *         int16_t frames[1024][2]; emu_audio_take(e, &frames[0][0], 1024);
 *     }
 *
 * Writes by the guest go to the in-memory copy of an image (disks.h); nothing is ever written to a file but by emu_save().
 */
#ifndef PC98APP_EMU_H
#define PC98APP_EMU_H

#include <pc98.h>
#include "audiopipe.h"
#include "config.h"
#include "disks.h"
#include "idle.h"
#include "keymap.h"
#include "pacer.h"

#define EMU_W 640
#define EMU_H 400
#define EMU_AUDIO_RATE 48000

/* emu_step() result bits */
enum { EMU_PICTURE = 1, EMU_STOPPED = 2, EMU_REBOOTED = 4 };
/* where the text of the DOS screens gets its glyphs */
enum { EMU_FONT_BUILTIN = 0, EMU_FONT_ROM = 1, EMU_FONT_ANK = 2 };

typedef struct Emu {
    Pc98Config cfg;
    Pc98 *m;                            /* 1 MiB */
    Opna *fm;                           /* NULL when the machine has no FM board */
    DiskSlot disk[DISK_SLOTS];
    /* fonts */
    uint8_t *font_rom;
    size_t font_len;
    uint8_t ank[4096];
    int font_kind;
    char font_note[200];                /* for the status line: which font the text is drawn with, and why */
    int16_t *rhythm[OPNA_RHYTHMS];      /* the drum samples (the owner's WAVs), used in place by the chip */
    uint32_t rhythm_count[OPNA_RHYTHMS], rhythm_rate[OPNA_RHYTHMS];
    int rhythm_loaded;
    /* time, sound, keys */
    Pacer pacer;
    IdleSkip *idle;
    AudioPipe *audio;
    int sound;                          /* sound is on: the chip synthesises and the pipe fills */
    uint64_t audio_clocks;              /* machine clocks run since the pipe was last fed */
    Keys keys;
    /* the picture */
    uint32_t pixels[EMU_W * EMU_H];     /* 0x00RRGGBB, what pc98_render drew at the last frame boundary */
    uint64_t picture_frame;
    int have_picture;
    /* state */
    int booted;                         /* a machine exists (emu_boot succeeded) */
    int stopped;                        /* it stopped: stop_msg says why; emu_boot() starts it again */
    int paused;
    int held;                           /* held by the program (a menu is open) */
    int boot_dev;                      /* DA/UA of the last boot */
    char stop_msg[300];
    uint64_t boot_cycles_mark;          /* the CPU's cycle count at the last automatic restart, to notice a restart loop */
    int auto_restarts;
    uint64_t last_frames;
    uint64_t disk_writes_seen;          /* the BIOS's count of sectors written when the changes were last counted */
    const char *clock_override;         /* "YYMMDDhhmmss": the calendar at boot (tests); NULL = the host's clock */
    /* the log (--trace and the tests): one line per event */
    void (*log)(void *user, const char *line);
    void *log_user;
} Emu;

Emu *emu_new(const Pc98Config *cfg);
void emu_free(Emu *e);

/* The font: a FONT.ROM image (288,768 bytes) or, when path is NULL or empty, the library's built-in 8x16 font (ASCII, no kanji).
 * Returns 0 when the ROM was used; -1 when it could not be read or is not a font ROM (the built-in font is used and err says why). */
int emu_set_font(Emu *e, const char *path, char *err, size_t errcap);
/* The YM2608's drum samples, 2608_BD.WAV and the other five, from a folder: how many were found (0 to 6). */
int emu_set_rhythm_dir(Emu *e, const char *dir);
/* 16-bit mono PCM WAV bytes to samples (malloc'd, the caller frees): 0 or -1. Exposed for the tests. */
int emu_parse_wav_mono16(const uint8_t *buf, size_t len, int16_t **pcm, uint32_t *count, uint32_t *rate);

/* Disks. Inserting into a floppy drive of a running machine takes effect at once, a hard disk needs a restart. */
int emu_insert(Emu *e, int slot, const char *path, char *err, size_t errcap);
/* Reads the image and puts it into the drive its format belongs to (disk_slot_for), or into `prefer` (a SLOT_ value, or -1) when that
 * drive is of the right kind (a hard disk image does not go into a floppy drive): *slot_out says which. */
int emu_insert_auto(Emu *e, const char *path, int prefer, int *slot_out, char *err, size_t errcap);
int emu_insert_memory(Emu *e, int slot, const uint8_t *bytes, size_t size, const char *name, char *err, size_t errcap);
void emu_eject(Emu *e, int slot);
/* Throws away what the guest wrote to the image and reads it again from its file. 0, or -1 with err. */
int emu_revert(Emu *e, int slot, char *err, size_t errcap);
int emu_save(Emu *e, int slot, const char *path, char *err, size_t errcap);
uint32_t emu_changed_blocks(Emu *e, int slot);
/* Blocks changed in all the images, and whether the guest has written to a disk since the last time this was asked. */
uint32_t emu_changed_total(Emu *e);
int emu_disks_written(const Emu *e);

/* (Re)starts the machine from the disks: da_ua is the boot device (90h first floppy, 80h first hard disk) or -1 for the first
 * floppy that has a disk, else the first hard disk. The CPU, the memory and the devices are reset; the disks keep what the guest
 * wrote. Returns 0, or -1 with err when there is nothing to boot from. */
int emu_boot(Emu *e, int da_ua, char *err, size_t errcap);
void emu_pause(Emu *e, int on, uint64_t now_ns);
/* The same without the log line and the user's choice: the program holds the machine while a menu or a dialog is open. */
void emu_hold(Emu *e, int on, uint64_t now_ns);
void emu_set_speed(Emu *e, int percent, uint64_t now_ns);          /* 0 = as fast as possible */
void emu_set_sound(Emu *e, int on);
void emu_set_volume(Emu *e, int percent);
void emu_set_idle(Emu *e, int on);

/* Runs the machine for the time that is owed at `now_ns` (pacer.h). Returns EMU_* bits. */
unsigned emu_step(Emu *e, uint64_t now_ns);
/* Draws the screen into e->pixels now. */
void emu_render(Emu *e);
/* A key for the guest (a PC-98 key number, keymap.h); `down` 1 make, 0 break. */
void emu_key(Emu *e, uint8_t key, int down);
/* Feeds a Fairies key event through the key table (press 1, release 0, repeat 2) and on to the guest; 1 when it was a key of the PC-98. */
int emu_host_key(Emu *e, uint8_t fairy_code, int down);
void emu_release_keys(Emu *e);
/* Stereo 48 kHz frames for the sound device: up to max, returns how many. */
size_t emu_audio_take(Emu *e, int16_t *out, size_t max);
/* The text screen as plain text (pc98_text_dump). */
void emu_text(Emu *e, char *buf);
/* A hash of everything the guest can see: registers, RAM, display memory (the tests compare runs with it). */
uint64_t emu_state_hash(const Emu *e);

#endif
