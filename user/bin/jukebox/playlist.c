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

/* The names of the sampled sound files libaudio reads (the content decides what a file really is; the name only says that it is worth looking at). */
static bool is_sound_name(const char *s) {
    static const char *const ext[] = { ".wav", ".wave", ".flac", ".fla", ".aif", ".aiff", ".aifc", ".au", ".snd", NULL };
    for (int i = 0; ext[i]; i++) if (has_suffix(s, ext[i])) return true;
    return false;
}

static bool is_music_name(const char *s) { return has_suffix(s, ".tms") || has_suffix(s, ".mod") || is_sound_name(s); }

static void base_title(const char *path, char *out, size_t n) {
    const char *b = k_basename(path);
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

/* "FLAC 44.1 kHz 16-bit stereo": what the window shows next to the time, and the tail of a playlist entry's second line. */
void jukebox_format_text(const AudioFileInfo *in, char *out, size_t n) {
    char rate[16], ch[16];
    if (in->rate % 1000 == 0) snprintf(rate, sizeof rate, "%u kHz", in->rate / 1000);
    else snprintf(rate, sizeof rate, "%u.%u kHz", in->rate / 1000, (in->rate % 1000) / 100);
    if (in->channels == 1) snprintf(ch, sizeof ch, "mono");
    else if (in->channels == 2) snprintf(ch, sizeof ch, "stereo");
    else snprintf(ch, sizeof ch, "%u ch", in->channels);
    const bool plain = !strcmp(in->codec, "PCM") || !strcmp(in->codec, in->format);
    snprintf(out, n, "%s%s%s %s %u-bit %s", in->format, plain ? "" : " ", plain ? "" : in->codec, rate, in->bits, ch);
}

int playlist_add(const char *path, bool shipped, bool named) {
    for (int i = 0; i < J.ntr; i++) if (!strcmp(J.tr[i].path, path)) return i;
    TrackKind kind;
    if (has_suffix(path, ".tms") || has_suffix(path, ".mod")) kind = TK_MUSIC;
    else if (is_sound_name(path)) kind = TK_FILE;
    else {
        char probe[12];
        if (named && afile_probe(path, probe)) kind = TK_FILE;                  /* a file the user chose, with a name that says nothing: look inside */
        else if (named && music_is_file(path)) kind = TK_MUSIC;
        else return -1;
    }
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
    if (kind == TK_MUSIC) {
        char err[200];
        MusicInfo info;
        Music *m = music_open(path, SONG_RATE, &info, err, sizeof err);
        if (!m) {
            snprintf(t->err, sizeof t->err, "%s", err);
            snprintf(t->sub, sizeof t->sub, "cannot be played");
        } else {
            snprintf(t->title, sizeof t->title, "%s", info.title);
            snprintf(t->sub, sizeof t->sub, "%s", info.sub);
            t->length = info.length;
            t->rate = SONG_RATE;
            t->channels = 2;
            t->format = AUDIO_FMT_S16;
            music_close(m);
        }
    } else {
        char err[200];
        AudioFile *f;
        AudioFileInfo in;
        int rc = afile_open(path, AFILE_DOWNMIX, &f, &in, err, sizeof err);
        if (rc) {                                                               /* stays in the list, with the decoder's own words */
            snprintf(t->err, sizeof t->err, "%s", err);
            snprintf(t->sub, sizeof t->sub, "cannot be played");
        } else if (in.rate < AUDIO_MIN_RATE || in.rate > AUDIO_MAX_RATE) {
            snprintf(t->err, sizeof t->err, "%u Hz is outside the %d to %d Hz the sound device plays", in.rate, AUDIO_MIN_RATE, AUDIO_MAX_RATE);
            snprintf(t->sub, sizeof t->sub, "cannot be played");
            afile_close(f);
        } else {
            char fmt[64];
            jukebox_format_text(&in, fmt, sizeof fmt);
            if (in.title[0] && in.artist[0]) snprintf(t->title, sizeof t->title, "%s - %s", in.artist, in.title);
            else if (in.title[0]) snprintf(t->title, sizeof t->title, "%s", in.title);
            else if (in.artist[0]) snprintf(t->title, sizeof t->title, "%s - %s", in.artist, t->title);
            if (in.album[0]) snprintf(t->sub, sizeof t->sub, "%s (%s)", in.album, fmt);
            else snprintf(t->sub, sizeof t->sub, "%s", fmt);
            t->length = in.length_known ? (double)in.frames / (double)in.rate : 0;
            t->rate = in.rate;
            t->channels = in.out_channels;
            t->format = in.out_format;
            afile_close(f);
        }
    }
    J.ntr++;
    return J.ntr - 1;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

#define SCAN_ENTRIES 1024               /* entries looked at in one folder */
#define SCAN_DEPTH   3                  /* folders inside folders: Music/Artist/Album/Disc 1 */
#define SCAN_TRACKS  2000               /* a whole disk is not a playlist */

/* The music in a folder, sorted by name, and (so that an album in its own folder is found) in the folders inside it, down to SCAN_DEPTH levels. */
static void add_directory(const char *dir, bool shipped, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    char **names = malloc(sizeof(char *) * SCAN_ENTRIES);
    int n = 0;
    struct dirent *e;
    while (names && (e = readdir(d)) && n < SCAN_ENTRIES) {
        if (e->d_name[0] == '.') continue;
        char *copy = strdup(e->d_name);
        if (copy) names[n++] = copy;                                            /* out of memory: the rest of the folder is left out, not crashed on */
    }
    closedir(d);
    if (names) qsort(names, (size_t)n, sizeof names[0], by_name);
    for (int i = 0; i < n; i++) {
        char path[320];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (J.ntr < SCAN_TRACKS) {
            struct stat st;
            if (is_music_name(names[i])) playlist_add(path, shipped, false);
            else if (depth < SCAN_DEPTH && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) add_directory(path, shipped, depth + 1);
        }
        free(names[i]);
    }
    free(names);
}

void playlist_add_directory(const char *dir, bool shipped) { add_directory(dir, shipped, 0); }

void playlist_scan(void) {
    add_directory(SHIPPED_DIR, true, SCAN_DEPTH);                               /* the shipped songs are one flat folder */
    const char *home = getenv("HOME");
    if (home && *home) {
        char dir[256];
        snprintf(dir, sizeof dir, "%s/Music", home);
        add_directory(dir, false, 0);
    }
}

void playlist_free(void) {
    free(J.tr);
    J.tr = NULL;
    J.ntr = J.cap = 0;
}
