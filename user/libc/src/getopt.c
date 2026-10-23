/* GNU-compatible getopt and getopt_long: arguments are permuted so options may follow operands,
 * "+" as the first optstring character or POSIXLY_CORRECT in the environment turns that off, optind = 0 resets. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *optarg;
int optind = 1, opterr = 1, optopt = '?';

static char *nextchar;
static int first_nonopt, last_nonopt;

static int is_option(const char *a) { return a[0] == '-' && a[1]; }

/* Swap the block of operands [first_nonopt, last_nonopt) with the block of options [last_nonopt, optind). */
static void exchange(char **argv) {
    int bottom = first_nonopt, middle = last_nonopt, top = optind;
    int n1 = middle - bottom, n2 = top - middle;
    if (n1 <= 0 || n2 <= 0) return;
    char *tmp[64];
    if (n1 <= 64) {
        memcpy(tmp, &argv[bottom], (size_t)n1 * sizeof(char *));
        memmove(&argv[bottom], &argv[middle], (size_t)n2 * sizeof(char *));
        memcpy(&argv[bottom + n2], tmp, (size_t)n1 * sizeof(char *));
    } else {                                    /* rare: rotate by single steps */
        for (int k = 0; k < n1; k++) {
            char *first = argv[bottom];
            memmove(&argv[bottom], &argv[bottom + 1], (size_t)(top - bottom - 1) * sizeof(char *));
            argv[top - 1] = first;
        }
    }
    first_nonopt += n2;
    last_nonopt = top;
}

static int complain(const char *prog, const char *what, const char *arg, int c) {
    if (opterr) fprintf(stderr, "%s: %s '%s'\n", prog, what, arg);
    optopt = c;
    return '?';
}

int getopt_long(int argc, char *const argv_c[], const char *optstring, const struct option *longopts, int *longindex) {
    char **argv = (char **)argv_c;
    const char *prog = argv[0];
    int permute = 1;
    if (*optstring == '+') { permute = 0; optstring++; }
    else if (getenv("POSIXLY_CORRECT")) permute = 0;
    int colon = *optstring == ':';
    if (colon) optstring++;

    optarg = NULL;
    if (optind == 0) { optind = 1; first_nonopt = last_nonopt = 1; nextchar = NULL; }
    if (first_nonopt == 0) first_nonopt = last_nonopt = optind;

    if (!nextchar || !*nextchar) {
        if (last_nonopt > optind) last_nonopt = optind;
        if (first_nonopt > optind) first_nonopt = optind;
        if (permute) {
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(argv);
            else if (last_nonopt != optind) first_nonopt = optind;
            while (optind < argc && !is_option(argv[optind]) && strcmp(argv[optind], "--")) optind++;
            last_nonopt = optind;
        }
        if (optind != argc && !strcmp(argv[optind], "--")) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(argv);
            else if (first_nonopt == last_nonopt) first_nonopt = optind;
            last_nonopt = argc;
            optind = argc;
        }
        if (optind == argc) {
            if (first_nonopt != last_nonopt) optind = first_nonopt;
            return -1;
        }
        if (!is_option(argv[optind])) return -1;        /* POSIX mode: first operand ends the options */
        nextchar = argv[optind] + 1;
        if (longopts && argv[optind][1] == '-') nextchar = argv[optind] + 2;
    }

    /* ---- long option ---- */
    if (longopts && argv[optind][1] == '-') {
        const char *name = nextchar;
        size_t len = strcspn(name, "=");
        const struct option *hit = NULL;
        int idx = -1, ambiguous = 0;
        for (int i = 0; longopts[i].name; i++) {
            if (strncmp(longopts[i].name, name, len)) continue;
            if (strlen(longopts[i].name) == len) { hit = &longopts[i]; idx = i; ambiguous = 0; break; }
            if (hit) ambiguous = 1;
            else { hit = &longopts[i]; idx = i; }
        }
        if (ambiguous) { nextchar = NULL; optind++; return complain(prog, "option is ambiguous:", argv[optind - 1], 0); }
        if (!hit) { nextchar = NULL; optind++; return complain(prog, "unrecognized option", argv[optind - 1], 0); }
        optind++;
        nextchar = NULL;
        if (name[len] == '=') {
            if (hit->has_arg == no_argument) return complain(prog, "option doesn't take an argument:", argv[optind - 1], hit->val);
            optarg = (char *)name + len + 1;
        } else if (hit->has_arg == required_argument) {
            if (optind < argc) optarg = argv[optind++];
            else {
                if (opterr && !colon) fprintf(stderr, "%s: option '--%s' requires an argument\n", prog, hit->name);
                optopt = hit->val;
                return colon ? ':' : '?';
            }
        }
        if (longindex) *longindex = idx;
        if (hit->flag) { *hit->flag = hit->val; return 0; }
        return hit->val;
    }

    /* ---- short option ---- */
    char c = *nextchar++;
    const char *spec = strchr(optstring, c);
    if (!*nextchar) optind++;
    if (!spec || c == ':') {
        optopt = c;
        if (opterr && !colon) fprintf(stderr, "%s: invalid option -- '%c'\n", prog, c);
        return '?';
    }
    if (spec[1] == ':') {
        if (spec[2] == ':') {                           /* optional argument: only when attached */
            if (*nextchar) { optarg = nextchar; optind++; }
            nextchar = NULL;
        } else {
            if (*nextchar) { optarg = nextchar; optind++; }
            else if (optind >= argc) {
                optopt = c;
                nextchar = NULL;
                if (opterr && !colon) fprintf(stderr, "%s: option requires an argument -- '%c'\n", prog, c);
                return colon ? ':' : '?';
            } else optarg = argv[optind++];
            nextchar = NULL;
        }
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *optstring) {
    return getopt_long(argc, argv, optstring, NULL, NULL);
}
