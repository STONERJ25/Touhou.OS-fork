/*
 * What differs between vncd on TouhouOS and the Linux build the host tests run (tests/host/test_vnc.py): the log, the random
 * numbers, who is at the other end of a UNIX socket, the virtual input device, the kernel command line and the atomic writes
 * of the password files. Everything else in vncd is the same code on both.
 */
#include "vncd.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifdef __linux__
#include <sys/random.h>
#else
#include <gensokyo.h>
#include <etcstore.h>
#endif

/* The record of /dev/vinput: struct ginput_event of shared/abi.h (type, pad, code, value, time); the kernel stamps the time. */
typedef struct { uint8_t type, pad; uint16_t code; int32_t value; uint64_t time_ms; } VinputEvent;
#define VEV_KEY 1
#define VEV_REL 2
#define VEV_ABS 3
#define VREL_WHEEL 8
#define VABS_X 0
#define VABS_Y 1
static const uint16_t g_btn_code[3] = { 0x110, 0x112, 0x111 };       /* BTN_LEFT, BTN_MIDDLE, BTN_RIGHT: RFB's buttons 1, 2, 3 */

void vlog(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef __linux__
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[vncd %ld.%03ld] %s\n", (long)ts.tv_sec, ts.tv_nsec / 1000000L, line);
#else
    gap_log(line);
#endif
}

long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void random_bytes(void *buf, size_t n) {
    uint8_t *p = buf;
    size_t got = 0;
#ifdef __linux__
    while (got < n) { ssize_t r = getrandom(p + got, n - got, 0); if (r <= 0) break; got += (size_t)r; }
#else
    while (got < n) { int r = gap_getrandom(p + got, (unsigned long)(n - got)); if (r <= 0) break; got += (size_t)r; }
#endif
    if (got < n) {                                          /* the kernel's generator did not answer: the challenge must still never repeat */
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { while (got < n) { ssize_t r = read(fd, p + got, n - got); if (r <= 0) break; got += (size_t)r; } close(fd); }
    }
    if (got < n) { vlog("no random numbers: cannot make a challenge"); exit(1); }
}

int peer_uid(int fd) {
#ifdef __linux__
    struct ucred cr;
    socklen_t len = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) < 0) return -1;
    return (int)cr.uid;
#else
    struct gpeercred cr;
    if (getpeercred(fd, &cr) < 0) return -1;
    return (int)cr.uid;
#endif
}

/* ------------------------------------------------------------------------------------------------ the virtual input device */

static int g_vinput = -1;
static VinputEvent g_pending[256];
static int g_npending;
static bool g_vinput_warned;

void vinput_open(void) {
    if (g_vinput >= 0 || !g_opt.vinput_path) return;
#ifdef __linux__
    g_vinput = open(g_opt.vinput_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);     /* the host tests read the records back from a file */
#else
    g_vinput = open(g_opt.vinput_path, O_WRONLY | O_CLOEXEC);
#endif
    if (g_vinput < 0 && !g_vinput_warned) {
        vlog("cannot open %s (%s): viewers can watch but their keys and clicks go nowhere", g_opt.vinput_path, strerror(errno));
        g_vinput_warned = true;
    }
}

static void emit(uint8_t type, uint16_t code, int32_t value) {
    if (g_npending == (int)(sizeof g_pending / sizeof g_pending[0])) vinput_flush();
    g_pending[g_npending++] = (VinputEvent){ type, 0, code, value, 0 };
}

void vinput_key(uint8_t code, int value) { emit(VEV_KEY, code, value); }
void vinput_pointer_abs(int x, int y) { emit(VEV_ABS, VABS_X, x); emit(VEV_ABS, VABS_Y, y); }
void vinput_button(int which, bool down) { if (which >= 0 && which < 3) emit(VEV_KEY, g_btn_code[which], down ? 1 : 0); }
void vinput_wheel(int clicks) { emit(VEV_REL, VREL_WHEEL, clicks); }

void vinput_flush(void) {
    if (!g_npending) return;
    if (g_vinput < 0) vinput_open();
    if (g_vinput >= 0) {
        size_t len = (size_t)g_npending * sizeof g_pending[0];
        ssize_t n = write(g_vinput, g_pending, len);
        if (n != (ssize_t)len && !g_vinput_warned) {
            vlog("writing to %s failed (%s): input is lost", g_opt.vinput_path, n < 0 ? strerror(errno) : "short write");
            g_vinput_warned = true;
        }
    }
    g_npending = 0;
}

/* ------------------------------------------------------------------------------------------------ the kernel command line */

static const char *cmdline(void) {
    static char buf[1024];
    static bool read_once;
    if (read_once) return buf;
    read_once = true;
#ifdef __linux__
    const char *e = getenv("VNCD_CMDLINE");                 /* the host tests pass their "kernel command line" this way */
    snprintf(buf, sizeof buf, "%s", e ? e : "");
#else
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n > 0) buf[n] = 0;
        close(fd);
    }
#endif
    for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
    return buf;
}

/* Words are separated by spaces: "vnc" must not match "vnc.listen=all" nor "novnc". */
bool cmdline_word(const char *word) {
    size_t len = strlen(word);
    for (const char *p = cmdline(); *p;) {
        while (*p == ' ') p++;
        const char *end = p;
        while (*end && *end != ' ') end++;
        if ((size_t)(end - p) == len && !strncmp(p, word, len)) return true;
        p = end;
    }
    return false;
}

/* NAME=VALUE: the last such word wins, as on other systems' kernel command lines (a word added at the end overrides a default). */
bool cmdline_value(const char *prefix, char *out, size_t n) {
    size_t len = strlen(prefix);
    bool found = false;
    for (const char *p = cmdline(); *p;) {
        while (*p == ' ') p++;
        const char *end = p;
        while (*end && *end != ' ') end++;
        if ((size_t)(end - p) >= len && !strncmp(p, prefix, len)) {
            size_t v = (size_t)(end - p) - len;
            if (v >= n) v = n - 1;
            memcpy(out, p + len, v);
            out[v] = 0;
            found = true;
        }
        p = end;
    }
    return found;
}

/* ------------------------------------------------------------------------------------------------ files */

/* Writes PATH atomically with mode 0600. A path on the HUMANVILLAGE volume: 1 when there is no volume to keep it on, -1 with
 * errno when the volume cannot (read-only after an unclean shutdown, full ...), 0 done. */
int keep_file_write(const char *path, const char *data, size_t len) {
#ifdef __linux__
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    bool ok = write(fd, data, len) == (ssize_t)len;
    if (close(fd) < 0) ok = false;
    if (!ok || chmod(tmp, 0600) < 0 || rename(tmp, path) < 0) { int e = errno; unlink(tmp); errno = e ? e : EIO; return -1; }
    return 0;
#else
    if (!strncmp(path, "/HumanVillage/", 14)) {
        int ro = 0;
        if (!etc_volume_present(&ro)) return 1;
        if (ro) { errno = EROFS; return -1; }
        char dir[256];
        snprintf(dir, sizeof dir, "%s", path);
        for (char *s = dir + 14; (s = strchr(s, '/')); s++) {      /* .system, .system/etc: root's 0700 like init makes them */
            *s = 0;
            if (mkdir(dir, 0700) < 0 && errno != EEXIST) return -1;
            *s = '/';
        }
        return etc_write_atomic(path, data, len, 0600, 1);
    }
    return etc_write_atomic(path, data, len, 0600, 0);
#endif
}

const char *host_name(void) {
    static char name[64];
    if (gethostname(name, sizeof name) != 0 || !name[0]) snprintf(name, sizeof name, "gensokyo");
    name[sizeof name - 1] = 0;
    return name;
}
