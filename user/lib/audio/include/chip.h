/*
 * chip.h: a small chiptune synthesizer and sequencer (part of libaudio).
 *
 * A song is a few channels, each a string in a tiny music notation (MML), each played by one monophonic voice of an
 * instrument: square or pulse, triangle, saw or noise, with an attack/decay/sustain/release envelope and vibrato.
 *
 *     ChipPlayer *p = chip_new(&song, 32000);
 *     int16_t pcm[2 * 1024];
 *     size_t n = chip_render(p, pcm, 1024);          // stereo frames; 0 when a song that does not loop has ended
 *     chip_seek(p, 30.0);                             // seconds
 *
 * Notation (spaces and | are ignored):
 *     c d e f g a b    a note; + or # raises and - lowers it by a semitone; a number is its length as a fraction of a
 *                      whole note (4 = quarter, 8 = eighth, 12 = an eighth triplet), dots lengthen it by half each
 *     r                a rest, same length syntax
 *     o4  < >          the octave (o4 holds middle C; c4 is MIDI note 60), and one octave down / up
 *     l8               the default length of notes that have none
 *     v12              volume 0..15
 *     @3               switch to instrument 3 of the instrument table
 *     &                tie: the next note's length is added to the note before it
 *     [ ... ]3         repeat the part inside three times (nesting is fine; a bare ] means twice)
 * Tempo, loop flag and the channels are in the ChipSong. The noise instrument uses the pitch as its "hiss rate":
 * c1 is a rumble, c7 is a hat.
 */
#ifndef LIBAUDIO_CHIP_H
#define LIBAUDIO_CHIP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CHIP_MAX_CHANNELS 6

typedef enum { CHIP_SQUARE, CHIP_PULSE25, CHIP_PULSE12, CHIP_TRIANGLE, CHIP_SAW, CHIP_NOISE } ChipWave;

typedef struct ChipInstrument {
    const char *name;
    ChipWave wave;
    float attack_ms, decay_ms, sustain, release_ms;     /* sustain is a level, 0..1 */
    float vibrato_semitones, vibrato_hz;
    float volume;                                       /* 0..1 */
} ChipInstrument;

typedef struct ChipChannel {
    const char *mml;
    int instrument;                                     /* used until the notation says @n */
    float pan;                                          /* -1 left .. +1 right */
    float gain;                                         /* 0..1 */
} ChipChannel;

typedef struct ChipSong {
    const char *title;
    const char *mood;                                   /* one line describing it */
    int bpm;                                            /* quarter notes per minute */
    bool loop;
    ChipChannel channel[CHIP_MAX_CHANNELS];
} ChipSong;

typedef struct ChipPlayer ChipPlayer;

extern const ChipInstrument chip_instruments[];
extern const int chip_ninstruments;

ChipPlayer *chip_new(const ChipSong *song, unsigned rate);          /* NULL if the notation is broken or memory is short */
void   chip_free(ChipPlayer *p);
size_t chip_render(ChipPlayer *p, int16_t *out, size_t frames);     /* interleaved stereo; returns the frames written */
void   chip_seek(ChipPlayer *p, double seconds);
double chip_position(const ChipPlayer *p);                          /* seconds into the song */
double chip_length(const ChipPlayer *p);                            /* seconds in one pass */
bool   chip_finished(const ChipPlayer *p);

/* The songs that come with the system (original compositions, written in MML in songs.c). */
const ChipSong *chip_songs(int *count);

#endif
