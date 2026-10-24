/* Keine's records: etckeep (which /etc files the HUMANVILLAGE volume keeps), timezone (show or set the zone for good).
 * The library half, including the checks and the atomic writes, is user/libc/src/etcstore.c; docs/PERSISTENT_ETC.md has the design. */
#include "kappabox.h"
#include <etcstore.h>
#include <sys/statfs.h>

static char *slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *data = kb_read_all(fd, len);
    close(fd);
    return data;
}

static void usage_etckeep(void) {
    kb_error("usage: etckeep [status] | etckeep check [FILE...] | etckeep save FILE... | etckeep forget FILE...\n"
             "  FILE is one of: passwd shadow group hostname timezone network.conf update.conf");
}

static int status(void) {
    int ro = 0;
    if (!etc_volume_present(&ro)) {
        printf("No HUMANVILLAGE volume is mounted: /etc lives in RAM and is rebuilt from the initrd at every boot.\n");
        return 0;
    }
    struct statfs st;
    statfs("/HumanVillage", &st);
    printf("Keine's records: %s on %s (%s)%s\n", ETC_KEEP_DIR, st.f_mntfromname, ro ? "read-only" : "read-write",
           ro ? ": changes last until the next restart" : "");
    bool root = geteuid() == 0;
    printf("%-14s %s\n", "FILE", "STATE");
    for (int i = 0; etc_kept_names[i]; i++) {
        const char *name = etc_kept_names[i];
        char kept[128], live[64];
        snprintf(kept, sizeof kept, "%s/%s", ETC_KEEP_DIR, name);
        snprintf(live, sizeof live, "/etc/%s", name);
        const char *state;
        if (!root) state = access("/HumanVillage/.system", X_OK) == 0 && access(kept, F_OK) == 0 ? "kept on the volume" : "(only root can look)";
        else {
            size_t klen = 0, llen = 0;
            char *k = slurp(kept, &klen), *l = slurp(live, &llen);
            char why[120];
            if (!k) state = "not kept: the initrd default is used at boot";
            else if (etc_validate(name, k, klen, why, sizeof why) < 0) state = "kept, but DAMAGED: the initrd default is used at boot (etckeep check)";
            else if (l && klen == llen && !memcmp(k, l, klen)) state = "kept on the volume, same as /etc";
            else state = "kept, but /etc differs: the volume's copy returns at the next boot (etckeep save keeps /etc)";
            free(k);
            free(l);
        }
        printf("%-14s %s\n", name, state);
    }
    return 0;
}

static int check(int argc, char **argv) {
    int rc = 0;
    for (int i = 0; etc_kept_names[i]; i++) {
        const char *name = etc_kept_names[i];
        bool wanted = argc == 0;
        for (int k = 0; k < argc; k++) if (!strcmp(argv[k], name)) wanted = true;
        if (!wanted) continue;
        char paths[2][128];
        snprintf(paths[0], sizeof paths[0], "/etc/%s", name);
        snprintf(paths[1], sizeof paths[1], "%s/%s", ETC_KEEP_DIR, name);
        for (int w = 0; w < 2; w++) {
            if (w == 1 && !etc_volume_present(NULL)) continue;
            size_t len = 0;
            char *data = slurp(paths[w], &len);
            if (!data) {
                if (errno != ENOENT) { printf("%s: cannot read: %s\n", paths[w], strerror(errno)); rc = 1; }
                continue;
            }
            char why[120];
            if (etc_validate(name, data, len, why, sizeof why) < 0) { printf("%s: damaged: %s\n", paths[w], why); rc = 1; }
            else printf("%s: fine\n", paths[w]);
            free(data);
        }
    }
    for (int k = 0; k < argc; k++) if (!etc_is_kept(argv[k])) { kb_error("%s is not one of the kept files", argv[k]); rc = 2; }
    return rc;
}

/* etckeep [status] | check [FILE...] | save FILE... | forget FILE... */
APPLET(etckeep) {
    if (argc < 2 || !strcmp(argv[1], "status")) return status();
    if (!strcmp(argv[1], "check")) return check(argc - 2, argv + 2);
    bool save = !strcmp(argv[1], "save"), forget = !strcmp(argv[1], "forget");
    if ((!save && !forget) || argc < 3) { usage_etckeep(); return 2; }
    if (geteuid() != 0) { kb_error("only root may change Keine's records (try: sudo etckeep %s ...)", argv[1]); return 1; }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (!etc_is_kept(argv[i])) { kb_error("%s is not one of the kept files", argv[i]); rc = 2; continue; }
        if (save) {
            int r = etc_save(argv[i]);
            if (r == 0) printf("etckeep: /etc/%s is kept on the volume\n", argv[i]);
            else { kb_error("/etc/%s was not kept: %s", argv[i], etc_note()); rc = 1; }
        } else {
            if (etc_forget(argv[i]) == 0) printf("etckeep: forgot %s: the initrd default returns at the next boot\n", argv[i]);
            else if (errno == ENOENT) printf("etckeep: %s was not kept anyway\n", argv[i]);
            else { kb_error("%s/%s: %s", ETC_KEEP_DIR, argv[i], strerror(errno)); rc = 1; }
        }
    }
    return rc;
}

/* timezone [ZONE]: shows the zone in effect, or (root) makes ZONE the system's zone in /etc/timezone, kept on the volume. */
APPLET(timezone) {
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) { kb_error("usage: timezone [ZONE]   (POSIX zones: UTC, JST-9, EST5EDT,M3.2.0,M11.1.0)"); return 2; }
    if (argc == 1) {
        const char *env = getenv("TZ");
        char file[96] = "";
        int fd = open("/etc/timezone", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, file, sizeof file - 1);
            close(fd);
            file[n > 0 ? n : 0] = 0;
            file[strcspn(file, "\r\n")] = 0;
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        long off = tm.tm_gmtoff / 60;
        printf("%s (%s, UTC%c%02ld:%02ld)\n", env && *env ? env : file[0] ? file : "UTC", tzname[tm.tm_isdst > 0], off < 0 ? '-' : '+',
               (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
        if (env && *env && strcmp(env, file)) printf("(from TZ in the environment, which wins over /etc/timezone: %s)\n", file[0] ? file : "none");
        return 0;
    }
    const char *zone = argv[1];
    if (geteuid() != 0) { kb_error("only root may set the time zone (try: sudo timezone %s)", zone); return 1; }
    setenv("TZ", zone, 1);
    tzset();
    if (!strcmp(tzname[0], "UTC") && strncmp(zone, "UTC", 3)) {
        kb_error("cannot understand the zone '%s': write it the POSIX way, e.g. JST-9 or EST5EDT,M3.2.0,M11.1.0 (names like Asia/Tokyo are not known)", zone);
        return 1;
    }
    char line[96];
    int n = snprintf(line, sizeof line, "%s\n", zone);
    if (n >= (int)sizeof line) { kb_error("the zone is too long"); return 1; }
    int rc = etc_commit("timezone", line, (size_t)n);
    if (rc < 0) { kb_error("%s", etc_note()); return 1; }
    printf("timezone: the shrine now keeps %s time\n", zone);
    if (rc > 0) fprintf(stderr, "timezone: the change lasts only until the next restart: %s\n", etc_note());
    unsetenv("TZ");
    return 0;
}
