/*
 * Gensokyo flavour: omikuji (shrine fortune slips), saisen (the offering box), reimusay and kappasay (speech bubbles).
 * All texts and drawings here are original to this project.
 */
#include "kappabox.h"

/* ------------------------------------------------------------------ small deterministic random numbers */

static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;

static unsigned long long rng_next(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1DULL;
}

static void rng_seed(unsigned long long s) {
    g_rng = s ? s : 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) rng_next();
}

static unsigned rng_below(unsigned n) { return (unsigned)((rng_next() >> 33) % n); }

static unsigned long long fnv1a(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

static size_t u8len(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

/* Breaks text into lines of at most `width` columns, words kept whole (long words are cut). Returns malloc'd lines. */
static char **wrap_text(const char *text, int width, int *count, int keep_lines) {
    char **out = NULL;
    int n = 0, cap = 0;
#define PUSH(str, len) do { if (n == cap) { cap = cap ? cap * 2 : 16; out = realloc(out, (size_t)cap * sizeof *out); } out[n++] = strndup((str), (len)); } while (0)
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (keep_lines) {
            PUSH(p, ll);
        } else if (ll == 0) {
            PUSH(p, 0);
        } else {
            size_t i = 0;
            while (i < ll) {
                while (i < ll && p[i] == ' ') i++;
                if (i >= ll) break;
                size_t start = i, last_space = (size_t)-1, cols = 0, j = i;
                while (j < ll && cols < (size_t)width) {
                    if (p[j] == ' ') last_space = j;
                    if (((unsigned char)p[j] & 0xC0) != 0x80) cols++;
                    j++;
                    while (j < ll && ((unsigned char)p[j] & 0xC0) == 0x80) j++;
                }
                size_t end = j;
                if (j < ll && p[j] != ' ' && last_space != (size_t)-1 && last_space > start) end = last_space;
                size_t e2 = end;
                while (e2 > start && p[e2 - 1] == ' ') e2--;
                PUSH(p + start, e2 - start);
                i = end;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
#undef PUSH
    *count = n;
    return out;
}

static void free_lines(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* ------------------------------------------------------------------ omikuji */

enum { C_WISH, C_TRAVEL, C_BUSINESS, C_HEALTH, C_LOST, C_LOVE, NCAT };

static const char *const cat_en[NCAT] = { "Wish", "Travel", "Business", "Health", "Lost item", "Love" };
static const char *const cat_jp[NCAT] = { "願望", "旅行", "商売", "健康", "失物", "恋愛" };

typedef struct {
    const char *romaji, *kanji, *meaning, *color;
    int weight;
    const char *general[2];
    const char *line[NCAT][2];
} Rank;

static const Rank ranks[6] = {
    { "Dai-kichi", "大吉", "great blessing", "1;31", 10,
      { "Even Yukari's gaps are lined up in your favour today.",
        "The shrine bell rang by itself. That has never once meant anything bad." },
      { { "It will come true, and sooner than you dared hope. Do not tell Marisa until it has.",
          "Wish boldly: today the wish and the wisher are pointing the same way." },
        { "Go anywhere. Every path is downhill and every ferry is on time, Komachi included.",
          "A long journey is blessed; even the fairies lose interest in pranking you." },
        { "Profit arrives by the shortest road. Sign things, but read them first.",
          "Your stall sells out before noon. If someone asks for a discount, give it: it returns tenfold." },
        { "Strong as a festival pole. Eat something warm anyway.",
          "Body and mind agree for once. Use it before they notice." },
        { "It is exactly where you left it, and it has missed you.",
          "Look under the thing you moved yesterday. It will be right there, slightly smug." },
        { "Someone is thinking of you, and for once it is not a fairy plotting something.",
          "Speak first. The answer has been rehearsed with you in mind." } } },

    { "Chu-kichi", "中吉", "middle blessing", "1;33", 22,
      { "A good day, with the usual small tax.",
        "The wind blows down from the shrine. Follow it, but bring a coat." },
      { { "It will come true if you carry it yourself instead of waiting for it to be delivered.",
          "Close, and getting closer. Water it daily like a plant in the shrine garden." },
        { "A good trip, though the way back is longer than the map admits.",
          "Take the mountain road. A kappa will offer directions: accept the cucumber, ignore the directions." },
        { "Steady gains. Keep a record, as Keine would.",
          "A partner is more useful than you think. Feed them tea." },
        { "Fine, with a small ache that is only the weather talking.",
          "Sleep more than you plan to. Eirin would agree, and she is usually right about this." },
        { "It will turn up within three days, in a place you have already checked once.",
          "Ask a friendly neighbour. Someone has been guarding it for you." },
        { "Pleasant company ahead. Do not rush it: tea takes as long as it takes.",
          "A kind word lands well today. Be sincere; Satori can tell, and so can everyone else." } } },

    { "Sho-kichi", "小吉", "small blessing", "1;32", 20,
      { "A small luck, like finding coins in the offering box that are not yours.",
        "Modest and pleasant. Do not ask for more." },
      { { "Part of it comes true. Be pleased with that part and ask nicely for the rest.",
          "It will come true, at a size you did not order." },
        { "A short trip suits you. Longer ones need better shoes.",
          "The road is dry. The destination is smaller than advertised, and nicer." },
        { "Small but sure profit. A coin saved today is a coin Tewi cannot lose for you tomorrow.",
          "Sales are slow in the morning and fine by evening. Be patient." },
        { "Good enough. Walk, drink water, avoid the strange mushrooms.",
          "A little tired. A short nap would be well spent." },
        { "It is nearby and not in a pocket. Check the pocket anyway.",
          "A child or a fairy has moved it. It is perfectly safe, and you will hear about it." },
        { "A mild breeze, nothing more. Mild breezes are underrated.",
          "A friendship is warming into something. Give it time and a good cup." } } },

    { "Kichi", "吉", "blessing", "32", 24,
      { "Good fortune, plainly delivered.",
        "A day to do what you meant to do. Nothing hidden, nothing demanded." },
      { { "It will come true if you stop describing it and start doing it.",
          "The gods have heard you. They are slow, not deaf." },
        { "A fine journey, if you leave before the sun gets lazy.",
          "Safe travel. Carry something to read, in case Patchouli's recommendation is the only company you have." },
        { "A fair year for fair work. Do not cut corners; Eiki would notice.",
          "Honest dealings pay off, slowly, like interest at a very polite bank." },
        { "Sound. Keep to your routine and the routine will keep you.",
          "Good, as long as you stay away from midnight tea parties at the mansion." },
        { "It will be found, probably by someone else, and handed back with a story.",
          "Retrace your steps. It was patient enough to wait." },
        { "Steady affection, the kind that does the dishes without being asked.",
          "An old acquaintance grows dearer. Say so, quietly." } } },

    { "Sue-kichi", "末吉", "future blessing", "36", 14,
      { "Luck is on its way but stopped for tea.",
        "The best of it comes later. Today is the preface." },
      { { "Not yet. Keep wishing; the shrine does not close.",
          "It will come true in its own good time, which is slower than yours." },
        { "Delay is likely. Use the waiting well; it is the best part of the trip.",
          "Travel is possible but the weather has opinions. Pack for two seasons." },
        { "Hold what you have and plan for spring. Sudden moves cost you today.",
          "Slow growth. Plant a bamboo shoot, not a firework." },
        { "Watch your rest. Small things get large when ignored.",
          "Fine if you are careful, tiresome if you are not." },
        { "It is further away than you hope, and in better condition than you fear.",
          "Look again tomorrow. Today it is hiding on purpose." },
        { "Wait. The wrong evening is not the right one, however pretty.",
          "A late bloom. Cherry trees do not apologise for their schedule." } } },

    { "Kyo", "凶", "misfortune", "1;35", 10,
      { "A bad day, but a short one. Tie this slip to the pine tree and walk away.",
        "Misfortune is a teacher with poor manners. Learn quickly and leave." },
      { { "Not this time. Leave the wish at the shrine and go for a walk.",
          "It will not come true as you wrote it. Rewrite it; the second draft may." },
        { "Stay home. Every road leads through a dispute with a fairy.",
          "Delays, detours and a ferryman on his break. Go next week." },
        { "Do not sign. Do not lend. Do not agree with Marisa about anything involving inventory.",
          "Losses are small if you notice them early. Count twice." },
        { "Rest. Do not test whether the mushroom is safe; it is not.",
          "A sniffle with ambitions. Warm drinks, early bed, no heroics." },
        { "It is gone, probably borrowed by someone who means to return it later (never).",
          "Do not search at night. Ask Sakuya; she knows where everything was a moment ago." },
        { "Silence is wiser than eloquence today. Say nothing you cannot unsay.",
          "Keep your distance from big decisions. They bite." } } },
};

static int pick_rank(void) {
    int total = 0;
    for (int i = 0; i < 6; i++) total += ranks[i].weight;
    int r = (int)rng_below((unsigned)total);
    for (int i = 0; i < 6; i++) {
        if (r < ranks[i].weight) return i;
        r -= ranks[i].weight;
    }
    return 3;
}

static void print_wrapped(const char *prefix_first, const char *prefix_rest, const char *text, int width, const char *border,
                          const char *style_first) {
    int n;
    char **lines = wrap_text(text, width, &n, 0);
    for (int i = 0; i < n; i++) {
        printf("%s%s%s", border, i ? prefix_rest : prefix_first, i ? "" : style_first);
        fputs(lines[i], stdout);
        if (!i && style_first[0]) fputs("\033[0m", stdout);
        putchar('\n');
    }
    free_lines(lines, n);
}

static int omikuji_main(int argc, char **argv, int force_random) {
    int random_mode = force_random, ascii = 0, kanji = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "rakh")) != -1) {
        switch (c) {
        case 'r': random_mode = 1; break;
        case 'a': ascii = 1; break;
        case 'k': kanji = 1; break;
        case 'h':
            printf("usage: %s [-r] [-a] [-k]\n  -r  draw a fresh slip instead of today's\n  -a  plain ASCII: no box-drawing characters\n"
                   "  -k  add the kanji (for terminals with a CJK font; the console font has none)\n", kb_name);
            return 0;
        default: return 2;
        }
    }
    if (ascii) kanji = 0;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[16], who[48];
    snprintf(date, sizeof date, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(who, sizeof who, "%s", kb_user_name(getuid()));
    if (random_mode) {
        unsigned long long seed = 0;
        if (gap_getrandom(&seed, sizeof seed) < 0) seed = (unsigned long long)now * 6364136223846793005ULL + (unsigned long long)gap_time_ms();
        rng_seed(seed);
    } else {
        char key[96];
        snprintf(key, sizeof key, "%u:%s", getuid(), date);
        rng_seed(fnv1a(key));
    }
    int rk = pick_rank();
    const Rank *r = &ranks[rk];
    int gen = (int)rng_below(2);
    int pick[NCAT];
    for (int i = 0; i < NCAT; i++) pick[i] = (int)rng_below(2);

    int cols = kb_term_cols();
    int fancy = kb_color;                        /* a terminal: box and colour */
    int width = (cols > 78 ? 78 : cols) - 6;
    if (width < 24) width = 24;

    if (!fancy) {
        printf("Omikuji for %s, %s\n", who, date);
        if (!kanji) printf("Fortune: %s (%s)\n", r->romaji, r->meaning);
        else printf("Fortune: %s (%s, %s)\n", r->romaji, r->kanji, r->meaning);
        printf("  %s\n\n", r->general[gen]);
        for (int i = 0; i < NCAT; i++) printf("%-10s %s\n", cat_en[i], r->line[i][pick[i]]);
        return 0;
    }

    const char *B = ascii ? "\033[31m|\033[0m  " : "\033[31m│\033[0m  ";
    const char *top = ascii ? "+" : "┌";
    const char *bot = ascii ? "+" : "└";
    const char *hl = ascii ? "-" : "─";
    printf("\033[31m%s%s%s\033[0m \033[1m%s\033[0m \033[31m%s%s%s\033[0m %s \033[90m%s\033[0m\n", top, hl, hl, kanji ? "御神籤 Omikuji" : "Omikuji", hl, hl, hl, who, date);
    printf("%s\n", B);
    printf("%s\033[%sm", B, r->color);
    if (!kanji) printf("%s", r->romaji); else printf("%s  %s", r->kanji, r->romaji);
    printf("\033[0m  (%s)\n", r->meaning);
    print_wrapped("   ", "   ", r->general[gen], width, B, "\033[3m");
    printf("%s\n", B);
    for (int i = 0; i < NCAT; i++) {
        char label[64];
        if (!kanji) snprintf(label, sizeof label, "%-10s", cat_en[i]);
        else snprintf(label, sizeof label, "%s %-10s", cat_jp[i], cat_en[i]);       /* a kanji is two columns wide */
        int labw = kanji ? 15 : 10;
        int n;
        int tw = width - labw > 20 ? width - labw : 20;
        char **lines = wrap_text(r->line[i][pick[i]], tw, &n, 0);
        for (int k = 0; k < n; k++) {
            if (k == 0) printf("%s\033[1m%s\033[0m  %s\n", B, label, lines[k]);
            else printf("%s%*s  %s\n", B, labw, "", lines[k]);
        }
        free_lines(lines, n);
    }
    printf("\033[31m%s", bot);
    for (int i = 0; i < 40; i++) fputs(hl, stdout);
    printf("\033[0m\n");
    return 0;
}

APPLET(omikuji) { return omikuji_main(argc, argv, 0); }
APPLET(fortune) { return omikuji_main(argc, argv, 1); }

/* ------------------------------------------------------------------ saisen */

static const char *total_path(void) {
    static char path[64];
    mkdir("/var/lib", 0755);
    mkdir("/var/lib/saisen", 0755);
    if (access("/var/lib/saisen", W_OK) == 0) snprintf(path, sizeof path, "/var/lib/saisen/total");
    else snprintf(path, sizeof path, "/tmp/saisen-total");
    return path;
}

static unsigned long long read_total(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned long long v = 0;
    if (f) {
        if (fscanf(f, "%llu", &v) != 1) v = 0;
        fclose(f);
    }
    return v;
}

static int write_total(const char *path, unsigned long long v) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%llu\n", v);
    return fclose(f);
}

static const char *reimu_reaction(unsigned long long yen) {
    if (yen == 0) return "...Nothing? You clapped twice, bowed twice and left the box empty? I will remember this.";
    if (yen == 1) return "One yen. It is the thought that counts. Barely. Thank you.";
    if (yen < 5) return "A few coppers. Fine. Fine! Every little bit keeps the roof on.";
    if (yen == 5) return "Five yen, the lucky coin: go-en, a bond of fate. You and this shrine are now connected. Please connect again soon.";
    if (yen < 50) return "Thank you. That will buy a very small amount of tea. A very, very small amount.";
    if (yen < 100) return "Fifty and a bit? A coin with a hole in it: the gods can see right through it. Generous!";
    if (yen < 500) return "A hundred-yen coin! Tea AND a rice cracker tonight. The shrine is in your debt.";
    if (yen < 1000) return "A five-hundred-yen coin! Heavy and shiny. I will tell the gods you were polite.";
    if (yen < 5000) return "A thousand?! ...Are you feeling well? Sit down, have tea. Stay as long as you like.";
    if (yen < 10000) return "Is that real money? Hands off, Marisa, I can hear you thinking from here. Thank you, truly.";
    if (yen < 1000000) return "Ten thousand yen. I am speechless. The whole village is invited to a banquet, and you sit at the head of the table.";
    return "That is an alarming amount of money to carry through a gap. Yukari? Yukari, did you do this?";
}

APPLET(saisen) {
    int show_only = 0, c;
    for (int i = 1; i < argc; i++)                      /* "-5" would look like an option; give it a proper answer */
        if (argv[i][0] == '-' && isdigit((unsigned char)argv[i][1])) {
            kb_error("%s: you cannot take money out of the offering box (Reimu is watching)", argv[i]);
            return 1;
        }
    optind = 0;
    while ((c = getopt(argc, argv, "sh")) != -1) {
        if (c == 's') show_only = 1;
        else if (c == 'h') { printf("usage: saisen [-s] [AMOUNT]\n  AMOUNT yen goes into the offering box (default 5, the lucky coin)\n  -s     only show what the box holds\n"); return 0; }
        else return 2;
    }
    const char *path = total_path();
    unsigned long long total = read_total(path), amount = 5;
    if (show_only) {
        printf("The offering box holds %llu yen.\n", total);
        return 0;
    }
    if (optind < argc) {
        const char *a = argv[optind];
        char *end;
        if (a[0] == '-') { kb_error("%s: you cannot take money out of the offering box (Reimu is watching)", a); return 1; }
        errno = 0;
        amount = strtoull(a, &end, 10);
        if (end == a || (*end && strcmp(end, "yen") && strcmp(end, "en")) || errno) { kb_error("'%s' is not an amount of yen", a); return 1; }
    }
    if (total + amount < total) amount = ~0ULL - total;
    if (amount == 0) printf("You stand before the offering box, clap twice, and offer... nothing.\n");
    else printf("You toss %llu yen into the offering box.\n*clink*\n", amount);
    if (kb_color) printf("\033[1;31mReimu:\033[0m \"%s\"\n", reimu_reaction(amount));
    else printf("Reimu: \"%s\"\n", reimu_reaction(amount));
    unsigned long long now_total = total + amount;
    if (amount && write_total(path, now_total) < 0) kb_error("cannot update %s: %s", path, strerror(errno));
    printf("The box holds %llu yen%s.\n", now_total, amount ? "" : " (nothing added)");
    return 0;
}

/* ------------------------------------------------------------------ reimusay / kappasay */

#define TAIL "\x01"                                    /* replaced by \ (speaking) or o (thinking) */

static const char *const art_reimu[] = {
    "   " TAIL,
    "    " TAIL "        ,_      _,",
    "     " TAIL "      (@ `-..-' @)",
    "              '-.    .-'",
    "            .-\"\"\"\"\"\"\"\"\"\"-.",
    "           /   _      _   \\",
    "          |   (o)    (o)   |",
    "          |       <>       |",
    "       .-' \\    '----'    / '-.",
    "      /  .--`-.________.-'--.  \\",
    "     /  /   |  .-\"\"\"\"-.  |   \\  \\",
    "    |  |    | (  (@)   ) |    |  |",
    "     \\_/     \\ '-.__.-' /     \\_/",
    "              |  |  |  |",
    "              |__|  |__|",
    NULL,
};

static const char *const art_kappa[] = {
    "   " TAIL,
    "    " TAIL,
    "     " TAIL "      _________",
    "           .'~ ~ ~ ~ ~'.",
    "           '-._______.-'",
    "         .-\"\"\"\"\"\"\"\"\"\"\"\"\"-.",
    "        /    ___   ___    \\",
    "       |    (o  ) (  o)    |",
    "       |     `-'  _  `-'   |",
    "        \\      ('---')    /",
    "         `-.    `---'  .-'",
    "       ,---'`-._______.-'`---,",
    "      /  .--.  |       |  .--.  \\",
    "     |  /    \\ | ===== | /    \\  |",
    "      \\_\\____/  \\_____/  \\____/_/",
    NULL,
};

static int say_main(int argc, char **argv, const char *const *art, const char *who) {
    int width = 40, keep = 0, think = 0, c;
    optind = 0;
    while ((c = getopt(argc, argv, "W:nth")) != -1) {
        switch (c) {
        case 'W': width = atoi(optarg); if (width < 8) width = 8; break;
        case 'n': keep = 1; break;
        case 't': think = 1; break;
        case 'h': printf("usage: %s [-W WIDTH] [-n] [-t] [TEXT...]\n  Without TEXT, %s reads standard input.\n  -W  wrap the speech at WIDTH columns (default 40)\n  -n  do not wrap: keep the lines as they are\n  -t  a thought bubble instead of a speech bubble\n", kb_name, who); return 0;
        default: return 2;
        }
    }
    char *text;
    if (optind < argc) {
        size_t total = 1;
        for (int i = optind; i < argc; i++) total += strlen(argv[i]) + 1;
        text = malloc(total);
        if (!text) kb_die("out of memory");
        text[0] = 0;
        for (int i = optind; i < argc; i++) {
            if (i > optind) strcat(text, " ");
            strcat(text, argv[i]);
        }
    } else {
        text = kb_read_all(0, NULL);
    }
    size_t tl = strlen(text);
    while (tl && (text[tl - 1] == '\n' || text[tl - 1] == '\r')) text[--tl] = 0;
    if (!tl) { free(text); text = strdup("..."); }
    int n;
    char **lines = wrap_text(text, width, &n, keep);
    size_t maxw = 0;
    for (int i = 0; i < n; i++) {
        size_t w = u8len(lines[i], strlen(lines[i]));
        if (w > maxw) maxw = w;
    }
    putchar(' ');
    for (size_t i = 0; i < maxw + 2; i++) putchar('_');
    putchar('\n');
    for (int i = 0; i < n; i++) {
        char l = '|', r = '|';
        if (think) { l = '('; r = ')'; }
        else if (n == 1) { l = '<'; r = '>'; }
        else if (i == 0) { l = '/'; r = '\\'; }
        else if (i == n - 1) { l = '\\'; r = '/'; }
        size_t w = u8len(lines[i], strlen(lines[i]));
        printf("%c %s", l, lines[i]);
        for (size_t k = w; k < maxw; k++) putchar(' ');
        printf(" %c\n", r);
    }
    putchar(' ');
    for (size_t i = 0; i < maxw + 2; i++) putchar('-');
    putchar('\n');
    for (int i = 0; art[i]; i++) {
        for (const char *p = art[i]; *p; p++) putchar(*p == '\x01' ? (think ? 'o' : '\\') : *p);
        putchar('\n');
    }
    free_lines(lines, n);
    free(text);
    return 0;
}

APPLET(reimusay) { return say_main(argc, argv, art_reimu, "Reimu"); }
APPLET(kappasay) { return say_main(argc, argv, art_kappa, "Nitori"); }
