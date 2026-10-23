/* libaudio: one front for the music file kinds (songs and modules). */
#include <music.h>
#include <modplay.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

struct Music {
    ChipSong *cs;
    ChipPlayer *cp;
    ModSong *ms;
    ModPlayer *mp;
};

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n > m && !strcasecmp(s + n - m, suffix);
}

typedef enum { K_NONE, K_SONG, K_MOD } Kind;

static Kind sniff(const char *path) {
    if (has_suffix(path, ".tms")) return K_SONG;
    if (has_suffix(path, ".mod")) return K_MOD;
    if (!strcmp(path, "-")) return K_NONE;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return K_NONE;
    char head[1084];
    ssize_t r = read(fd, head, sizeof head);
    close(fd);
    if (r >= 4 && !memcmp(head, "tms", 3) && (head[3] == ' ' || head[3] == '\n' || head[3] == '\r')) return K_SONG;
    if (r >= 1084 && mod_signature_ok(head + 1080)) return K_MOD;
    return K_NONE;
}

bool music_is_file(const char *path) { return sniff(path) != K_NONE; }

Music *music_open(const char *path, unsigned rate, MusicInfo *info, char *err, size_t errsz) {
    Kind k = sniff(path);
    Music *m = calloc(1, sizeof *m);
    MusicInfo dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof *info);
    if (!m) { snprintf(err, errsz, "out of memory"); return NULL; }
    if (k == K_SONG) {
        m->cs = chip_song_load(path, err, errsz);
        if (!m->cs) { free(m); return NULL; }
        m->cp = chip_new(m->cs, rate);
        if (!m->cp) { snprintf(err, errsz, "out of memory"); music_close(m); return NULL; }
        snprintf(info->title, sizeof info->title, "%s", m->cs->title);
        snprintf(info->sub, sizeof info->sub, "%s", m->cs->mood);
        snprintf(info->kind, sizeof info->kind, "song");
        info->channels = m->cs->nchannels;
        info->bpm = m->cs->bpm;
        info->loops = m->cs->loop;
        info->length = chip_length(m->cp);
        snprintf(info->notes, sizeof info->notes, "%s", m->cs->warnings);
    } else if (k == K_MOD) {
        m->ms = mod_load(path, err, errsz);
        if (!m->ms) { free(m); return NULL; }
        m->mp = mod_player_new(m->ms, rate);
        if (!m->mp) { snprintf(err, errsz, "out of memory"); music_close(m); return NULL; }
        snprintf(info->title, sizeof info->title, "%s", mod_title(m->ms)[0] ? mod_title(m->ms) : "Untitled module");
        snprintf(info->sub, sizeof info->sub, "ProTracker module, %d channels", mod_channel_count(m->ms));
        snprintf(info->kind, sizeof info->kind, "module");
        info->channels = mod_channel_count(m->ms);
        info->length = mod_length(m->mp);
    } else {
        snprintf(err, errsz, "not a song (.tms) or module (.mod)");
        free(m);
        return NULL;
    }
    return m;
}

size_t music_render(Music *m, int16_t *out, size_t frames) {
    return m->cp ? chip_render(m->cp, out, frames) : mod_render(m->mp, out, frames);
}

void music_seek(Music *m, double seconds) {
    if (m->cp) chip_seek(m->cp, seconds);
    else mod_seek(m->mp, seconds);
}

double music_length(const Music *m) { return m->cp ? chip_length(m->cp) : mod_length(m->mp); }

const ChipSong *music_chip_song(const Music *m) { return m->cs; }

void music_close(Music *m) {
    if (!m) return;
    chip_free(m->cp);
    chip_song_free(m->cs);
    mod_player_free(m->mp);
    mod_free(m->ms);
    free(m);
}
