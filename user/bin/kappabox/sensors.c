/*
 * sensors: the CPU's temperature, its limit, its frequency and the package power on one screen, from /proc/thermal (docs/SENSORS.md).
 *
 *   sensors                the readings (two looks half a second apart, so that the power is a fresh measurement)
 *   sensors -r             /proc/thermal as the kernel writes it
 *   sensors --probe-lpc    (root) look for the chip that has the fan: writes key bytes to the Super I/O configuration ports, see sensors(1)
 *
 * The fan is not in the CPU. This says so, and does not pretend to know its speed.
 */
#include "kappabox.h"
#include <gap_thermal.h>

static int show_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { kb_error("%s: %s", path, strerror(errno)); return 1; }
    char line[512];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
    return 0;
}

static int probe_lpc(void) {
    int fd = open("/proc/superio", O_WRONLY);
    if (fd < 0) { kb_error("/proc/superio: %s (only root may look at the Super I/O ports)", strerror(errno)); return 1; }
    puts("looking at the Super I/O configuration ports 0x2e and 0x4e (key bytes go to their index registers; man sensors says exactly which)...");
    ssize_t n = write(fd, "probe\n", 6);
    close(fd);
    if (n != 6) { kb_error("the kernel refused the probe: %s", strerror(errno)); return 1; }
    return show_file("/proc/superio");
}

APPLET(sensors) {
    bool raw = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--raw")) raw = true;
        else if (!strcmp(argv[i], "--probe-lpc")) return probe_lpc();
        else { kb_error("usage: sensors [-r | --probe-lpc]"); return 2; }
    }
    if (raw) return show_file("/proc/thermal");

    struct gap_thermal t;
    if (gap_thermal_read(&t) < 0) { kb_error("/proc/thermal: %s (this kernel has no sensor interface)", strerror(errno)); return 1; }
    if (!t.present) {
        printf("no thermal sensor: %s\n", t.reason[0] ? t.reason : "the kernel reports none");
        return 1;
    }
    usleep(600000);                                                     /* the second look: the power is the energy used between the two */
    gap_thermal_read(&t);

    if (t.synthetic) puts("(synthetic readings: thermal.fake= is on the kernel command line)");
    if (t.core_valid) {
        printf("%-15s %3d C  (%d C below the limit)", "Core 0", t.core_temp, t.core_margin);
        if (strcmp(t.core_status, "-")) printf("  [%s]", t.core_status);
        putchar('\n');
    } else {
        printf("%-15s the sensor says its reading is not valid\n", "Core 0");
    }
    if (t.pkg_valid) {
        printf("%-15s %3d C  (%d C below the limit)", "Package", t.pkg_temp, t.pkg_margin);
        if (strcmp(t.pkg_status, "-")) printf("  [%s]", t.pkg_status);
        putchar('\n');
    }
    if (t.tjmax) printf("%-15s %3d C\n", "Limit (TjMax)", t.tjmax);
    if (t.mhz) printf("%-15s %4d MHz  (%d x %d MHz)\n", "Frequency", t.mhz, t.ratio, t.ratio ? t.mhz / t.ratio : 0);
    else if (t.ratio) printf("%-15s multiplier %d\n", "Frequency", t.ratio);
    if (t.power_mw >= 0) printf("%-15s %ld.%ld W  (average over %ld.%ld s)\n", "Package power", t.power_mw / 1000, t.power_mw % 1000 / 100, t.power_window_ms / 1000, t.power_window_ms % 1000 / 100);
    printf("%-15s not readable here: the fan is not on the CPU (man sensors; as root, sensors --probe-lpc looks for the chip that has it)\n", "Fan");
    return 0;
}
