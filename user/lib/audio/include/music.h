/*
 * music.h: one way to play the music files libaudio knows, whatever they are: songs (.tms, see chip.h and tms(5)) and
 * ProTracker modules (.mod, see modplay.h). The player and the Jukebox use this; WAV files have their own functions in audio.h.
 *
 *     MusicInfo info;
 *     char err[200];
 *     Music *m = music_open("song.tms", 32000, &info, err, sizeof err);
 *     size_t n = music_render(m, pcm, 1024);              // interleaved stereo frames, 0 at the end
 */
#ifndef LIBAUDIO_MUSIC_H
#define LIBAUDIO_MUSIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <chip.h>

typedef struct Music Music;

typedef struct MusicInfo {
    char   title[96];
    char   sub[100];                    /* the mood of a song, or what kind of module it is */
    char   kind[12];                    /* "song" or "module" */
    int    channels;
    int    bpm;                         /* 0 for a module (its tempo changes) */
    bool   loops;
    double length;                      /* seconds, one pass */
    char   notes[256];                  /* what the parser fixed up; empty if nothing */
} MusicInfo;

bool   music_is_file(const char *path);                    /* named *.tms or *.mod, or starts like one */
Music *music_open(const char *path, unsigned rate, MusicInfo *info, char *err, size_t errsz);
size_t music_render(Music *m, int16_t *out, size_t frames);
void   music_seek(Music *m, double seconds);
double music_length(const Music *m);
void   music_close(Music *m);
const ChipSong *music_chip_song(const Music *m);            /* the parsed song, NULL for a module: channels and instruments for `play -i` */

#endif
