/* Kernel command line: space-separated words and key=value pairs, from cmdline.txt on the boot volume. */
#include "cmdline.h"
#include "kernel.h"

static char g_cmdline[BOOTINFO_CMDLINE_MAX];

void cmdline_init(const char *s) {
    strlcpy(g_cmdline, s, sizeof g_cmdline);
}

const char *cmdline_string(void) {
    return g_cmdline;
}

/* Finds the start of the word equal to name (or starting with "name=" when with_eq). */
static const char *find_word(const char *name, bool with_eq) {
    size_t len = strlen(name);
    const char *p = g_cmdline;
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t wl = (size_t)(p - start);
        if (with_eq) {
            if (wl > len && start[len] == '=' && strncmp(start, name, len) == 0) return start + len + 1;
        } else if (wl == len && strncmp(start, name, len) == 0) {
            return start;
        }
    }
    return NULL;
}

bool cmdline_flag(const char *name) {
    return find_word(name, false) != NULL;
}

bool cmdline_value(const char *key, char *out, size_t n) {
    const char *v = find_word(key, true);
    if (!v) return false;
    size_t i = 0;
    while (v[i] && v[i] != ' ' && i + 1 < n) { out[i] = v[i]; i++; }
    out[i] = 0;
    return true;
}

long cmdline_int(const char *key, long def) {
    char buf[24];
    if (!cmdline_value(key, buf, sizeof buf)) return def;
    long v = 0;
    bool neg = false;
    const char *p = buf;
    if (*p == '-') { neg = true; p++; }
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}
