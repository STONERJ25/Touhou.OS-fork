/* The playlist: the songs that ship with the system, then the user's own music. */
#include "jukebox.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHIPPED_DIR "/ScarletDevilMansion/share/music"
#define SONG_RATE   32000

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && !strcasecmp(s + n - m, suffix);
}

static void base_title(const char *path, char *out, size_t n) {
    const char *b = k_basename(path);
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static int g_names_n;

static void rebuild_names(void) {
    for (int i = 0; J.names && i < g_names_n; i++) free(J.names[i]);
    free(J.names);
    g_names_n = J.ntr;
    J.names = calloc((size_t)(J.ntr ? J.ntr : 1), sizeof(char *));
    for (int i = 0; i < J.ntr && J.names; i++) {
        char clock[16], label[120];
        if (J.tr[i].length > 0) fmt_clock(J.tr[i].length, clock, sizeof clock);
        else snprintf(clock, sizeof clock, "?");
        snprintf(label, sizeof label, "%s  (%s)", J.tr[i].title, clock);
        J.names[i] = strdup(label);
    }
}

int playlist_add(const char *path, bool shipped) {
    for (int i = 0; i < J.ntr; i++) if (!strcmp(J.tr[i].path, path)) return i;
    TrackKind kind;
    if (has_suffix(path, ".tms")) kind = TK_SONG;
    else if (has_suffix(path, ".wav")) kind = TK_WAV;
    else return -1;
    if (J.ntr == J.cap) {
        int cap = J.cap ? J.cap * 2 : 16;
        Track *t = realloc(J.tr, sizeof(Track) * (size_t)cap);
        if (!t) return -1;
        J.tr = t;
        J.cap = cap;
    }
    Track *t = &J.tr[J.ntr];
    memset(t, 0, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", path);
    t->kind = kind;
    t->shipped = shipped;
    base_title(path, t->title, sizeof t->title);
    if (kind == TK_SONG) {
        char err[160];
        ChipSong *s = chip_song_load(path, err, sizeof err);
        if (!s) {
            snprintf(t->err, sizeof t->err, "%s", err);
            snprintf(t->sub, sizeof t->sub, "cannot be played");
        } else {
            snprintf(t->title, sizeof t->title, "%s", s->title);
            snprintf(t->sub, sizeof t->sub, "%s", s->mood);
            ChipPlayer *p = chip_new(s, SONG_RATE);
            if (p) { t->length = chip_length(p); chip_free(p); }
            chip_song_free(s);
        }
    } else {
        AudioWav w;
        int rc = audio_wav_open(&w, path);
        if (rc) {
            snprintf(t->err, sizeof t->err, "not a WAV file this player understands");
            snprintf(t->sub, sizeof t->sub, "cannot be played");
        } else {
            t->length = (double)audio_wav_frames(&w) / (double)w.rate;
            snprintf(t->sub, sizeof t->sub, "%u Hz, %u-bit, %s", (unsigned)w.rate, (unsigned)w.bits, w.channels == 1 ? "mono" : "stereo");
            audio_wav_close(&w);
        }
    }
    J.ntr++;
    rebuild_names();
    return J.ntr - 1;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void add_directory(const char *dir, bool shipped) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        if (e->d_name[0] == '.') continue;
        if (!has_suffix(e->d_name, ".tms") && !has_suffix(e->d_name, ".wav")) continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    for (int i = 0; i < n; i++) {
        char path[320];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        playlist_add(path, shipped);
        free(names[i]);
    }
}

void playlist_scan(void) {
    add_directory(SHIPPED_DIR, true);
    const char *home = getenv("HOME");
    if (home && *home) {
        char dir[256];
        snprintf(dir, sizeof dir, "%s/Music", home);
        add_directory(dir, false);
    }
}

void playlist_free(void) {
    if (J.names) for (int i = 0; i < g_names_n; i++) free(J.names[i]);
    free(J.names);
    g_names_n = 0;
    free(J.tr);
    J.names = NULL;
    J.tr = NULL;
    J.ntr = J.cap = 0;
}
