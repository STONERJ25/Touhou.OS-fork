/* emu: the PC-98 machine of the application, without a window (emu.h). */
#include "emu.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <font8x16.h>

#define CHUNK 256                       /* clocks per stretch: the machine's own slice, so the devices see what they see without the application */
#define LEAD_NS 50000000ull             /* the machine runs 50 ms ahead of the wall clock while there is sound to feed */
#define MAX_BEHIND_NS 250000000ull      /* further behind than this and the debt is forgiven */

static void logf_(Emu *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(Emu *e, const char *fmt, ...) {
    if (!e->log) return;
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->log(e->log_user, line);
}

static int say(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int say(char *err, size_t cap, const char *fmt, ...) {
    if (err && cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return -1;
}

Emu *emu_new(const Pc98Config *cfg) {
    Emu *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->m = calloc(1, sizeof *e->m);
    e->idle = calloc(1, sizeof *e->idle);
    e->audio = calloc(1, sizeof *e->audio);
    if (!e->m || !e->idle || !e->audio) { emu_free(e); return NULL; }
    if (cfg) e->cfg = *cfg; else config_defaults(&e->cfg);
    e->sound = e->cfg.sound;
    e->boot_dev = -1;
    emu_set_font(e, NULL, NULL, 0);
    return e;
}

void emu_free(Emu *e) {
    if (!e) return;
    for (int i = 0; i < DISK_SLOTS; i++) disk_unload(&e->disk[i]);
    for (int i = 0; i < OPNA_RHYTHMS; i++) free(e->rhythm[i]);
    free(e->font_rom);
    free(e->fm);
    free(e->m);
    free(e->idle);
    free(e->audio);
    free(e);
}

/* ---------------------------------------------------------------- files: the font and the drum samples */

static uint8_t *read_file(const char *path, size_t *len, size_t max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            if (cap >= max) break;
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        ssize_t got = read(fd, buf + n, cap - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += (size_t)got;
    }
    close(fd);
    if (buf && n >= max) { free(buf); return NULL; }
    *len = n;
    return buf;
}

int emu_set_font(Emu *e, const char *path, char *err, size_t errcap) {
    free(e->font_rom);
    e->font_rom = NULL;
    e->font_len = 0;
    e->font_kind = EMU_FONT_BUILTIN;
    pc98gfx_ank_from_unicode(e->ank, font8x16_glyph);
    snprintf(e->font_note, sizeof e->font_note, "built-in font: ASCII only, no kanji (set font= in ~/.config/pc98/config)");
    if (!path || !*path) return 0;
    size_t n = 0;
    uint8_t *rom = read_file(path, &n, 1u << 20);
    if (!rom) return say(err, errcap, "cannot read the font ROM %s: %s", path, errno ? strerror(errno) : "it is larger than 1 MiB");
    if (pc98gfx_fontrom_valid(rom, n)) {
        e->font_rom = rom;
        e->font_len = n;
        e->font_kind = EMU_FONT_ROM;
        snprintf(e->font_note, sizeof e->font_note, "font: %s", disk_basename(path));
        return 0;
    }
    if (n >= 0x800 + sizeof e->ank) {           /* some other dump: its 8x16 ANK table at 800h (no kanji) */
        memcpy(e->ank, rom + 0x800, sizeof e->ank);
        e->font_kind = EMU_FONT_ANK;
        snprintf(e->font_note, sizeof e->font_note, "font: %s (ANK only, no kanji)", disk_basename(path));
        free(rom);
        return 0;
    }
    free(rom);
    pc98gfx_ank_from_unicode(e->ank, font8x16_glyph);
    return say(err, errcap, "%s is not a font ROM (it should be 288,768 bytes); using the built-in font", path);
}

static void apply_font(Emu *e) {
    if (e->font_kind == EMU_FONT_ROM) pc98gfx_set_font(&e->m->gfx, pc98gfx_fontrom_ank16(e->font_rom), pc98gfx_fontrom_kanji, e->font_rom);
    else pc98gfx_set_font(&e->m->gfx, e->ank, NULL, NULL);
}

int emu_parse_wav_mono16(const uint8_t *buf, size_t len, int16_t **pcm, uint32_t *count, uint32_t *rate) {
    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return -1;
    size_t pos = 12;
    unsigned channels = 0, bits = 0, format = 0;
    uint32_t rt = 0;
    while (pos + 8 <= len) {
        uint32_t n = buf[pos + 4] | buf[pos + 5] << 8 | buf[pos + 6] << 16 | (uint32_t)buf[pos + 7] << 24;
        const uint8_t *d = buf + pos + 8;
        if (n > len - pos - 8) n = (uint32_t)(len - pos - 8);       /* a data chunk that claims more than the file has */
        if (!memcmp(buf + pos, "fmt ", 4) && n >= 16) {
            format = d[0] | d[1] << 8;
            channels = d[2] | d[3] << 8;
            rt = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
            bits = d[14] | d[15] << 8;
        } else if (!memcmp(buf + pos, "data", 4)) {
            if (format != 1 || channels != 1 || bits != 16 || !rt) return -1;
            uint32_t frames = n / 2;
            int16_t *out = malloc(frames ? (size_t)frames * 2 : 2);
            if (!out) return -1;
            for (uint32_t i = 0; i < frames; i++) out[i] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8);
            *pcm = out;
            *count = frames;
            *rate = rt;
            return 0;
        }
        pos += 8 + (size_t)n + (n & 1);
    }
    return -1;
}

int emu_set_rhythm_dir(Emu *e, const char *dir) {
    static const char *const names[OPNA_RHYTHMS] = { "BD", "SD", "TOP", "HH", "TOM", "RIM" };
    for (int i = 0; i < OPNA_RHYTHMS; i++) { free(e->rhythm[i]); e->rhythm[i] = NULL; e->rhythm_count[i] = e->rhythm_rate[i] = 0; }
    e->rhythm_loaded = 0;
    if (!dir || !*dir) return 0;
    for (int i = 0; i < OPNA_RHYTHMS; i++) {
        char path[CONFIG_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/2608_%s.WAV", dir, names[i]);
        size_t n = 0;
        uint8_t *buf = read_file(path, &n, 4u << 20);
        if (!buf) continue;
        if (emu_parse_wav_mono16(buf, n, &e->rhythm[i], &e->rhythm_count[i], &e->rhythm_rate[i]) == 0) e->rhythm_loaded++;
        free(buf);
    }
    return e->rhythm_loaded;
}

/* ---------------------------------------------------------------- disks */

static void attach_slot(Emu *e, int slot) {
    DiskSlot *s = &e->disk[slot];
    pc98_bios_attach(e->m, disk_slot_daua(slot), s->loaded ? &s->disk : NULL, s->loaded ? s->data : NULL);
}

int emu_insert(Emu *e, int slot, const char *path, char *err, size_t errcap) {
    if (slot < 0 || slot >= DISK_SLOTS) return say(err, errcap, "no such drive");
    if (disk_load_file(&e->disk[slot], path, err, errcap)) return -1;
    int edited = e->cfg.vconfig ? disk_virtual_config(&e->disk[slot]) : 0;
    if (e->booted) attach_slot(e, slot);
    logf_(e, "pc98app: %s: %s (%s)", disk_slot_name(slot), path, pc98_disk_format_name(e->disk[slot].disk.format));
    if (edited) logf_(e, "pc98app: %s: %d extended-memory lines of CONFIG.SYS commented out in the copy", disk_slot_name(slot), edited);
    return 0;
}

int emu_insert_auto(Emu *e, const char *path, int prefer, int *slot_out, char *err, size_t errcap) {
    DiskSlot tmp;
    memset(&tmp, 0, sizeof tmp);
    if (disk_load_file(&tmp, path, err, errcap)) return -1;
    int slot = disk_slot_for(tmp.disk.format, e->disk);
    if (prefer >= 0 && prefer < DISK_SLOTS && disk_slot_is_hd(prefer) == (tmp.disk.format == PC98_DISK_HDI)) slot = prefer;   /* the drive asked for, when the kind fits */
    disk_unload(&e->disk[slot]);
    e->disk[slot] = tmp;                        /* (a struct copy: the image's bytes stay where they are, the slot owns them now) */
    int edited = e->cfg.vconfig ? disk_virtual_config(&e->disk[slot]) : 0;
    if (e->booted) attach_slot(e, slot);
    logf_(e, "pc98app: %s: %s (%s)", disk_slot_name(slot), path, pc98_disk_format_name(e->disk[slot].disk.format));
    if (edited) logf_(e, "pc98app: %s: %d extended-memory lines of CONFIG.SYS commented out in the copy", disk_slot_name(slot), edited);
    if (slot_out) *slot_out = slot;
    return 0;
}

int emu_insert_memory(Emu *e, int slot, const uint8_t *bytes, size_t size, const char *name, char *err, size_t errcap) {
    if (slot < 0 || slot >= DISK_SLOTS) return say(err, errcap, "no such drive");
    if (disk_load_memory(&e->disk[slot], bytes, size, name, err, errcap)) return -1;
    if (e->cfg.vconfig) disk_virtual_config(&e->disk[slot]);
    if (e->booted) attach_slot(e, slot);
    return 0;
}

void emu_eject(Emu *e, int slot) {
    if (slot < 0 || slot >= DISK_SLOTS || !e->disk[slot].loaded) return;
    if (e->booted) pc98_bios_attach(e->m, disk_slot_daua(slot), NULL, NULL);
    disk_unload(&e->disk[slot]);
    logf_(e, "pc98app: %s: ejected", disk_slot_name(slot));
}

int emu_revert(Emu *e, int slot, char *err, size_t errcap) {
    if (slot < 0 || slot >= DISK_SLOTS || !e->disk[slot].loaded) return say(err, errcap, "there is no disk in this drive");
    char path[DISK_PATH_MAX];
    snprintf(path, sizeof path, "%s", e->disk[slot].path);
    if (!path[0]) return say(err, errcap, "this image has no file to read it back from");
    return emu_insert(e, slot, path, err, errcap);
}

int emu_save(Emu *e, int slot, const char *path, char *err, size_t errcap) {
    if (slot < 0 || slot >= DISK_SLOTS) return say(err, errcap, "no such drive");
    uint32_t first = 0, n = disk_changed(&e->disk[slot], &first);
    if (disk_save_as(&e->disk[slot], path, err, errcap)) return -1;
    logf_(e, "pc98app: saved %s (%u blocks changed)", path, n);
    return 0;
}

uint32_t emu_changed_blocks(Emu *e, int slot) { return slot >= 0 && slot < DISK_SLOTS ? disk_changed(&e->disk[slot], NULL) : 0; }

uint32_t emu_changed_total(Emu *e) {
    uint32_t n = 0;
    for (int i = 0; i < DISK_SLOTS; i++) n += disk_changed(&e->disk[i], NULL);
    e->disk_writes_seen = e->m->bios.disk_writes;
    return n;
}

int emu_disks_written(const Emu *e) { return e->m->bios.disk_writes != e->disk_writes_seen; }

/* ---------------------------------------------------------------- the machine */

static uint8_t to_bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

static void set_calendar(Emu *e) {
    uint8_t bcd[6];
    if (e->clock_override && strlen(e->clock_override) == 12) {          /* YYMMDDhhmmss */
        const char *c = e->clock_override;
        for (int i = 0; i < 6; i++) bcd[i] = (uint8_t)((c[2 * i] - '0') << 4 | (c[2 * i + 1] - '0'));
        bcd[1] = (uint8_t)(((bcd[1] >> 4) * 10 + (bcd[1] & 15)) << 4);   /* the month as a number in the high nibble */
    } else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        bcd[0] = to_bcd((unsigned)(tm.tm_year + 1900) % 100);
        bcd[1] = (uint8_t)((tm.tm_mon + 1) << 4);
        bcd[2] = to_bcd((unsigned)tm.tm_mday);
        bcd[3] = to_bcd((unsigned)tm.tm_hour);
        bcd[4] = to_bcd((unsigned)tm.tm_min);
        bcd[5] = to_bcd((unsigned)tm.tm_sec);
    }
    pc98_bios_set_clock(e->m, bcd);
}

int emu_boot(Emu *e, int da_ua, char *err, size_t errcap) {
    if (da_ua < 0) {
        static const int order[DISK_SLOTS] = { SLOT_FD1, SLOT_FD2, SLOT_HD1, SLOT_HD2 };
        for (int i = 0; i < DISK_SLOTS && da_ua < 0; i++) if (e->disk[order[i]].loaded) da_ua = disk_slot_daua(order[i]);
        if (da_ua < 0) return say(err, errcap, "there is no disk in any drive: insert a disk image first");
    }
    Pc98 *m = e->m;
    pc98_init(m, (X86Model)e->cfg.cpu, (uint32_t)e->cfg.mhz * 1000000u);
    apply_font(e);
    pc98_bios_init(m);
    if (e->cfg.fm != CFG_FM_NONE) {
        if (!e->fm) e->fm = calloc(1, sizeof *e->fm);
        if (e->fm) {
            pc98_attach_opna(m, e->fm, e->cfg.fm == CFG_FM_86 ? OPNA_BOARD_86 : OPNA_BOARD_26K, 12);
            e->fm->synth = e->sound ? 1 : 0;        /* a silent machine only needs the chip's timers and interrupts */
            for (int i = 0; i < OPNA_RHYTHMS; i++)
                if (e->rhythm[i]) opna_set_rhythm_sample(e->fm, i, e->rhythm[i], e->rhythm_count[i], e->rhythm_rate[i]);
        }
    }
    for (int i = 0; i < DISK_SLOTS; i++) if (e->disk[i].loaded) attach_slot(e, i);
    set_calendar(e);
    e->booted = 0;
    if (pc98_bios_boot(m, (uint8_t)da_ua)) {
        e->stopped = 1;
        snprintf(e->stop_msg, sizeof e->stop_msg, "Nothing to boot from device %02X: the disk has no readable first sector.", da_ua);
        return say(err, errcap, "%s", e->stop_msg);
    }
    e->boot_dev = da_ua;
    e->booted = 1;
    e->stopped = 0;
    e->paused = 0;
    e->stop_msg[0] = 0;
    e->have_picture = 0;
    e->last_frames = 0;
    e->audio_clocks = 0;
    e->disk_writes_seen = 0;
    e->boot_cycles_mark = m->cpu.cycles;
    keys_init(&e->keys);
    idle_init(e->idle, e->cfg.idle);
    uint32_t num = 0, den = 0;
    if (e->fm) { num = opna_rate_num(e->fm); den = opna_rate_den(e->fm); }
    apipe_init(e->audio, num, den, EMU_AUDIO_RATE, m->cpu_hz);
    apipe_set_volume(e->audio, e->cfg.volume);
    pacer_init(&e->pacer, m->cpu_hz, (uint32_t)e->cfg.speed, e->sound ? LEAD_NS : 0, MAX_BEHIND_NS);
    pacer_set_done(&e->pacer, m->cpu.cycles);
    emu_render(e);
    logf_(e, "pc98app: boot %02X (%s %u MHz%s)", da_ua, x86_model_name(m->cpu.model), m->cpu_hz / 1000000u, e->fm ? ", FM board" : "");
    return 0;
}

void emu_pause(Emu *e, int on, uint64_t now_ns) {
    if (on == e->paused) return;
    e->paused = on;
    emu_release_keys(e);
    apipe_clear(e->audio);
    if (!on) pacer_rebase(&e->pacer, now_ns);
    logf_(e, "pc98app: %s", on ? "paused" : "resumed");
}

void emu_hold(Emu *e, int on, uint64_t now_ns) {
    if (on == e->held) return;
    e->held = on;
    emu_release_keys(e);
    apipe_clear(e->audio);
    if (!on && e->booted) pacer_rebase(&e->pacer, now_ns);      /* the time spent in a menu is not owed to the game */
}

void emu_set_speed(Emu *e, int percent, uint64_t now_ns) {
    e->cfg.speed = percent;
    if (e->booted) pacer_set_speed(&e->pacer, (uint32_t)percent, now_ns);
}

void emu_set_sound(Emu *e, int on) {
    e->sound = on;
    e->cfg.sound = on;
    if (e->fm) e->fm->synth = on ? 1 : 0;
    if (e->booted) {
        apipe_clear(e->audio);
        e->pacer.lead_ns = on ? LEAD_NS : 0;
    }
}

void emu_set_volume(Emu *e, int percent) {
    e->cfg.volume = percent;
    if (e->booted) apipe_set_volume(e->audio, percent);
}

void emu_set_idle(Emu *e, int on) {
    e->cfg.idle = on;
    if (e->booted) e->idle->enabled = on;
}

void emu_render(Emu *e) {
    pc98_render(e->m, e->pixels, EMU_W);
    e->picture_frame = e->m->frames;
    e->have_picture = 1;
}

/* ---------------------------------------------------------------- running */

/* What the sound pipe is owed for the machine time since the last call: the chip's frames (as many as it made) and the buzzer's tone. */
static void feed_audio(Emu *e) {
    Pc98 *m = e->m;
    if (!e->sound) {
        e->audio_clocks = 0;
        if (m->fm) { int16_t trash[256][2]; while (opna_take(m->fm, &trash[0][0], 256) == 256) { } }
        return;
    }
    uint32_t beep = m->beep_on ? pc98_beep_hz(m) : 0;
    uint64_t clocks = e->audio_clocks;
    e->audio_clocks = 0;
    if (!m->fm || !m->fm->synth) {
        apipe_run(e->audio, clocks, NULL, 0, beep);
        return;
    }
    int16_t buf[1024][2];
    for (;;) {
        size_t n = opna_take(m->fm, &buf[0][0], 1024);
        apipe_run(e->audio, clocks, &buf[0][0], n, beep);
        clocks = 0;
        if (n < 1024) break;
    }
}

static const char *stop_reason(const Pc98 *m) { return pc98_stop_name(m->cpu.stop); }

static void describe_stop(Emu *e) {
    const X86Cpu *c = &e->m->cpu;
    char where[48];
    snprintf(where, sizeof where, "%04X:%04X", c->s[X86_CS], c->ip);
    switch (c->stop) {
    case X86_STOP_UNIMPLEMENTED:
        if (c->bad_opcode == 0x0F) snprintf(e->stop_msg, sizeof e->stop_msg, "The program ran an instruction the emulator does not have: opcode 0F %02X at %s.", c->bad_opcode2, where);
        else snprintf(e->stop_msg, sizeof e->stop_msg, "The program ran an instruction the emulator does not have: opcode %02X at %s.", c->bad_opcode, where);
        break;
    case X86_STOP_PROTECTED:
        snprintf(e->stop_msg, sizeof e->stop_msg, "The program tried to enter protected mode, which the emulator does not have (at %s).", where);
        break;
    case X86_STOP_SHUTDOWN:
        snprintf(e->stop_msg, sizeof e->stop_msg, "The CPU shut down: a fault while it was handling a fault (at %s).", where);
        break;
    case X86_STOP_PREFIXES:
        snprintf(e->stop_msg, sizeof e->stop_msg, "The program is an endless run of instruction prefixes (at %s).", where);
        break;
    case PC98_STOP_BOOT_FAILED:
        snprintf(e->stop_msg, sizeof e->stop_msg, "Nothing to boot: the boot sector gave control back to the BIOS.");
        break;
    case PC98_STOP_ROM_ENTRY:
        snprintf(e->stop_msg, sizeof e->stop_msg, "The program jumped into the BIOS at an address the emulator does not know (%s).", where);
        break;
    case PC98_STOP_FAULT_LOOP:
        snprintf(e->stop_msg, sizeof e->stop_msg, "The program raises the same CPU exception again and again (at %s).", where);
        break;
    case PC98_STOP_RESET:
        snprintf(e->stop_msg, sizeof e->stop_msg, "The program asked the machine to restart.");
        break;
    default:
        if (c->halted && !(c->flags & X86_IF)) snprintf(e->stop_msg, sizeof e->stop_msg, "The CPU halted with interrupts off (at %s): nothing can wake it. Use Reset.", where);
        else snprintf(e->stop_msg, sizeof e->stop_msg, "The machine stopped (%s) at %s.", stop_reason(e->m), where);
        break;
    }
}

unsigned emu_step(Emu *e, uint64_t now_ns) {
    if (!e->booted || e->stopped || e->paused || e->held) return 0;
    Pc98 *m = e->m;
    unsigned flags = 0;
    if (!e->have_picture) { emu_render(e); flags |= EMU_PICTURE; }
    uint64_t due = pacer_due(&e->pacer, now_ns, m->cpu_hz / 10);
    uint64_t end = m->cpu.cycles + due;
    while (m->cpu.cycles < end) {
        uint64_t room = end - m->cpu.cycles, chunk = room < CHUNK ? room : CHUNK;
        e->audio_clocks += idle_run(e->idle, m, chunk, room);
        if (e->audio_clocks >= 2048 || (m->fm && m->fm->fifo_len >= 512)) feed_audio(e);
        if (m->frames != e->last_frames) {              /* a frame boundary went by */
            e->last_frames = m->frames;
            /* Draw it only when it is the last one this pass owes: a machine that is behind runs several frames and shows the newest. */
            if (end - m->cpu.cycles < m->frame_cycles - m->frame_pos) { emu_render(e); flags |= EMU_PICTURE; }
        }
        if (pc98_dead(m)) break;
    }
    feed_audio(e);
    pacer_set_done(&e->pacer, m->cpu.cycles);
    pacer_measure(&e->pacer, now_ns, m->frames);
    if (pc98_dead(m)) {
        if (m->cpu.stop == PC98_STOP_RESET && e->auto_restarts < 3 && m->cpu.cycles - e->boot_cycles_mark > m->cpu_hz) {
            char err[200];
            e->auto_restarts++;
            logf_(e, "pc98app: the program asked for a restart");
            if (emu_boot(e, e->boot_dev, err, sizeof err) == 0) return flags | EMU_REBOOTED | EMU_PICTURE;
        }
        describe_stop(e);
        e->stopped = 1;
        emu_render(e);
        logf_(e, "pc98app: stopped: %s", e->stop_msg);
        flags |= EMU_STOPPED | EMU_PICTURE;
    }
    return flags;
}

void emu_key(Emu *e, uint8_t key, int down) {
    if (!e->booted || e->stopped) return;
    pc98_key(e->m, key, down);
}

static void key_sink(void *user, uint8_t k, int down) { emu_key((Emu *)user, k, down); }

int emu_host_key(Emu *e, uint8_t fairy_code, int down) {
    return keys_event(&e->keys, fairy_code, down, key_sink, e);
}

void emu_release_keys(Emu *e) { keys_release_all(&e->keys, key_sink, e); }

size_t emu_audio_take(Emu *e, int16_t *out, size_t max) {
    if (!e->booted) return 0;
    return apipe_take(e->audio, out, max);
}

void emu_text(Emu *e, char *buf) { pc98_text_dump(e->m, buf); }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

uint64_t emu_state_hash(const Emu *e) {
    const Pc98 *m = e->m;
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, m->cpu.r, sizeof m->cpu.r);
    h = fnv(h, m->cpu.rh, sizeof m->cpu.rh);
    h = fnv(h, m->cpu.s, sizeof m->cpu.s);
    h = fnv(h, &m->cpu.ip, sizeof m->cpu.ip);
    h = fnv(h, &m->cpu.flags, sizeof m->cpu.flags);
    h = fnv(h, m->ram, sizeof m->ram);
    h = fnv(h, m->gfx.tvram, sizeof m->gfx.tvram);
    h = fnv(h, m->gfx.gvram, sizeof m->gfx.gvram);
    return h;
}
