/*
 * chip.h: a small chiptune / FM synthesizer and pattern sequencer (part of libaudio), and the song file format it plays.
 *
 *     char err[160];
 *     ChipSong *song = chip_song_load("/ScarletDevilMansion/share/music/shrine-steps.tms", err, sizeof err);
 *     ChipPlayer *p = chip_new(song, 32000);
 *     int16_t pcm[2 * 1024];
 *     size_t n = chip_render(p, pcm, 1024);              // interleaved stereo frames; 0 when a song that does not loop has ended
 *     chip_seek(p, 30.0);                                 // seconds
 *
 * A song is a few channels, each played by one monophonic voice of an instrument: pulse (50/25/12.5 %), triangle, saw or
 * noise, or a two operator FM voice for the PC-98 flavour, each with an attack/decay/sustain/release envelope. The
 * notes of a channel are written in a small music notation (MML) and grouped into patterns; an order list says which
 * patterns play in turn. The file format is documented in tms(5) and in docs/AUDIO.md; in short:
 *
 *     tms 1
 *     title Shrine Steps            ; a comment
 *     tempo 84
 *     loop yes
 *     inst 12 fm ratio=3.5 index=3 idecay=300 a=0 d=500 s=0 r=100 vol=0.9
 *     channel lead  inst=0 pan=-0.2
 *     channel bass  inst=2
 *     pattern A
 *       lead: o5 e4 g4 e4 d4 c2
 *       bass: o2 [c4 g4]2
 *     order A A
 *
 * MML (spaces and | are ignored):
 *     c d e f g a b    a note; + or # raises and - lowers it a semitone; a number is its length as a fraction of a whole
 *                      note (4 = quarter, 8 = eighth, 12 = an eighth triplet), dots lengthen it by half each
 *     r                a rest, same length syntax          w48   wait this many ticks (192 ticks make a whole note)
 *     o4  < >          the octave (o4 holds middle C; c4 is MIDI 60), one octave down / up
 *     l8               the default length of notes that have none
 *     v12              volume 0..15                        q6    gate: how much of its length a note sounds (1..8)
 *     @3  @bell        switch instrument (a number, or the name of an instrument)
 *     &                tie: the next note's length is added to the note before it
 *     [ ... ]3         repeat the part inside three times (nesting is fine; a bare ] means twice)
 * Every pattern starts a channel again from o4 l8 v12 q7 and the channel's own instrument. The noise instruments use the
 * pitch as their "hiss rate": c1 is a rumble, c7 is a hat.
 */
#ifndef LIBAUDIO_CHIP_H
#define LIBAUDIO_CHIP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CHIP_MAX_CHANNELS    6
#define CHIP_MAX_INSTRUMENTS 32

typedef enum { CHIP_SQUARE, CHIP_PULSE25, CHIP_PULSE12, CHIP_TRIANGLE, CHIP_SAW, CHIP_NOISE, CHIP_FM } ChipWave;

typedef struct ChipInstrument {
    char     name[16];
    ChipWave wave;
    float    attack_ms, decay_ms, sustain, release_ms;      /* sustain is a level, 0..1 */
    float    vibrato_semitones, vibrato_hz;
    float    volume;                                        /* 0..1 */
    float    fm_ratio, fm_index, fm_feedback, fm_index_decay_ms;   /* CHIP_FM: modulator frequency ratio, peak phase deviation in radians, ... */
} ChipInstrument;

typedef struct ChipChannelDef {
    char  name[16];
    int   instrument;                                       /* what the channel starts every pattern with */
    float pan;                                              /* -1 left .. +1 right */
    float gain;                                             /* 0..1 */
} ChipChannelDef;

typedef struct ChipSong {
    char  title[64], mood[96], author[48];
    int   bpm;                                              /* quarter notes per minute */
    bool  loop;
    ChipInstrument inst[CHIP_MAX_INSTRUMENTS];              /* the default bank, with the song's own instruments over it */
    int   ninst;
    int   nchannels;
    ChipChannelDef def[CHIP_MAX_CHANNELS];
    char *mml[CHIP_MAX_CHANNELS];                           /* each channel, every pattern in order, loops expanded */
    uint32_t ticks;                                         /* length of one pass, in ticks */
    char  warnings[256];                                    /* what the parser had to fix up (a short channel padded with a wait, ...) */
} ChipSong;

/* Parses the text of a song file. NULL, with a message in err ("line 12: no pattern called B"), if it cannot be played. */
ChipSong *chip_song_parse(const char *text, char *err, size_t errsz);
ChipSong *chip_song_load(const char *path, char *err, size_t errsz);
void      chip_song_free(ChipSong *song);
const ChipInstrument *chip_default_instruments(int *count);

typedef struct ChipPlayer ChipPlayer;

ChipPlayer *chip_new(const ChipSong *song, unsigned rate);          /* NULL if memory is short */
void   chip_free(ChipPlayer *p);
size_t chip_render(ChipPlayer *p, int16_t *out, size_t frames);     /* interleaved stereo; returns the frames written */
void   chip_seek(ChipPlayer *p, double seconds);
double chip_position(const ChipPlayer *p);                          /* seconds into the song */
double chip_length(const ChipPlayer *p);                            /* seconds in one pass */
bool   chip_finished(const ChipPlayer *p);

#endif
