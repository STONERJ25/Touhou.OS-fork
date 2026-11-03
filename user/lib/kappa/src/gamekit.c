/* kappa_gamekit: RNG, fixed timestep, FPS meter, settings files and high-score tables. */
#include <kappa_gamekit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- RNG */

void kg_rng_seed(KRng *r, uint32_t seed) {
    r->state = 0;
    kg_rng_next(r);
    r->state += seed;
    kg_rng_next(r);
}

uint32_t kg_rng_next(KRng *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t xs = (uint32_t)(((old >> 18) ^ old) >> 27);
    uint32_t rot = (uint32_t)(old >> 59);
    return (xs >> rot) | (xs << ((32 - rot) & 31));
}

int kg_rng_range(KRng *r, int n) {
    if (n <= 1) return 0;
    return (int)(kg_rng_next(r) % (uint32_t)n);
}

int kg_rng_between(KRng *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + kg_rng_range(r, hi - lo + 1);
}

uint32_t kg_seed_from_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t a = (uint32_t)ts.tv_nsec ^ (uint32_t)ts.tv_sec * 2654435761u;
    return a ^ (uint32_t)time(NULL) * 40503u;
}

/* ---------------------------------------------------------------- time */

uint64_t kg_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void kg_step_init(KStepper *st, int step_us, int max_steps) {
    memset(st, 0, sizeof *st);
    st->step_us = step_us;
    st->max_steps = max_steps < 1 ? 1 : max_steps;
}

void kg_step_set(KStepper *st, int step_us) { st->step_us = step_us; }

void kg_step_reset(KStepper *st) {
    st->started = false;
    st->acc_us = 0;
    st->last_steps = 0;
}

int kg_step_due(KStepper *st) {
    uint64_t now = kg_now_us();
    if (!st->started) {
        st->started = true;
        st->last_us = now;
        st->acc_us = st->step_us;                       /* the first call runs one step straight away */
    } else {
        st->acc_us += (int64_t)(now - st->last_us);
        st->last_us = now;
    }
    int n = 0;
    while (st->acc_us >= st->step_us && n < st->max_steps) { st->acc_us -= st->step_us; n++; }
    if (st->acc_us >= st->step_us) {                    /* still behind after the catch-up limit: drop the rest */
        st->dropped += (int)(st->acc_us / st->step_us);
        st->acc_us %= st->step_us;
    }
    st->last_steps = n;
    return n;
}

int kg_step_alpha256(const KStepper *st) {
    if (st->step_us <= 0) return 0;
    int64_t a = st->acc_us * 256 / st->step_us;
    return a < 0 ? 0 : a > 256 ? 256 : (int)a;
}

int kg_step_wait_ms(const KStepper *st) {
    int64_t left = st->step_us - st->acc_us - (int64_t)(kg_now_us() - st->last_us);
    if (left <= 0) return 1;
    int ms = (int)((left + 999) / 1000);
    return ms < 1 ? 1 : ms;
}

void kg_fps_frame(KFps *f, uint64_t work_us) {
    uint64_t now = kg_now_us();
    if (!f->t0) f->t0 = now;
    f->frames++;
    f->work_us += work_us;
    if (work_us > (uint64_t)f->max_acc) f->max_acc = (int)work_us;
    if (now - f->t0 >= 1000000u) {
        f->fps10 = (int)((uint64_t)f->frames * 10000000u / (now - f->t0));
        f->work_us_avg = (int)(f->work_us / (uint64_t)f->frames);
        f->work_us_max = f->max_acc;
        f->t0 = now;
        f->frames = 0;
        f->work_us = 0;
        f->max_acc = 0;
    }
}

/* ---------------------------------------------------------------- settings files */

bool kg_conf_path(char *out, size_t cap, const char *name) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    char dir[256];
    snprintf(dir, sizeof dir, "%s/.config", home);
    mkdir(dir, 0755);                                   /* already there is fine */
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap;
}

char *kg_conf_load(const char *name, size_t *len) {
    char path[320];
    if (!kg_conf_path(path, sizeof path, name)) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0 || n >= 1 << 20) break;
        if (n == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) break;
            buf = nb;
        }
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

bool kg_conf_save(const char *name, const void *data, size_t len) {
    char path[320], tmp[330];
    if (!kg_conf_path(path, sizeof path, name)) return false;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    size_t w = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    if (w != len) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) {                       /* no atomic rename available: write in place */
        unlink(tmp);
        f = fopen(path, "w");
        if (!f) return false;
        w = len ? fwrite(data, 1, len, f) : 0;
        fclose(f);
        return w == len;
    }
    return true;
}

/* ---------------------------------------------------------------- high scores */

void kg_scores_init(KScoreTable *t, bool ascending, int per_category) {
    memset(t, 0, sizeof *t);
    t->ascending = ascending;
    t->per_category = per_category < 1 ? 10 : per_category;
}

static bool better(const KScoreTable *t, long a, long b) { return t->ascending ? a < b : a > b; }   /* strictly better */

/* Index in e[] where a new entry would be inserted so the category stays sorted (after equal scores). */
static int insert_pos(const KScoreTable *t, int category, long score, int *place) {
    int pos = t->n, rank = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->e[i].category != category) continue;
        if (better(t, score, t->e[i].score)) { pos = i; break; }
        rank++;
    }
    *place = rank;
    return pos;
}

int kg_scores_rank(const KScoreTable *t, int category, long score) {
    int place;
    insert_pos(t, category, score, &place);
    return place < t->per_category ? place : -1;
}

int kg_scores_add(KScoreTable *t, int category, long score, const char *name, long extra0, long extra1) {
    int place;
    int pos = insert_pos(t, category, score, &place);
    if (place >= t->per_category || t->n >= KG_SCORE_MAX) return -1;
    memmove(&t->e[pos + 1], &t->e[pos], (size_t)(t->n - pos) * sizeof t->e[0]);
    KScoreEntry *e = &t->e[pos];
    memset(e, 0, sizeof *e);
    e->category = category;
    e->score = score;
    e->extra[0] = extra0;
    e->extra[1] = extra1;
    snprintf(e->name, sizeof e->name, "%s", name ? name : "");
    for (char *c = e->name; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    t->n++;
    int count = 0;                                      /* trim the category back to per_category entries */
    for (int i = 0; i < t->n; i++) {
        if (t->e[i].category != category) continue;
        if (++count > t->per_category) {
            memmove(&t->e[i], &t->e[i + 1], (size_t)(t->n - i - 1) * sizeof t->e[0]);
            t->n--;
            i--;
        }
    }
    return place;
}

int kg_scores_list(const KScoreTable *t, int category, const KScoreEntry **out, int max) {
    int n = 0;
    for (int i = 0; i < t->n && n < max; i++) if (t->e[i].category == category) out[n++] = &t->e[i];
    return n;
}

long kg_scores_best(const KScoreTable *t, int category, long none) {
    for (int i = 0; i < t->n; i++) if (t->e[i].category == category) return t->e[i].score;
    return none;
}

size_t kg_scores_format(const KScoreTable *t, char *buf, size_t cap) {
    size_t n = 0;
    if (cap) buf[0] = 0;
    for (int i = 0; i < t->n; i++) {
        const KScoreEntry *e = &t->e[i];
        int w = snprintf(buf + n, cap > n ? cap - n : 0, "%d %ld %ld %ld %s\n", e->category, e->score, e->extra[0], e->extra[1], e->name);
        if (w < 0 || n + (size_t)w >= cap) break;
        n += (size_t)w;
    }
    return n;
}

static const char *next_long(const char *p, long *out) {
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return NULL;
    *out = v;
    return end;
}

bool kg_scores_parse(KScoreTable *t, const char *text) {
    bool asc = t->ascending;
    int per = t->per_category;
    kg_scores_init(t, asc, per);
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[160];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = eol ? eol + 1 : NULL;
        long cat, score, e0, e1;
        const char *q = next_long(line, &cat);
        if (q) q = next_long(q, &score);
        if (q) q = next_long(q, &e0);
        if (q) q = next_long(q, &e1);
        if (!q) continue;
        while (*q == ' ') q++;
        char name[KG_SCORE_NAME];
        snprintf(name, sizeof name, "%s", q);
        for (char *c = name + strlen(name); c > name && (c[-1] == ' ' || c[-1] == '\r'); ) *--c = 0;
        kg_scores_add(t, (int)cat, score, name, e0, e1);
    }
    return true;
}

bool kg_scores_load(KScoreTable *t, const char *file) {
    char *text = kg_conf_load(file, NULL);
    if (!text) return false;
    kg_scores_parse(t, text);
    free(text);
    return true;
}

bool kg_scores_save(const KScoreTable *t, const char *file) {
    char buf[KG_SCORE_MAX * 64];
    size_t n = kg_scores_format(t, buf, sizeof buf);
    return kg_conf_save(file, buf, n);
}

/* ---------------------------------------------------------------- helpers */

int kg_isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

void kg_format_int(char *out, size_t cap, long v) {
    char tmp[32];
    bool neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    int n = 0;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; if (u && n % 4 == 3) tmp[n++] = ','; } while (u);
    size_t o = 0;
    if (neg && o + 1 < cap) out[o++] = '-';
    while (n && o + 1 < cap) out[o++] = tmp[--n];
    if (cap) out[o] = 0;
}
