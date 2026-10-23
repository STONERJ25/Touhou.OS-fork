#include <shadow.h>
#include <crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>

#define SHADOW_PATH "/etc/shadow"

int gap_shadow_lookup(const char *user, char *hash, size_t n) {
    FILE *f = fopen(SHADOW_PATH, "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    size_t ul = strlen(user);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, user, ul) || line[ul] != ':') continue;
        char *h = line + ul + 1;
        char *end = strchr(h, ':');
        if (!end) end = h + strcspn(h, "\n");
        *end = 0;
        snprintf(hash, n, "%s", h);
        found = 0;
        break;
    }
    fclose(f);
    return found;
}

int gap_shadow_set(const char *user, const char *hash) {
    FILE *f = fopen(SHADOW_PATH, "r");
    char *old = NULL;
    size_t len = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        old = malloc((size_t)sz + 1);
        len = fread(old, 1, (size_t)sz, f);
        old[len] = 0;
        fclose(f);
    }
    char tmp[] = SHADOW_PATH ".new";
    FILE *out = fopen(tmp, "w");
    if (!out) { free(old); return -1; }
    size_t ul = strlen(user);
    bool done = false;
    char *p = old;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p + 1) : strlen(p);
        if (!strncmp(p, user, ul) && p[ul] == ':') {
            fprintf(out, "%s:%s:19000:0:99999:7:::\n", user, hash);
            done = true;
        } else fwrite(p, 1, ll, out);
        p += ll;
    }
    if (!done) fprintf(out, "%s:%s:19000:0:99999:7:::\n", user, hash);
    fclose(out);
    free(old);
    chmod(tmp, 0600);
    return rename(tmp, SHADOW_PATH);
}

int gap_authenticate(const char *user, const char *password) {
    char hash[160];
    if (gap_shadow_lookup(user, hash, sizeof hash) < 0) return 0;
    if (hash[0] == 0) return password[0] == 0;
    if (!strcmp(hash, "!") || !strcmp(hash, "*") || !strncmp(hash, "!!", 2)) return 0;
    return gap_check_password(password, hash);
}

char *getpass(const char *prompt) {
    static char buf[128];
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) fd = 0;
    struct termios old, raw;
    int have = tcgetattr(fd, &old) == 0;
    if (have) {
        raw = old;
        raw.c_lflag &= ~(unsigned)ECHO;
        tcsetattr(fd, TCSAFLUSH, &raw);
    }
    if (prompt) { write(fd == 0 ? 2 : fd, prompt, strlen(prompt)); }
    size_t n = 0;
    char c;
    while (n < sizeof buf - 1 && read(fd, &c, 1) == 1 && c != '\n' && c != '\r') buf[n++] = c;
    buf[n] = 0;
    if (have) tcsetattr(fd, TCSAFLUSH, &old);
    write(fd == 0 ? 2 : fd, "\n", 1);
    if (fd > 2) close(fd);
    return buf;
}
