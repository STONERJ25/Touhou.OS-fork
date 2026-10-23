/*
 * jukebox: Reimu's Jukebox, the music player of HakureiDE, staged in the Prismriver Concert Hall.
 *
 * The Prismriver sisters (Lunasa, Merlin and Lyrica) play whatever the playlist holds: the songs that ship with the
 * system (the .tms files in /ScarletDevilMansion/share/music, see tms(5)) and the .wav, .tms and .mod files in ~/Music. The sound goes
 * through /dev/audio like any other program's; the window is one KApp whose tick keeps the device fed.
 *
 *   player.c    what is loaded, the stream to the sound device, play / pause / seek, the picture of the sound
 *   playlist.c  finding the music
 *   ui.c        the window
 *   main.c      command line and start-up
 */
#ifndef JUKEBOX_H
#define JUKEBOX_H

#include <stdio.h>
#include <kappa.h>
#include <audio.h>
#include <chip.h>
#include <music.h>

typedef enum { TK_MUSIC, TK_WAV } TrackKind;           /* a song or module (libaudio's Music), or a WAV file */

typedef struct Track {
    char path[256];
    char title[72];
    char sub[100];                      /* the mood of a song, or the format of a WAV file */
    TrackKind kind;
    double length;                      /* seconds, 0 when unknown */
    bool shipped;                       /* part of the system, not the user's own */
    char err[100];                      /* why it cannot be played, or "" */
} Track;

#define SPEC_BANDS 32
#define HIST_FRAMES 32768

typedef struct Jukebox {
    KApp *app;
    bool trace;

    /* the playlist */
    Track *tr;
    int ntr, cap;
    int cur;                            /* the track that is loaded (playing or paused), -1 for none */
    KListState list;

    /* playing */
    Audio *au;
    bool no_device;
    bool playing, paused;
    bool repeat;
    char status[160];
    Music *mus;                         /* the loaded song or module, NULL for a WAV file */
    MusicInfo minfo;
    AudioWav wav;
    bool wav_open;
    unsigned rate;
    unsigned bytes_per_frame;
    double start_pos;                   /* the source position that `rendered` counts from, in seconds */
    uint64_t rendered;                  /* source frames pushed to the device since then */
    uint64_t limit_frames;              /* frames to render in all (one pass of a song) */
    double paused_pos;
    bool source_done;
    uint8_t *carry;                     /* bytes the device did not take yet */
    size_t carry_n;
    int volume;                         /* 0..100, mirrors the master volume */
    bool muted;

    /* what the audience sees */
    int16_t hist[HIST_FRAMES];          /* recent source audio, mixed down to mono */
    uint64_t hist_total;                /* frames ever written into hist for this track */
    float level[3], level_peak[3];      /* the three performers' bands */
    float spec[SPEC_BANDS], spec_peak[SPEC_BANDS];
    uint64_t last_analysis_ms;
} Jukebox;

extern Jukebox J;

/* playlist.c */
void playlist_scan(void);
int  playlist_add(const char *path, bool shipped);              /* index of the new (or existing) track, -1 if it is not music */
void playlist_free(void);

/* player.c */
void player_init(void);
void player_shutdown(void);
void player_tick(uint64_t now_ms);                              /* feed the device, move on at the end of a track, analyse the sound */
bool player_load(int index, double at, bool start);             /* false if the track cannot be played */
void player_toggle(void);                                       /* play / pause (starts the selected track if nothing is loaded) */
void player_stop(void);
void player_next(int dir);                                      /* +1 next, -1 previous (restarts the track first if it has played a while) */
void player_seek(double seconds);
void player_set_volume(int percent);
void player_set_mute(bool mute);
double player_position(void);                                   /* seconds into the loaded track as it is heard now */
double player_length(void);
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ui.c */
void ui_init(void);
void ui_draw(KApp *app, KUI *ui, void *user);
void ui_open_file(void);

static inline void fmt_clock(double s, char *out, size_t n) {
    int t = s < 0 ? 0 : (int)(s + 0.5);
    snprintf(out, n, "%d:%02d", t / 60, t % 60);
}

#endif
