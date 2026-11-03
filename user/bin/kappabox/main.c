#include "kappabox.h"

struct KbApplet { const char *name; int (*fn)(int, char **); };
extern const struct KbApplet kb_applets[];
extern const int kb_napplets;

const char *kb_name = "kappabox";

static const char *base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv) {
    const char *name = base(argv[0]);
    if (!strcmp(name, "kappabox")) {
        if (argc < 2 || !strcmp(argv[1], "--list")) {
            printf("kappabox -- Nitori's toolbox, %d applets:\n", kb_napplets);
            int col = 0;
            for (int i = 0; i < kb_napplets; i++) {
                printf("%-14s", kb_applets[i].name);
                if (++col % 6 == 0) putchar('\n');
            }
            if (col % 6) putchar('\n');
            return 0;
        }
        argv++;
        argc--;
        name = argv[0];
    }
    kb_name = name;
    kb_color = isatty(1);
    for (int i = 0; i < kb_napplets; i++)
        if (!strcmp(kb_applets[i].name, name)) return kb_applets[i].fn(argc, argv);
    fprintf(stderr, "kappabox: applet '%s' not found (Nitori has not built it yet)\n", name);
    return 127;
}
