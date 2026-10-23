#include "spell.h"
#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool g_trace;

void trace(const char *fmt, ...) {
    if (!g_trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

ssize_t read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return (ssize_t)n;
}

void fmt_bytes(unsigned long long b, char *out, size_t cap) {
    if (b < 1024) snprintf(out, cap, "%llu B", b);
    else if (b < 1024ull * 1024) snprintf(out, cap, "%llu.%llu KiB", b >> 10, (b & 1023) * 10 >> 10);
    else if (b < 1024ull * 1024 * 1024) snprintf(out, cap, "%llu.%llu MiB", b >> 20, (b & 0xFFFFF) * 10 >> 20);
    else snprintf(out, cap, "%llu.%llu GiB", b >> 30, (b & 0x3FFFFFFF) * 10 >> 30);
}

void fmt_kb(unsigned long kb, char *out, size_t cap) {
    if (kb < 1024) snprintf(out, cap, "%lu kB", kb);
    else if (kb < 1024 * 1024) snprintf(out, cap, "%lu.%lu MB", kb >> 10, (kb & 1023) * 10 >> 10);
    else snprintf(out, cap, "%lu.%lu GB", kb >> 20, (kb & 0xFFFFF) * 10 >> 20);
}

void fmt_duration(unsigned long long ms, char *out, size_t cap) {
    unsigned long long s = ms / 1000;
    if (s < 60) snprintf(out, cap, "%llu s", s);
    else if (s < 3600) snprintf(out, cap, "%llu min %llu s", s / 60, s % 60);
    else if (s < 86400) snprintf(out, cap, "%llu h %llu min", s / 3600, s / 60 % 60);
    else snprintf(out, cap, "%llu d %llu h", s / 86400, s / 3600 % 24);
}

const char *user_name(unsigned uid) {
    static struct { unsigned uid; char name[24]; bool used; } cache[16];
    for (int i = 0; i < 16; i++) if (cache[i].used && cache[i].uid == uid) return cache[i].name;
    static int next;
    int slot = next++ % 16;
    cache[slot].used = true;
    cache[slot].uid = uid;
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) snprintf(cache[slot].name, sizeof cache[slot].name, "%s", pw->pw_name);
    else snprintf(cache[slot].name, sizeof cache[slot].name, "%u", uid);
    return cache[slot].name;
}

const char *state_word(char st) {
    switch (st) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'Z': return "zombie";
    default: return "unknown";
    }
}
