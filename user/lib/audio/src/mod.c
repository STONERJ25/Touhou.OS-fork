/* libaudio: a ProTracker module player (see modplay.h). */
#include <modplay.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CH   32
#define MAX_FILE (12 * 1024 * 1024)
#define PAL_CLOCK 3546894.6                     /* Paula's sample clock divided by two: frequency = PAL_CLOCK / period */

typedef struct {
    char name[23];
    uint32_t length;                            /* bytes */
    int finetune;                               /* -8 .. 7 */
    int volume;                                 /* 0 .. 64 */
    uint32_t loop_start, loop_len;              /* bytes; a loop of 2 bytes or less means "no loop" */
    int8_t *data;
} ModSample;

typedef struct { uint16_t period; uint8_t sample, effect, param; } ModCell;

struct ModSong {
    char title[24];
    int nch;
    int song_len, restart;
    uint8_t order[128];
    int npat;
    ModCell *cells;                             /* npat * 64 * nch */
    ModSample smp[31];
};

static void set_err(char *err, size_t errsz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || !errsz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static unsigned be16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

static int channels_for(const uint8_t *sig) {
    if (!memcmp(sig, "M.K.", 4) || !memcmp(sig, "M!K!", 4) || !memcmp(sig, "FLT4", 4) || !memcmp(sig, "4CHN", 4)) return 4;
    if (!memcmp(sig, "6CHN", 4)) return 6;
    if (!memcmp(sig, "8CHN", 4) || !memcmp(sig, "FLT8", 4) || !memcmp(sig, "OCTA", 4) || !memcmp(sig, "CD81", 4)) return 8;
    if (sig[0] >= '1' && sig[0] <= '3' && sig[1] >= '0' && sig[1] <= '9' && !memcmp(sig + 2, "CH", 2)) {
        int n = (sig[0] - '0') * 10 + (sig[1] - '0');
        return n <= MAX_CH ? n : 0;
    }
    if (!memcmp(sig, "TDZ", 3) && sig[3] >= '1' && sig[3] <= '9') return sig[3] - '0';
    return 0;
}

bool mod_signature_ok(const char *sig4) { return channels_for((const uint8_t *)sig4) != 0; }

void mod_free(ModSong *s) {
    if (!s) return;
    for (int i = 0; i < 31; i++) free(s->smp[i].data);
    free(s->cells);
    free(s);
}

const char *mod_title(const ModSong *s) { return s->title; }
int mod_channel_count(const ModSong *s) { return s->nch; }

ModSong *mod_load(const char *path, char *err, size_t errsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { set_err(err, errsz, "cannot open %s", path); return NULL; }
    size_t cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n + 65536 > cap) {
            if (cap >= MAX_FILE) { free(buf); close(fd); set_err(err, errsz, "the module is too large"); return NULL; }
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (!buf) { set_err(err, errsz, "out of memory"); return NULL; }
    ModSong *s = NULL;
    if (n < 1084) { set_err(err, errsz, "too short to be a ProTracker module"); goto fail; }
    int nch = channels_for(buf + 1080);
    if (!nch) { set_err(err, errsz, "not a ProTracker module (no known signature at byte 1080)"); goto fail; }
    s = calloc(1, sizeof *s);
    if (!s) { set_err(err, errsz, "out of memory"); goto fail; }
    s->nch = nch;
    memcpy(s->title, buf, 20);
    for (int i = 0; i < 31; i++) {
        const uint8_t *p = buf + 20 + 30 * i;
        ModSample *m = &s->smp[i];
        memcpy(m->name, p, 22);
        m->length = be16(p + 22) * 2u;
        m->finetune = p[24] & 15;
        if (m->finetune > 7) m->finetune -= 16;
        m->volume = p[25] > 64 ? 64 : p[25];
        m->loop_start = be16(p + 26) * 2u;
        m->loop_len = be16(p + 28) * 2u;
    }
    s->song_len = buf[950];
    s->restart = buf[951];
    memcpy(s->order, buf + 952, 128);
    if (s->song_len < 1 || s->song_len > 128) { set_err(err, errsz, "bad song length %d", s->song_len); goto fail; }
    int maxpat = 0;
    for (int i = 0; i < 128; i++) if (s->order[i] > maxpat) maxpat = s->order[i];
    s->npat = maxpat + 1;
    size_t pat_bytes = (size_t)s->npat * 64 * (size_t)nch * 4;
    if (1084 + pat_bytes > n) { set_err(err, errsz, "the module is cut short: it should hold %d patterns", s->npat); goto fail; }
    s->cells = calloc((size_t)s->npat * 64 * (size_t)nch, sizeof(ModCell));
    if (!s->cells) { set_err(err, errsz, "out of memory"); goto fail; }
    for (size_t i = 0; i < (size_t)s->npat * 64 * (size_t)nch; i++) {
        const uint8_t *c = buf + 1084 + i * 4;
        s->cells[i].sample = (uint8_t)((c[0] & 0xF0) | (c[2] >> 4));
        s->cells[i].period = (uint16_t)(((c[0] & 0x0F) << 8) | c[1]);
        s->cells[i].effect = c[2] & 0x0F;
        s->cells[i].param = c[3];
    }
    size_t off = 1084 + pat_bytes;
    for (int i = 0; i < 31; i++) {
        ModSample *m = &s->smp[i];
        if (!m->length) continue;
        size_t have = off < n ? n - off : 0;
        size_t take = m->length < have ? m->length : have;
        m->data = malloc(m->length);
        if (!m->data) { set_err(err, errsz, "out of memory"); goto fail; }
        memcpy(m->data, buf + off, take);
        memset(m->data + take, 0, m->length - take);
        off += m->length;
        if (m->loop_start >= m->length) m->loop_len = 0;
        else if (m->loop_start + m->loop_len > m->length) m->loop_len = m->length - m->loop_start;
    }
    free(buf);
    return s;
fail:
    free(buf);
    mod_free(s);
    return NULL;
}

/* ------------------------------------------------------------------ the player */

typedef struct {
    int sample;                                 /* 1..31, 0 = none */
    double pos;                                 /* in sample bytes */
    int period;                                 /* the note's period with slides applied */
    int out_period;                             /* what is mixed: with arpeggio and vibrato */
    int target;
    int vol, out_vol;
    int finetune;
    int slide_up, slide_down, porta, vib_speed, vib_depth, vib_pos, vib_wave, trem_speed, trem_depth, trem_pos, trem_wave;
    int arp_x, arp_y, volslide, offset_mem;
    int loop_row, loop_count;
    int retrig, cut_tick, delay_tick;
    ModCell delayed;
    bool active;
} Chan;

struct ModPlayer {
    const ModSong *song;
    unsigned rate;
    Chan ch[MAX_CH];
    int speed, tempo, tick;
    int order_pos, row;
    int jump_order, jump_row, loop_jump;
    int row_delay;                              /* EEx: this many more times round the same row */
    bool repeating;                             /* the row now starting is such a repeat: effects run, notes do not start again */
    double samples_per_tick, in_tick;
    bool have_tick, finished;
    uint64_t pos, length;
    uint8_t visited[128 * 64 / 8];
};

static double period_scale(int semitones) { return pow(2.0, -(double)semitones / 12.0); }

static int fine_period(int period, int finetune) {
    if (!finetune) return period;
    return (int)((double)period * pow(2.0, -(double)finetune / 96.0) + 0.5);
}

static void update_tempo(ModPlayer *p) { p->samples_per_tick = (double)p->rate * 2.5 / (double)p->tempo; }

static void reset(ModPlayer *p) {
    memset(p->ch, 0, sizeof p->ch);
    memset(p->visited, 0, sizeof p->visited);
    p->speed = 6;
    p->tempo = 125;
    p->tick = 0;
    p->order_pos = p->row = 0;
    p->jump_order = p->jump_row = p->loop_jump = -1;
    p->row_delay = 0;
    p->repeating = false;
    p->in_tick = 0;
    p->have_tick = false;
    p->finished = false;
    p->pos = 0;
    update_tempo(p);
}

static void do_cell(ModPlayer *p, Chan *ch, const ModCell *cell) {
    const ModSong *s = p->song;
    if (cell->sample && cell->sample <= 31) {
        ch->sample = cell->sample;
        ch->vol = s->smp[cell->sample - 1].volume;
        ch->finetune = s->smp[cell->sample - 1].finetune;
    }
    int eff = cell->effect, par = cell->param;
    if (eff == 0xE && (par >> 4) == 0x5) ch->finetune = (par & 15) > 7 ? (par & 15) - 16 : (par & 15);      /* set finetune: before the note is tuned */
    if (cell->period) {
        int per = fine_period(cell->period, ch->finetune);
        if (eff == 3 || eff == 5) ch->target = per;
        else {
            ch->period = per;
            ch->pos = 0;
            ch->active = ch->sample != 0;
            if (ch->vib_wave < 4) ch->vib_pos = 0;
            if (ch->trem_wave < 4) ch->trem_pos = 0;
        }
    }
    switch (eff) {
    case 0x0: ch->arp_x = par >> 4; ch->arp_y = par & 15; break;
    case 0x1: ch->slide_up = par; break;
    case 0x2: ch->slide_down = par; break;
    case 0x3: if (par) ch->porta = par; break;
    case 0x4: if (par >> 4) ch->vib_speed = par >> 4; if (par & 15) ch->vib_depth = par & 15; break;
    case 0x5: case 0x6: case 0xA: ch->volslide = par; break;
    case 0x7: if (par >> 4) ch->trem_speed = par >> 4; if (par & 15) ch->trem_depth = par & 15; break;
    case 0x9:
        if (par) ch->offset_mem = par;
        if (cell->period || cell->sample) ch->pos = (double)ch->offset_mem * 256.0;
        break;
    case 0xB: p->jump_order = par; break;
    case 0xC: ch->vol = par > 64 ? 64 : par; break;
    case 0xD: p->jump_row = (par >> 4) * 10 + (par & 15); if (p->jump_order < 0) p->jump_order = p->order_pos + 1; break;
    case 0xE: {
        int x = par & 15;
        switch (par >> 4) {
        case 0x1: ch->period -= x; if (ch->period < 113) ch->period = 113; break;
        case 0x2: ch->period += x; if (ch->period > 856) ch->period = 856; break;
        case 0x4: ch->vib_wave = x; break;
        case 0x6:
            if (x == 0) ch->loop_row = p->row;
            else {
                if (ch->loop_count == 0) ch->loop_count = x; else ch->loop_count--;
                if (ch->loop_count > 0) p->loop_jump = ch->loop_row;
            }
            break;
        case 0x7: ch->trem_wave = x; break;
        case 0x9: ch->retrig = x; break;
        case 0xA: ch->vol += x; if (ch->vol > 64) ch->vol = 64; break;
        case 0xB: ch->vol -= x; if (ch->vol < 0) ch->vol = 0; break;
        case 0xC: ch->cut_tick = x ? x : -1; if (!x) ch->vol = 0; break;
        case 0xE: if (p->row_delay == 0) p->row_delay = x; break;
        default: break;
        }
        break;
    }
    case 0xF:
        if (par == 0) { p->finished = true; break; }
        if (par < 32) p->speed = par; else { p->tempo = par; update_tempo(p); }
        break;
    default: break;
    }
}

static void start_row(ModPlayer *p) {
    const ModSong *s = p->song;
    const ModCell *cells = &s->cells[((size_t)s->order[p->order_pos] * 64 + (size_t)p->row) * (size_t)s->nch];
    for (int c = 0; c < s->nch; c++) {
        Chan *ch = &p->ch[c];
        ch->retrig = 0;
        ch->cut_tick = 0;
        ch->delay_tick = 0;
        ch->arp_x = ch->arp_y = 0;
        const ModCell *cell = &cells[c];
        if (cell->effect == 0xE && (cell->param >> 4) == 0xD && (cell->param & 15)) {      /* note delay: the note starts later in the row */
            ch->delayed = *cell;
            ch->delay_tick = cell->param & 15;
            continue;
        }
        do_cell(p, ch, cell);
        ch->out_period = ch->period;
        ch->out_vol = ch->vol;
    }
}

static int vib_value(int wave, int pos, int depth) {
    int pp = pos & 31, amp;
    switch (wave & 3) {
    case 1: amp = 255 - pp * 8; break;                              /* ramp down */
    case 2: amp = 255; break;                                       /* square */
    default: amp = (int)(255.0 * sin(3.14159265358979 * (double)pp / 32.0)); break;
    }
    int v = amp * depth / 128;
    return (pos & 32) ? -v : v;
}

static void tick_effects(ModPlayer *p) {
    const ModSong *s = p->song;
    const ModCell *cells = &s->cells[((size_t)s->order[p->order_pos] * 64 + (size_t)p->row) * (size_t)s->nch];
    for (int c = 0; c < s->nch; c++) {
        Chan *ch = &p->ch[c];
        const ModCell *cell = &cells[c];
        int eff = cell->effect, par = cell->param;
        if (ch->delay_tick && p->tick == ch->delay_tick) {
            do_cell(p, ch, &ch->delayed);
            ch->out_period = ch->period;
            ch->out_vol = ch->vol;
            ch->delay_tick = 0;
            continue;
        }
        ch->out_period = ch->period;
        ch->out_vol = ch->vol;
        switch (eff) {
        case 0x0:
            if (par) {
                int phase = p->tick % 3;
                int semis = phase == 0 ? 0 : phase == 1 ? (par >> 4) : (par & 15);
                ch->out_period = (int)((double)ch->period * period_scale(semis) + 0.5);
            }
            break;
        case 0x1: ch->period -= ch->slide_up; if (ch->period < 113) ch->period = 113; ch->out_period = ch->period; break;
        case 0x2: ch->period += ch->slide_down; if (ch->period > 856) ch->period = 856; ch->out_period = ch->period; break;
        case 0x3: case 0x5:
            if (ch->target && ch->period != ch->target) {
                if (ch->period > ch->target) { ch->period -= ch->porta; if (ch->period < ch->target) ch->period = ch->target; }
                else { ch->period += ch->porta; if (ch->period > ch->target) ch->period = ch->target; }
            }
            ch->out_period = ch->period;
            if (eff == 5) goto volslide;
            break;
        case 0x4: case 0x6:
            ch->vib_pos += ch->vib_speed;
            ch->out_period = ch->period + vib_value(ch->vib_wave, ch->vib_pos, ch->vib_depth);
            if (eff == 6) goto volslide;
            break;
        case 0x7:
            ch->trem_pos += ch->trem_speed;
            ch->out_vol = ch->vol + vib_value(ch->trem_wave, ch->trem_pos, ch->trem_depth) * 2;    /* the table times depth over 64 */
            if (ch->out_vol < 0) ch->out_vol = 0;
            if (ch->out_vol > 64) ch->out_vol = 64;
            break;
        case 0xA:
        volslide:
            if (ch->volslide >> 4) ch->vol += ch->volslide >> 4; else ch->vol -= ch->volslide & 15;
            if (ch->vol < 0) ch->vol = 0;
            if (ch->vol > 64) ch->vol = 64;
            ch->out_vol = ch->vol;
            break;
        case 0xE:
            if ((par >> 4) == 0x9 && ch->retrig && p->tick % ch->retrig == 0) ch->pos = 0;
            if ((par >> 4) == 0xC && p->tick == (par & 15)) { ch->vol = 0; ch->out_vol = 0; }
            break;
        default: break;
        }
        if (ch->period < 1) ch->period = 1;
    }
}

/* The row is over: where does the song go next? Returns false at its end. */
static bool advance_row(ModPlayer *p) {
    if (p->row_delay > 0) { p->row_delay--; p->repeating = true; return true; }
    int order = p->order_pos, row = p->row + 1;
    if (p->loop_jump >= 0) { row = p->loop_jump; p->loop_jump = -1; p->jump_order = p->jump_row = -1; }
    else if (p->jump_order >= 0 || p->jump_row >= 0) {
        order = p->jump_order >= 0 ? p->jump_order : p->order_pos + 1;
        row = p->jump_row >= 0 && p->jump_row < 64 ? p->jump_row : 0;      /* a break to a row that is not there goes to the first */
        p->jump_order = p->jump_row = -1;
        if (order < p->song->song_len && (p->visited[(order * 64 + row) >> 3] & (1 << ((order * 64 + row) & 7)))) return false;   /* round again: the song is over */
    } else if (row >= 64) { order++; row = 0; }
    if (order >= p->song->song_len) return false;
    p->order_pos = order;
    p->row = row;
    p->visited[(order * 64 + row) >> 3] |= (uint8_t)(1 << ((order * 64 + row) & 7));
    return true;
}

static bool next_tick(ModPlayer *p) {
    if (p->have_tick) {
        p->tick++;
        if (p->tick >= p->speed) {
            p->tick = 0;
            if (!advance_row(p)) { p->finished = true; return false; }
        }
    } else {
        p->visited[0] |= 1;
    }
    p->have_tick = true;
    if (p->tick == 0 && !p->repeating) start_row(p);
    else { p->repeating = false; tick_effects(p); }
    if (p->finished) return false;
    p->in_tick += p->samples_per_tick;
    return true;
}

ModPlayer *mod_player_new(const ModSong *song, unsigned rate) {
    ModPlayer *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->song = song;
    p->rate = rate;
    reset(p);
    /* a dry run measures the song: every tick is processed, nothing is mixed */
    double total = 0;
    long guard = 0;
    while (next_tick(p) && guard++ < 2000000) {
        total += p->samples_per_tick;
        p->in_tick = 0;
        if (total > (double)rate * 3600) break;
    }
    p->length = (uint64_t)total;
    reset(p);
    return p;
}

void mod_player_free(ModPlayer *p) { free(p); }

static inline int16_t to16(float x) {
    float a = x < 0 ? -x : x;
    if (a > 0.8f) { float over = a - 0.8f; a = 0.8f + over / (1.0f + over * 5.0f); x = x < 0 ? -a : a; }
    int v = (int)(x * 32767.0f);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void mix_frame(ModPlayer *p, float *l, float *r) {
    const ModSong *s = p->song;
    float left = 0, right = 0;
    for (int c = 0; c < s->nch; c++) {
        Chan *ch = &p->ch[c];
        if (!ch->active || !ch->sample || ch->out_period < 1) continue;
        const ModSample *m = &s->smp[ch->sample - 1];
        if (!m->data || m->length < 2) { ch->active = false; continue; }
        bool looped = m->loop_len > 2;
        uint32_t end = looped ? m->loop_start + m->loop_len : m->length;
        if (ch->pos >= (double)end) {
            if (!looped) { ch->active = false; continue; }
            ch->pos -= (floor((ch->pos - (double)end) / (double)m->loop_len) + 1.0) * (double)m->loop_len;     /* back into the loop */
        }
        uint32_t i = (uint32_t)ch->pos;
        uint32_t j = i + 1;
        if (j >= end) j = looped ? m->loop_start : i;
        float frac = (float)(ch->pos - (double)i);
        float v = ((float)m->data[i] + ((float)m->data[j] - (float)m->data[i]) * frac) * (1.0f / 128.0f) * ((float)ch->out_vol * (1.0f / 64.0f));
        ch->pos += PAL_CLOCK / (double)ch->out_period / (double)p->rate;
        bool is_left = ((c & 3) == 0 || (c & 3) == 3);
        left += v * (is_left ? 1.0f : 0.3f);
        right += v * (is_left ? 0.3f : 1.0f);
    }
    float scale = 0.8f / ((float)s->nch * 0.65f);
    *l = left * scale;
    *r = right * scale;
}

size_t mod_render(ModPlayer *p, int16_t *out, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        if (p->in_tick <= 0.0) {
            if (p->finished || !next_tick(p)) break;
        }
        size_t n = (size_t)p->in_tick;
        if (n < 1) n = 1;
        if (n > frames - done) n = frames - done;
        for (size_t i = 0; i < n; i++) {
            float l, r;
            mix_frame(p, &l, &r);
            out[2 * (done + i)] = to16(l);
            out[2 * (done + i) + 1] = to16(r);
        }
        p->in_tick -= (double)n;
        p->pos += n;
        done += n;
    }
    return done;
}

/* Seeking runs the song without mixing: the voices still move on through their samples, so a drum hit from long ago is over. */
static void skip_frames(ModPlayer *p, uint64_t frames) {
    for (int c = 0; c < p->song->nch; c++) {
        Chan *ch = &p->ch[c];
        if (ch->active && ch->sample && ch->out_period >= 1) ch->pos += (double)frames * PAL_CLOCK / (double)ch->out_period / (double)p->rate;
    }
}

void mod_seek(ModPlayer *p, double seconds) {
    if (seconds < 0) seconds = 0;
    uint64_t target = (uint64_t)(seconds * p->rate);
    reset(p);
    long guard = 0;
    while (p->pos < target && guard++ < 2000000) {
        if (!next_tick(p)) break;
        uint64_t take = (uint64_t)p->in_tick;
        if (p->pos + take > target) take = target - p->pos;
        skip_frames(p, take);
        p->pos += take;
        p->in_tick -= (double)take;
        if (p->in_tick >= 1.0) break;                       /* the target is inside this tick */
    }
}

double mod_position(const ModPlayer *p) { return (double)p->pos / p->rate; }
double mod_length(const ModPlayer *p) { return (double)p->length / p->rate; }
bool mod_finished(const ModPlayer *p) { return p->finished; }
