/* Account administration: adduser, deluser. They edit the plain text files in /etc and so need root (use sudo). */
#include "kappabox.h"
#include <stdbool.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <crypto.h>

#define HOME_ROOT "/HumanVillage"
#define SEAT_GROUP "shrine"

static char **load_lines(const char *path, size_t *n) {
    *n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char **lines = NULL;
    *n = kb_read_lines(f, &lines);
    fclose(f);
    return lines;
}

/* Replaces the file with the given lines (plus `extra`, if any) by writing a sibling and renaming it over the original. */
static int store_lines(const char *path, mode_t mode, char **lines, size_t n, const char *extra) {
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    for (size_t i = 0; i < n; i++) if (lines[i]) fprintf(f, "%s\n", lines[i]);
    if (extra) fprintf(f, "%s\n", extra);
    if (fclose(f) != 0) return -1;
    chmod(tmp, mode);
    return rename(tmp, path);
}

static bool valid_name(const char *s) {
    if (!*s || strlen(s) > 31) return false;
    if (!(islower((unsigned char)s[0]) || s[0] == '_')) return false;
    for (const char *p = s; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '-')) return false;
    return true;
}

/* Adds `user` to the member list of `group` in the group file's lines (the line is replaced by a malloc'd longer one). */
static void add_member(char **lines, size_t n, const char *group, const char *user) {
    size_t gl = strlen(group);
    for (size_t i = 0; i < n; i++) {
        if (strncmp(lines[i], group, gl) || lines[i][gl] != ':') continue;
        size_t len = strlen(lines[i]);
        char *grown = malloc(len + strlen(user) + 2);
        if (!grown) return;
        sprintf(grown, "%s%s%s", lines[i], lines[i][len - 1] == ':' ? "" : ",", user);
        free(lines[i]);
        lines[i] = grown;
        return;
    }
}

/* Removes `user` from every member list in the group file (and drops the group named after the user). */
static void drop_member(char **lines, size_t n, const char *user) {
    size_t ul = strlen(user);
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(lines[i], user, ul) && lines[i][ul] == ':') { free(lines[i]); lines[i] = NULL; continue; }
        char *c3 = lines[i];
        for (int k = 0; k < 3 && c3; k++) { c3 = strchr(c3, ':'); if (c3) c3++; }
        if (!c3) continue;
        char *rebuilt = malloc(strlen(lines[i]) + 1);
        if (!rebuilt) continue;
        size_t head = (size_t)(c3 - lines[i]);
        memcpy(rebuilt, lines[i], head);
        rebuilt[head] = 0;
        char *copy = strdup(c3), *save, *m;
        bool first = true;
        for (m = strtok_r(copy, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
            if (!strcmp(m, user)) continue;
            if (!first) strcat(rebuilt, ",");
            strcat(rebuilt, m);
            first = false;
        }
        free(copy);
        free(lines[i]);
        lines[i] = rebuilt;
    }
}

static void free_lines(char **lines, size_t n) {
    for (size_t i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* adduser [-c "Full Name"] [-s SHELL] [-L] [--no-seat] NAME
 * -L creates the account locked (no password prompt); --no-seat leaves it out of group shrine, so it gets no desktop. */
APPLET(adduser) {
    const char *gecos = NULL, *shell = "/bin/gapsh";
    bool locked = false, seat = true;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) gecos = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shell = argv[++i];
        else if (!strcmp(argv[i], "-L")) locked = true;
        else if (!strcmp(argv[i], "--no-seat")) seat = false;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { kb_error("usage: adduser [-c \"Full Name\"] [-s SHELL] [-L] [--no-seat] NAME"); return 2; }
    }
    if (i + 1 != argc) { kb_error("usage: adduser [-c \"Full Name\"] [-s SHELL] [-L] [--no-seat] NAME"); return 2; }
    const char *name = argv[i];
    if (geteuid() != 0) { kb_error("only root may add residents to the Human Village (try: sudo adduser %s)", name); return 1; }
    if (!valid_name(name)) { kb_error("'%s' is not a good name (lower case letters, digits, '_' and '-', starting with a letter)", name); return 1; }
    if (getpwnam(name)) { kb_error("'%s' already lives here", name); return 1; }

    int id = 1000;                                                         /* the next free number, for both the user and the group */
    struct passwd *p;
    setpwent();
    while ((p = getpwent())) if ((int)p->pw_uid >= id && p->pw_uid < 60000) id = (int)p->pw_uid + 1;
    endpwent();
    while (getgrgid((gid_t)id)) id++;

    char pretty[64];
    if (!gecos) {
        snprintf(pretty, sizeof pretty, "%s", name);
        pretty[0] = (char)toupper((unsigned char)pretty[0]);
        gecos = pretty;
    }
    char hash[160] = "!";
    if (!locked) {
        char first[128], second[128];
        snprintf(first, sizeof first, "%s", getpass("New password (empty: no password): "));
        snprintf(second, sizeof second, "%s", getpass("Retype new password: "));
        if (strcmp(first, second)) { kb_error("the passwords do not match; nothing was created"); return 1; }
        hash[0] = 0;
        if (first[0]) {
            char salt[12];
            gap_make_salt(salt, sizeof salt);
            gap_hash_password(first, salt, hash, sizeof hash);
        }
    }

    char home[96], line[512];
    snprintf(home, sizeof home, "%s/%s", HOME_ROOT, name);
    snprintf(line, sizeof line, "%s:x:%d:%d:%s:%s:%s", name, id, id, gecos, home, shell);
    size_t n;
    char **lines = load_lines("/etc/passwd", &n);
    if (store_lines("/etc/passwd", 0644, lines, n, line) < 0) { kb_error("/etc/passwd: %s", strerror(errno)); free_lines(lines, n); return 1; }
    free_lines(lines, n);

    snprintf(line, sizeof line, "%s:x:%d:%s", name, id, name);
    lines = load_lines("/etc/group", &n);
    if (seat) add_member(lines, n, SEAT_GROUP, name);
    store_lines("/etc/group", 0644, lines, n, line);
    free_lines(lines, n);

    snprintf(line, sizeof line, "%s:%s:19000:0:99999:7:::", name, hash);
    lines = load_lines("/etc/shadow", &n);
    store_lines("/etc/shadow", 0600, lines, n, line);
    free_lines(lines, n);

    if (mkdir(home, 0755) < 0 && errno != EEXIST) kb_error("%s: %s", home, strerror(errno));
    chown(home, (uid_t)id, (gid_t)id);
    printf("adduser: %s joins the Human Village (uid %d)%s%s\n", name, id, seat ? "" : ", without a seat at the shrine", locked ? "; the account is locked until passwd sets a password" : "");
    return 0;
}

/* deluser [--remove-home] [--force] NAME */
APPLET(deluser) {
    bool remove_home = false, force = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--remove-home")) remove_home = true;
        else if (!strcmp(argv[i], "--force")) force = true;
        else { kb_error("usage: deluser [--remove-home] [--force] NAME"); return 2; }
    }
    if (i + 1 != argc) { kb_error("usage: deluser [--remove-home] [--force] NAME"); return 2; }
    char name[64], home[96];
    snprintf(name, sizeof name, "%s", argv[i]);
    if (geteuid() != 0) { kb_error("only root may send residents away (try: sudo deluser %s)", name); return 1; }
    struct passwd *p = getpwnam(name);
    if (!p) { kb_error("'%s' does not live here", name); return 1; }
    if (p->pw_uid < 1000 && !force) { kb_error("'%s' is part of the shrine itself (use --force if you really mean it)", name); return 1; }
    snprintf(home, sizeof home, "%s", p->pw_dir);

    static const char *const files[] = { "/etc/passwd", "/etc/shadow", "/etc/group" };
    for (int f = 0; f < 3; f++) {
        size_t n;
        char **lines = load_lines(files[f], &n);
        if (!lines && n == 0) continue;
        if (f == 2) drop_member(lines, n, name);
        else {
            size_t ul = strlen(name);
            for (size_t k = 0; k < n; k++) if (!strncmp(lines[k], name, ul) && lines[k][ul] == ':') { free(lines[k]); lines[k] = NULL; }
        }
        store_lines(files[f], f == 1 ? 0600 : 0644, lines, n, NULL);
        free_lines(lines, n);
    }
    if (remove_home && strncmp(home, HOME_ROOT "/", strlen(HOME_ROOT) + 1) == 0) kb_remove_tree(home, 1, 0);
    printf("deluser: %s has left the Human Village%s\n", name, remove_home ? " (home removed)" : "");
    return 0;
}
