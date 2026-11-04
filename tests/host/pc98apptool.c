#define _GNU_SOURCE
// pc98apptool: the host side of the PC-98 application's tests (tests/host/test_pc98app.py). It links the application's own sources
// (user/bin/pc98app: keymap, config, pacer, audiopipe, disks, idle, emu: everything that needs no window) with libpc98, so what is
// tested is the code the desktop program runs, not a copy of it.
//
//   pc98apptool list                      the names of the unit tests
//   pc98apptool test NAME [DIR]           one unit test; prints "PASS NAME" or "FAIL ..." lines (DIR: a folder of image files the Python side made)
//   pc98apptool run [options]             a headless machine on a fake clock (below)
//
// run: boots the images through emu.c exactly as the program does, with the pacer driven by a fake clock (so a run is deterministic:
// every pass of the loop moves the clock by --step-ns, the way the desktop program's ticks do), and prints what the tests look at.
//     --disk IMAGE         (repeatable) an image: a hard disk image goes to the first hard disk, anything else to the next floppy drive
//     --cycles N           stop after N million machine clocks (default 100)
//     --idle on|off        the idle skip (default on)       --cpu 8086|v30|286|386|486   --mhz N   --fm none|86|26k
//     --speed N|max        percent of real time for the pacer (default 100)      --step-ns N   the fake clock's step (default 4000000)
//     --font FILE          a font ROM      --rhythm DIR   the drum samples      --no-sound   the sound pipe off
//     --clock YYMMDDhhmmss the calendar at boot (default 261102120000)
//     --key N:HEX[:MS]     (repeatable) press the PC-98 key HEX at N million clocks, release it MS milliseconds of machine time later (default 60)
//     --hostkey N:HEX:D    (repeatable) a Fairies key event (HEX = KEY_* code, D = 1 press, 0 release, 2 repeat) at N million clocks
//     --dump ADDR:LEN      (repeatable) memory at the end (hex)       --text   the text screen       --ppm FILE   the picture at the end
//     --wav FILE           the sound pipe's output as a WAV (48 kHz stereo)
//     --save SLOT:FILE     at the end write disk SLOT (0 floppy 1, 1 floppy 2, 2 hard disk 1) to a new file (emu_save)
//     --disk-changed       print the number of changed blocks per drive at the end
// Output lines: BOOT, STEP counts, END, IDLE, HASH, DISKS, MEM, AUDIO, TEXT lines ("|...").
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../../user/bin/pc98app/emu.h"
#include <keys.h>

static int g_failed;
static const char *g_dir = ".";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } } while (0)
#define CHECK_EQ(a, b) do { long long x_ = (long long)(a), y_ = (long long)(b); if (x_ != y_) { printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, x_, y_); g_failed++; } } while (0)
#define CHECK_NEAR(a, b, tol) do { double x_ = (double)(a), y_ = (double)(b); if (x_ < y_ - (tol) || x_ > y_ + (tol)) { printf("FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, x_, y_); g_failed++; } } while (0)

// ------------------------------------------------------------------------------------------------ keymap

typedef struct { uint8_t seq[64]; int n; } KeyLog;
static void log_key(void *user, uint8_t k, int down) {
    KeyLog *l = user;
    if (l->n < 32) { l->seq[2 * l->n] = k; l->seq[2 * l->n + 1] = (uint8_t)down; l->n++; }
}

static void t_keymap_game_keys(void) {
    CHECK_EQ(keymap_to_pc98(KEY_ESC), 0x00);
    CHECK_EQ(keymap_to_pc98(KEY_Z), 0x29);              // Z and X are shoot and bomb on both machines
    CHECK_EQ(keymap_to_pc98(KEY_X), 0x2A);
    CHECK_EQ(keymap_to_pc98(KEY_ENTER), 0x1C);
    CHECK_EQ(keymap_to_pc98(KEY_SPACE), 0x34);
    CHECK_EQ(keymap_to_pc98(KEY_UP), 0x3A);
    CHECK_EQ(keymap_to_pc98(KEY_LEFT), 0x3B);
    CHECK_EQ(keymap_to_pc98(KEY_RIGHT), 0x3C);
    CHECK_EQ(keymap_to_pc98(KEY_DOWN), 0x3D);
    CHECK_EQ(keymap_to_pc98(KEY_LSHIFT), 0x70);
    CHECK_EQ(keymap_to_pc98(KEY_LCTRL), 0x74);
    CHECK_EQ(keymap_to_pc98(KEY_LALT), 0x73);           // GRPH
    CHECK_EQ(keymap_to_pc98(KEY_F1), 0x62);
    CHECK_EQ(keymap_to_pc98(KEY_F10), 0x6B);
    CHECK_EQ(keymap_to_pc98(KEY_KP8), 0x43);            // the keypad's arrows, which the games also read
    CHECK_EQ(keymap_to_pc98(KEY_KP4), 0x46);
    CHECK_EQ(keymap_to_pc98(KEY_KP6), 0x48);
    CHECK_EQ(keymap_to_pc98(KEY_KP2), 0x4B);
    CHECK_EQ(keymap_to_pc98(KEY_1), 0x01);
    CHECK_EQ(keymap_to_pc98(KEY_0), 0x0A);
    CHECK_EQ(keymap_to_pc98(KEY_Q), 0x10);
    CHECK_EQ(keymap_to_pc98(KEY_A), 0x1D);
    CHECK_EQ(keymap_to_pc98(KEY_M), 0x2F);
    CHECK_EQ(keymap_to_pc98(KEY_BACKSPACE), 0x0E);
    CHECK_EQ(keymap_to_pc98(KEY_DELETE), 0x39);
}

static void t_keymap_table_is_consistent(void) {
    int seen[128] = { 0 };
    int mapped = 0;
    for (int c = 0; c < 256; c++) {
        uint8_t p = keymap_to_pc98((uint8_t)c);
        if (p == PC98K_NONE) continue;
        CHECK(p < 0x80);                                // a PC-98 key number is seven bits
        seen[p]++;
        mapped++;
    }
    CHECK(mapped > 90);
    // the aliases: the two Shift keys, the two Ctrl keys, the two Alt keys, the two Enter keys; every other key has its own number
    CHECK_EQ(seen[0x70], 2);
    CHECK_EQ(seen[0x74], 2);
    CHECK_EQ(seen[0x73], 2);
    CHECK_EQ(seen[0x1C], 2);
    for (int p = 0; p < 128; p++) if (p != 0x70 && p != 0x74 && p != 0x73 && p != 0x1C) CHECK(seen[p] <= 1);
    // keys that must not reach the guest: the emulator's menu key and the Windows keys
    CHECK_EQ(keymap_to_pc98(KEY_F12), PC98K_NONE);
    CHECK_EQ(keymap_to_pc98(KEY_LSUPER), PC98K_NONE);
    CHECK_EQ(keymap_to_pc98(0), PC98K_NONE);
}

static void t_keys_press_release(void) {
    Keys k;
    KeyLog l = { .n = 0 };
    keys_init(&k);
    CHECK_EQ(keys_event(&k, KEY_Z, 1, log_key, &l), 1);
    CHECK_EQ(keys_event(&k, KEY_Z, 0, log_key, &l), 1);
    CHECK_EQ(l.n, 2);
    CHECK(l.seq[0] == 0x29 && l.seq[1] == 1 && l.seq[2] == 0x29 && l.seq[3] == 0);
    CHECK_EQ(keys_held(&k), 0);
    CHECK_EQ(keys_event(&k, KEY_F12, 1, log_key, &l), 0);       // not a PC-98 key: not taken, nothing sent
    CHECK_EQ(l.n, 2);
}

static void t_keys_repeat_is_ignored(void) {
    Keys k;
    KeyLog l = { .n = 0 };
    keys_init(&k);
    keys_event(&k, KEY_LEFT, 1, log_key, &l);
    for (int i = 0; i < 10; i++) keys_event(&k, KEY_LEFT, 2, log_key, &l);        // the host's auto-repeat
    keys_event(&k, KEY_LEFT, 1, log_key, &l);                                      // even a repeat that arrives as a press
    CHECK_EQ(l.n, 1);                                                              // the guest saw one make
    keys_event(&k, KEY_LEFT, 0, log_key, &l);
    CHECK_EQ(l.n, 2);
    CHECK(l.seq[2] == 0x3B && l.seq[3] == 0);
}

static void t_keys_two_shift_keys(void) {
    Keys k;
    KeyLog l = { .n = 0 };
    keys_init(&k);
    keys_event(&k, KEY_LSHIFT, 1, log_key, &l);
    keys_event(&k, KEY_RSHIFT, 1, log_key, &l);
    CHECK_EQ(l.n, 1);                                                              // SHIFT went down once
    keys_event(&k, KEY_LSHIFT, 0, log_key, &l);
    CHECK_EQ(l.n, 1);                                                              // still held by the other one
    CHECK_EQ(keys_held(&k), 1);
    keys_event(&k, KEY_RSHIFT, 0, log_key, &l);
    CHECK_EQ(l.n, 2);
    CHECK(l.seq[2] == 0x70 && l.seq[3] == 0);
}

static void t_keys_release_without_press(void) {
    Keys k;
    KeyLog l = { .n = 0 };
    keys_init(&k);
    keys_event(&k, KEY_X, 0, log_key, &l);                                         // held before the window had the focus
    CHECK_EQ(l.n, 0);
    CHECK_EQ(keys_held(&k), 0);
}

static void t_keys_release_all(void) {
    Keys k;
    KeyLog l = { .n = 0 };
    keys_init(&k);
    keys_event(&k, KEY_Z, 1, log_key, &l);
    keys_event(&k, KEY_LSHIFT, 1, log_key, &l);
    keys_event(&k, KEY_RSHIFT, 1, log_key, &l);
    keys_event(&k, KEY_UP, 1, log_key, &l);
    CHECK_EQ(l.n, 3);                                                              // Z, SHIFT (once, for both keys), UP
    CHECK_EQ(keys_held(&k), 3);
    keys_release_all(&k, log_key, &l);
    CHECK_EQ(keys_held(&k), 0);
    CHECK_EQ(l.n, 6);                                                              // one break per key the guest believed down
    int breaks = 0;
    for (int i = 3; i < l.n; i++) if (l.seq[2 * i + 1] == 0) breaks++;
    CHECK_EQ(breaks, 3);
    keys_release_all(&k, log_key, &l);                                             // nothing left: nothing sent
    CHECK_EQ(l.n, 6);
    keys_event(&k, KEY_Z, 0, log_key, &l);                                         // the real release arrives late: ignored
    CHECK_EQ(l.n, 6);
}

// ------------------------------------------------------------------------------------------------ config

static void t_config_defaults(void) {
    Pc98Config c;
    config_defaults(&c);
    CHECK_EQ(c.cpu, CFG_CPU_386);
    CHECK_EQ(c.mhz, 16);
    CHECK_EQ(c.fm, CFG_FM_86);
    CHECK_EQ(c.sound, 1);
    CHECK_EQ(c.volume, 80);
    CHECK_EQ(c.scale, 1);
    CHECK_EQ(c.speed, 100);
    CHECK_EQ(c.idle, 1);
    CHECK_EQ(c.vconfig, 1);
    CHECK(c.font[0] == 0 && c.rhythm[0] == 0);
}

static void t_config_parse_every_key(void) {
    Pc98Config c;
    config_defaults(&c);
    const char *text =
        "# comment\n"
        "font=/HumanVillage/pc98/FONT.ROM\n"
        "  rhythm = /HumanVillage/pc98/rhythm with spaces  \n"
        "CPU=486\nmhz=33\nfm=26k\nsound=off\nvolume=35\nscale=fit\nspeed=max\nidle=no\nvirtual_config=off\n";
    char rep[256];
    int n = config_parse(&c, text, strlen(text), rep, sizeof rep);
    CHECK_EQ(n, 11);
    CHECK_EQ(c.vconfig, 0);
    CHECK(rep[0] == 0);
    CHECK(!strcmp(c.font, "/HumanVillage/pc98/FONT.ROM"));
    CHECK(!strcmp(c.rhythm, "/HumanVillage/pc98/rhythm with spaces"));
    CHECK_EQ(c.cpu, CFG_CPU_486);
    CHECK_EQ(c.mhz, 33);
    CHECK_EQ(c.fm, CFG_FM_26K);
    CHECK_EQ(c.sound, 0);
    CHECK_EQ(c.volume, 35);
    CHECK_EQ(c.scale, 0);
    CHECK_EQ(c.speed, 0);
    CHECK_EQ(c.idle, 0);
}

static void t_config_bad_lines_are_skipped_and_reported(void) {
    Pc98Config c;
    config_defaults(&c);
    const char *text =
        "volume=loud\n"            // 1: not a number
        "colour=blue\n"            // 2: unknown key
        "no equals sign\n"         // 3
        "mhz=0\n"                  // 4: out of range
        "mhz=999999999999\n"       // 5: too long a number
        "cpu=pentium\n"            // 6
        "scale=5\n"                // 7
        "fm=\n"                    // 8: empty
        "volume=60\n"              // 9: good, after the bad ones
        "sound=maybe\n";           // 10
    char rep[512];
    int n = config_parse(&c, text, strlen(text), rep, sizeof rep);
    CHECK_EQ(n, 1);
    CHECK_EQ(c.volume, 60);
    CHECK_EQ(c.mhz, 16);                                  // the defaults survive
    CHECK_EQ(c.cpu, CFG_CPU_386);
    CHECK_EQ(c.scale, 1);
    CHECK_EQ(c.sound, 1);
    int lines = 0;
    for (const char *p = rep; *p; p++) if (*p == '\n') lines++;
    CHECK_EQ(lines, 9);
    CHECK(strstr(rep, "line 1:") && strstr(rep, "line 2: unknown key 'colour'") && strstr(rep, "line 10:"));
    CHECK(!strstr(rep, "line 9:"));
    // a path too long for its field and a line longer than the parser's buffer
    char big[1024];
    memset(big, 'a', sizeof big - 1);
    memcpy(big, "font=", 5);
    big[sizeof big - 1] = 0;
    config_defaults(&c);
    n = config_parse(&c, big, strlen(big), rep, sizeof rep);
    CHECK_EQ(n, 0);
    CHECK(c.font[0] == 0);
    CHECK(strstr(rep, "too long"));
    // no report wanted, binary rubbish
    const char junk[] = { 0, 1, 2, '\n', '=', '=', '\n', (char)0xFF, '=', 'x' };
    config_defaults(&c);
    n = config_parse(&c, junk, sizeof junk, NULL, 0);
    CHECK_EQ(n, 0);
}

static void t_config_round_trip(void) {
    Pc98Config a, b;
    config_defaults(&a);
    snprintf(a.font, sizeof a.font, "/HumanVillage/pc98/FONT.ROM");
    snprintf(a.rhythm, sizeof a.rhythm, "/x/y z");
    a.cpu = CFG_CPU_286; a.mhz = 12; a.fm = CFG_FM_NONE; a.sound = 0; a.volume = 7; a.scale = 0; a.speed = 0; a.idle = 0; a.vconfig = 0;
    char text[1024];
    size_t n = config_format(&a, text, sizeof text);
    CHECK(n > 40 && n < sizeof text);
    config_defaults(&b);
    int applied = config_parse(&b, text, n, NULL, 0);
    CHECK_EQ(applied, 11);
    CHECK(!memcmp(&a, &b, sizeof a));
    char tiny[16];
    size_t need = config_format(&a, tiny, sizeof tiny);        // too small: the length it needs, and a terminated string
    CHECK_EQ(need, n);
    CHECK_EQ(strlen(tiny), sizeof tiny - 1);
}

static void t_recent_list(void) {
    Pc98Recent r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 12; i++) { char p[64]; snprintf(p, sizeof p, "/HumanVillage/img%d.hdi", i); recent_add(&r, p); }
    CHECK_EQ(r.n, RECENT_MAX);
    CHECK(!strcmp(r.path[0], "/HumanVillage/img11.hdi"));              // newest first
    CHECK(!strcmp(r.path[RECENT_MAX - 1], "/HumanVillage/img4.hdi"));  // the oldest were dropped
    recent_add(&r, "/HumanVillage/img6.hdi");                           // a repeat moves up, no duplicate
    CHECK_EQ(r.n, RECENT_MAX);
    CHECK(!strcmp(r.path[0], "/HumanVillage/img6.hdi"));
    CHECK(!strcmp(r.path[1], "/HumanVillage/img11.hdi"));
    int dups = 0;
    for (int i = 0; i < r.n; i++) for (int j = i + 1; j < r.n; j++) if (!strcmp(r.path[i], r.path[j])) dups++;
    CHECK_EQ(dups, 0);
    char text[RECENT_MAX * (CONFIG_PATH_MAX + 1) + 1];
    size_t n = recent_format(&r, text, sizeof text);
    Pc98Recent q;
    recent_parse(&q, text, n);
    CHECK(!memcmp(&q, &r, sizeof r));
    recent_add(&r, "");                                                 // nothing to add
    CHECK_EQ(r.n, RECENT_MAX);
    const char list[] = "# a comment\n\n  \na.hdi\na.hdi\r\nb.d88\n";
    recent_parse(&q, list, strlen(list));                               // blank lines, comments, duplicates, a CR
    CHECK_EQ(q.n, 2);
    CHECK(!strcmp(q.path[0], "a.hdi") && !strcmp(q.path[1], "b.d88"));
}

// ------------------------------------------------------------------------------------------------ pacer

#define MS 1000000ull
#define SEC 1000000000ull

static void t_pacer_real_time(void) {
    Pacer p;
    pacer_init(&p, 16000000, 100, 0, 250 * MS);
    uint64_t t = 5 * SEC, total = 0;
    CHECK_EQ(pacer_due(&p, t, 1ull << 40), 0);                  // the first call starts the clock
    for (int i = 0; i < 1000; i++) {                            // one second in 1 ms steps
        t += MS;
        uint64_t d = pacer_due(&p, t, 1ull << 40);
        total += d;
        pacer_set_done(&p, p.done + d);
    }
    CHECK_NEAR(total, 16000000, 20);
    CHECK_EQ(p.resyncs, 0);
}

static void t_pacer_speed_settings(void) {
    uint32_t speeds[4] = { 50, 100, 200, 400 };
    for (int s = 0; s < 4; s++) {
        Pacer p;
        pacer_init(&p, 10000000, speeds[s], 0, 250 * MS);
        uint64_t t = 0, total = 0;
        pacer_due(&p, t, 1ull << 40);
        for (int i = 0; i < 200; i++) {
            t += 5 * MS;
            uint64_t d = pacer_due(&p, t, 1ull << 40);
            total += d;
            pacer_set_done(&p, p.done + d);
        }
        CHECK_NEAR(total, 10000000.0 * speeds[s] / 100.0, 20);
    }
    Pacer p;                                                    // unlimited: always owes what the caller allows
    pacer_init(&p, 10000000, 0, 0, 250 * MS);
    pacer_due(&p, 0, 1000);
    CHECK_EQ(pacer_due(&p, 1, 1000), 1000);
    CHECK_EQ(pacer_due(&p, 1, 777), 777);
    CHECK_EQ(pacer_wait_ms(&p, 1, MS), 0);
}

static void t_pacer_speed_change_does_not_jump(void) {
    Pacer p;
    pacer_init(&p, 10000000, 100, 0, 250 * MS);
    uint64_t t = 0;
    pacer_due(&p, t, 1ull << 40);
    t += 100 * MS;
    uint64_t d = pacer_due(&p, t, 1ull << 40);
    pacer_set_done(&p, p.done + d);
    CHECK_NEAR(d, 1000000, 5);
    pacer_set_speed(&p, 200, t);                                // from now on twice as fast; what came before is not recounted
    CHECK_EQ(pacer_due(&p, t, 1ull << 40), 0);
    t += 100 * MS;
    d = pacer_due(&p, t, 1ull << 40);
    CHECK_NEAR(d, 2000000, 5);
}

static void t_pacer_forgives_a_long_stall(void) {
    Pacer p;
    pacer_init(&p, 16000000, 100, 0, 250 * MS);
    uint64_t t = 0;
    pacer_due(&p, t, 1ull << 40);
    t += 10 * SEC;                                              // the desktop was stuck for ten seconds
    uint64_t d = pacer_due(&p, t, 1ull << 40);
    CHECK_EQ(d, 0);                                             // no burst of ten seconds of game
    CHECK_EQ(p.resyncs, 1);
    t += 20 * MS;
    d = pacer_due(&p, t, 1ull << 40);
    CHECK_NEAR(d, 320000, 5);                                   // and the clock runs normally from there
    // a stall under the limit is made up
    pacer_set_done(&p, p.done + d);
    t += 200 * MS;
    d = pacer_due(&p, t, 1ull << 40);
    CHECK_NEAR(d, 3200000, 5);
    CHECK_EQ(p.resyncs, 1);
}

static void t_pacer_caps_each_pass_and_loses_nothing(void) {
    Pacer p;
    pacer_init(&p, 16000000, 100, 0, 2 * SEC);
    uint64_t t = 0, total = 0;
    pacer_due(&p, t, 100000);
    t += 500 * MS;                                              // half a second owed, taken 100,000 clocks at a time
    int passes = 0;
    for (;;) {
        uint64_t d = pacer_due(&p, t, 100000);
        if (!d) break;
        CHECK(d <= 100000);
        total += d;
        pacer_set_done(&p, p.done + d);
        passes++;
    }
    CHECK_NEAR(total, 8000000, 20);
    CHECK_EQ(passes, 80);
    CHECK_EQ(p.resyncs, 0);
}

static void t_pacer_lead_and_wait(void) {
    Pacer p;
    pacer_init(&p, 16000000, 100, 50 * MS, 250 * MS);
    uint64_t t = 1 * SEC;
    uint64_t d = pacer_due(&p, t, 1ull << 40);
    CHECK_NEAR(d, 800000, 5);                                   // 50 ms ahead of the clock straight away
    pacer_set_done(&p, p.done + d);
    CHECK_EQ(pacer_due(&p, t, 1ull << 40), 0);
    uint32_t w = pacer_wait_ms(&p, t, 10 * MS);                 // 10 ms of machine time will be owed in 10 ms
    CHECK(w >= 9 && w <= 11);
    t += 4 * MS;
    w = pacer_wait_ms(&p, t, 10 * MS);
    CHECK(w >= 5 && w <= 7);
    t += 20 * MS;
    CHECK_EQ(pacer_wait_ms(&p, t, 10 * MS), 0);                 // more is due already
    CHECK(pacer_wait_ms(&p, t - 20 * MS, 5 * SEC) <= 20);       // never more than 20 ms, whatever the step
}

static void t_pacer_measures_speed_and_frames(void) {
    Pacer p;
    pacer_init(&p, 16000000, 100, 0, 250 * MS);
    uint64_t t = 0, frames = 0;
    pacer_due(&p, t, 1ull << 40);
    for (int i = 0; i < 2100; i++) {                            // 2.1 s at real time: 56.4 frames a second
        t += MS;
        uint64_t d = pacer_due(&p, t, 1ull << 40);
        pacer_set_done(&p, p.done + d);
        frames = p.done * 5642 / 100 / 16000000;
        pacer_measure(&p, t, frames);
    }
    CHECK_NEAR(p.speed_x10, 1000, 8);
    CHECK_NEAR(p.fps_x10, 564, 8);
    // a machine that is only half as fast as it should be: the pacer is not what slows it, so the measurement shows 50 %
    pacer_init(&p, 16000000, 100, 0, 250 * MS);
    t = 0;
    pacer_due(&p, t, 1ull << 40);
    for (int i = 0; i < 2100; i++) {
        t += MS;
        uint64_t d = pacer_due(&p, t, 1ull << 40);
        if (d > 8000) d = 8000;                                     // 8,000 of the 16,000 clocks a millisecond is worth
        pacer_set_done(&p, p.done + d);
        pacer_measure(&p, t, p.done * 5642 / 100 / 16000000);
    }
    CHECK(p.speed_x10 > 400 && p.speed_x10 < 600);
}

// ------------------------------------------------------------------------------------------------ audio pipe

static int16_t g_buf[APIPE_RING * 2];

static void t_apipe_timeline_without_a_chip(void) {
    static AudioPipe p;
    apipe_init(&p, 0, 0, 48000, 16000000);
    size_t n = 0;
    int loud = 0;
    for (int i = 0; i < 100; i++) {                                         // 100 stretches of 10 ms, taken as they come
        apipe_run(&p, 160000, NULL, 0, 0);
        size_t got = apipe_take(&p, g_buf, 100000);
        for (size_t k = 0; k < 2 * got; k++) if (g_buf[k]) loud++;
        n += got;
    }
    CHECK_NEAR(n, 48000, 1);
    CHECK_EQ(loud, 0);                                                      // no chip, no buzzer: silence
    CHECK_EQ(p.underruns, 0);
    CHECK_EQ(p.dropped, 0);
}

static void t_apipe_buzzer_pitch(void) {
    static AudioPipe p;
    apipe_init(&p, 0, 0, 48000, 16000000);
    apipe_run(&p, 16000000, NULL, 0, 2000);                                 // one second of a 2 kHz buzzer
    size_t n = apipe_take(&p, g_buf, APIPE_RING);                           // the ring holds a third of a second: the last part
    CHECK(n == APIPE_RING);
    int crossings = 0, peak = 0;
    for (size_t i = 1; i < n; i++) {
        if ((g_buf[2 * i] < 0) != (g_buf[2 * (i - 1)] < 0)) crossings++;
        if (abs(g_buf[2 * i]) > peak) peak = abs(g_buf[2 * i]);
    }
    double hz = crossings / 2.0 / ((double)n / 48000.0);
    CHECK_NEAR(hz, 2000, 10);
    CHECK(peak > 3000 && peak < 12000);                                     // quiet: a buzzer, not a siren
    CHECK_EQ(g_buf[0], g_buf[1]);                                           // the same in both channels
    // silent again afterwards, and the tone restarts at the same phase
    apipe_clear(&p);
    apipe_run(&p, 160000, NULL, 0, 0);
    n = apipe_take(&p, g_buf, 1000);
    for (size_t i = 0; i < 2 * n; i++) if (g_buf[i]) { CHECK(0); break; }
    apipe_run(&p, 160000, NULL, 0, 1000);
    int16_t first[2];
    apipe_clear(&p);
    apipe_run(&p, 160000, NULL, 0, 1000);
    apipe_take(&p, first, 1);
    apipe_run(&p, 160000, NULL, 0, 0);
    apipe_clear(&p);
    apipe_run(&p, 160000, NULL, 0, 1000);
    int16_t again[2];
    apipe_take(&p, again, 1);
    CHECK_EQ(first[0], again[0]);
}

static void t_apipe_overrun_drops_the_oldest(void) {
    static AudioPipe p;
    apipe_init(&p, 48000, 1, 48000, 48000);                                 // a "chip" at the output rate: its frames pass one to one
    int16_t in[1000][2];
    long next = 0;
    for (int round = 0; round < 40; round++) {                              // 40,000 frames of a rising ramp into a ring of 16,384
        for (int i = 0; i < 1000; i++) { in[i][0] = (int16_t)(next % 20000); in[i][1] = (int16_t)(-(next % 20000)); next++; }
        apipe_run(&p, 1000, &in[0][0], 1000, 0);
    }
    CHECK_EQ(apipe_queued(&p), APIPE_RING);
    CHECK(p.dropped > 0);
    CHECK_EQ(p.produced - p.dropped, APIPE_RING);
    size_t n = apipe_take(&p, g_buf, APIPE_RING);
    CHECK_EQ(n, APIPE_RING);
    // what is left is the NEWEST audio: the ramp ends where the input ended (less the resampler's delay of a few frames)
    long last = g_buf[2 * (n - 1)];
    long behind = (((next - 1 - last) % 20000) + 20000) % 20000;
    CHECK(behind < 40);
    int steady = 0;
    for (size_t i = 1; i < n; i++) { int d = g_buf[2 * i] - g_buf[2 * (i - 1)]; if (d >= 0 && d <= 2) steady++; }
    CHECK(steady > (int)n - 100);                                           // a clean ramp, but for the wrap where it rings
    CHECK_EQ(apipe_queued(&p), 0);
}

static void t_apipe_underrun_is_counted_silence(void) {
    static AudioPipe p;
    apipe_init(&p, 0, 0, 48000, 48000);
    int16_t out[16];
    CHECK_EQ(apipe_take(&p, out, 8), 0);
    CHECK_EQ(p.underruns, 1);
    apipe_run(&p, 4, NULL, 0, 0);
    CHECK_EQ(apipe_take(&p, out, 8), 4);                                    // a short answer is not an underrun
    CHECK_EQ(p.underruns, 1);
    CHECK_EQ(apipe_take(&p, out, 0), 0);                                    // asking for nothing is not one either
    CHECK_EQ(p.underruns, 1);
    CHECK_EQ(apipe_take(&p, out, 8), 0);
    CHECK_EQ(p.underruns, 2);
}

static void t_apipe_volume_curve(void) {
    static AudioPipe p;
    int peaks[4];
    int vols[4] = { 100, 50, 10, 0 };
    for (int k = 0; k < 4; k++) {
        apipe_init(&p, 0, 0, 48000, 16000000);
        apipe_set_volume(&p, vols[k]);
        apipe_run(&p, 1600000, NULL, 0, 1000);
        size_t n = apipe_take(&p, g_buf, 4800);
        int pk = 0;
        for (size_t i = 0; i < n; i++) if (abs(g_buf[2 * i]) > pk) pk = abs(g_buf[2 * i]);
        peaks[k] = pk;
    }
    CHECK(peaks[0] > 5000);
    CHECK_NEAR(peaks[1], peaks[0] / 4.0, peaks[0] * 0.03);                  // 50 % is a quarter of the amplitude
    CHECK_NEAR(peaks[2], peaks[0] / 100.0, peaks[0] * 0.01);
    CHECK_EQ(peaks[3], 0);
    apipe_set_volume(&p, 250);                                              // out of range: clamped
    apipe_clear(&p);
    apipe_run(&p, 1600000, NULL, 0, 1000);
    size_t n = apipe_take(&p, g_buf, 4800);
    int pk = 0;
    for (size_t i = 0; i < n; i++) if (abs(g_buf[2 * i]) > pk) pk = abs(g_buf[2 * i]);
    CHECK_EQ(pk, peaks[0]);
}

#include <math.h>
static void t_apipe_fm_stream_is_resampled(void) {
    static AudioPipe p;
    static int16_t fm[8192][2];
    const double chip_hz = 7987200.0 / 144.0;
    apipe_init(&p, 7987200, 144, 48000, 16000000);
    double ph = 0;
    size_t fed = 0, total_out = 0;
    for (int round = 0; round < 10; round++) {                              // one second of a 1 kHz tone at the chip's rate, in pieces
        size_t n = (size_t)(chip_hz / 10.0);
        if (n > 8192) n = 8192;
        for (size_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(sin(ph) * 8000.0);
            fm[i][0] = v; fm[i][1] = (int16_t)(v / 2);
            ph += 2 * M_PI * 1000.0 / chip_hz;
        }
        apipe_run(&p, 1600000, &fm[0][0], n, 0);
        fed += n;
    }
    total_out = p.produced;
    CHECK_NEAR((double)total_out, 48000.0 * fed / chip_hz, 40);             // the rate converted, the timeline kept
    size_t n = apipe_take(&p, g_buf, APIPE_RING);
    CHECK(n > 8000);
    int crossings = 0, peak = 0;
    for (size_t i = 100; i < n; i++) {
        if ((g_buf[2 * i] < 0) != (g_buf[2 * (i - 1)] < 0)) crossings++;
        if (abs(g_buf[2 * i]) > peak) peak = abs(g_buf[2 * i]);
    }
    CHECK_NEAR(crossings / 2.0 / ((double)(n - 100) / 48000.0), 1000, 12);  // the pitch survives
    CHECK_NEAR(peak, 8000, 400);                                            // so does the level
    CHECK(abs(g_buf[2 * 500 + 1] - g_buf[2 * 500] / 2) < 200);              // and the stereo balance
}

// ------------------------------------------------------------------------------------------------ disks

static uint8_t *slurp(const char *name, size_t *n) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        size_t got = fread(b + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    fclose(f);
    *n = len;
    return b;
}

static uint64_t sum_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t h = 1469598103934665603ull;
    int c;
    while ((c = fgetc(f)) != EOF) h = (h ^ (uint64_t)c) * 1099511628211ull;
    fclose(f);
    return h;
}

static void path_in(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", g_dir, name); }

static void t_disk_loads_the_formats(void) {
    static DiskSlot s;
    char err[200], path[600];
    const char *names[4] = { "a.d88", "b.hdi", "c.fdi", "d.hdm" };
    Pc98DiskFormat want[4] = { PC98_DISK_D88, PC98_DISK_HDI, PC98_DISK_FDI, PC98_DISK_RAW };
    for (int i = 0; i < 4; i++) {
        path_in(path, sizeof path, names[i]);
        err[0] = 0;
        int rc = disk_load_file(&s, path, err, sizeof err);
        CHECK_EQ(rc, 0);
        if (rc) { printf("  %s: %s\n", names[i], err); continue; }
        CHECK(s.loaded);
        CHECK_EQ(s.disk.format, want[i]);
        CHECK(s.size > 1000);
        CHECK_EQ(disk_changed(&s, NULL), 0);                        // nothing is changed right after loading
        CHECK(!strcmp(s.path, path));
        disk_unload(&s);
        CHECK(!s.loaded && !s.data);
    }
    // the drive an image goes to
    DiskSlot slots[DISK_SLOTS];
    memset(slots, 0, sizeof slots);
    CHECK_EQ(disk_slot_for(PC98_DISK_HDI, slots), SLOT_HD1);
    CHECK_EQ(disk_slot_for(PC98_DISK_D88, slots), SLOT_FD1);
    CHECK_EQ(disk_slot_for(PC98_DISK_RAW, slots), SLOT_FD1);
    slots[SLOT_FD1].loaded = 1;
    CHECK_EQ(disk_slot_for(PC98_DISK_FDI, slots), SLOT_FD2);
    slots[SLOT_FD2].loaded = 1;
    CHECK_EQ(disk_slot_for(PC98_DISK_D88, slots), SLOT_FD1);        // both busy: the first is replaced
    slots[SLOT_HD1].loaded = 1;
    CHECK_EQ(disk_slot_for(PC98_DISK_HDI, slots), SLOT_HD2);
    CHECK_EQ(disk_slot_daua(SLOT_FD1), 0x90);
    CHECK_EQ(disk_slot_daua(SLOT_FD2), 0x91);
    CHECK_EQ(disk_slot_daua(SLOT_HD1), 0x80);
    CHECK_EQ(disk_slot_daua(SLOT_HD2), 0x81);
}

static void t_disk_errors_are_sentences(void) {
    static DiskSlot s;
    char err[300], path[600];
    path_in(path, sizeof path, "nonexistent.d88");
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), -1);
    CHECK(strlen(err) > 3);
    CHECK(!s.loaded);
    path_in(path, sizeof path, "junk.bin");
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), -1);
    CHECK(strstr(err, "not a PC-98 disk image"));
    path_in(path, sizeof path, "empty.d88");
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), -1);
    CHECK(strstr(err, "empty"));
    CHECK_EQ(disk_load_file(&s, g_dir, err, sizeof err), -1);       // a folder
    CHECK(strstr(err, "not a file"));
    path_in(path, sizeof path, "damaged.d88");
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), -1);
    CHECK(strstr(err, "damaged"));
    // a failed load leaves the drive as it was
    path_in(path, sizeof path, "a.d88");
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), 0);
    uint8_t *before = s.data;
    path_in(path, sizeof path, "junk.bin");
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), -1);
    CHECK(s.loaded && s.data == before);
    disk_unload(&s);
}

static void t_disk_overlay_never_reaches_the_file(void) {
    static DiskSlot s;
    char err[200], path[600];
    path_in(path, sizeof path, "b.hdi");
    uint64_t file_before = sum_file(path);
    CHECK(file_before != 0);
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), 0);
    // the guest writes three sectors (what pc98_bios_attach's `writable` pointer lets the HLE BIOS do)
    s.data[s.size - 1] ^= 0xFF;
    s.data[s.size / 2] ^= 0x01;
    s.data[s.size / 2 + 700] ^= 0x01;                               // a different block
    uint32_t first = 999999;
    CHECK_EQ(disk_changed(&s, &first), 3);
    CHECK_EQ(first, (s.size / 2) / DISK_BLOCK);
    s.data[s.size / 2 + 700] ^= 0x01;                               // writing the old value back is no change
    CHECK_EQ(disk_changed(&s, NULL), 2);
    CHECK_EQ(sum_file(path), file_before);                          // the file is untouched
    disk_unload(&s);
    CHECK_EQ(sum_file(path), file_before);
    // the whole run through the machine's BIOS: a write by the guest changes the copy and counts as one block
}

static void t_disk_save_as_is_always_a_new_file(void) {
    static DiskSlot s;
    char err[300], orig[600], out[600], out2[600];
    path_in(orig, sizeof orig, "a.d88");
    path_in(out, sizeof out, "saved1.d88");
    path_in(out2, sizeof out2, "saved2.d88");
    unlink(out);
    unlink(out2);
    uint64_t file_before = sum_file(orig);
    CHECK_EQ(disk_load_file(&s, orig, err, sizeof err), 0);
    s.data[2000] ^= 0x5A;
    CHECK_EQ(disk_changed(&s, NULL), 1);
    CHECK_EQ(disk_save_as(&s, orig, err, sizeof err), -1);          // never over the original
    CHECK(strstr(err, "exists"));
    CHECK_EQ(sum_file(orig), file_before);
    CHECK_EQ(disk_save_as(&s, out, err, sizeof err), 0);            // a new name works
    CHECK_EQ(disk_changed(&s, NULL), 0);                            // and is the new starting point
    CHECK(!strcmp(s.path, out));
    size_t n = 0;
    char name[64];
    snprintf(name, sizeof name, "saved1.d88");
    uint8_t *saved = slurp(name, &n);
    CHECK(saved && n == s.size);
    CHECK(saved && !memcmp(saved, s.data, n));
    CHECK(saved && saved[2000] != 0 && s.data[2000] == saved[2000]);
    free(saved);
    CHECK_EQ(disk_save_as(&s, out, err, sizeof err), -1);           // not even over the copy just made
    s.data[3000] ^= 0x11;
    CHECK_EQ(disk_changed(&s, NULL), 1);
    CHECK_EQ(disk_save_as(&s, out2, err, sizeof err), 0);
    CHECK_EQ(sum_file(orig), file_before);                          // the original was never touched, through all of it
    CHECK_EQ(disk_save_as(&s, "/nonexistent-folder/x.d88", err, sizeof err), -1);
    CHECK(strlen(err) > 3);
    CHECK_EQ(disk_save_as(&s, "", err, sizeof err), -1);
    CHECK_EQ(access("/nonexistent-folder/x.d88", F_OK), -1);
    disk_unload(&s);
    unlink(out);
    unlink(out2);
}

static void t_disk_virtual_config_comments_out_the_memory_lines(void) {
    static DiskSlot s;
    char err[200], path[600];
    path_in(path, sizeof path, "vcfg.d88");
    uint64_t file_before = sum_file(path);
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), 0);
    CHECK(memmem(s.data, s.size, "DEVICE=HIMEM.SYS", 16) != NULL);
    CHECK_EQ(disk_virtual_config(&s), 4);
    CHECK(memmem(s.data, s.size, "DEVICE=HIMEM.SYS", 16) == NULL);
    CHECK(memmem(s.data, s.size, "DEVICE=EMM386.EXE /UMB", 22) == NULL);
    static const char want[] = "REM HIMEM.SYS   \r\nREM EMM386.EXE /UMB   \r\nREM HIGH\r\nREM UMB\r\n";
    CHECK(memmem(s.data, s.size, want, sizeof want - 1) != NULL);    // same lengths, so nothing else moved
    CHECK_EQ(disk_changed(&s, NULL), 0);                            // the edit is the starting point, not a change by the guest
    CHECK_EQ(disk_virtual_config(&s), 0);                           // nothing left to edit
    s.data[100] ^= 1;
    CHECK_EQ(disk_changed(&s, NULL), 1);                            // real changes are still seen
    CHECK_EQ(sum_file(path), file_before);                          // the file is as it was
    disk_unload(&s);
    path_in(path, sizeof path, "a.d88");                            // an image without such lines is left alone
    CHECK_EQ(disk_load_file(&s, path, err, sizeof err), 0);
    CHECK_EQ(disk_virtual_config(&s), 0);
    disk_unload(&s);
}

static void t_disk_damaged_images_never_crash(void) {
    size_t n = 0;
    uint8_t *good[3] = { slurp("a.d88", &n), slurp("b.hdi", &n), slurp("c.fdi", &n) };
    size_t sizes[3];
    char *names[3] = { "a.d88", "b.hdi", "c.fdi" };
    for (int i = 0; i < 3; i++) { size_t k = 0; free(slurp(names[i], &k)); sizes[i] = k; }
    uint32_t rnd = 12345;
    int loaded = 0, refused = 0;
    static DiskSlot s;
    char err[200];
    for (int i = 0; i < 600; i++) {
        int which = i % 3;
        size_t len = sizes[which];
        uint8_t *copy = malloc(len + 1);
        memcpy(copy, good[which], len);
        rnd = rnd * 1664525u + 1013904223u;
        int kind = (int)(rnd >> 24) % 4;
        for (int k = 0; k < 1 + (int)(rnd >> 20) % 12; k++) {          // flip bytes, mostly in the headers
            rnd = rnd * 1664525u + 1013904223u;
            size_t at = (rnd >> 8) % (kind == 0 ? len : (len < 6000 ? len : 6000));
            copy[at] = (uint8_t)(rnd >> 3);
        }
        if (kind == 1) len = (rnd >> 8) % len;                           // truncated
        if (kind == 2 && len > 100) memset(copy, 0, 100);
        if (len == 0) len = 1;
        if (disk_load_memory(&s, copy, len, "damaged", err, sizeof err) == 0) { loaded++; CHECK(s.loaded); disk_changed(&s, NULL); }
        else { refused++; CHECK(strlen(err) > 3); }
        free(copy);
    }
    disk_unload(&s);
    CHECK(loaded > 0 && refused > 0);                                    // both outcomes happen; neither crashes (the sanitizers watch)
    for (int i = 0; i < 3; i++) free(good[i]);
}

// ------------------------------------------------------------------------------------------------ emu: font, drums

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static size_t make_wav(uint8_t *out, int channels, int bits, int rate, int frames, int16_t start) {
    memcpy(out, "RIFF", 4);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    out[20] = 1; out[21] = 0;
    out[22] = (uint8_t)channels; out[23] = 0;
    put32(out + 24, (uint32_t)rate);
    put32(out + 28, (uint32_t)(rate * channels * bits / 8));
    out[32] = (uint8_t)(channels * bits / 8); out[33] = 0;
    out[34] = (uint8_t)bits; out[35] = 0;
    memcpy(out + 36, "data", 4);
    uint32_t bytes = (uint32_t)(frames * channels * bits / 8);
    put32(out + 40, bytes);
    for (int i = 0; i < frames; i++) { int16_t v = (int16_t)(start + i); out[44 + 2 * i] = (uint8_t)v; out[45 + 2 * i] = (uint8_t)(v >> 8); }
    put32(out + 4, 36 + bytes);
    return 44 + bytes;
}

static void t_wav_parse(void) {
    uint8_t buf[1024];
    int16_t *pcm = NULL;
    uint32_t count = 0, rate = 0;
    size_t n = make_wav(buf, 1, 16, 44100, 100, -50);
    CHECK_EQ(emu_parse_wav_mono16(buf, n, &pcm, &count, &rate), 0);
    CHECK_EQ(count, 100);
    CHECK_EQ(rate, 44100);
    CHECK(pcm && pcm[0] == -50 && pcm[99] == 49);
    free(pcm);
    n = make_wav(buf, 2, 16, 44100, 100, 0);                                // stereo: not what the board's drums are
    CHECK_EQ(emu_parse_wav_mono16(buf, n, &pcm, &count, &rate), -1);
    n = make_wav(buf, 1, 8, 44100, 100, 0);
    CHECK_EQ(emu_parse_wav_mono16(buf, n, &pcm, &count, &rate), -1);
    n = make_wav(buf, 1, 16, 44100, 100, 0);
    CHECK_EQ(emu_parse_wav_mono16(buf, 20, &pcm, &count, &rate), -1);       // cut short
    put32(buf + 40, 0x7FFFFFFF);                                            // a data chunk that claims more than the file has
    CHECK_EQ(emu_parse_wav_mono16(buf, n, &pcm, &count, &rate), 0);
    CHECK_EQ(count, 100);
    free(pcm);
    CHECK_EQ(emu_parse_wav_mono16((const uint8_t *)"RIFFxxxxWAVE", 12, &pcm, &count, &rate), -1);
    CHECK_EQ(emu_parse_wav_mono16((const uint8_t *)"not a wav at all", 16, &pcm, &count, &rate), -1);
}

static void t_emu_rhythm_dir(void) {
    Pc98Config c;
    config_defaults(&c);
    Emu *e = emu_new(&c);
    CHECK(e != NULL);
    if (!e) return;
    char dir[600];
    path_in(dir, sizeof dir, "rhythm");
    CHECK_EQ(emu_set_rhythm_dir(e, dir), 6);
    for (int i = 0; i < 6; i++) CHECK(e->rhythm[i] && e->rhythm_count[i] == 100u + (uint32_t)i && e->rhythm_rate[i] == 44100);
    CHECK_EQ(emu_set_rhythm_dir(e, "/nonexistent-folder"), 0);
    CHECK(!e->rhythm[0]);
    CHECK_EQ(emu_set_rhythm_dir(e, NULL), 0);
    emu_free(e);
}

static void t_emu_font(void) {
    Pc98Config c;
    config_defaults(&c);
    Emu *e = emu_new(&c);
    CHECK(e != NULL);
    if (!e) return;
    char path[600], err[300];
    CHECK_EQ(e->font_kind, EMU_FONT_BUILTIN);
    CHECK(strstr(e->font_note, "built-in"));
    // a font ROM of the right size (made by the test: it holds a recognisable ANK 'A')
    path_in(path, sizeof path, "FONT.ROM");
    CHECK_EQ(emu_set_font(e, path, err, sizeof err), 0);
    CHECK_EQ(e->font_kind, EMU_FONT_ROM);
    CHECK(strstr(e->font_note, "FONT.ROM"));
    // not a font: refused, built-in font again, a reason
    path_in(path, sizeof path, "junk.bin");
    CHECK_EQ(emu_set_font(e, path, err, sizeof err), -1);
    CHECK(strstr(err, "not a font ROM"));
    CHECK_EQ(e->font_kind, EMU_FONT_BUILTIN);
    CHECK_EQ(emu_set_font(e, "/nonexistent/FONT.ROM", err, sizeof err), -1);
    CHECK(strstr(err, "cannot read"));
    CHECK_EQ(emu_set_font(e, NULL, err, sizeof err), 0);
    emu_free(e);
}

// ------------------------------------------------------------------------------------------------ the table

typedef struct { const char *name; void (*fn)(void); } Test;
static const Test g_tests[] = {
    { "keymap_game_keys", t_keymap_game_keys },
    { "keymap_table_is_consistent", t_keymap_table_is_consistent },
    { "keys_press_release", t_keys_press_release },
    { "keys_repeat_is_ignored", t_keys_repeat_is_ignored },
    { "keys_two_shift_keys", t_keys_two_shift_keys },
    { "keys_release_without_press", t_keys_release_without_press },
    { "keys_release_all", t_keys_release_all },
    { "config_defaults", t_config_defaults },
    { "config_parse_every_key", t_config_parse_every_key },
    { "config_bad_lines_are_skipped_and_reported", t_config_bad_lines_are_skipped_and_reported },
    { "config_round_trip", t_config_round_trip },
    { "recent_list", t_recent_list },
    { "pacer_real_time", t_pacer_real_time },
    { "pacer_speed_settings", t_pacer_speed_settings },
    { "pacer_speed_change_does_not_jump", t_pacer_speed_change_does_not_jump },
    { "pacer_forgives_a_long_stall", t_pacer_forgives_a_long_stall },
    { "pacer_caps_each_pass_and_loses_nothing", t_pacer_caps_each_pass_and_loses_nothing },
    { "pacer_lead_and_wait", t_pacer_lead_and_wait },
    { "pacer_measures_speed_and_frames", t_pacer_measures_speed_and_frames },
    { "apipe_timeline_without_a_chip", t_apipe_timeline_without_a_chip },
    { "apipe_buzzer_pitch", t_apipe_buzzer_pitch },
    { "apipe_overrun_drops_the_oldest", t_apipe_overrun_drops_the_oldest },
    { "apipe_underrun_is_counted_silence", t_apipe_underrun_is_counted_silence },
    { "apipe_volume_curve", t_apipe_volume_curve },
    { "apipe_fm_stream_is_resampled", t_apipe_fm_stream_is_resampled },
    { "disk_loads_the_formats", t_disk_loads_the_formats },
    { "disk_errors_are_sentences", t_disk_errors_are_sentences },
    { "disk_overlay_never_reaches_the_file", t_disk_overlay_never_reaches_the_file },
    { "disk_save_as_is_always_a_new_file", t_disk_save_as_is_always_a_new_file },
    { "disk_virtual_config_comments_out_the_memory_lines", t_disk_virtual_config_comments_out_the_memory_lines },
    { "disk_damaged_images_never_crash", t_disk_damaged_images_never_crash },
    { "wav_parse", t_wav_parse },
    { "emu_rhythm_dir", t_emu_rhythm_dir },
    { "emu_font", t_emu_font },
};

// ------------------------------------------------------------------------------------------------ run: a headless machine on a fake clock

typedef struct { uint64_t at; uint8_t code; int down; int host; } KeyEvent;

static void on_log(void *user, const char *line) { (void)user; printf("%s\n", line); }

static double cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void put_le16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); }
static void put_le32(FILE *f, uint32_t v) { put_le16(f, v & 0xFFFF); put_le16(f, v >> 16); }

static int cmp_key(const void *a, const void *b) {
    const KeyEvent *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

static int cmd_run(int argc, char **argv) {
    Pc98Config cfg;
    config_defaults(&cfg);
    const char *disks[DISK_SLOTS * 2], *font = NULL, *rhythm = NULL, *ppm = NULL, *wav = NULL, *clock = "261102120000", *saves[DISK_SLOTS] = { 0 };
    const char *dumps[16];
    int ndisks = 0, ndumps = 0, show_text = 0, show_changed = 0;
    double mcycles = 100;
    uint64_t step_ns = 4000000;
    KeyEvent keys[256];
    int nkeys = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--disk") && v && ndisks < DISK_SLOTS * 2) { disks[ndisks++] = v; i++; }
        else if (!strcmp(a, "--cycles") && v) { mcycles = strtod(v, NULL); i++; }
        else if (!strcmp(a, "--idle") && v) { cfg.idle = !strcmp(v, "on"); i++; }
        else if (!strcmp(a, "--cpu") && v) {
            cfg.cpu = !strcmp(v, "8086") ? CFG_CPU_8086 : !strcmp(v, "v30") ? CFG_CPU_V30 : !strcmp(v, "286") ? CFG_CPU_286 : !strcmp(v, "486") ? CFG_CPU_486 : CFG_CPU_386;
            i++;
        }
        else if (!strcmp(a, "--mhz") && v) { cfg.mhz = atoi(v); i++; }
        else if (!strcmp(a, "--fm") && v) { cfg.fm = !strcmp(v, "86") ? CFG_FM_86 : !strcmp(v, "26k") ? CFG_FM_26K : CFG_FM_NONE; i++; }
        else if (!strcmp(a, "--speed") && v) { cfg.speed = !strcmp(v, "max") ? 0 : atoi(v); i++; }
        else if (!strcmp(a, "--step-ns") && v) { step_ns = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--font") && v) { font = v; i++; }
        else if (!strcmp(a, "--rhythm") && v) { rhythm = v; i++; }
        else if (!strcmp(a, "--no-sound")) cfg.sound = 0;
        else if (!strcmp(a, "--clock") && v) { clock = v; i++; }
        else if ((!strcmp(a, "--key") || !strcmp(a, "--hostkey")) && v && nkeys + 2 < 256) {
            int host = !strcmp(a, "--hostkey");
            char *end;
            double at = strtod(v, &end);
            unsigned code = 0, ms = 60;
            int d = 1;
            if (*end != ':') { fprintf(stderr, "bad %s\n", a); return 2; }
            code = (unsigned)strtoul(end + 1, &end, 16);
            if (host) { if (*end == ':') d = atoi(end + 1); }
            else if (*end == ':') ms = (unsigned)strtoul(end + 1, NULL, 10);
            uint64_t t = (uint64_t)(at * 1e6);
            if (host) keys[nkeys++] = (KeyEvent){ t, (uint8_t)code, d, 1 };
            else {
                keys[nkeys++] = (KeyEvent){ t, (uint8_t)code, 1, 0 };
                keys[nkeys++] = (KeyEvent){ t + (uint64_t)ms * cfg.mhz * 1000, (uint8_t)code, 0, 0 };
            }
            i++;
        }
        else if (!strcmp(a, "--dump") && v && ndumps < 16) { dumps[ndumps++] = v; i++; }
        else if (!strcmp(a, "--text")) show_text = 1;
        else if (!strcmp(a, "--ppm") && v) { ppm = v; i++; }
        else if (!strcmp(a, "--wav") && v) { wav = v; i++; }
        else if (!strcmp(a, "--save") && v) {
            int slot = atoi(v);
            const char *c = strchr(v, ':');
            if (c && slot >= 0 && slot < DISK_SLOTS) saves[slot] = c + 1;
            i++;
        }
        else if (!strcmp(a, "--disk-changed")) show_changed = 1;
        else { fprintf(stderr, "pc98apptool run: unknown option %s\n", a); return 2; }
    }
    qsort(keys, (size_t)nkeys, sizeof keys[0], cmp_key);
    Emu *e = emu_new(&cfg);
    if (!e) { fprintf(stderr, "out of memory\n"); return 1; }
    e->log = on_log;
    e->clock_override = clock;
    char err[300];
    if (font && emu_set_font(e, font, err, sizeof err)) printf("FONT %s\n", err);
    if (rhythm) printf("RHYTHM %d\n", emu_set_rhythm_dir(e, rhythm));
    for (int i = 0; i < ndisks; i++) {
        DiskSlot probe;
        memset(&probe, 0, sizeof probe);
        int slot = -1;
        if (disk_load_file(&probe, disks[i], err, sizeof err)) { printf("DISKERR %s: %s\n", disks[i], err); return 1; }
        slot = disk_slot_for(probe.disk.format, e->disk);
        disk_unload(&probe);
        if (emu_insert(e, slot, disks[i], err, sizeof err)) { printf("DISKERR %s: %s\n", disks[i], err); return 1; }
    }
    if (emu_boot(e, -1, err, sizeof err)) { printf("BOOTERR %s\n", err); return 1; }

    FILE *wf = NULL;
    uint32_t wav_frames = 0;
    int wav_peak = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) { fprintf(stderr, "cannot write %s\n", wav); return 1; }
        fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wf);
        put_le32(wf, 16); put_le16(wf, 1); put_le16(wf, 2); put_le32(wf, 48000); put_le32(wf, 48000 * 4); put_le16(wf, 4); put_le16(wf, 16);
        fwrite("data\0\0\0\0", 1, 8, wf);
    }
    uint64_t start_cycles = e->m->cpu.cycles, budget = (uint64_t)(mcycles * 1e6), now = 1000000000ull, steps = 0, pictures = 0;
    int ki = 0;
    double cpu0 = cpu_seconds();
    while (e->m->cpu.cycles - start_cycles < budget && !e->stopped) {
        now += step_ns;
        while (ki < nkeys && keys[ki].at <= e->m->cpu.cycles - start_cycles) {
            if (keys[ki].host) emu_host_key(e, keys[ki].code, keys[ki].down);
            else emu_key(e, keys[ki].code, keys[ki].down);
            ki++;
        }
        unsigned what = emu_step(e, now);
        steps++;
        if (what & EMU_PICTURE) pictures++;
        if (wf) {
            int16_t buf[1024][2];
            size_t n;
            while ((n = emu_audio_take(e, &buf[0][0], 1024)) > 0) {
                for (size_t k = 0; k < 2 * n; k++) { put_le16(wf, (uint16_t)(&buf[0][0])[k]); if (abs((&buf[0][0])[k]) > wav_peak) wav_peak = abs((&buf[0][0])[k]); }
                wav_frames += (uint32_t)n;
            }
        }
        if (e->m->cpu.cycles - start_cycles >= budget) break;
    }
    double cpu_s = cpu_seconds() - cpu0;
    Pc98 *m = e->m;
    printf("END cycles=%llu instructions=%llu frames=%llu steps=%llu pictures=%llu stopped=%d halted=%d cs:ip=%04X:%04X\n",
           (unsigned long long)(m->cpu.cycles - start_cycles), (unsigned long long)m->cpu.instructions, (unsigned long long)m->frames,
           (unsigned long long)steps, (unsigned long long)pictures, e->stopped, m->cpu.halted, m->cpu.s[X86_CS], m->cpu.ip);
    if (e->stopped) printf("STOP %s\n", e->stop_msg);
    printf("IDLE enabled=%d probes=%llu hits=%llu skips=%llu skipped=%llu skipped_instr=%llu near=%llu room=%llu short=%llu\n", e->idle->enabled,
           (unsigned long long)e->idle->probes, (unsigned long long)e->idle->hits, (unsigned long long)e->idle->skips,
           (unsigned long long)e->idle->skipped_clocks, (unsigned long long)e->idle->skipped_instrs, (unsigned long long)e->idle->near_event,
           (unsigned long long)e->idle->room_limited, (unsigned long long)e->idle->too_short);
    printf("HASH %016llx\n", (unsigned long long)emu_state_hash(e));
    printf("PACER resyncs=%u speed_x10=%u fps_x10=%u\n", e->pacer.resyncs, e->pacer.speed_x10, e->pacer.fps_x10);
    printf("CPUTIME cpu_s=%.3f x_real=%.1f\n", cpu_s, cpu_s > 0 ? (double)(m->cpu.cycles - start_cycles) / m->cpu_hz / cpu_s : 0.0);
    printf("BIOS disk_reads=%u disk_writes=%u unknown=%u\n", m->bios.disk_reads, m->bios.disk_writes, m->bios.unknown);
    if (show_changed) {
        for (int s = 0; s < DISK_SLOTS; s++)
            if (e->disk[s].loaded) printf("DISK %d %s changed=%u\n", s, disk_slot_name(s), emu_changed_blocks(e, s));
        printf("DISKTOTAL %u\n", emu_changed_total(e));
    }
    for (int s = 0; s < DISK_SLOTS; s++)
        if (saves[s]) {
            int rc = emu_save(e, s, saves[s], err, sizeof err);
            printf("SAVE %d rc=%d %s\n", s, rc, rc ? err : "ok");
        }
    for (int d = 0; d < ndumps; d++) {
        unsigned addr, len;
        if (sscanf(dumps[d], "%x:%x", &addr, &len) != 2) continue;
        for (unsigned o = 0; o < len; o += 16) {
            printf("MEM %05X:", addr + o);
            for (unsigned k = 0; k < 16 && o + k < len; k++) printf(" %02X", pc98_read8(m, (addr + o + k) & 0xFFFFF));
            printf("\n");
        }
    }
    if (show_text) {
        char buf[PC98_TEXT_ROWS * (PC98_TEXT_COLS + 1) + 1];
        emu_text(e, buf);
        for (char *p = buf; *p;) {
            char *nl = strchr(p, '\n');
            printf("|%.*s\n", (int)(nl - p), p);
            p = nl + 1;
        }
    }
    if (ppm) {
        emu_render(e);
        FILE *f = fopen(ppm, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", EMU_W, EMU_H);
            for (int i = 0; i < EMU_W * EMU_H; i++) { fputc((int)((e->pixels[i] >> 16) & 255), f); fputc((int)((e->pixels[i] >> 8) & 255), f); fputc((int)(e->pixels[i] & 255), f); }
            fclose(f);
        }
    }
    if (wf) {
        uint32_t bytes = wav_frames * 4;
        fseek(wf, 4, SEEK_SET);
        put_le32(wf, 36 + bytes);
        fseek(wf, 40, SEEK_SET);
        put_le32(wf, bytes);
        fclose(wf);
        printf("AUDIO frames=%u peak=%d produced=%llu dropped=%llu underruns=%llu\n", wav_frames, wav_peak, (unsigned long long)e->audio->produced,
               (unsigned long long)e->audio->dropped, (unsigned long long)e->audio->underruns);
    }
    emu_free(e);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        for (unsigned i = 0; i < sizeof g_tests / sizeof g_tests[0]; i++) printf("%s\n", g_tests[i].name);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "test")) {
        if (argc >= 4) g_dir = argv[3];
        for (unsigned i = 0; i < sizeof g_tests / sizeof g_tests[0]; i++) {
            if (strcmp(g_tests[i].name, argv[2])) continue;
            g_tests[i].fn();
            if (g_failed) { printf("FAILED %s (%d)\n", argv[2], g_failed); return 1; }
            printf("PASS %s\n", argv[2]);
            return 0;
        }
        printf("NO SUCH TEST %s\n", argv[2]);
        return 2;
    }
    if (argc >= 2 && !strcmp(argv[1], "run")) return cmd_run(argc - 1, argv + 1);
    fprintf(stderr, "usage: pc98apptool list | test NAME [DIR] | run [options] (see the header of tests/host/pc98apptool.c)\n");
    return 2;
}
