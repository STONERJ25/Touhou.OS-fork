/* /etc/passwd and /etc/group lookups, and supplementary group handling. */
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "syscall_priv.h"

static char g_line[512];
static struct passwd g_pw;
static struct group g_gr;
static char *g_members[33];
static FILE *g_pwf;

static int split(char *line, char **f, int max) {
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ':');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    char *nl = strchr(f[n - 1], '\n');
    if (nl) *nl = 0;
    return n;
}

static struct passwd *parse_pw(char *line) {
    char *f[7];
    if (split(line, f, 7) < 7) return NULL;
    g_pw.pw_name = f[0]; g_pw.pw_passwd = f[1]; g_pw.pw_uid = (uid_t)atoi(f[2]); g_pw.pw_gid = (gid_t)atoi(f[3]);
    g_pw.pw_gecos = f[4]; g_pw.pw_dir = f[5]; g_pw.pw_shell = f[6];
    return &g_pw;
}

static struct group *parse_gr(char *line) {
    char *f[4];
    if (split(line, f, 4) < 4) return NULL;
    g_gr.gr_name = f[0]; g_gr.gr_passwd = f[1]; g_gr.gr_gid = (gid_t)atoi(f[2]);
    int n = 0;
    char *save, *m;
    for (m = strtok_r(f[3], ",", &save); m && n < 32; m = strtok_r(NULL, ",", &save)) g_members[n++] = m;
    g_members[n] = NULL;
    g_gr.gr_mem = g_members;
    return &g_gr;
}

struct passwd *getpwent(void) {
    if (!g_pwf && !(g_pwf = fopen("/etc/passwd", "r"))) return NULL;
    while (fgets(g_line, sizeof g_line, g_pwf)) {
        if (g_line[0] == '#' || g_line[0] == '\n') continue;
        struct passwd *p = parse_pw(g_line);
        if (p) return p;
    }
    return NULL;
}

void setpwent(void) { if (g_pwf) rewind(g_pwf); }
void endpwent(void) { if (g_pwf) { fclose(g_pwf); g_pwf = NULL; } }

struct passwd *getpwnam(const char *name) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return NULL;
    struct passwd *r = NULL;
    while (fgets(g_line, sizeof g_line, f)) {
        if (g_line[0] == '#' || g_line[0] == '\n') continue;
        char copy[sizeof g_line];
        strcpy(copy, g_line);
        char *colon = strchr(copy, ':');
        if (!colon) continue;
        *colon = 0;
        if (!strcmp(copy, name)) { r = parse_pw(g_line); break; }
    }
    fclose(f);
    return r;
}

struct passwd *getpwuid(uid_t uid) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return NULL;
    struct passwd *r = NULL;
    while (fgets(g_line, sizeof g_line, f)) {
        if (g_line[0] == '#' || g_line[0] == '\n') continue;
        char copy[sizeof g_line];
        strcpy(copy, g_line);
        char *fld[4];
        if (split(copy, fld, 4) < 3) continue;
        if ((uid_t)atoi(fld[2]) == uid) { r = parse_pw(g_line); break; }
    }
    fclose(f);
    return r;
}

struct group *getgrnam(const char *name) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return NULL;
    struct group *r = NULL;
    while (fgets(g_line, sizeof g_line, f)) {
        if (g_line[0] == '#' || g_line[0] == '\n') continue;
        char copy[sizeof g_line];
        strcpy(copy, g_line);
        char *colon = strchr(copy, ':');
        if (!colon) continue;
        *colon = 0;
        if (!strcmp(copy, name)) { r = parse_gr(g_line); break; }
    }
    fclose(f);
    return r;
}

struct group *getgrgid(gid_t gid) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return NULL;
    struct group *r = NULL;
    while (fgets(g_line, sizeof g_line, f)) {
        if (g_line[0] == '#' || g_line[0] == '\n') continue;
        char copy[sizeof g_line];
        strcpy(copy, g_line);
        char *fld[4];
        if (split(copy, fld, 4) < 3) continue;
        if ((gid_t)atoi(fld[2]) == gid) { r = parse_gr(g_line); break; }
    }
    fclose(f);
    return r;
}

int getgroups(int n, gid_t list[]) { return (int)__ret(__sc2(SYS_getgroups, n, (long)list)); }
int setgroups(size_t n, const gid_t *list) { return (int)__ret(__sc2(SYS_setgroups, (long)n, (long)list)); }

int initgroups(const char *user, gid_t gid) {
    gid_t groups[16];
    int n = 0;
    groups[n++] = gid;
    FILE *f = fopen("/etc/group", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *fld[4];
        if (line[0] == '#' || split(line, fld, 4) < 4) continue;
        gid_t g = (gid_t)atoi(fld[2]);
        char *save, *m;
        for (m = strtok_r(fld[3], ",", &save); m; m = strtok_r(NULL, ",", &save)) {
            if (!strcmp(m, user)) {
                int dup = 0;
                for (int i = 0; i < n; i++) if (groups[i] == g) dup = 1;
                if (!dup && n < 16) groups[n++] = g;
                break;
            }
        }
    }
    if (f) fclose(f);
    return setgroups((size_t)n, groups);
}
