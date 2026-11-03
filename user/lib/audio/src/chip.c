/* libaudio: the chiptune synthesizer and sequencer (see chip.h for the notation). */
#include <chip.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TICKS_PER_WHOLE 192
#define MAX_EXPANDED (256 * 1024)

const ChipInstrument chip_instruments[] = {
    /* 0 */ { "lead square",   CHIP_SQUARE,   2, 90, 0.62, 70, 0.10, 5.5, 1.00 },
    /* 1 */ { "soft pulse",    CHIP_PULSE25,  1, 70, 0.50, 50, 0.00, 0.0, 0.90 },
    /* 2 */ { "triangle bass", CHIP_TRIANGLE, 1, 25, 0.90, 35, 0.00, 0.0, 1.00 },
    /* 3 */ { "closed hat",    CHIP_NOISE,    0, 28, 0.00,  5, 0.00, 0.0, 0.55 },
    /* 4 */ { "snare",         CHIP_NOISE,    0, 95, 0.00, 10, 0.00, 0.0, 0.80 },
    /* 5 */ { "kick",          CHIP_NOISE,    0, 70, 0.00, 10, 0.00, 0.0, 1.00 },
    /* 6 */ { "pad",           CHIP_TRIANGLE, 140, 260, 0.70, 300, 0.12, 4.5, 0.85 },
    /* 7 */ { "pluck",         CHIP_PULSE25,  0, 150, 0.00, 20, 0.00, 0.0, 0.95 },
    /* 8 */ { "bell",          CHIP_TRIANGLE, 0, 520, 0.00, 80, 0.06, 6.0, 1.00 },
    /* 9 */ { "thin pulse",    CHIP_PULSE12,  1, 80, 0.45, 45, 0.00, 0.0, 0.85 },
    /* 10 */ { "saw lead",     CHIP_SAW,      3, 110, 0.55, 90, 0.08, 5.0, 0.80 },
    /* 11 */ { "open hat",     CHIP_NOISE,    0, 160, 0.00, 20, 0.00, 0.0, 0.50 },
};
const int chip_ninstruments = (int)(sizeof chip_instruments / sizeof chip_instruments[0]);

typedef struct { uint64_t start, gate; uint8_t note, vol, inst; } Ev;

typedef struct {
    Ev *ev;
    int n, next;
    /* the voice */
    bool on;
    const ChipInstrument *ins;
    double freq, phase, vib;
    int stage;                              /* 0 attack, 1 decay, 2 sustain, 3 release */
    float env, rel_step;
    uint64_t gate_end;
    float amp;
    uint32_t rng;
    float nval;
    float gl, gr;
} Track;

struct ChipPlayer {
    const ChipSong *song;
    unsigned rate;
    Track tr[CHIP_MAX_CHANNELS];
    int ntr;
    uint64_t pos, length;
};

/* ------------------------------------------------------------------ notation */

typedef struct { char *s; size_t n, cap; } Buf;

static bool buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > MAX_EXPANDED) return false;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) return false;
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
    return true;
}

/* Expands [ ... ]n loops by copying their text; *pp stops after the closing bracket (or at the end of the string). */
static bool expand(const char **pp, Buf *out, int depth, bool in_loop) {
    const char *p = *pp;
    if (depth > 8) return false;
    while (*p) {
        if (*p == '[') {
            Buf inner = { 0 };
            const char *q = p + 1;
            if (!expand(&q, &inner, depth + 1, true)) { free(inner.s); return false; }
            int count = 2;
            if (isdigit((unsigned char)*q)) { count = atoi(q); while (isdigit((unsigned char)*q)) q++; }
            if (count < 1) count = 1;
            if (count > 64) count = 64;
            for (int i = 0; i < count; i++) if (inner.s && !buf_add(out, inner.s, inner.n)) { free(inner.s); return false; }
            free(inner.s);
            p = q;
        } else if (*p == ']') {
            if (!in_loop) return false;
            *pp = p + 1;
            return true;
        } else {
            if (!buf_add(out, p, 1)) return false;
            p++;
        }
    }
    *pp = p;
    return !in_loop;
}

static int read_int(const char **pp, int def) {
    const char *p = *pp;
    if (!isdigit((unsigned char)*p)) return def;
    int v = 0;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; if (v > 100000) break; }
    *pp = p;
    return v;
}

/* A length: a divisor of a whole note (or the default) and dots. Returns ticks. */
static uint32_t read_length(const char **pp, int deflen) {
    int d = read_int(pp, deflen);
    if (d < 1) d = 1;
    if (d > TICKS_PER_WHOLE) d = TICKS_PER_WHOLE;
    uint32_t t = TICKS_PER_WHOLE / (uint32_t)d, add = t;
    while (**pp == '.') { add /= 2; t += add; (*pp)++; }
    return t ? t : 1;
}

static bool parse_channel(const char *mml, int def_inst, double spt, Track *tr) {
    Buf ex = { 0 };
    const char *src = mml;
    if (!expand(&src, &ex, 0, false)) { free(ex.s); return false; }
    const char *p = ex.s ? ex.s : "";
    int octave = 4, deflen = 8, vol = 12, inst = def_inst;
    uint32_t t = 0;
    bool tie = false;
    int cap = 64;
    tr->ev = malloc(sizeof(Ev) * (size_t)cap);
    uint32_t *dur = malloc(sizeof(uint32_t) * (size_t)cap), *st = malloc(sizeof(uint32_t) * (size_t)cap);
    if (!tr->ev || !dur || !st) { free(ex.s); free(tr->ev); free(dur); free(st); tr->ev = NULL; return false; }
    int n = 0;
    while (*p) {
        char c = *p++;
        if (isspace((unsigned char)c) || c == '|') continue;
        if (c == 'o') { octave = read_int(&p, 4); if (octave < 0) octave = 0; if (octave > 9) octave = 9; continue; }
        if (c == '<') { if (octave > 0) octave--; continue; }
        if (c == '>') { if (octave < 9) octave++; continue; }
        if (c == 'l') { deflen = read_int(&p, 8); if (deflen < 1) deflen = 8; continue; }
        if (c == 'v') { vol = read_int(&p, 12); if (vol > 15) vol = 15; continue; }
        if (c == '@') { inst = read_int(&p, 0); if (inst < 0 || inst >= chip_ninstruments) inst = def_inst; continue; }
        if (c == '&') { tie = true; continue; }
        int semi = -1;
        switch (c) {
        case 'c': semi = 0; break;  case 'd': semi = 2; break;  case 'e': semi = 4; break;  case 'f': semi = 5; break;
        case 'g': semi = 7; break;  case 'a': semi = 9; break;  case 'b': semi = 11; break;
        case 'r': semi = -2; break;
        default: break;
        }
        if (semi == -1) continue;                                   /* unknown character: skipped */
        int midi = 0;
        if (semi >= 0) {
            while (*p == '+' || *p == '#' || *p == '-') { semi += (*p == '-') ? -1 : 1; p++; }
            midi = 12 * (octave + 1) + semi;
            if (midi < 1) midi = 1;
            if (midi > 126) midi = 126;
        }
        uint32_t len = read_length(&p, deflen);
        if (tie && n > 0) {                                         /* extend the previous note */
            dur[n - 1] += len;
            t += len;
            tie = false;
            continue;
        }
        tie = false;
        if (semi >= 0) {
            if (n == cap) {
                cap *= 2;
                Ev *e2 = realloc(tr->ev, sizeof(Ev) * (size_t)cap);
                uint32_t *d2 = realloc(dur, sizeof(uint32_t) * (size_t)cap), *s2 = realloc(st, sizeof(uint32_t) * (size_t)cap);
                if (!e2 || !d2 || !s2) { free(ex.s); free(e2 ? e2 : tr->ev); free(d2 ? d2 : dur); free(s2 ? s2 : st); tr->ev = NULL; return false; }
                tr->ev = e2; dur = d2; st = s2;
            }
            tr->ev[n].note = (uint8_t)midi;
            tr->ev[n].vol = (uint8_t)vol;
            tr->ev[n].inst = (uint8_t)inst;
            st[n] = t;
            dur[n] = len;
            n++;
        }
        t += len;
    }
    for (int i = 0; i < n; i++) {
        double gate = (double)dur[i] * 0.88;
        tr->ev[i].start = (uint64_t)((double)st[i] * spt);
        tr->ev[i].gate = tr->ev[i].start + (uint64_t)(gate * spt) + 1;
    }
    tr->n = n;
    tr->next = 0;
    free(dur);
    free(st);
    free(ex.s);
    return true;
}

/* ------------------------------------------------------------------ the synthesizer */

static void voice_reset(Track *t) { t->on = false; t->next = 0; t->phase = 0; t->vib = 0; t->env = 0; t->stage = 4; }

ChipPlayer *chip_new(const ChipSong *song, unsigned rate) {
    ChipPlayer *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->song = song;
    p->rate = rate;
    double spt = (double)rate * 60.0 / ((double)song->bpm * (TICKS_PER_WHOLE / 4));        /* samples per tick */
    uint64_t end = 0;
    for (int c = 0; c < CHIP_MAX_CHANNELS; c++) {
        const ChipChannel *ch = &song->channel[c];
        if (!ch->mml) continue;
        Track *t = &p->tr[p->ntr];
        if (!parse_channel(ch->mml, ch->instrument, spt, t)) { chip_free(p); return NULL; }
        float pan = ch->pan, g = ch->gain > 0 ? ch->gain : 1.0f;
        double ang = ((double)pan + 1.0) * M_PI / 4.0;
        t->gl = (float)(cos(ang) * g);
        t->gr = (float)(sin(ang) * g);
        t->rng = 0x1234567u + (uint32_t)c * 7919u;
        voice_reset(t);
        if (t->n) {
            uint64_t e = t->ev[t->n - 1].gate + (uint64_t)(0.5 * rate);
            if (e > end) end = e;
        }
        p->ntr++;
    }
    p->length = end;
    return p;
}

void chip_free(ChipPlayer *p) {
    if (!p) return;
    for (int i = 0; i < p->ntr; i++) free(p->tr[i].ev);
    free(p);
}

static inline float polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static void trigger(ChipPlayer *p, Track *t, const Ev *e) {
    const ChipInstrument *ins = &chip_instruments[e->inst < chip_ninstruments ? e->inst : 0];
    t->ins = ins;
    t->on = true;
    t->freq = 440.0 * pow(2.0, ((double)e->note - 69.0) / 12.0);
    if (ins->wave != CHIP_NOISE || t->stage == 4) t->phase = 0;
    t->stage = ins->attack_ms > 0 ? 0 : 1;
    t->env = ins->attack_ms > 0 ? 0.0f : 1.0f;
    t->gate_end = e->gate;
    t->amp = (float)e->vol / 15.0f * ins->volume * 0.30f;
    (void)p;
}

static inline float voice_sample(ChipPlayer *p, Track *t, uint64_t pos) {
    const ChipInstrument *ins = t->ins;
    const double inv_rate = 1.0 / (double)p->rate;
    /* envelope */
    if (t->stage < 3 && pos >= t->gate_end) {
        t->stage = 3;
        float r = ins->release_ms * 0.001f * (float)p->rate;
        t->rel_step = r > 1 ? t->env / r : t->env;
    }
    switch (t->stage) {
    case 0: {
        float a = ins->attack_ms * 0.001f * (float)p->rate;
        t->env += 1.0f / (a > 1 ? a : 1);
        if (t->env >= 1.0f) { t->env = 1.0f; t->stage = 1; }
        break;
    }
    case 1: {
        float d = ins->decay_ms * 0.001f * (float)p->rate;
        t->env -= (1.0f - ins->sustain) / (d > 1 ? d : 1);
        if (t->env <= ins->sustain) { t->env = ins->sustain; t->stage = 2; }
        break;
    }
    case 3:
        t->env -= t->rel_step;
        if (t->env <= 0.0001f) { t->on = false; t->stage = 4; return 0.0f; }
        break;
    default: break;
    }
    double f = t->freq;
    if (ins->vibrato_semitones > 0) {
        t->vib += ins->vibrato_hz * inv_rate;
        if (t->vib >= 1.0) t->vib -= 1.0;
        float lfo = t->vib < 0.5 ? 4.0f * (float)t->vib - 1.0f : 3.0f - 4.0f * (float)t->vib;
        f *= 1.0 + (double)(ins->vibrato_semitones * lfo) * 0.05946;
    }
    double dt = f * inv_rate;
    if (dt > 0.45) dt = 0.45;
    t->phase += dt;
    if (t->phase >= 1.0) {
        t->phase -= 1.0;
        if (ins->wave == CHIP_NOISE) {
            uint32_t x = t->rng;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            t->rng = x;
            t->nval = (float)(int32_t)x * (1.0f / 2147483648.0f);
        }
    }
    float ph = (float)t->phase, s;
    float fdt = (float)dt;
    switch (ins->wave) {
    case CHIP_SQUARE:
    case CHIP_PULSE25:
    case CHIP_PULSE12: {
        float duty = ins->wave == CHIP_SQUARE ? 0.5f : ins->wave == CHIP_PULSE25 ? 0.25f : 0.125f;
        s = ph < duty ? 1.0f : -1.0f;
        s += polyblep(ph, fdt);
        float q = ph - duty;
        if (q < 0) q += 1.0f;
        s -= polyblep(q, fdt);
        break;
    }
    case CHIP_TRIANGLE: s = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
    case CHIP_SAW: s = 2.0f * ph - 1.0f - polyblep(ph, fdt); break;
    default: s = t->nval; break;
    }
    return s * t->env * t->amp;
}

static inline int16_t to16(float x) {
    float a = x < 0 ? -x : x;
    if (a > 0.8f) {                                                  /* soft knee above 0.8 */
        float over = a - 0.8f;
        a = 0.8f + over / (1.0f + over * 5.0f);
        x = x < 0 ? -a : a;
    }
    int v = (int)(x * 32767.0f);
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

size_t chip_render(ChipPlayer *p, int16_t *out, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        if (p->pos >= p->length) {
            if (!p->song->loop || p->length == 0) break;
            p->pos = 0;
            for (int i = 0; i < p->ntr; i++) voice_reset(&p->tr[i]);
        }
        size_t n = frames - done;
        if (n > p->length - p->pos) n = (size_t)(p->length - p->pos);
        uint64_t pos = p->pos;
        for (size_t i = 0; i < n; i++, pos++) {
            float l = 0, r = 0;
            for (int c = 0; c < p->ntr; c++) {
                Track *t = &p->tr[c];
                while (t->next < t->n && t->ev[t->next].start <= pos) trigger(p, t, &t->ev[t->next++]);
                if (!t->on) continue;
                float s = voice_sample(p, t, pos);
                l += s * t->gl;
                r += s * t->gr;
            }
            out[2 * (done + i)] = to16(l);
            out[2 * (done + i) + 1] = to16(r);
        }
        p->pos = pos;
        done += n;
    }
    return done;
}

void chip_seek(ChipPlayer *p, double seconds) {
    if (seconds < 0) seconds = 0;
    uint64_t pos = (uint64_t)(seconds * p->rate);
    if (pos > p->length) pos = p->length;
    p->pos = pos;
    for (int i = 0; i < p->ntr; i++) {
        Track *t = &p->tr[i];
        voice_reset(t);
        while (t->next < t->n && t->ev[t->next].start < pos) t->next++;
    }
}

double chip_position(const ChipPlayer *p) { return (double)p->pos / p->rate; }
double chip_length(const ChipPlayer *p) { return (double)p->length / p->rate; }
bool chip_finished(const ChipPlayer *p) { return !p->song->loop && p->pos >= p->length; }
