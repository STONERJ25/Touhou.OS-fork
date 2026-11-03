/* gap_thermal_read(): /proc/thermal ("key: value" lines, docs/SENSORS.md) as a struct. */
#include <gap_thermal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int gap_thermal_read(struct gap_thermal *t) {
    memset(t, 0, sizeof *t);
    t->power_mw = -1;
    strcpy(t->core_status, "-");
    strcpy(t->pkg_status, "-");
    FILE *f = fopen("/proc/thermal", "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *colon = strstr(line, ": ");
        if (!colon) continue;
        *colon = 0;
        const char *key = line, *val = colon + 2;
        if (!strcmp(key, "thermal")) t->present = strcmp(val, "no thermal sensor") != 0;
        else if (!strcmp(key, "reason")) copy(t->reason, sizeof t->reason, val);
        else if (!strcmp(key, "source")) t->synthetic = strncmp(val, "synthetic", 9) == 0;
        else if (!strcmp(key, "tjmax")) t->tjmax = atoi(val);
        else if (!strcmp(key, "core0.valid")) t->core_valid = !strcmp(val, "yes");
        else if (!strcmp(key, "core0.temp")) t->core_temp = atoi(val);
        else if (!strcmp(key, "core0.margin")) t->core_margin = atoi(val);
        else if (!strcmp(key, "core0.status")) copy(t->core_status, sizeof t->core_status, val);
        else if (!strcmp(key, "package.valid")) t->pkg_valid = !strcmp(val, "yes");
        else if (!strcmp(key, "package.temp")) t->pkg_temp = atoi(val);
        else if (!strcmp(key, "package.margin")) t->pkg_margin = atoi(val);
        else if (!strcmp(key, "package.status")) copy(t->pkg_status, sizeof t->pkg_status, val);
        else if (!strcmp(key, "ratio")) t->ratio = atoi(val);
        else if (!strcmp(key, "mhz")) t->mhz = atoi(val);
        else if (!strcmp(key, "power.mw")) t->power_mw = strcmp(val, "-") ? atol(val) : -1;
        else if (!strcmp(key, "power.window_ms")) t->power_window_ms = atol(val);
    }
    fclose(f);
    return 0;
}
