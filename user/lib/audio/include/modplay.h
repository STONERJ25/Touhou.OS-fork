/*
 * modplay.h: a ProTracker module (.mod) player, part of libaudio.
 *
 *     char err[160];
 *     ModSong *song = mod_load("/HumanVillage/reimu/Music/tune.mod", err, sizeof err);
 *     ModPlayer *p = mod_player_new(song, 32000);
 *     int16_t pcm[2 * 1024];
 *     size_t n = mod_render(p, pcm, 1024);               // interleaved stereo frames; 0 when the song has ended
 *
 * Reads the common 4 to 32 channel signatures (M.K., M!K!, FLT4, 4CHN, 6CHN, 8CHN, xxCH; not FLT8, which stores its patterns in pairs). Plays the usual effects:
 * arpeggio, slides, tone portamento, vibrato, tremolo, sample offset, volume slides, position jump, pattern break, set
 * volume, speed and tempo, and from the extended set fine slides, finetune, pattern loop, retrigger, note cut, note delay and pattern delay.
 * Channels are panned Amiga style (left right right left) with a little crosstalk. Not read: S3M, XM, IT, 15-sample modules.
 */
#ifndef LIBAUDIO_MODPLAY_H
#define LIBAUDIO_MODPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct ModSong ModSong;
typedef struct ModPlayer ModPlayer;

bool mod_signature_ok(const char *sig4);              /* do these four bytes (at offset 1080 of a file) name a module format? */
ModSong *mod_load(const char *path, char *err, size_t errsz);
void mod_free(ModSong *song);
const char *mod_title(const ModSong *song);
int mod_channel_count(const ModSong *song);

ModPlayer *mod_player_new(const ModSong *song, unsigned rate);
void   mod_player_free(ModPlayer *p);
size_t mod_render(ModPlayer *p, int16_t *out, size_t frames);
void   mod_seek(ModPlayer *p, double seconds);
double mod_position(const ModPlayer *p);
double mod_length(const ModPlayer *p);
bool   mod_finished(const ModPlayer *p);

#endif
