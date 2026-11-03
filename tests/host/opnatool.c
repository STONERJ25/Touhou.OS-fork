// Host test driver for the YM2203/YM2608 model (user/lib/pc98/src/opna.c). Reads commands on stdin, numbers in hex
// unless noted, and prints one answer line per query. Also renders register scripts to WAV files for listening:
//
//     opnatool --script FILE.txt --out FILE.wav [--rate 48000] [--rhythm DIR]
//
// Commands:
//   init CHIP CLOCK          new chip: CHIP 2203|2608, CLOCK in Hz (decimal); synthesis on, default mix
//   board 26k|86 IRQ         a PC-98 board (IRQ decimal: 3, 10, 12, 13)
//   host HZ                  unit of 'adv' (decimal; 0 = master clocks)
//   w BANK REG VAL           address then data write
//   a BANK REG / d BANK VAL  address only / data only
//   st BANK / rd BANK        status / data read                                        -> "S v" / "D v"
//   bin PORT / bout PORT V   board port access (opna_board_in/out)                     -> bin: "B v" (-1: not ours)
//   adv N                    let N host ticks pass (decimal)
//   irq                      the interrupt line                                         -> "I level rising_edges"
//   next                     host ticks until a timer raises the line                   -> "N ticks" (-1: never)
//   synth 0|1 / dc 0|1       synthesis on/off, SSG DC blocker on/off
//   mix FM SSG RHY ADPCM     gains, decimal, 256 = 1.0
//   render N [FILE]          N native frames (opna_render)                              -> "H n crc32 min max"
//   take N [FILE]            up to N frames from the FIFO                               -> "H n crc32 min max"
//   frames N                 N native frames printed                                    -> N lines "F l r"
//   parts                    the last frame's parts                                     -> "P fm_l fm_r ssg rh_l rh_r ad_l ad_r"
//   trace N CH OP            N samples; after each, operator OP (0-3 = S1-S4) of CH     -> N lines "T att state phase out"
//   ssgframes N              N samples; the SSG part of each (box filtered, before the DC blocker) -> N lines "G sum"
//   opout PHASE MOD ATT      one operator lookup (MOD decimal, may be negative)        -> "V value"
//   inc FNUM BLOCK DT MUL PM phase increment (PM decimal)                               -> "C value"
//   table NAME               logsin|exptab|ssgdac|rhythm|pm                            -> "T name v0 v1 ..." (decimal)
//   rhythmpcm IDX RATE N SEED  a synthetic drum: N samples of a decaying square, rate decimal
//   ram ADDR N               ADPCM RAM bytes                                            -> "R b0 b1 ..."
//   poke ADDR B0 B1 ...      writes ADPCM RAM directly
//   rs NUM DEN OUT           resampler set-up (decimal)
//   rsrender N [FILE]        N frames at the resampler's rate                           -> "H n crc32 min max"
//   state                    -> "Z key=value ..."
//
// A whole machine, to run a driver-like program against the board (the integration a PC-98 emulator would do, through
// pc98.h's host port hooks; nothing in the machine itself changes):
//   pc98 MODEL MHZ BOARD IRQ   a PC-98 (8086|v30|286, MHz decimal) with a 26k|86 board, the board's interrupt line on
//                              the 8259s, the chip advanced to the CPU's clock at every port access and after every slice
//   pc98boot FILE              FILE as the first sector of a 2HD floppy, started the way the BIOS would  -> "B rc"
//   pc98run CYCLES [WAV]       runs (decimal); the audio it made, optionally to a WAV  -> "M used dead edges frames crc peak"
//   pc98text                   -> 25 lines "|<80 chars>"
//
//   bench SECONDS            all six FM channels (with LFO), three SSG tones, rendered for SECONDS of audio (decimal)
//                            -> "K audio_seconds cpu_seconds"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <opna.h>
#include <pc98.h>

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    c ^= 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static Opna *chip;
static OpnaResampler rs;
static int16_t *rhythm_pcm[OPNA_RHYTHMS];
static int16_t *loaded_wav[OPNA_RHYTHMS];

static void report(const int16_t *buf, size_t n, const char *file) {
    uint32_t crc = crc32_update(0, (const uint8_t *)buf, n * 4);
    int mn = 0, mx = 0;
    for (size_t i = 0; i < 2 * n; i++) {
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
    }
    if (file && *file && strcmp(file, "-")) {
        FILE *f = fopen(file, "wb");
        if (f) { fwrite(buf, 4, n, f); fclose(f); }
    }
    printf("H %zu %08x %d %d\n", n, crc, mn, mx);
}

/* ---- WAV files */

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static int write_wav(const char *path, const int16_t *frames, size_t n, uint32_t rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("RIFF", 1, 4, f);
    put32(f, (uint32_t)(36 + n * 4));
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, (uint32_t)(n * 4));
    for (size_t i = 0; i < 2 * n; i++) put16(f, (uint16_t)frames[i]);
    fclose(f);
    return 0;
}

/* A 16-bit mono PCM WAV (the rhythm samples): returns the samples (malloc'd), the count and the rate. */
static int16_t *read_wav_mono(const char *path, uint32_t *count, uint32_t *rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static uint8_t buf[1 << 20];
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return NULL;
    size_t pos = 12;
    unsigned channels = 0, bits = 0;
    *rate = 0;
    while (pos + 8 <= len) {
        uint32_t n = buf[pos + 4] | buf[pos + 5] << 8 | buf[pos + 6] << 16 | (uint32_t)buf[pos + 7] << 24;
        const uint8_t *d = buf + pos + 8;
        if (n > len - pos - 8) break;
        if (!memcmp(buf + pos, "fmt ", 4) && n >= 16) {
            channels = d[2] | d[3] << 8;
            *rate = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
            bits = d[14] | d[15] << 8;
        } else if (!memcmp(buf + pos, "data", 4) && channels == 1 && bits == 16) {
            *count = n / 2;
            int16_t *s = malloc(n ? n : 2);
            for (uint32_t i = 0; i < *count; i++) s[i] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8);
            return s;
        }
        pos += 8 + n + (n & 1);
    }
    return NULL;
}

static void load_rhythm_dir(const char *dir) {
    static const char *names[OPNA_RHYTHMS] = { "BD", "SD", "TOP", "HH", "TOM", "RIM" };
    for (int i = 0; i < OPNA_RHYTHMS; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/2608_%s.WAV", dir, names[i]);
        uint32_t n = 0, rate = 0;
        int16_t *s = read_wav_mono(path, &n, &rate);
        if (!s) { fprintf(stderr, "opnatool: cannot read %s\n", path); continue; }
        free(loaded_wav[i]);
        loaded_wav[i] = s;
        opna_set_rhythm_sample(chip, i, s, n, rate);
    }
}

/* ---- register scripts
 *
 *   # comment                 anything after '#' is ignored
 *   chip 2203|2608 [CLOCK]    (first; default 2608 at 7987200 Hz)
 *   w BANK REG VAL            register write (hex), applied at the current time
 *   wait MS                   let MS milliseconds pass (decimal, fractions allowed)
 *   at MS                     let time pass until MS milliseconds from the start
 *   mix FM SSG RHY ADPCM      (decimal)
 *   end MS                    stop at MS (otherwise 500 ms after the last command)
 */
static size_t cap_frames, n_frames;
static int16_t *frames_buf;

static void collect(void) {
    for (;;) {
        if (n_frames + 1024 > cap_frames) {
            cap_frames = cap_frames ? cap_frames * 2 : 1 << 16;
            frames_buf = realloc(frames_buf, cap_frames * 4);
        }
        size_t got = opna_take(chip, frames_buf + 2 * n_frames, 1024);
        n_frames += got;
        if (got < 1024) break;
    }
}

static void run_until(double ms, double *now_ms) {
    if (ms <= *now_ms) return;
    /* master clocks from the start, so rounding never accumulates */
    uint64_t target = (uint64_t)(ms * chip->clock / 1000.0);
    while (chip->now < target) {
        uint64_t step = target - chip->now;
        if (step > 4096) step = 4096;
        opna_advance(chip, step);
        collect();
    }
    *now_ms = ms;
}

static int run_script(const char *script, const char *out, uint32_t rate, const char *rhythm_dir) {
    FILE *f = fopen(script, "r");
    if (!f) { fprintf(stderr, "opnatool: cannot open %s\n", script); return 1; }
    chip = calloc(1, sizeof *chip);
    opna_init(chip, OPNA_YM2608, 7987200);
    if (rhythm_dir) load_rhythm_dir(rhythm_dir);
    double now_ms = 0, end_ms = -1;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char cmd[32];
        if (sscanf(line, "%31s", cmd) != 1) continue;
        if (!strcmp(cmd, "chip")) {
            unsigned kind = 2608, clock = 0;
            sscanf(line, "%*s %u %u", &kind, &clock);
            int c = kind == 2203 ? OPNA_YM2203 : OPNA_YM2608;
            opna_init(chip, c, clock ? clock : (c == OPNA_YM2608 ? 7987200 : 3993600));
            if (rhythm_dir) load_rhythm_dir(rhythm_dir);
        } else if (!strcmp(cmd, "w")) {
            unsigned bank, reg, val;
            if (sscanf(line, "%*s %x %x %x", &bank, &reg, &val) != 3) { fprintf(stderr, "%s:%d: bad write\n", script, lineno); return 1; }
            opna_write_addr(chip, (int)bank, (uint8_t)reg);
            opna_write_data(chip, (int)bank, (uint8_t)val);
        } else if (!strcmp(cmd, "wait") || !strcmp(cmd, "at")) {
            double ms = 0;
            sscanf(line, "%*s %lf", &ms);
            run_until(cmd[0] == 'w' ? now_ms + ms : ms, &now_ms);
        } else if (!strcmp(cmd, "mix")) {
            int a = 256, b = 256, c = 128, d = 128;
            sscanf(line, "%*s %d %d %d %d", &a, &b, &c, &d);
            opna_set_mix(chip, a, b, c, d);
        } else if (!strcmp(cmd, "end")) {
            sscanf(line, "%*s %lf", &end_ms);
        } else {
            fprintf(stderr, "%s:%d: unknown command %s\n", script, lineno, cmd);
            return 1;
        }
    }
    fclose(f);
    run_until(end_ms >= 0 ? end_ms : now_ms + 500, &now_ms);
    int rc;
    if (rate) {
        /* resample everything to the requested rate */
        opna_resampler_init(&rs, opna_rate_num(chip), opna_rate_den(chip), rate);
        size_t cap = (size_t)((double)n_frames * rate * opna_rate_den(chip) / opna_rate_num(chip)) + 64;
        int16_t *outb = malloc(cap * 4);
        size_t used = n_frames;
        size_t made = opna_resample(&rs, frames_buf, &used, outb, cap);
        rc = write_wav(out, outb, made, rate);
        printf("W %zu %u %s\n", made, rate, out);
        free(outb);
    } else {
        uint32_t r = (uint32_t)((opna_rate_num(chip) + opna_rate_den(chip) / 2) / opna_rate_den(chip));
        rc = write_wav(out, frames_buf, n_frames, r);
        printf("W %zu %u %s\n", n_frames, r, out);
    }
    if (rc) fprintf(stderr, "opnatool: cannot write %s\n", out);
    return rc ? 1 : 0;
}

/* ---- a machine around the board */

static Pc98 *mach;
static uint64_t synced;                 /* CPU cycle the chip has been advanced to */
static uint8_t *floppy;
static Pc98Disk disk;

static void sync_chip(void) {
    uint64_t now = mach->cpu.cycles;
    if (now > synced) {
        opna_advance(chip, now - synced);
        synced = now;
    }
}

static int hook_in(void *host, uint16_t port) {
    sync_chip();
    return opna_board_in(chip, port);
}

static int hook_out(void *host, uint16_t port, uint8_t v) {
    sync_chip();
    return opna_board_out(chip, port, v);
}

static void board_irq(void *ctx, int level) {
    pc98_irq(mach, chip->board_irq, level);
}

static void machine_new(const char *model, unsigned mhz, const char *board, int irq) {
    if (!mach) mach = calloc(1, sizeof *mach);
    X86Model md = !strcmp(model, "8086") ? X86_MODEL_8086 : !strcmp(model, "286") ? X86_MODEL_80286 : X86_MODEL_V30;
    pc98_init(mach, md, mhz * 1000000u);
    opna_board_init(chip, !strcmp(board, "86") ? OPNA_BOARD_86 : OPNA_BOARD_26K, irq);
    opna_set_host_clock(chip, mach->cpu_hz);
    opna_set_irq_callback(chip, board_irq, NULL);
    mach->port_in = hook_in;
    mach->port_out = hook_out;
    mach->host = chip;
    synced = mach->cpu.cycles;
}

static int machine_boot(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -9;
    size_t size = 77u * 2 * 8 * 1024;           /* a raw 2HD floppy: 77 cylinders, 2 heads, 8 sectors of 1 KiB */
    free(floppy);
    floppy = calloc(1, size);
    size_t n = fread(floppy, 1, 1024, f);
    fclose(f);
    if (!n) return -8;
    int rc = pc98_disk_open(&disk, floppy, size, PC98_DISK_RAW);
    if (rc) return rc;
    return pc98_boot(mach, &disk, 0x90);
}

static void machine_run(uint64_t cycles, const char *wav) {
    uint64_t used = 0;
    n_frames = 0;
    while (used < cycles && !pc98_dead(mach)) {
        uint64_t slice = cycles - used;
        if (slice > 2000) slice = 2000;
        uint64_t to_irq = opna_ticks_to_irq(chip);  /* stop where the board's interrupt comes, so it is on time */
        if (to_irq != UINT64_MAX && to_irq + 1 < slice) slice = to_irq + 1;
        used += pc98_run(mach, slice);
        sync_chip();
        collect();
    }
    uint32_t crc = crc32_update(0, (const uint8_t *)frames_buf, n_frames * 4);
    int peak = 0;
    for (size_t i = 0; i < 2 * n_frames; i++) {
        int v = frames_buf[i] < 0 ? -frames_buf[i] : frames_buf[i];
        if (v > peak) peak = v;
    }
    if (wav && *wav) write_wav(wav, frames_buf, n_frames, 55466);
    printf("M %llu %d %u %zu %08x %d\n", (unsigned long long)used, pc98_dead(mach), chip->irq_count, n_frames, crc, peak);
}

static void bench(double seconds) {
    opna_init(chip, OPNA_YM2608, 7987200);
    static const uint8_t setup[][3] = {
        { 0, 0x29, 0x80 }, { 0, 0x22, 0x0B }, { 0, 0x07, 0x38 }, { 0, 0x08, 0x0C }, { 0, 0x09, 0x0B }, { 0, 0x0A, 0x0A },
        { 0, 0x00, 0xEF }, { 0, 0x02, 0xBD }, { 0, 0x04, 0x9F },
    };
    for (size_t i = 0; i < sizeof setup / sizeof setup[0]; i++) {
        opna_write_addr(chip, setup[i][0], setup[i][1]);
        opna_write_data(chip, setup[i][0], setup[i][2]);
    }
    for (int c = 0; c < 6; c++) {
        int bank = c / 3, cc = c % 3;
        for (int r = 0x30; r < 0xA0; r += 4) {
            uint8_t v = r < 0x40 ? 0x21 : r < 0x50 ? (uint8_t)(0x10 + 4 * cc) : r < 0x60 ? 0x1F : r < 0x70 ? 0x85 : 0x26;
            if (r >= 0x90) v = 0;
            opna_write_addr(chip, bank, (uint8_t)(r + cc));
            opna_write_data(chip, bank, v);
        }
        const uint8_t regs[][2] = { { 0xB0, (uint8_t)(c << 3 | c) }, { 0xB4, 0xF3 }, { 0xA4, 0x22 }, { 0xA0, (uint8_t)(0x6A + 9 * c) } };
        for (int i = 0; i < 4; i++) {
            opna_write_addr(chip, bank, (uint8_t)(regs[i][0] + cc));
            opna_write_data(chip, bank, regs[i][1]);
        }
        opna_write_addr(chip, 0, 0x28);
        opna_write_data(chip, 0, (uint8_t)(0xF0 | cc | (bank ? 4 : 0)));
    }
    size_t frames = (size_t)(seconds * 55466.67);
    static int16_t buf[4096 * 2];
    clock_t t0 = clock();
    for (size_t done = 0; done < frames;) {
        size_t n = frames - done < 4096 ? frames - done : 4096;
        opna_render(chip, buf, n);
        done += n;
    }
    double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("K %.3f %.3f\n", seconds, cpu);
}

/* ---- the command interpreter */

static void state(void) {
    Opna *o = chip;
    printf("Z chip=%d clock=%u now=%llu samples=%llu sample_clocks=%u ssg_clocks=%u flags=%x irq=%d irq_count=%u "
           "ta_left=%u tb_left=%u tb_pre=%u timer_ctrl=%x irq_enable=%x flag_mask=%x six=%d lfo=%x lfo_cnt=%u "
           "eg_cnt=%u fifo=%u dropped=%u adpcm_mode=%d adpcm_addr=%x adpcm_end=%x adpcm_playing=%d adpcm_x=%d "
           "adpcm_step=%d mem_writes=%u mem_reads=%u early=%u a460=%x busy_until=%llu size=%zu "
           "pcm_vol=%x pcm_ctrl=%x pcm_mode=%x pcm_bytes=%u\n",
           o->chip, o->clock, (unsigned long long)o->now, (unsigned long long)o->samples, o->sample_clocks,
           o->ssg_clocks, o->flags, o->irq, o->irq_count, o->ta_left, o->tb_left, o->tb_pre, o->timer_ctrl,
           o->irq_enable, o->flag_mask, o->six_channels, o->lfo, o->lfo_cnt, o->eg_cnt, o->fifo_len, o->fifo_dropped,
           o->adpcm.mode, o->adpcm.addr, o->adpcm.end, o->adpcm.playing, o->adpcm.x, o->adpcm.step,
           o->adpcm.mem_writes, o->adpcm.mem_reads, o->adpcm.early_writes, o->board_a460,
           (unsigned long long)o->busy_until, sizeof *o, o->pcm_volume, o->pcm_ctrl, o->pcm_mode, o->pcm_bytes);
}

int main(int argc, char **argv) {
    crc_init();
    if (argc > 1) {
        const char *script = NULL, *out = NULL, *rdir = NULL;
        uint32_t rate = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
            else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
            else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (uint32_t)strtoul(argv[++i], NULL, 10);
            else if (!strcmp(argv[i], "--rhythm") && i + 1 < argc) rdir = argv[++i];
            else { fprintf(stderr, "usage: opnatool [--script FILE --out FILE.wav [--rate HZ] [--rhythm DIR]]\n"); return 2; }
        }
        if (!script || !out) { fprintf(stderr, "opnatool: --script and --out go together\n"); return 2; }
        return run_script(script, out, rate, rdir);
    }
    chip = calloc(1, sizeof *chip);
    opna_init(chip, OPNA_YM2608, 7987200);
    static char line[16384];
    static int16_t buf[1 << 20];
    while (fgets(line, sizeof line, stdin)) {
        static char a1[4096], a2[4096];
        char cmd[32], a3[64] = "", a4[64] = "", a5[64] = "";
        a1[0] = a2[0] = 0;
        int n = sscanf(line, "%31s %4095s %4095s %63s %63s %63s", cmd, a1, a2, a3, a4, a5);
        if (n < 1) continue;
        unsigned long x1 = strtoul(a1, NULL, 16), x2 = strtoul(a2, NULL, 16), x3 = strtoul(a3, NULL, 16);
        if (!strcmp(cmd, "init")) {
            unsigned long kind = strtoul(a1, NULL, 10), clock = strtoul(a2, NULL, 10);
            opna_init(chip, kind == 2203 ? OPNA_YM2203 : OPNA_YM2608, (uint32_t)clock);
        } else if (!strcmp(cmd, "board")) {
            opna_board_init(chip, !strcmp(a1, "86") ? OPNA_BOARD_86 : OPNA_BOARD_26K, (int)strtol(a2, NULL, 10));
        } else if (!strcmp(cmd, "host")) {
            opna_set_host_clock(chip, (uint32_t)strtoul(a1, NULL, 10));
        } else if (!strcmp(cmd, "w")) {
            opna_write_addr(chip, (int)x1, (uint8_t)x2);
            opna_write_data(chip, (int)x1, (uint8_t)x3);
        } else if (!strcmp(cmd, "a")) {
            opna_write_addr(chip, (int)x1, (uint8_t)x2);
        } else if (!strcmp(cmd, "d")) {
            opna_write_data(chip, (int)x1, (uint8_t)x2);
        } else if (!strcmp(cmd, "st")) {
            printf("S %x\n", opna_read_status(chip, (int)x1));
        } else if (!strcmp(cmd, "rd")) {
            printf("D %x\n", opna_read_data(chip, (int)x1));
        } else if (!strcmp(cmd, "bin")) {
            int v = opna_board_in(chip, (uint16_t)x1);
            if (v < 0) printf("B -1\n"); else printf("B %x\n", v);
        } else if (!strcmp(cmd, "bout")) {
            printf("O %d\n", opna_board_out(chip, (uint16_t)x1, (uint8_t)x2));
        } else if (!strcmp(cmd, "adv")) {
            opna_advance(chip, strtoull(a1, NULL, 10));
        } else if (!strcmp(cmd, "irq")) {
            printf("I %d %u\n", opna_irq(chip), chip->irq_count);
        } else if (!strcmp(cmd, "next")) {
            uint64_t t = opna_ticks_to_irq(chip);
            if (t == UINT64_MAX) printf("N -1\n"); else printf("N %llu\n", (unsigned long long)t);
        } else if (!strcmp(cmd, "synth")) {
            chip->synth = (uint8_t)x1;
        } else if (!strcmp(cmd, "dc")) {
            chip->ssg_dc_block = (uint8_t)x1;
        } else if (!strcmp(cmd, "mix")) {
            opna_set_mix(chip, atoi(a1), atoi(a2), atoi(a3), atoi(a4));
        } else if (!strcmp(cmd, "render") || !strcmp(cmd, "take")) {
            size_t want = strtoul(a1, NULL, 10);
            if (want > sizeof buf / 4) want = sizeof buf / 4;
            size_t got = want;
            if (cmd[0] == 'r') opna_render(chip, buf, want);
            else got = opna_take(chip, buf, want);
            report(buf, got, a2);
        } else if (!strcmp(cmd, "frames")) {
            size_t want = strtoul(a1, NULL, 10);
            if (want > sizeof buf / 4) want = sizeof buf / 4;
            opna_render(chip, buf, want);
            for (size_t i = 0; i < want; i++) printf("F %d %d\n", buf[2 * i], buf[2 * i + 1]);
        } else if (!strcmp(cmd, "parts")) {
            printf("P %d %d %d %d %d %d %d\n", chip->last_fm[0], chip->last_fm[1], chip->last_ssg, chip->last_rhythm[0],
                   chip->last_rhythm[1], chip->last_adpcm[0], chip->last_adpcm[1]);
        } else if (!strcmp(cmd, "trace")) {
            unsigned long count = strtoul(a1, NULL, 10);
            int c = (int)strtol(a2, NULL, 10), op = (int)strtol(a3, NULL, 10);
            for (unsigned long i = 0; i < count; i++) {
                int16_t f[2];
                opna_render(chip, f, 1);
                const OpnaOp *p = &chip->ch[c].op[op];
                printf("T %u %u %u %d\n", p->att, p->eg_state, p->phase, p->out);
            }
        } else if (!strcmp(cmd, "ssgframes")) {
            unsigned long count = strtoul(a1, NULL, 10);
            for (unsigned long i = 0; i < count; i++) {
                int16_t f[2];
                opna_render(chip, f, 1);
                printf("G %d\n", chip->last_ssg);
            }
        } else if (!strcmp(cmd, "opout")) {
            printf("V %d\n", opna_op_output(chip, (uint32_t)x1, (int)strtol(a2, NULL, 10), (unsigned)x3));
        } else if (!strcmp(cmd, "inc")) {
            printf("C %u\n", opna_phase_inc(chip, (unsigned)x1, (unsigned)x2, (unsigned)x3, (unsigned)strtoul(a4, NULL, 16),
                                            (int)strtol(a5, NULL, 10)));
        } else if (!strcmp(cmd, "table")) {
            printf("T %s", a1);
            if (!strcmp(a1, "logsin")) for (int i = 0; i < 256; i++) printf(" %u", chip->logsin[i]);
            else if (!strcmp(a1, "exptab")) for (int i = 0; i < 256; i++) printf(" %u", chip->exptab[i]);
            else if (!strcmp(a1, "ssgdac")) for (int i = 0; i < 32; i++) printf(" %u", chip->ssg_dac[i]);
            else if (!strcmp(a1, "rhythm")) for (int i = 0; i < 96; i++) printf(" %u", chip->rhythm_gain[i]);
            else if (!strcmp(a1, "pm")) for (int i = 0; i < 64; i++) printf(" %u", chip->pm_ratio[i / 8][i % 8]);
            printf("\n");
        } else if (!strcmp(cmd, "rhythmpcm")) {
            int idx = (int)strtol(a1, NULL, 10);
            uint32_t rate = (uint32_t)strtoul(a2, NULL, 10), cnt = (uint32_t)strtoul(a3, NULL, 10);
            uint32_t seed = (uint32_t)strtoul(a4, NULL, 10);
            if (idx < 0 || idx >= OPNA_RHYTHMS) continue;
            free(rhythm_pcm[idx]);
            rhythm_pcm[idx] = malloc((cnt ? cnt : 1) * sizeof(int16_t));
            for (uint32_t i = 0; i < cnt; i++) {
                int amp = 20000 - (int)((uint64_t)i * 20000 / (cnt ? cnt : 1));
                rhythm_pcm[idx][i] = (int16_t)((((i / (seed ? seed : 8)) & 1) ? amp : -amp));
            }
            opna_set_rhythm_sample(chip, idx, rhythm_pcm[idx], cnt, rate);
        } else if (!strcmp(cmd, "ram")) {
            unsigned long cnt = x2 ? x2 : 1;
            printf("R");
            for (unsigned long i = 0; i < cnt && x1 + i < OPNA_ADPCM_RAM; i++) printf(" %x", chip->adpcm_ram[x1 + i]);
            printf("\n");
        } else if (!strcmp(cmd, "poke")) {
            char *p = line;
            strtok(p, " \t\n");
            char *t = strtok(NULL, " \t\n");
            unsigned long at = t ? strtoul(t, NULL, 16) : 0;
            while ((t = strtok(NULL, " \t\n")) && at < OPNA_ADPCM_RAM) chip->adpcm_ram[at++] = (uint8_t)strtoul(t, NULL, 16);
        } else if (!strcmp(cmd, "rs")) {
            opna_resampler_init(&rs, (uint32_t)strtoul(a1, NULL, 10), (uint32_t)strtoul(a2, NULL, 10),
                                (uint32_t)strtoul(a3, NULL, 10));
        } else if (!strcmp(cmd, "rsrender")) {
            size_t want = strtoul(a1, NULL, 10);
            if (want > sizeof buf / 4) want = sizeof buf / 4;
            opna_render_resampled(chip, &rs, buf, want);
            report(buf, want, a2);
        } else if (!strcmp(cmd, "state")) {
            state();
        } else if (!strcmp(cmd, "bench")) {
            bench(strtod(a1, NULL));
        } else if (!strcmp(cmd, "pc98")) {
            machine_new(a1, (unsigned)strtoul(a2, NULL, 10), a3, (int)strtol(a4, NULL, 10));
        } else if (!strcmp(cmd, "pc98boot")) {
            printf("B %d\n", mach ? machine_boot(a1) : -7);
        } else if (!strcmp(cmd, "pc98run")) {
            if (mach) machine_run(strtoull(a1, NULL, 10), a2);
        } else if (!strcmp(cmd, "pc98text")) {
            char text[PC98_TEXT_ROWS * (PC98_TEXT_COLS + 1) + 1];
            if (mach) {
                pc98_text_dump(mach, text);
                for (char *p = text; *p;) {
                    char *e = strchr(p, '\n');
                    printf("|%.*s\n", (int)(e - p), p);
                    p = e + 1;
                }
            }
        } else {
            printf("? %s\n", cmd);
        }
        fflush(stdout);
    }
    free(chip);
    return 0;
}
