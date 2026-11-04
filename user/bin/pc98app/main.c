/*
 * pc98app ("Yukari's Gap to PC-98", also `pc98`): the PC-98 emulator of the desktop. A window with the 640x400 screen of a NEC PC-9801 (a 386 at
 * 16 MHz with the HLE BIOS, the display module and the FM board of libpc98), real-time pacing, an idle skip for the games' busy waits, keys,
 * sound, disk images that the guest can write to without the files ever changing. docs/PC98_APP.md has the whole story.
 *
 *   pc98 [options] [IMAGE ...]       a hard disk image (.hdi) goes to the hard disk, every other image to the next floppy drive
 *     --font FILE      the font ROM (288,768 bytes; the owner's own: it is read at run time and never shipped)    --rhythm DIR   2608_*.WAV drum samples
 *     --cpu 8086|v30|286|386|486   --mhz N     the machine (default 386 at 16 MHz)      --fm 86|26k|none   the FM board
 *     --no-sound   --volume N      --scale 1|2|3|4|fit      --speed PERCENT|max      --no-idle      --no-virtual-config      --paused
 *     --trace          lines on stdout about what happens (the tests wait for them)
 *     --bench SECONDS  no window: run SECONDS of machine time flat out with the idle skip off and on and say how many times real time that was
 *     --bench-key S:HEX  (with --bench, repeatable) press the PC-98 key HEX (a key number, docs/PC98_BIOS.md) S seconds into the machine's time
 *   Settings (the same words) are read from ~/.config/pc98/config first; the command line wins for that run and is not written back.
 */
#include "app.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

App A;

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void say(const char *fmt, ...) {
    if (!A.trace) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void log_line(void *user, const char *line) {
    (void)user;
    say("%s", line);
}

/* ---------------------------------------------------------------- the settings files */

static int conf_path(const char *name, char *out, size_t cap, int make) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    char dir[300];
    if (make) {
        snprintf(dir, sizeof dir, "%s/.config", home);
        mkdir(dir, 0755);
        snprintf(dir, sizeof dir, "%s/.config/pc98", home);
        mkdir(dir, 0755);
    }
    int n = snprintf(out, cap, "%s/.config/pc98/%s", home, name);
    return n > 0 && (size_t)n < cap;
}

static char *read_text(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        ssize_t got = read(fd, buf + n, cap - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += (size_t)got;
        if (n >= (1u << 20)) break;
        if (n == cap) {
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
    }
    close(fd);
    if (!buf) return NULL;
    buf[n] = 0;
    *len = n;
    return buf;
}

/* The file is written whole under another name and renamed over the old one, so a power cut leaves the old file or the new, not half of one. */
static int write_text(const char *name, const char *text, size_t len) {
    char path[400], tmp[420];
    if (!conf_path(name, path, sizeof path, 1)) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t put = 0;
    while (put < len) {
        ssize_t w = write(fd, text + put, len - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        put += (size_t)w;
    }
    close(fd);
    if (put != len) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

void settings_save(void) {
    Pc98Config out = A.file_cfg;                                    /* what the file said ... */
    const Pc98Config *live = &A.emu->cfg;                           /* ... with what the menus changed since */
    if (A.touched & TOUCH_SPEED) out.speed = live->speed;
    if (A.touched & TOUCH_SOUND) out.sound = live->sound;
    if (A.touched & TOUCH_VOLUME) out.volume = live->volume;
    if (A.touched & TOUCH_SCALE) out.scale = live->scale;
    if (A.touched & TOUCH_IDLE) out.idle = live->idle;
    A.file_cfg = out;
    A.touched = 0;
    char text[2048];
    size_t n = config_format(&out, text, sizeof text);
    if (n >= sizeof text || write_text("config", text, n)) say("pc98app: cannot write the settings file");
}

static void recent_save(void) {
    char rec[RECENT_MAX * (CONFIG_PATH_MAX + 1) + 1];
    size_t n = recent_format(&A.recent, rec, sizeof rec);
    if (n >= sizeof rec || write_text("recent", rec, n)) say("pc98app: cannot write the list of recent images");
}

void recent_note(const char *path) {
    char full[CONFIG_PATH_MAX], cwd[CONFIG_PATH_MAX - 2];
    if (path[0] != '/' && getcwd(cwd, sizeof cwd)) {                /* a relative path would mean something else from another folder */
        snprintf(full, sizeof full, "%s/%s", cwd, path);
        recent_add(&A.recent, full);
    } else recent_add(&A.recent, path);
    recent_save();
}

/* ---------------------------------------------------------------- the benchmark */

typedef struct { double at; uint8_t key; } BenchKey;

static void bench_pass(double secs, int idle, const BenchKey *keys, int nkeys) {
    Emu *e = A.emu;
    char err[300];
    e->cfg.idle = idle;
    e->cfg.speed = 0;                                               /* no pacing: as fast as the host goes */
    if (emu_boot(e, -1, err, sizeof err)) { printf("pc98app: bench: %s\n", err); return; }
    uint64_t t0 = now_ns(), target = (uint64_t)(secs * e->m->cpu_hz), start = e->m->cpu.cycles;
    int next = 0, held = -1;
    uint64_t release_at = 0;
    while (e->m->cpu.cycles - start < target && !e->stopped) {
        double machine_s = (double)(e->m->cpu.cycles - start) / e->m->cpu_hz;
        while (next < nkeys && keys[next].at <= machine_s && held < 0) {
            emu_key(e, keys[next].key, 1);
            held = keys[next++].key;
            release_at = e->m->cpu.cycles + e->m->cpu_hz / 12;      /* held for about 80 ms of machine time */
        }
        if (held >= 0 && e->m->cpu.cycles >= release_at) { emu_key(e, (uint8_t)held, 0); held = -1; }
        emu_step(e, now_ns());
        int16_t drain[1024][2];
        while (emu_audio_take(e, &drain[0][0], 1024) == 1024) { }   /* the sound is made (it is part of what a game costs) and thrown away */
    }
    double host = (double)(now_ns() - t0) / 1e9, machine = (double)(e->m->cpu.cycles - start) / e->m->cpu_hz;
    printf("pc98app: bench idle=%s: %.2f s of machine time in %.2f s of host time = %.1f x real time (%llu frames", idle ? "on " : "off", machine, host,
           host > 0 ? machine / host : 0.0, (unsigned long long)e->m->frames);
    if (idle) printf(", the idle skip took %.0f%% of the clocks and %.0f%% of the instructions in %llu skips and %llu probes",
                     100.0 * (double)e->idle->skipped_clocks / (double)(e->m->cpu.cycles - start),
                     100.0 * (double)e->idle->skipped_instrs / (double)(e->m->cpu.instructions ? e->m->cpu.instructions : 1),
                     (unsigned long long)e->idle->skips, (unsigned long long)e->idle->probes);
    printf(")%s\n", e->stopped ? " (the machine stopped)" : "");
    fflush(stdout);
}

static int run_bench(double secs, const BenchKey *keys, int nkeys) {
    printf("pc98app: bench: %s %u MHz, %s, sound %s\n", x86_model_name((X86Model)A.emu->cfg.cpu), (unsigned)A.emu->cfg.mhz,
           A.emu->cfg.fm == CFG_FM_NONE ? "no FM board" : "FM board", A.emu->cfg.sound ? "synthesised" : "off");
    bench_pass(secs, 0, keys, nkeys);
    bench_pass(secs, 1, keys, nkeys);
    return 0;
}

/* ---------------------------------------------------------------- the window */

static void on_event(KApp *app, const FairyEvent *ev, void *user) {
    (void)user;
    ui_event(app, ev);
}

static void usage(void) {
    fprintf(stderr,
            "usage: pc98 [options] [IMAGE ...]\n"
            "  --font FILE  --rhythm DIR  --cpu 8086|v30|286|386|486  --mhz N  --fm 86|26k|none  --no-sound  --volume N\n"
            "  --scale 1|2|3|4|fit  --speed PERCENT|max  --no-idle  --no-virtual-config  --paused  --trace\n"
            "  --bench SECONDS [--bench-key SECONDS:HEXKEY]...   (no window: how fast the machine runs, the idle skip off and on)\n");
}

/* One `key=value` of the settings language from the command line; false when it is not valid. */
static int setting(Pc98Config *cfg, const char *key, const char *value) {
    char line[320];
    snprintf(line, sizeof line, "%s=%s", key, value);
    return config_parse(cfg, line, strlen(line), NULL, 0) == 1;
}

int main(int argc, char **argv) {
    memset(&A, 0, sizeof A);
    Pc98Config cfg;
    config_defaults(&cfg);
    char path[400];
    size_t len = 0;
    if (conf_path("config", path, sizeof path, 0)) {
        char *text = read_text(path, &len);
        if (text) {
            char report[600];
            config_parse(&cfg, text, len, report, sizeof report);
            if (report[0]) fprintf(stderr, "pc98: %s: some lines were skipped:\n%s", path, report);
            free(text);
        }
    }
    A.file_cfg = cfg;
    if (conf_path("recent", path, sizeof path, 0)) {
        char *text = read_text(path, &len);
        if (text) { recent_parse(&A.recent, text, len); free(text); }
    }
    const char *images[8];
    int nimages = 0, paused = 0, nkeys = 0;
    double bench = 0;
    BenchKey keys[32];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--font") && v) { setting(&cfg, "font", v); i++; }
        else if (!strcmp(a, "--rhythm") && v) { setting(&cfg, "rhythm", v); i++; }
        else if (!strcmp(a, "--cpu") && v) { if (!setting(&cfg, "cpu", v)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--mhz") && v) { if (!setting(&cfg, "mhz", v)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--fm") && v) { if (!setting(&cfg, "fm", v)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--volume") && v) { if (!setting(&cfg, "volume", v)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--scale") && v) { if (!setting(&cfg, "scale", v)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--speed") && v) { if (!setting(&cfg, "speed", v)) { usage(); return 2; } i++; }
        else if (!strcmp(a, "--no-sound")) cfg.sound = 0;
        else if (!strcmp(a, "--no-idle")) cfg.idle = 0;
        else if (!strcmp(a, "--no-virtual-config")) cfg.vconfig = 0;
        else if (!strcmp(a, "--paused")) paused = 1;
        else if (!strcmp(a, "--trace")) A.trace = 1;
        else if (!strcmp(a, "--bench") && v) { bench = atof(v); i++; }
        else if (!strcmp(a, "--bench-key") && v && nkeys < 32) {
            char *end;
            double at = strtod(v, &end);
            if (*end != ':') { usage(); return 2; }
            keys[nkeys].at = at;
            keys[nkeys++].key = (uint8_t)strtoul(end + 1, NULL, 16);
            i++;
        }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (a[0] == '-' && a[1]) { usage(); return 2; }
        else if (nimages < 8) images[nimages++] = a;
    }

    A.emu = emu_new(&cfg);
    if (!A.emu) { fprintf(stderr, "pc98: out of memory\n"); return 1; }
    Emu *e = A.emu;
    e->log = log_line;
    char err[300], problems[1200] = "";
    if (cfg.font[0] && emu_set_font(e, cfg.font, err, sizeof err)) {
        snprintf(problems + strlen(problems), sizeof problems - strlen(problems), "%s\n\n", err);
        fprintf(stderr, "pc98: %s\n", err);
    }
    if (cfg.rhythm[0]) say("pc98app: %d drum samples found in %s", emu_set_rhythm_dir(e, cfg.rhythm), cfg.rhythm);
    say("pc98app: settings: %s %d MHz, FM %s, sound %s, volume %d, scale %s, speed %d%%, idle skip %s, virtual config %s", config_cpu_name(cfg.cpu), cfg.mhz,
        config_fm_name(cfg.fm), cfg.sound ? "on" : "off", cfg.volume, cfg.scale ? (cfg.scale == 1 ? "1x" : cfg.scale == 2 ? "2x" : cfg.scale == 3 ? "3x" : "4x") : "fit", cfg.speed,
        cfg.idle ? "on" : "off", cfg.vconfig ? "on" : "off");
    say("pc98app: %s", e->font_note);
    int loaded = 0;
    for (int i = 0; i < nimages; i++) {
        int slot = -1;
        if (emu_insert_auto(e, images[i], -1, &slot, err, sizeof err)) {
            snprintf(problems + strlen(problems), sizeof problems - strlen(problems), "%s\n%s\n\n", images[i], err);
            fprintf(stderr, "pc98: %s: %s\n", images[i], err);
            say("pc98app: cannot use %s: %s", images[i], err);
        } else {
            loaded++;
            if (!bench) recent_note(images[i]);
            if (!A.dir[0]) {
                snprintf(A.dir, sizeof A.dir, "%s", images[i]);
                char *slash = strrchr(A.dir, '/');
                if (slash) *slash = 0; else A.dir[0] = 0;
            }
        }
    }
    if (bench > 0) {
        if (!loaded) { usage(); return 2; }
        return run_bench(bench, keys, nkeys);
    }

    kapp_set_hidpi(false);                                          /* the machine's picture is pixels of its own: a 1x window, enlarged by the window server on a hi-dpi screen */
    int s = cfg.scale ? cfg.scale : 1;
    KApp *app = kapp_new("PC-98", EMU_W * s, MENU_H + EMU_H * s + STATUS_H, FW_RESIZABLE);
    if (!app) return 1;
    A.kapp = app;
    A.focused = 1;
    kapp_set_minsize(app, 320, MENU_H + 200 + STATUS_H);
    sound_open();
    if (loaded) {
        if (emu_boot(e, -1, err, sizeof err)) snprintf(problems + strlen(problems), sizeof problems - strlen(problems), "%s\n", err);
        else if (paused) emu_pause(e, 1, now_ns());
    }
    ui_update_title();
    kapp_set_draw(app, ui_draw, NULL);
    kapp_set_event_hook(app, on_event, NULL);
    kapp_set_close_handler(app, dlg_close_handler, NULL);
    kapp_set_tick(app, 16);
    if (problems[0]) dlg_message("The emulator could not use everything", KI_WARNING, "%s", problems);
    int rc = kapp_run(app);
    sound_close();
    emu_free(e);
    kapp_destroy(app);
    return rc;
}
