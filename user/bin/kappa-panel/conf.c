/*
 * ~/.config/hakurei.conf: the desktop's small settings file.
 *
 * Plain text, one `key=value` per line; blank lines and lines starting with `#` are comments; unknown keys are kept as they
 * are. Writing replaces the file atomically (temporary file, then rename), so a crash cannot leave half a file behind.
 *
 *   clock24=1      24 hour clock (0 = 12 hour with AM/PM)
 */
#include "panel.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool conf_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(out, cap, "%s/.config/hakurei.conf", home);
    return true;
}

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    for (size_t n = strlen(s); n && isspace((unsigned char)s[n - 1]); ) s[--n] = 0;
}

bool conf_get(const char *key, char *value, size_t cap) {
    char path[300];
    if (!conf_path(path, sizeof path)) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool found = false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        if (!strcmp(k, key)) { snprintf(value, cap, "%s", v); found = true; }      /* the last one wins */
    }
    fclose(f);
    return found;
}

bool conf_set(const char *key, const char *value, char *err, size_t errcap) {
    char path[300], tmp[320], dir[300];
    if (!conf_path(path, sizeof path)) { snprintf(err, errcap, "There is no home folder to keep the settings in."); return false; }
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;                                               /* ~/.config */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) { snprintf(err, errcap, "Cannot create %s: %s", dir, strerror(errno)); return false; }

    /* read what is there, replace or append the key */
    char *old = NULL;
    size_t oldn = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            char *no = realloc(old, oldn + n + 1);
            if (!no) { free(old); fclose(f); snprintf(err, errcap, "Out of memory."); return false; }
            old = no;
            memcpy(old + oldn, buf, n);
            oldn += n;
        }
        fclose(f);
    }
    if (old) old[oldn] = 0;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { free(old); snprintf(err, errcap, "Cannot write %s: %s", tmp, strerror(errno)); return false; }
    bool replaced = false;
    if (old) {
        char *p = old;
        while (*p) {
            char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            char line[300];
            snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1), p);
            char probe[300];
            snprintf(probe, sizeof probe, "%s", line);
            trim(probe);
            char *eq = strchr(probe, '=');
            bool match = false;
            if (probe[0] && probe[0] != '#' && eq) {
                *eq = 0;
                trim(probe);
                match = !strcmp(probe, key);
            }
            if (match) { if (!replaced) fprintf(out, "%s=%s\n", key, value); replaced = true; }
            else fprintf(out, "%s\n", line);
            p += len + (nl ? 1 : 0);
        }
    } else {
        fprintf(out, "# HakureiDE settings (written by the Kappa Control Panel)\n");
    }
    if (!replaced) fprintf(out, "%s=%s\n", key, value);
    bool ok = fflush(out) == 0 && !ferror(out);
    fclose(out);
    free(old);
    if (!ok || rename(tmp, path) != 0) {
        snprintf(err, errcap, "Cannot save %s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}
