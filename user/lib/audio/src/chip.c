/* libaudio: the chiptune / FM synthesizer, the pattern sequencer and the song file parser (see chip.h). */
#include <chip.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TICKS_PER_WHOLE 192
#define MAX_TEXT        (256 * 1024)
#define MAX_PATTERNS    48
#define MAX_ORDER       1024

/* ------------------------------------------------------------------ the default instrument bank */

#define I(nm, w, a, d, s, r, vs, vh, v)                 { nm, w, a, d, s, r, vs, vh, v, 0, 0, 0, 0 }
#define F(nm, a, d, s, r, v, ratio, idx, fb, idec, vs, vh) { nm, CHIP_FM, a, d, s, r, vs, vh, v, ratio, idx, fb, idec }

static const ChipInstrument g_bank[] = {
    /*  0 */ I("lead",      CHIP_SQUARE,   2,  90, 0.62,  70, 0.10, 5.5, 1.00),
    /*  1 */ I("pulse",     CHIP_PULSE25,  1,  70, 0.50,  50, 0.00, 0.0, 0.90),
    /*  2 */ I("bass",      CHIP_TRIANGLE, 1,  25, 0.90,  35, 0.00, 0.0, 1.00),
    /*  3 */ I("hat",       CHIP_NOISE,    0,  28, 0.00,   5, 0.00, 0.0, 0.55),
    /*  4 */ I("snare",     CHIP_NOISE,    0,  95, 0.00,  10, 0.00, 0.0, 0.80),
    /*  5 */ I("kick",      CHIP_NOISE,    0,  70, 0.00,  10, 0.00, 0.0, 1.00),
    /*  6 */ I("pad",       CHIP_TRIANGLE, 140, 260, 0.70, 300, 0.12, 4.5, 0.85),
    /*  7 */ I("pluck",     CHIP_PULSE25,  0, 150, 0.00,  20, 0.00, 0.0, 0.95),
    /*  8 */ I("tri-bell",  CHIP_TRIANGLE, 0, 520, 0.00,  80, 0.06, 6.0, 1.00),
    /*  9 */ I("thin",      CHIP_PULSE12,  1,  80, 0.45,  45, 0.00, 0.0, 0.85),
    /* 10 */ I("saw",       CHIP_SAW,      3, 110, 0.55,  90, 0.08, 5.0, 0.80),
    /* 11 */ I("open-hat",  CHIP_NOISE,    0, 160, 0.00,  20, 0.00, 0.0, 0.50),
    /* 12 */ F("fm-bell",   0, 600, 0.00, 150, 0.90, 3.5, 3.2, 0.0,  380, 0.00, 0.0),
    /* 13 */ F("fm-epiano", 1, 900, 0.10, 160, 0.95, 1.0, 1.7, 0.0,  450, 0.00, 0.0),
    /* 14 */ F("fm-bass",   1, 160, 0.75,  45, 1.00, 1.0, 2.6, 0.5,  140, 0.00, 0.0),
    /* 15 */ F("fm-pad",  150, 350, 0.70, 380, 0.80, 2.0, 1.1, 0.1, 5000, 0.07, 4.8),
    /* 16 */ F("fm-pluck",  0, 380, 0.00,  90, 0.95, 2.0, 3.0, 0.2,  100, 0.00, 0.0),
    /* 17 */ F("fm-lead",   4, 120, 0.65,  90, 0.85, 1.0, 1.4, 0.3,  700, 0.10, 5.2),
    /* 18 */ F("fm-brass", 25, 140, 0.70,  80, 0.85, 1.0, 2.2, 0.6,  600, 0.06, 5.0),
};
#define BANK_N ((int)(sizeof g_bank / sizeof g_bank[0]))

const ChipInstrument *chip_default_instruments(int *count) {
    if (count) *count = BANK_N;
    return g_bank;
}

/* ------------------------------------------------------------------ sine table */

#define SINE_N 2048
static float g_sin[SINE_N + 1];

static void sine_init(void) {
    if (g_sin[SINE_N / 4] != 0.0f) return;
    for (int i = 0; i <= SINE_N; i++) g_sin[i] = (float)sin(2.0 * M_PI * (double)i / SINE_N);
}

static inline float wrap01(float x) {
    x -= (float)(int)x;
    return x < 0.0f ? x + 1.0f : x;
}

static inline float sine_cycles(float x) {
    float p = wrap01(x) * SINE_N;
    int i = (int)p;
    float f = p - (float)i;
    return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * f;
}

/* ------------------------------------------------------------------ growing buffers */

typedef struct { char *s; size_t n, cap; } Buf;

static bool buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > MAX_TEXT) return false;
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

static bool buf_str(Buf *b, const char *s) { return buf_add(b, s, strlen(s)); }

static void set_err(char *err, size_t errsz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || !errsz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ MML */

typedef struct { uint32_t start, dur; uint8_t note, vol, inst, gate; } TEv;
typedef struct { TEv *ev; int n, cap; uint32_t total; } Seq;

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

static int find_instrument(const ChipSong *song, const char *name, size_t len) {
    for (int i = 0; i < song->ninst; i++)
        if (strlen(song->inst[i].name) == len && !strncmp(song->inst[i].name, name, len)) return i;
    return -1;
}

static bool seq_add(Seq *s, const TEv *e) {
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 128;
        TEv *p = realloc(s->ev, sizeof(TEv) * (size_t)cap);
        if (!p) return false;
        s->ev = p;
        s->cap = cap;
    }
    s->ev[s->n++] = *e;
    return true;
}

/* Turns (already loop-expanded) notation into events. Returns false with *bad set to the offending text on a mistake. */
static bool mml_parse(const ChipSong *song, const char *text, int def_inst, Seq *seq, const char **bad) {
    int octave = 4, deflen = 8, vol = 12, inst = def_inst, gate = 7;
    uint32_t t = 0;
    bool tie = false;
    const char *p = text;
    memset(seq, 0, sizeof *seq);
    while (*p) {
        const char *at = p;
        char c = *p++;
        if (isspace((unsigned char)c) || c == '|') continue;
        if (c == 'o') { octave = read_int(&p, 4); if (octave > 9) octave = 9; continue; }
        if (c == '<') { if (octave > 0) octave--; continue; }
        if (c == '>') { if (octave < 9) octave++; continue; }
        if (c == 'l') { deflen = read_int(&p, 8); if (deflen < 1) deflen = 8; continue; }
        if (c == 'v') { vol = read_int(&p, 12); if (vol > 15) vol = 15; continue; }
        if (c == 'q') { gate = read_int(&p, 7); if (gate < 1) gate = 1; if (gate > 8) gate = 8; continue; }
        if (c == '&') { tie = true; continue; }
        if (c == 'w') { uint32_t w = (uint32_t)read_int(&p, 0); t += w; continue; }
        if (c == '@') {
            if (isdigit((unsigned char)*p)) inst = read_int(&p, 0);
            else {
                const char *s = p;
                while (isalnum((unsigned char)*p) || *p == '-') p++;
                inst = find_instrument(song, s, (size_t)(p - s));
            }
            if (inst < 0 || inst >= song->ninst) { *bad = at; return false; }
            continue;
        }
        int semi;
        switch (c) {
        case 'c': semi = 0; break;  case 'd': semi = 2; break;  case 'e': semi = 4; break;  case 'f': semi = 5; break;
        case 'g': semi = 7; break;  case 'a': semi = 9; break;  case 'b': semi = 11; break;
        case 'r': semi = -1; break;
        default: *bad = at; return false;
        }
        int midi = 0;
        if (semi >= 0) {
            while (*p == '+' || *p == '#' || *p == '-') { semi += (*p == '-') ? -1 : 1; p++; }
            midi = 12 * (octave + 1) + semi;
            if (midi < 1) midi = 1;
            if (midi > 126) midi = 126;
        }
        uint32_t len = read_length(&p, deflen);
        if (tie && seq->n > 0) {                                    /* extend the previous note */
            seq->ev[seq->n - 1].dur += len;
            t += len;
            tie = false;
            continue;
        }
        tie = false;
        if (semi >= 0) {
            TEv e = { t, len, (uint8_t)midi, (uint8_t)vol, (uint8_t)inst, (uint8_t)gate };
            if (!seq_add(seq, &e)) { *bad = at; return false; }
        }
        t += len;
    }
    seq->total = t;
    return true;
}

/* ------------------------------------------------------------------ song files */

typedef struct {
    char name[16];
    uint32_t ticks;
} Pattern;

/* Everything the parser allocates besides the song itself, so that every exit path can free it the same way. */
typedef struct {
    Pattern pat[MAX_PATTERNS];
    int npat;
    Buf text[MAX_PATTERNS][CHIP_MAX_CHANNELS];       /* the notation of each channel in each pattern, as written (one line, continuations joined) */
    char *expanded[MAX_PATTERNS][CHIP_MAX_CHANNELS]; /* the same with the loops expanded */
    int order[MAX_ORDER];
    int norder;
    bool warned[MAX_PATTERNS][CHIP_MAX_CHANNELS];
} Ctx;

static void ctx_free(Ctx *x) {
    if (!x) return;
    for (int i = 0; i < MAX_PATTERNS; i++)
        for (int c = 0; c < CHIP_MAX_CHANNELS; c++) { free(x->text[i][c].s); free(x->expanded[i][c]); }
    free(x);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static ChipWave wave_by_name(const char *s, bool *ok) {
    static const struct { const char *name; ChipWave w; } t[] = {
        { "square", CHIP_SQUARE }, { "pulse50", CHIP_SQUARE }, { "pulse25", CHIP_PULSE25 }, { "pulse12", CHIP_PULSE12 },
        { "triangle", CHIP_TRIANGLE }, { "saw", CHIP_SAW }, { "noise", CHIP_NOISE }, { "fm", CHIP_FM },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(s, t[i].name)) { *ok = true; return t[i].w; }
    *ok = false;
    return CHIP_SQUARE;
}

static bool apply_inst_key(ChipInstrument *in, const char *key, const char *val) {
    if (!strcmp(key, "a")) in->attack_ms = (float)atof(val);
    else if (!strcmp(key, "d")) in->decay_ms = (float)atof(val);
    else if (!strcmp(key, "s")) in->sustain = (float)atof(val);
    else if (!strcmp(key, "r")) in->release_ms = (float)atof(val);
    else if (!strcmp(key, "vol")) in->volume = (float)atof(val);
    else if (!strcmp(key, "ratio")) in->fm_ratio = (float)atof(val);
    else if (!strcmp(key, "index")) in->fm_index = (float)atof(val);
    else if (!strcmp(key, "fb")) in->fm_feedback = (float)atof(val);
    else if (!strcmp(key, "idecay")) in->fm_index_decay_ms = (float)atof(val);
    else if (!strcmp(key, "vib")) {
        in->vibrato_semitones = (float)atof(val);
        const char *slash = strchr(val, '/');
        in->vibrato_hz = slash ? (float)atof(slash + 1) : 5.5f;
    } else if (!strcmp(key, "name")) {
        snprintf(in->name, sizeof in->name, "%s", val);
    } else return false;
    return true;
}

void chip_song_free(ChipSong *s) {
    if (!s) return;
    for (int i = 0; i < CHIP_MAX_CHANNELS; i++) free(s->mml[i]);
    free(s);
}

/* The line "word: notation" names a channel? Returns its index, -1 if there is a colon but no such channel, -2 if no colon. */
static int channel_line(const ChipSong *song, const char *line, const char **notation) {
    const char *colon = strchr(line, ':');
    if (!colon) return -2;
    size_t n = (size_t)(colon - line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
    *notation = colon + 1;
    for (int c = 0; c < song->nchannels; c++)
        if (strlen(song->def[c].name) == n && !strncmp(song->def[c].name, line, n)) return c;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)line[i]) && line[i] != '-' && line[i] != '_') return -2;   /* a colon in some other text */
    return n ? -1 : -2;
}

#define FAIL(...) do { set_err(err, errsz, __VA_ARGS__); goto fail; } while (0)

ChipSong *chip_song_parse(const char *text, char *err, size_t errsz) {
    ChipSong *song = calloc(1, sizeof *song);
    Ctx *x = calloc(1, sizeof *x);
    char *copy = NULL;
    int cur = -1, cur_ch = -1, lineno = 0;
    if (!song || !x) { set_err(err, errsz, "out of memory"); goto fail; }
    size_t len = strlen(text);
    if (len > MAX_TEXT) FAIL("the song file is too large");
    copy = malloc(len + 1);
    if (!copy) FAIL("out of memory");
    memcpy(copy, text, len + 1);

    song->bpm = 100;
    memcpy(song->inst, g_bank, sizeof g_bank);
    song->ninst = BANK_N;
    snprintf(song->title, sizeof song->title, "Untitled");

    char *save = NULL;
    for (char *raw = copy; raw; raw = save) {
        char *nl = strchr(raw, '\n');
        if (nl) { *nl = 0; save = nl + 1; } else save = NULL;
        lineno++;
        bool indented = raw[0] == ' ' || raw[0] == '\t';
        char *semi = strchr(raw, ';');
        if (semi) *semi = 0;
        char *line = trim(raw);
        if (!*line) continue;

        if (cur >= 0) {                                              /* inside a pattern: "channel: notation" lines and indented continuations */
            const char *notation = NULL;
            int ch = channel_line(song, line, &notation);
            if (ch == -1 && !indented) FAIL("line %d: there is no channel called '%.*s'", lineno, (int)(strchr(line, ':') - line), line);
            if (ch >= 0) {
                cur_ch = ch;
                Buf *b = &x->text[cur][ch];
                char *n = trim((char *)notation);
                if ((b->n && !buf_str(b, " ")) || !buf_str(b, n)) FAIL("line %d: out of memory", lineno);
                continue;
            }
            if (indented && cur_ch >= 0) {
                Buf *b = &x->text[cur][cur_ch];
                if (!buf_str(b, " ") || !buf_str(b, line)) FAIL("line %d: out of memory", lineno);
                continue;
            }
            cur = -1;                                                /* an unindented directive ends the pattern */
            cur_ch = -1;
        }

        char *word = line, *rest = line;
        while (*rest && *rest != ' ' && *rest != '\t') rest++;
        if (*rest) { *rest++ = 0; rest = trim(rest); }
        if (!strcmp(word, "tms")) {
            if (atoi(rest) != 1) FAIL("line %d: unknown format version '%s'", lineno, rest);
        } else if (!strcmp(word, "title")) snprintf(song->title, sizeof song->title, "%s", rest);
        else if (!strcmp(word, "mood")) snprintf(song->mood, sizeof song->mood, "%s", rest);
        else if (!strcmp(word, "author")) snprintf(song->author, sizeof song->author, "%s", rest);
        else if (!strcmp(word, "tempo")) {
            song->bpm = atoi(rest);
            if (song->bpm < 30 || song->bpm > 400) FAIL("line %d: the tempo must be between 30 and 400", lineno);
        } else if (!strcmp(word, "loop")) song->loop = !strcmp(rest, "yes") || !strcmp(rest, "on") || !strcmp(rest, "true");
        else if (!strcmp(word, "inst")) {
            char *num = rest, *kind = rest;
            while (*kind && *kind != ' ') kind++;
            if (*kind) *kind++ = 0;
            kind = trim(kind);
            char *args = kind;
            while (*args && *args != ' ') args++;
            if (*args) *args++ = 0;
            int n = atoi(num);
            if (!isdigit((unsigned char)num[0]) || n < 0 || n >= CHIP_MAX_INSTRUMENTS) FAIL("line %d: instrument numbers run from 0 to %d", lineno, CHIP_MAX_INSTRUMENTS - 1);
            bool ok;
            ChipInstrument in;
            memset(&in, 0, sizeof in);
            in.wave = wave_by_name(kind, &ok);
            if (!ok) FAIL("line %d: unknown instrument kind '%s' (square, pulse25, pulse12, triangle, saw, noise, fm)", lineno, kind);
            in.sustain = 0.6f; in.decay_ms = 100; in.release_ms = 60; in.volume = 1.0f;
            if (in.wave == CHIP_FM) { in.fm_ratio = 1; in.fm_index = 1; }
            snprintf(in.name, sizeof in.name, "inst%d", n);
            for (char *tok = strtok(args, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *eq = strchr(tok, '=');
                if (!eq) FAIL("line %d: '%s' should be key=value", lineno, tok);
                *eq = 0;
                if (!apply_inst_key(&in, tok, eq + 1)) FAIL("line %d: unknown instrument setting '%s'", lineno, tok);
            }
            song->inst[n] = in;
            if (n >= song->ninst) song->ninst = n + 1;
        } else if (!strcmp(word, "channel")) {
            if (song->nchannels >= CHIP_MAX_CHANNELS) FAIL("line %d: at most %d channels", lineno, CHIP_MAX_CHANNELS);
            char *args = rest;
            while (*args && *args != ' ') args++;
            if (*args) *args++ = 0;
            if (!*rest || strlen(rest) >= sizeof song->def[0].name) FAIL("line %d: a channel needs a short name", lineno);
            ChipChannelDef *d = &song->def[song->nchannels];
            snprintf(d->name, sizeof d->name, "%s", rest);
            d->gain = 1.0f;
            for (char *tok = strtok(args, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *eq = strchr(tok, '=');
                if (!eq) FAIL("line %d: '%s' should be key=value", lineno, tok);
                *eq = 0;
                if (!strcmp(tok, "inst")) {
                    if (isdigit((unsigned char)eq[1])) d->instrument = atoi(eq + 1);
                    else d->instrument = find_instrument(song, eq + 1, strlen(eq + 1));
                    if (d->instrument < 0 || d->instrument >= song->ninst) FAIL("line %d: no instrument '%s'", lineno, eq + 1);
                } else if (!strcmp(tok, "pan")) d->pan = (float)atof(eq + 1);
                else if (!strcmp(tok, "gain")) d->gain = (float)atof(eq + 1);
                else FAIL("line %d: unknown channel setting '%s'", lineno, tok);
            }
            song->nchannels++;
        } else if (!strcmp(word, "pattern")) {
            if (!*rest || strlen(rest) >= sizeof x->pat[0].name) FAIL("line %d: a pattern needs a short name", lineno);
            if (x->npat >= MAX_PATTERNS) FAIL("line %d: too many patterns", lineno);
            if (!song->nchannels) FAIL("line %d: define the channels before the patterns", lineno);
            for (int i = 0; i < x->npat; i++) if (!strcmp(x->pat[i].name, rest)) FAIL("line %d: pattern '%s' is defined twice", lineno, rest);
            snprintf(x->pat[x->npat].name, sizeof x->pat[x->npat].name, "%s", rest);
            cur = x->npat++;
            cur_ch = -1;
        } else if (!strcmp(word, "order")) {
            for (char *tok = strtok(rest, " \t"); tok; tok = strtok(NULL, " \t")) {
                int times = 1;
                char *star = strchr(tok, '*');
                if (star) { *star = 0; times = atoi(star + 1); if (times < 1 || times > 64) FAIL("line %d: bad repeat count after '%s*'", lineno, tok); }
                int pi = -1;
                for (int i = 0; i < x->npat; i++) if (!strcmp(x->pat[i].name, tok)) pi = i;
                if (pi < 0) FAIL("line %d: no pattern called '%s'", lineno, tok);
                for (int k = 0; k < times; k++) {
                    if (x->norder >= MAX_ORDER) FAIL("line %d: the order list is too long", lineno);
                    x->order[x->norder++] = pi;
                }
            }
        } else {
            FAIL("line %d: don't know '%s'", lineno, word);
        }
    }

    if (!song->nchannels) FAIL("the song defines no channel");
    if (!x->npat) FAIL("the song defines no pattern");
    if (!x->norder) for (int i = 0; i < x->npat; i++) x->order[x->norder++] = i;

    /* measure every pattern channel (with the start-of-pattern defaults) to find each pattern's length */
    for (int pi = 0; pi < x->npat; pi++) {
        for (int c = 0; c < song->nchannels; c++) {
            Buf ex = { 0 };
            const char *src = x->text[pi][c].s ? x->text[pi][c].s : "";
            if (!expand(&src, &ex, 0, false)) { free(ex.s); FAIL("pattern %s, %s: unbalanced [ ] in the notation", x->pat[pi].name, song->def[c].name); }
            x->expanded[pi][c] = ex.s ? ex.s : strdup("");
            Seq seq;
            const char *bad = NULL;
            if (!mml_parse(song, x->expanded[pi][c], song->def[c].instrument, &seq, &bad)) {
                free(seq.ev);
                FAIL("pattern %s, %s: cannot read the notation at '%.12s'", x->pat[pi].name, song->def[c].name, bad ? bad : "?");
            }
            if (seq.total > x->pat[pi].ticks) x->pat[pi].ticks = seq.total;
            free(seq.ev);
        }
        if (!x->pat[pi].ticks) FAIL("pattern %s is empty", x->pat[pi].name);
    }

    /* assemble each channel: for every pattern in the order, the defaults, the notation, and a wait that pads it to the pattern's length */
    for (int c = 0; c < song->nchannels; c++) {
        Buf out = { 0 };
        for (int k = 0; k < x->norder; k++) {
            int pi = x->order[k];
            char head[64];
            snprintf(head, sizeof head, "o4 l8 v12 q7 @%d ", song->def[c].instrument);
            if (!buf_str(&out, head) || !buf_str(&out, x->expanded[pi][c])) { free(out.s); FAIL("out of memory"); }
            Seq seq;
            const char *bad = NULL;
            mml_parse(song, x->expanded[pi][c], song->def[c].instrument, &seq, &bad);
            uint32_t have = seq.total;
            free(seq.ev);
            if (have < x->pat[pi].ticks) {
                char w[32];
                snprintf(w, sizeof w, " w%u ", (unsigned)(x->pat[pi].ticks - have));
                buf_str(&out, w);
                if (have && !x->warned[pi][c]) {
                    x->warned[pi][c] = true;
                    size_t used = strlen(song->warnings);
                    if (used + 70 < sizeof song->warnings)
                        snprintf(song->warnings + used, sizeof song->warnings - used, "pattern %s: %s is %u ticks short; ", x->pat[pi].name, song->def[c].name,
                                 (unsigned)(x->pat[pi].ticks - have));
                }
            }
            if (c == 0) song->ticks += x->pat[pi].ticks;
        }
        song->mml[c] = out.s ? out.s : strdup("");
    }

    ctx_free(x);
    free(copy);
    return song;

fail:
    ctx_free(x);
    chip_song_free(song);
    free(copy);
    return NULL;
}

ChipSong *chip_song_load(const char *path, char *err, size_t errsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { set_err(err, errsz, "cannot open %s", path); return NULL; }
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (n + 4096 > cap) {
            if (cap >= MAX_TEXT) { free(buf); close(fd); set_err(err, errsz, "the song file is too large"); return NULL; }
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    if (!buf) { set_err(err, errsz, "out of memory"); return NULL; }
    buf[n] = 0;
    ChipSong *s = chip_song_parse(buf, err, errsz);
    free(buf);
    return s;
}

/* ------------------------------------------------------------------ the synthesizer */

typedef struct { uint64_t start, gate; uint8_t note, vol, inst; } Ev;

typedef struct {
    Ev *ev;
    int n, next;
    /* the voice */
    bool on;
    const ChipInstrument *ins;
    double freq, phase, mphase;
    float vib, mprev, ienv, idec;
    int stage;                              /* 0 attack, 1 decay, 2 sustain, 3 release, 4 off */
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

static void voice_reset(Track *t) { t->on = false; t->next = 0; t->phase = 0; t->mphase = 0; t->vib = 0; t->env = 0; t->stage = 4; }

ChipPlayer *chip_new(const ChipSong *song, unsigned rate) {
    sine_init();
    ChipPlayer *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->song = song;
    p->rate = rate;
    double spt = (double)rate * 60.0 / ((double)song->bpm * (TICKS_PER_WHOLE / 4));        /* samples per tick */
    uint64_t end = 0;
    for (int c = 0; c < song->nchannels; c++) {
        Track *t = &p->tr[p->ntr];
        Seq seq;
        const char *bad = NULL;
        if (!mml_parse(song, song->mml[c], song->def[c].instrument, &seq, &bad)) { free(seq.ev); chip_free(p); return NULL; }
        t->ev = malloc(sizeof(Ev) * (size_t)(seq.n ? seq.n : 1));
        if (!t->ev) { free(seq.ev); chip_free(p); return NULL; }
        for (int i = 0; i < seq.n; i++) {
            t->ev[i].start = (uint64_t)((double)seq.ev[i].start * spt);
            t->ev[i].gate = t->ev[i].start + (uint64_t)((double)seq.ev[i].dur * spt * (double)seq.ev[i].gate / 8.0) + 1;
            t->ev[i].note = seq.ev[i].note;
            t->ev[i].vol = seq.ev[i].vol;
            t->ev[i].inst = seq.ev[i].inst;
        }
        t->n = seq.n;
        free(seq.ev);
        double g = song->def[c].gain > 0 ? song->def[c].gain : 1.0;
        double ang = ((double)song->def[c].pan + 1.0) * M_PI / 4.0;
        t->gl = (float)(cos(ang) * g);
        t->gr = (float)(sin(ang) * g);
        t->rng = 0x1234567u + (uint32_t)c * 7919u;
        voice_reset(t);
        uint64_t e = (uint64_t)((double)song->ticks * spt) + (uint64_t)(0.4 * rate);
        if (e > end) end = e;
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
    const ChipInstrument *ins = &p->song->inst[e->inst < p->song->ninst ? e->inst : 0];
    t->ins = ins;
    t->on = true;
    t->freq = 440.0 * pow(2.0, ((double)e->note - 69.0) / 12.0);
    if (ins->wave != CHIP_NOISE || t->stage == 4) t->phase = 0;
    t->mphase = 0;
    t->mprev = 0;
    t->ienv = 1.0f;
    t->idec = ins->fm_index_decay_ms > 0 ? (float)exp(-1.0 / ((double)ins->fm_index_decay_ms * 0.001 * (double)p->rate)) : 1.0f;
    t->stage = ins->attack_ms > 0 ? 0 : 1;
    t->env = ins->attack_ms > 0 ? 0.0f : 1.0f;
    t->gate_end = e->gate;
    t->amp = (float)e->vol / 15.0f * ins->volume * 0.36f;
    if (ins->wave == CHIP_NOISE) {                                   /* every hit starts with a fresh random value */
        uint32_t x = t->rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        t->rng = x;
        t->nval = (float)(int32_t)x * (1.0f / 2147483648.0f);
    }
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
        t->vib += (float)(ins->vibrato_hz * inv_rate);
        if (t->vib >= 1.0f) t->vib -= 1.0f;
        float lfo = t->vib < 0.5f ? 4.0f * t->vib - 1.0f : 3.0f - 4.0f * t->vib;
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
    case CHIP_FM: {
        /* two operators: a modulator at ratio x the pitch (with a little feedback) bends the carrier's phase; the
         * modulation index fades with its own decay, which is what gives bells and electric pianos their attack */
        t->mphase += dt * (double)ins->fm_ratio;
        if (t->mphase >= 1.0) t->mphase -= (double)(int)t->mphase;
        float m = sine_cycles((float)t->mphase + ins->fm_feedback * 0.15915f * t->mprev);
        t->mprev = m;
        float dev = ins->fm_index * 0.15915f * t->ienv * m;
        t->ienv *= t->idec;
        s = sine_cycles(ph + dev);
        break;
    }
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
