/* Putting one issue together: the date picks the stories, the machine supplies the system news. */
#include "news.h"
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool g_news_trace;

void news_trace(const char *fmt, ...) {
    if (!g_news_trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

void shuffle_indices(int *idx, int n, uint32_t seed) {
    uint32_t st = hash32(seed) | 1;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rng_next(&st) % (uint32_t)(i + 1));
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
}

char *pool_printf(Issue *is, const char *fmt, ...) {
    size_t room = sizeof is->pool - is->pool_len;
    if (room < 2) return (char *)"";
    char *out = is->pool + is->pool_len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, room, fmt, ap);
    va_end(ap);
    if (n < 0) return (char *)"";
    if ((size_t)n >= room) n = (int)room - 1;
    is->pool_len += (size_t)n + 1;
    return out;
}

static long days_from_civil(int y, int m, int d) {
    long yy = y - (m <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    long yoe = yy - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static const char *const k_months[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
static const char *const k_days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const k_pages[N_PAGES] = { "Front page", "The Shrine Gazette", "Around Gensokyo", "Features", "Classifieds" };

const char *issue_page_name(int page) { return page >= 0 && page < N_PAGES ? k_pages[page] : "?"; }

static const char *roman(int n, char *buf, size_t cap) {
    static const struct { int v; const char *s; } t[] = { { 1000, "M" }, { 900, "CM" }, { 500, "D" }, { 400, "CD" }, { 100, "C" }, { 90, "XC" },
                                                         { 50, "L" }, { 40, "XL" }, { 10, "X" }, { 9, "IX" }, { 5, "V" }, { 4, "IV" }, { 1, "I" } };
    buf[0] = 0;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) while (n >= t[i].v) { strncat(buf, t[i].s, cap - strlen(buf) - 1); n -= t[i].v; }
    return buf;
}

static void pick_distinct(int *out, int count, int n, uint32_t seed) {
    int idx[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) idx[i] = i;
    shuffle_indices(idx, n, seed);
    for (int i = 0; i < count; i++) out[i] = idx[i % n];
}

void issue_build(Issue *is, int y, int m, int d) {
    memset(is, 0, sizeof *is);
    is->year = y; is->month = m; is->day = d;
    is->daynum = days_from_civil(y, m, d);
    int wd = (int)((is->daynum + 4) % 7);
    if (wd < 0) wd += 7;
    is->weekday = wd;
    is->yday = (int)(is->daynum - days_from_civil(y, 1, 1)) + 1;
    char rom[24];
    snprintf(is->date_line, sizeof is->date_line, "%s, %d %s %d   |   Vol. %s, No. %d   |   Price: one rice ball", k_days[wd], d, k_months[m - 1], y,
             roman(y - 1989 > 0 ? y - 1989 : 1, rom, sizeof rom), is->yday);

    struct passwd *pw = getpwuid(getuid());
    if (pw) snprintf(is->user_name, sizeof is->user_name, "%s", pw->pw_gecos && pw->pw_gecos[0] ? pw->pw_gecos : pw->pw_name);
    else snprintf(is->user_name, sizeof is->user_name, "a valued reader");

    /* the stories of the day: a fresh shuffle of the pool for every date */
    int idx[128];
    int n = g_nfan_news < 128 ? g_nfan_news : 128;
    for (int i = 0; i < n; i++) idx[i] = i;
    shuffle_indices(idx, n, (uint32_t)is->daynum * 2246822519u + 3266489917u);
    is->nfan = n < 24 ? n : 24;
    for (int i = 0; i < is->nfan; i++) is->fan[i] = &g_fan_news[idx[i]];

    int ad_idx[64];
    int na = g_nads < 64 ? g_nads : 64;
    for (int i = 0; i < na; i++) ad_idx[i] = i;
    shuffle_indices(ad_idx, na, (uint32_t)is->daynum * 668265263u + 374761393u);
    is->nads = na < 10 ? na : 10;
    for (int i = 0; i < is->nads; i++) is->ads[i] = &g_ads[ad_idx[i]];

    /* each spell card appears once per cycle, in a different order every cycle */
    int cycle = (int)(is->daynum / g_ncards), pos = (int)(is->daynum % g_ncards);
    int cidx[64];
    int nc = g_ncards < 64 ? g_ncards : 64;
    for (int i = 0; i < nc; i++) cidx[i] = i;
    shuffle_indices(cidx, nc, (uint32_t)cycle * 2654435761u + 12345u);
    is->card = &g_cards[cidx[pos % nc]];

    uint32_t h = hash32((uint32_t)is->daynum + 99);
    is->weather = (int)(h % (uint32_t)g_nweather);
    static const int base[12] = { 3, 4, 9, 14, 19, 23, 27, 28, 23, 17, 10, 5 };
    is->temp_hi = base[m - 1] + (int)(hash32(h) % 7) - 3;
    is->temp_lo = is->temp_hi - 6 - (int)(hash32(h + 1) % 4);
    is->tip = (int)((is->daynum + 3) % g_ntips);
    pick_distinct(is->letters, 3, g_nletters, (uint32_t)is->daynum + 41);
    pick_distinct(is->overheard, 3, g_noverheard, (uint32_t)is->daynum + 77);
    puzzle_build(&is->puzzle, is->daynum);
    sysnews_build(is);
}

/* ---------------------------------------------------------------- the paper as plain text */

static void print_article(const Article *a) {
    printf("[%s] %s\n", a->kicker ? a->kicker : "NEWS", a->headline);
    if (a->byline) printf("  %s\n", a->byline);
    printf("%s\n\n", a->body);
}

void issue_print(const Issue *is) {
    printf("THE BUNBUNMARU NEWS\n%s\nDelivered to: %s\n\n", is->date_line, is->user_name);
    printf("== PAGE 1: %s ==\n", k_pages[0]);
    if (is->nsys) print_article(&is->sys[is->sys_lead]);
    for (int i = 0; i < 2 && i < is->nfan; i++) print_article(is->fan[i]);
    printf("Weather: %s High %d, low %d.\n\n", g_weather[is->weather], is->temp_hi, is->temp_lo);
    printf("== PAGE 2: %s ==\n", k_pages[1]);
    for (int i = 0; i < is->nsys; i++) if (i != is->sys_lead) print_article(&is->sys[i]);
    printf("== PAGE 3: %s ==\n", k_pages[2]);
    for (int i = 2; i < is->nfan; i++) print_article(is->fan[i]);
    printf("== PAGE 4: %s ==\n", k_pages[3]);
    printf("Spell card of the day: %s (%s, %s)\n%s\n\n", is->card->name, is->card->owner, is->card->rating, is->card->text);
    for (int s = 0; s < 12; s++) printf("%s: %s\n", g_zodiac[s], g_fortunes[(hash32((uint32_t)is->daynum * 12 + (uint32_t)s)) % (uint32_t)g_nfortunes]);
    putchar('\n');
    puzzle_dump(&is->puzzle);
    printf("\n== PAGE 5: %s ==\n", k_pages[4]);
    for (int i = 0; i < is->nads; i++) printf("* %s\n", is->ads[i]->text);
    printf("\nTip: %s\n", g_tips[is->tip]);
}
