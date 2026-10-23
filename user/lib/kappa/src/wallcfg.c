/* The wallpaper setting file, the pictures folder and screenshot names. No drawing code in here (the `wallpaper` and
 * `screenshot` commands link it without the rest of the toolkit). */
#include <kappa_media.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

static const char *const k_modes[] = { "cover", "fit", "center", "tile", "stretch" };

const char *k_wall_mode_name(KWallMode m) { return (unsigned)m < 5 ? k_modes[m] : "cover"; }

bool k_wall_mode_parse(const char *s, KWallMode *m) {
    for (int i = 0; i < 5; i++) if (!strcasecmp(s, k_modes[i])) { *m = (KWallMode)i; return true; }
    return false;
}

static bool user_conf_path(char *buf, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(buf, n, "%s/.config/hakurei/wallpaper", home);
    return true;
}

static bool read_conf(const char *file, KWallConfig *cfg) {
    int fd = open(file, O_RDONLY);
    if (fd < 0) return false;
    char text[1200];
    ssize_t r = read(fd, text, sizeof text - 1);
    close(fd);
    if (r <= 0) return false;
    text[r] = 0;
    int line = 0;
    cfg->path[0] = 0;
    cfg->mode = KW_COVER;
    for (char *p = strtok(text, "\r\n"); p; p = strtok(NULL, "\r\n")) {
        while (*p == ' ' || *p == '\t') p++;
        size_t l = strlen(p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
        if (!*p || *p == '#') continue;
        if (line == 0) { if (l >= sizeof cfg->path) return false; strcpy(cfg->path, p); }
        else if (line == 1) { KWallMode m; if (k_wall_mode_parse(p, &m)) cfg->mode = m; }
        line++;
    }
    return cfg->path[0] != 0;
}

bool k_wallpaper_config(KWallConfig *cfg) {
    char file[600];
    if (user_conf_path(file, sizeof file) && read_conf(file, cfg)) return true;
    return read_conf("/etc/hakurei/wallpaper", cfg);
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *s = tmp + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
        *s = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

int k_wallpaper_set(const char *path, KWallMode mode) {
    char file[600];
    if (!user_conf_path(file, sizeof file)) return -ENOENT;
    if (!path) return unlink(file) < 0 && errno != ENOENT ? -errno : 0;
    char dir[600];
    snprintf(dir, sizeof dir, "%s", file);
    *strrchr(dir, '/') = 0;
    if (mkdir_p(dir, 0755) < 0) return -errno;
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.new", file);
    FILE *f = fopen(tmp, "w");
    if (!f) return -errno;
    fprintf(f, "# the picture on the desktop; the second line is cover, fit, center, tile or stretch\n%s\n%s\n", path, k_wall_mode_name(mode));
    if (fclose(f) != 0) { unlink(tmp); return -EIO; }
    if (rename(tmp, file) < 0) { int e = errno; unlink(tmp); return -e; }
    return 0;
}

uint64_t k_wallpaper_stamp(void) {
    char file[600];
    KWallConfig cfg;
    struct stat st;
    uint64_t h = 1469598103934665603ULL;
    int any = 0;
    const char *candidates[2] = { NULL, "/etc/hakurei/wallpaper" };
    if (user_conf_path(file, sizeof file)) candidates[0] = file;
    for (int i = 0; i < 2; i++) {
        if (!candidates[i] || stat(candidates[i], &st) < 0) continue;
        any = 1;
        h = (h ^ (uint64_t)st.st_mtime) * 1099511628211ULL;
        h = (h ^ (uint64_t)st.st_size) * 1099511628211ULL;
    }
    if (!any) return 0;
    if (k_wallpaper_config(&cfg) && stat(cfg.path, &st) == 0) {
        h = (h ^ (uint64_t)st.st_mtime) * 1099511628211ULL;
        h = (h ^ (uint64_t)st.st_size) * 1099511628211ULL;
    }
    return h ? h : 1;
}

bool k_pictures_dir(char *buf, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(buf, n, "%s/Pictures", home);
    if (mkdir(buf, 0755) < 0 && errno != EEXIST) return false;
    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

void k_screenshot_path(char *buf, size_t n) {
    char dir[600];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[48];
    strftime(stamp, sizeof stamp, "Screenshot-%Y-%m-%d-%H%M%S", &tm);
    if (!k_pictures_dir(dir, sizeof dir)) snprintf(dir, sizeof dir, "/tmp");
    snprintf(buf, n, "%s/%s.png", dir, stamp);
    for (int i = 2; i < 100 && access(buf, F_OK) == 0; i++) snprintf(buf, n, "%s/%s-%d.png", dir, stamp, i);
}
