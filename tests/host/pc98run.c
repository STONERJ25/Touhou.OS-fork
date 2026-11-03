// pc98run: boots a PC-98 disk image through the HLE BIOS (user/lib/pc98, docs/PC98_BIOS.md) in a headless machine and
// reports what happened. tools/pc98run.py builds and runs it (and turns the PPM into a PNG); tests/host/test_pc98bios.py
// uses it too.
//
//   pc98run --disk IMAGE [--disk IMAGE2 ...] [options]
//     --disk IMAGE         attach an image: the first hard disk image is 80h, the next 81h; floppy images 90h, 91h
//     --boot DAUA          boot device (hex; default: the first attached disk)
//     --cpu 8086|v30|286|386|486   CPU model (default 286)  --mhz N     CPU clock (default 10)
//     --cycles N           budget in millions of CPU clocks (default 400)
//     --until-key-wait     stop when the guest waits for a key and no scripted key is left: blocked in INT 18h AH=00h
//                          ("key-wait"), or reading the BIOS buffer's key count 20000 times with nothing arriving, the
//                          way DOS's console driver waits ("key-poll")
//     --until-exec NAME    stop when a program whose path contains NAME (any case) is started (INT 21h AH=4Bh)
//     --break SEG:OFF      report when execution reaches SEG:OFF (hex) and stop soon after (a few instructions later)
//     --keys TEXT          type TEXT once the guest first waits for a key (\r Return, \e ESC, \t TAB, \b BS, ^X CTRL+X,
//                          {XX} one raw key event: key number XX, plus 80h for its release); one event per 30 ms
//     --keys-at N          start typing after N million cycles instead of at the first key wait
//     --type N:TEXT        (repeatable) type TEXT at N million cycles of machine time, whatever the guest is doing: the
//                          games sense the keyboard themselves and never wait the way DOS does. TEXT is the --keys syntax
//                          plus <MS>, which delays the next key event by MS milliseconds instead of the usual 30 (so
//                          "{3C}<2000>{BC}" holds the right-arrow key down for two seconds)
//     --ppm-at N:FILE      (repeatable) write the screen (640x400, P6) when N million cycles have run
//     --stats-every N      print a STATS line every N million cycles: machine frames, interrupts taken, the display and
//                          timer ports' write counts and the host seconds used (frame pacing: docs/PC98.md, M5)
//     --watch ADDR:LEN     report every byte written into [ADDR, ADDR+LEN) (hex, linear addresses; the planes are A8000
//                          B, B0000 R, B8000 G, E0000 E) with the writer's CS:IP, the first --watch-max N (default 300)
//     --watch-plane OFF:LEN  the same for an offset range inside the planes (hex): a write to that offset in any of the four
//                          planes counts, which is what a GRCG write to one plane's address does to all of them
//     --virtual-config     four --edit lines at once: TH04's CONFIG.SYS loses `DEVICE=HIMEM.SYS`, `DEVICE=EMM386.EXE /UMB`, `DOS=HIGH` and
//                          `DOS=UMB` (each becomes `REM ...` of the same length) in the loaded image, so the game runs without HIMEM and
//                          EMM386 (which needs V86 mode). In memory only; every occurrence in the image is replaced, no more than that
//     --trace-calls FROM[:MAX]  after FROM million cycles print the first time each CALL target (near, far, indirect) is reached,
//                          with its caller, the time and the stack: the order in which a program meets its functions (MAX lines, 300)
//     --profile FROM[:TO]  between FROM and TO million cycles count the instructions executed in each 16 bytes of the address space
//                          and print the busiest (PROFILE lines): where a program spends its time, and what is only waiting
//     --edit OLD=NEW       (repeatable) change every occurrence of the text OLD in the loaded disk images into NEW (the
//                          same length) before the boot: the images' bytes in memory only, never the file (experiments such
//                          as a CONFIG.SYS without its device lines)
//     --opna 86|26k        put the FM board on the machine (pc98_attach_opna: the PC-9801-86 with its YM2608, or the -26K with a
//                          YM2203; ports 188h-18Eh and, on the -86, A460h; docs/PC98_SOUND.md): drivers that look for it find it.
//                          Without --wav the board only keeps its timers and interrupts (no synthesis)
//     --opna-irq N         the board's interrupt jumper (3, 10, 12 or 13; default 12 = INT5)
//     --opna-log FILE      every register write as CSV "cycle,bank,reg,val" (tools/opnalog.py reads it)
//     --opna-rhythm DIR    the YM2608's six drum samples from DIR/2608_BD.WAV ... (the owner's pack; local use only)
//     --wav FILE           the board's output as a 16-bit stereo WAV (needs --opna); --wav-rate HZ resamples (default 48000, 0 = the
//                          chip's own 55,466 Hz). The last lines then say OPNA (register counts) and WAV (frames and seconds)
//     --readonly           attach the disks write-protected (by default writes land in this process's copy)
//     --dump ADDR:LEN      print LEN bytes of memory at the linear address ADDR (hex) at the end (repeatable)
//     --plane-dump PG:PL:OFF:LEN  print LEN bytes (hex) of graphics page PG (0, 1), plane PL (0 B, 1 R, 2 G, 3 E) from offset OFF
//                          at the end, whatever page the CPU sees (repeatable)
//     --dumpbin ADDR:LEN:FILE  write LEN raw bytes of memory at ADDR into FILE at the end (repeatable; tools/pc98run.py --dis)
//     --dump-text          print the text screen at the end ("|" + 80 columns per line)
//     --state              print a STATE line (display, cursor, beeper, interrupt masks, keyboard buffer) at the end
//     --ppm FILE           write the screen (640x400, P6) at the end
//     --font FILE          the owner's FONT.ROM image (288,768 bytes: ANK and kanji; local use only, read at run time), else
//                          the repository's own 8x16 font (shared/font8x16.c: ASCII, no kanji)
//     --trace-int[=LIST]   BIOS calls (with registers before and after) and the guest's INT n; LIST = hex vectors
//     --trace-dos          INT 21h with file names: open, create, exec, find, delete (DS:DX strings)
//     --trace-irq          hardware interrupts taken
//     --trace-io           every port access
//     --trace-unhandled    ports nobody answers, BIOS functions not implemented, BIOS notes (on by default)
//     --trace-work         the first read of each BIOS work-area byte (0000:0400-05FF), with the reader's CS:IP
//     --history N          keep the last N instruction addresses and print them when the CPU stops
//     --trace-faults       report CPU exceptions (divide error INT 00h, invalid opcode INT 06h on the 286) with the
//                          faulting address and its bytes, even when a program has its own handler there
//     --clock YYMMDDhhmmss the calendar at power-on (default 261102120000)
//     --quiet              no trace at all (overrides the trace options)
//
// The last line printed is always "END reason=... cycles=... instructions=... cs:ip=...". The image files are only
// read; guest writes to a disk land in this process's copy and are dropped at exit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pc98.h>
#include "../../shared/font8x16.h"

typedef struct {
    Pc98 *m;
    int trace_int, trace_dos, trace_irq, trace_io, trace_unhandled, trace_work, quiet;
    uint8_t int_filter[256];        // 1 = trace this vector (all when no list given)
    uint16_t call_regs[8], call_s[4];
    int key_wait;                   // the guest waited for a key since we last looked
    uint32_t history_n, history_pos;
    uint32_t *history;
    uint64_t history_count;
    const char *until_exec;
    int trace_faults;
    uint32_t prev_at;               // CS << 16 | IP of the previous instruction (fault tracing)
    uint8_t prev_op[2];
    uint32_t fault_last;            // the last fault reported (CS << 16 | IP) and how often it came again
    uint8_t fault_vec;
    uint64_t fault_repeats;
    int break_set;
    uint32_t break_at;              // CS << 16 | IP
    const char *stop_reason;        // set by a hook: the main loop ends
    // Counts for --stats-every (always kept: an array increment per port access and per interrupt).
    uint64_t irq_count[256];        // hardware interrupts taken, by vector
    uint64_t out_count[0x500], in_count[0x500];     // port accesses below 500h (the EGC's 4A0h-4AEh are the highest)
    // --trace-calls FROM:MAX: the first time each call target is reached after FROM million cycles, with its caller
    int calls_on;
    uint64_t calls_from, calls_max, calls_n;
    uint8_t call_pending;           // the instruction before this one was a CALL (so this one is its target)
    uint32_t call_from_at;
    uint32_t *call_seen;            // a hash set of the targets already printed (CS << 16 | IP; 0 = empty slot)
    // --profile FROM:TO: instructions executed per 16-byte piece of the address space between FROM and TO million cycles
    int prof_on;
    uint64_t prof_from, prof_to, prof_total;
    struct { uint32_t key; uint64_t count, clocks; } *prof;     // 1 << 16 slots, open addressing, key = linear address >> 4 plus 1
    uint32_t prof_last_slot;        // the slot of the instruction before this one and the clock it started at: its cost is the difference
    uint64_t prof_last_cycles, prof_clocks_total;
    int prof_have_last;
    uint64_t watch_n, watch_max;    // --watch: writes seen and how many are printed
    struct { uint32_t at; uint64_t count; } writers[64];    // --watch: the distinct writers (CS << 16 | IP) and their byte counts
    int writers_n;
} Run;

static int contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && ((hay[i] | 0x20) == (needle[i] | 0x20) || hay[i] == needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static Run run;

static void opna_port_out(Run *r, uint32_t port, uint32_t v);      // the FM board's register log (below)

static void regs_str(char *out, size_t n, const uint16_t *r, const uint16_t *s) {
    snprintf(out, n, "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X DS=%04X ES=%04X", r[X86_AX], r[X86_BX],
             r[X86_CX], r[X86_DX], r[X86_SI], r[X86_DI], r[X86_BP], s[X86_DS], s[X86_ES]);
}

static void string_at(Pc98 *m, uint16_t seg, uint16_t off, char *out, size_t n) {
    size_t i = 0;
    for (; i + 1 < n; i++) {
        uint8_t c = pc98_read8(m, (((uint32_t)seg << 4) + (uint16_t)(off + i)) & 0xFFFFF);
        if (!c) break;
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[i] = 0;
}

// The caller's CF after a BIOS call: the FLAGS image the stub's IRET is about to pop.
static int caller_cf(Pc98 *m) {
    uint32_t a = (((uint32_t)m->cpu.s[X86_SS] << 4) + (uint16_t)(m->cpu.r[X86_SP] + 4)) & 0xFFFFF;
    return pc98_read8(m, a) & 1;
}

static void on_event(void *host, int kind, uint32_t a, uint32_t b) {
    Run *r = (Run *)host;
    Pc98 *m = r->m;
    X86Cpu *c = &m->cpu;
    char regs[160];
    if (kind == PC98_EV_NOTE && a == PC98_NOTE_KEY_WAIT) r->key_wait++;     // every retry of a waiting call counts
    if (kind == PC98_EV_SOFT_INT && a == 0x21 && (c->r[X86_AX] >> 8) == 0x4B && r->until_exec) {
        char s[80];
        string_at(m, c->s[X86_DS], c->r[X86_DX], s, sizeof s);
        if (contains_nocase(s, r->until_exec)) {
            printf("EXEC \"%s\"   at %04X:%04X\n", s, c->s[X86_CS], c->op_ip);
            r->stop_reason = "exec";
            m->yield = 1;
        }
    }
    if (kind == PC98_EV_OUT && a >= 0x188 && a <= 0x18E) opna_port_out(r, a, b);
    if (kind == PC98_EV_OUT && a < 0x500) r->out_count[a]++;
    if (kind == PC98_EV_IN && a < 0x500) r->in_count[a]++;
    if (kind == PC98_EV_HW_INT) r->irq_count[a & 0xFF]++;
    if (kind == PC98_EV_MEM_WRITE) {                // who writes: the distinct CS:IP, counted (printed at the end)
        uint32_t at = (uint32_t)c->s[X86_CS] << 16 | c->op_ip;
        int k = 0;
        while (k < r->writers_n && r->writers[k].at != at) k++;
        if (k == r->writers_n && k < (int)(sizeof r->writers / sizeof *r->writers)) { r->writers[k].at = at; r->writers[k].count = 0; r->writers_n++; }
        if (k < r->writers_n) r->writers[k].count++;
    }
    if (kind == PC98_EV_MEM_WRITE && r->watch_n++ < r->watch_max)
        printf("WRITE %05X <- %02X   at %04X:%04X (cycle %llu) grcg=%02X tiles=%02X%02X%02X%02X egc=%d draw_page=%d\n", a, b,
               c->s[X86_CS], c->op_ip, (unsigned long long)c->cycles, m->gfx.grcg.mode, m->gfx.grcg.tile[0],
               m->gfx.grcg.tile[1], m->gfx.grcg.tile[2], m->gfx.grcg.tile[3], m->gfx.egc_mode, m->gfx.draw_page);
    if (r->quiet) return;
    switch (kind) {
    case PC98_EV_IN:
        if (r->trace_io) printf("IO  in  %04X -> %02X   at %04X:%04X\n", a, b, c->s[X86_CS], c->op_ip);
        break;
    case PC98_EV_OUT:
        if (r->trace_io) printf("IO  out %04X <- %02X   at %04X:%04X\n", a, b, c->s[X86_CS], c->op_ip);
        break;
    case PC98_EV_UNHANDLED_IN:
        if (r->trace_unhandled) printf("UNHANDLED in  %04X   at %04X:%04X\n", a, c->s[X86_CS], c->op_ip);
        break;
    case PC98_EV_UNHANDLED_OUT:
        if (r->trace_unhandled) printf("UNHANDLED out %04X <- %02X   at %04X:%04X\n", a, b, c->s[X86_CS], c->op_ip);
        break;
    case PC98_EV_SOFT_INT:
        if (r->trace_dos && a == 0x21) {
            uint8_t ah = (uint8_t)(c->r[X86_AX] >> 8);
            char s[80];
            if (ah == 0x3D || ah == 0x3C || ah == 0x4B || ah == 0x4E || ah == 0x41 || ah == 0x43 || ah == 0x5B) {
                string_at(m, c->s[X86_DS], c->r[X86_DX], s, sizeof s);
                printf("DOS %02X \"%s\"   at %04X:%04X (%.1f M)\n", ah, s, c->s[X86_CS], c->op_ip, (double)c->cycles / 1e6);
            } else if (r->trace_int && r->int_filter[0x21]) {
                regs_str(regs, sizeof regs, c->r, c->s);
                printf("INT 21 %s   at %04X:%04X\n", regs, c->s[X86_CS], c->op_ip);
            }
        } else if (r->trace_int && r->int_filter[a & 0xFF]) {
            regs_str(regs, sizeof regs, c->r, c->s);
            printf("INT %02X %s   at %04X:%04X\n", a, regs, c->s[X86_CS], c->op_ip);
        }
        break;
    case PC98_EV_HW_INT:
        if (r->trace_irq) printf("IRQ vector %02X   at %04X:%04X\n", a, c->s[X86_CS], c->ip);
        break;
    case PC98_EV_BIOS_CALL:
        memcpy(r->call_regs, c->r, sizeof r->call_regs);
        memcpy(r->call_s, c->s, sizeof r->call_s);
        break;
    case PC98_EV_BIOS_DONE:
        if (r->trace_int && r->int_filter[a & 0xFF] && !(a >= 0x08 && a <= 0x17 && !r->trace_irq)) {
            char after[160];
            regs_str(regs, sizeof regs, r->call_regs, r->call_s);
            regs_str(after, sizeof after, c->r, c->s);
            // the caller's return address is on the stack: IP, CS
            uint32_t sp = (((uint32_t)c->s[X86_SS] << 4) + c->r[X86_SP]) & 0xFFFFF;
            uint16_t rip = (uint16_t)(pc98_read8(m, sp) | pc98_read8(m, sp + 1) << 8);
            uint16_t rcs = (uint16_t)(pc98_read8(m, sp + 2) | pc98_read8(m, sp + 3) << 8);
            printf("BIOS %02X %s\n        -> %s CF=%d%s   from %04X:%04X\n", a, regs, after, caller_cf(m),
                   b ? " (waiting)" : "", rcs, rip);
        }
        break;
    case PC98_EV_BIOS_UNKNOWN:
        if (r->trace_unhandled) {
            regs_str(regs, sizeof regs, r->call_regs, r->call_s);
            printf("BIOS-UNKNOWN %02X AH=%02X %s\n", a, b, regs);
        }
        break;
    case PC98_EV_WORK_READ:
        if (r->trace_work) printf("WORK %04X = %02X   read at %04X:%04X\n", a, b, c->s[X86_CS], c->op_ip);
        break;
    case PC98_EV_NOTE:
        if (r->trace_unhandled && !(a == PC98_NOTE_KEY_WAIT && r->key_wait > 1))     // the first wait, not the retries
            printf("NOTE %s (%X)   at %04X:%04X\n", pc98_bios_note_name((int)a), b, c->s[X86_CS], c->op_ip);
        break;
    }
}

static uint16_t rd16(Pc98 *m, uint32_t a) { return (uint16_t)(pc98_read8(m, a & 0xFFFFF) | pc98_read8(m, (a + 1) & 0xFFFFF) << 8); }

// A fault enters its vector without an INT instruction: execution arrives at the vector's target while the previous
// instruction was not INT n (CD n) for that vector. The faulting address is the return address on the stack.
static void check_fault(X86Cpu *c, uint32_t at) {
    Pc98 *m = run.m;
    static const uint8_t vecs[] = { 0x00, 0x06 };
    for (unsigned i = 0; i < sizeof vecs; i++) {
        uint8_t v = vecs[i];
        uint32_t target = (uint32_t)rd16(m, v * 4u + 2) << 16 | rd16(m, v * 4u);
        if (at != target) continue;
        if (run.prev_op[0] == 0xCD && run.prev_op[1] == v) continue;     // an INT n, not a fault
        uint32_t sp = ((uint32_t)c->s[X86_SS] << 4) + c->r[X86_SP];
        uint16_t ip = rd16(m, sp), cs = rd16(m, sp + 2);
        uint32_t fa = ((uint32_t)cs << 4) + ip;
        uint32_t key = (uint32_t)cs << 16 | ip;
        if (key == run.fault_last && v == run.fault_vec) { run.fault_repeats++; continue; }   // a fault loop: once
        if (run.fault_repeats) printf("FAULT (the one before repeated %llu more times)\n", (unsigned long long)run.fault_repeats);
        run.fault_last = key;
        run.fault_vec = v;
        run.fault_repeats = 0;
        printf("FAULT INT %02X at %04X:%04X (previous instruction %04X:%04X), bytes", v, cs, ip, run.prev_at >> 16,
               run.prev_at & 0xFFFF);
        for (int k = 0; k < 8; k++) printf(" %02X", pc98_read8(m, (fa + (uint32_t)k) & 0xFFFFF));
        printf("\n");
    }
}

static void on_trace(void *ctx, X86Cpu *c) {
    (void)ctx;
    uint32_t at = (uint32_t)c->s[X86_CS] << 16 | c->ip;
    if (run.trace_faults) {
        check_fault(c, at);
        uint32_t lin = (((uint32_t)c->s[X86_CS] << 4) + c->ip) & 0xFFFFF;
        run.prev_at = at;
        run.prev_op[0] = pc98_read8(run.m, lin);
        run.prev_op[1] = pc98_read8(run.m, (lin + 1) & 0xFFFFF);
    }
    if (run.prof_on && c->cycles >= run.prof_from * 1000000ull && c->cycles < run.prof_to * 1000000ull) {
        uint32_t key = ((((uint32_t)c->s[X86_CS] << 4) + c->ip) & 0xFFFFF) >> 4;
        uint32_t h = (key + 1) * 2654435761u >> 16;
        while (run.prof[h & 0xFFFF].key && run.prof[h & 0xFFFF].key != key + 1) h++;
        run.prof[h & 0xFFFF].key = key + 1;
        run.prof[h & 0xFFFF].count++;
        run.prof_total++;
        if (run.prof_have_last) {                       // the clocks the previous instruction took go to its piece
            run.prof[run.prof_last_slot].clocks += c->cycles - run.prof_last_cycles;
            run.prof_clocks_total += c->cycles - run.prof_last_cycles;
        }
        run.prof_last_slot = h & 0xFFFF;
        run.prof_last_cycles = c->cycles;
        run.prof_have_last = 1;
    } else run.prof_have_last = 0;
    if (run.calls_on && c->cycles >= run.calls_from * 1000000ull) {
        if (run.call_pending) {                     // `at` is where the CALL went
            uint32_t h = (at * 2654435761u) >> 16;
            while (run.call_seen[h & 0xFFFF] && run.call_seen[h & 0xFFFF] != at) h++;
            if (run.call_seen[h & 0xFFFF] != at) {
                run.call_seen[h & 0xFFFF] = at;
                if (run.calls_n++ < run.calls_max)
                    printf("CALL %04X:%04X -> %04X:%04X  at %.2f M (SS:SP %04X:%04X)\n", run.call_from_at >> 16, run.call_from_at & 0xFFFF,
                           at >> 16, at & 0xFFFF, (double)c->cycles / 1e6, c->s[X86_SS], c->r[X86_SP]);
            }
        }
        uint32_t lin = (((uint32_t)c->s[X86_CS] << 4) + c->ip) & 0xFFFFF;
        uint8_t op = pc98_read8(run.m, lin);
        for (int k = 1; k < 5 && (op == 0x66 || op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 ||
                                  op == 0x65 || op == 0xF0); k++)
            op = pc98_read8(run.m, (lin + (uint32_t)k) & 0xFFFFF);
        uint8_t modrm = pc98_read8(run.m, (lin + 1) & 0xFFFFF);
        run.call_pending = op == 0xE8 || op == 0x9A || (op == 0xFF && (((modrm >> 3) & 7) == 2 || ((modrm >> 3) & 7) == 3));
        run.call_from_at = at;
    }
    if (run.history_n) {
        run.history[run.history_pos] = at;
        run.history_pos = (run.history_pos + 1) % run.history_n;
        run.history_count++;
    }
    if (run.break_set && at == run.break_at && !run.stop_reason) {
        char regs[160];
        regs_str(regs, sizeof regs, c->r, c->s);
        printf("BREAK %04X:%04X reached after %llu cycles: %s SS:SP=%04X:%04X stack", c->s[X86_CS], c->ip,
               (unsigned long long)c->cycles, regs, c->s[X86_SS], c->r[X86_SP]);
        for (int k = 0; k < 6; k++) {
            uint32_t a = (((uint32_t)c->s[X86_SS] << 4) + (uint16_t)(c->r[X86_SP] + 2 * k)) & 0xFFFFF;
            printf(" %04X", pc98_read8(run.m, a) | pc98_read8(run.m, a + 1) << 8);
        }
        printf("\n");
        run.stop_reason = "break";
        run.m->yield = 1;
    }
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 20, len = 0;
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

// ---- scripted keys: (key number | 80h for a release) events. --keys fills `keyq`, typed one event per tick of `key_gap`
// cycles once the guest first waits for a key (or at --keys-at); --type puts events with their own times into `tkeys`, so
// a game that senses the keyboard by itself (and never waits the way DOS does) gets its keys at known moments.
static uint8_t keyq[4096];
static size_t keyq_len, keyq_pos;

typedef struct { uint64_t at; uint32_t seq; uint8_t code; } TimedKey;
static TimedKey tkeys[16384];
static size_t tkeys_len, tkeys_pos;
static int kp_timed;                    // the parser's target: 1 = tkeys (with times), 0 = keyq
static uint64_t kp_t, kp_last, kp_gap, kp_ms;   // time of the next timed event, of the last one, the usual gap, a millisecond

static void key_emit(uint8_t code) {
    if (!kp_timed) {
        if (keyq_len < sizeof keyq) keyq[keyq_len++] = code;
        return;
    }
    if (tkeys_len == sizeof tkeys / sizeof *tkeys) return;
    tkeys[tkeys_len].at = kp_t;
    tkeys[tkeys_len].seq = (uint32_t)tkeys_len;
    tkeys[tkeys_len++].code = code;
    kp_last = kp_t;
    kp_t += kp_gap;
}

static void key_add(uint8_t key, uint8_t mods) {
    if (mods & 0x10) key_emit(0x74);                    // CTRL down
    if (mods & 0x01) key_emit(0x70);                    // SHIFT down
    key_emit(key);
    key_emit(key | 0x80);
    if (mods & 0x01) key_emit(0x70 | 0x80);
    if (mods & 0x10) key_emit(0x74 | 0x80);
}

static void keys_parse(const char *s) {
    for (; *s; s++) {
        uint8_t ch = (uint8_t)*s, mods;
        if (ch == '{') {                                // {XX}: one raw key event (XX < 80h make, else break)
            char *end;
            unsigned long raw = strtoul(s + 1, &end, 16);
            if (*end == '}') { key_emit((uint8_t)raw); s = end; continue; }
        }
        if (ch == '<' && kp_timed) {                    // <MS>: the next event comes MS milliseconds after the last one
            char *end;
            unsigned long ms = strtoul(s + 1, &end, 10);
            if (*end == '>') { kp_t = kp_last + ms * kp_ms; s = end; continue; }
        }
        if (ch == '\\' && s[1]) {
            s++;
            ch = *s == 'r' ? 0x0D : *s == 'e' ? 0x1B : *s == 't' ? 0x09 : *s == 'b' ? 0x08 : (uint8_t)*s;
        } else if (ch == '^' && s[1]) {
            s++;
            ch = (uint8_t)(*s & 0x1F);
        }
        int k = pc98_key_for_char(ch, &mods);
        if (k >= 0) key_add((uint8_t)k, mods);
        else fprintf(stderr, "pc98run: no key types %02X\n", ch);
    }
}

// --type N:TEXT: TEXT starting at N million cycles; gap is 30 ms and ms 1 ms of machine time, in cycles.
static void type_parse(const char *spec, uint64_t gap, uint64_t ms) {
    char *end;
    double at = strtod(spec, &end);
    if (*end != ':') { fprintf(stderr, "pc98run: --type N:TEXT\n"); exit(2); }
    kp_timed = 1;
    kp_t = kp_last = (uint64_t)(at * 1e6);
    kp_gap = gap;
    kp_ms = ms;
    keys_parse(end + 1);
    kp_timed = 0;
}

static int tkey_cmp(const void *a, const void *b) {
    const TimedKey *x = a, *y = b;
    if (x->at != y->at) return x->at < y->at ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

// --ppm-at N:FILE pictures
typedef struct { uint64_t at; const char *path; } PpmAt;
static int ppm_cmp(const void *a, const void *b) {
    const PpmAt *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

static uint8_t ank_table[4096];
static uint8_t *font_rom;       // the owner's FONT.ROM image, read at run time (local use only; never copied anywhere)

// Fonts for the display: a FONT.ROM image (ANK and kanji) with --font, else the repository's own 8x16 font (ASCII and the
// half-width kana as boxes, no kanji). The BIOS's font calls (INT 18h AH=14h) and the renderer both use them.
static void setup_fonts(Pc98 *m, const char *font_path) {
    if (font_path) {
        size_t n = 0;
        font_rom = slurp(font_path, &n);
        if (font_rom && pc98gfx_fontrom_valid(font_rom, n)) {
            pc98gfx_set_font(&m->gfx, pc98gfx_fontrom_ank16(font_rom), pc98gfx_fontrom_kanji, font_rom);
            return;
        }
        if (font_rom && n >= 0x800 + sizeof ank_table) {         // some other dump: the 8x16 ANK table at 800h
            memcpy(ank_table, font_rom + 0x800, sizeof ank_table);
            pc98gfx_set_font(&m->gfx, ank_table, NULL, NULL);
            return;
        }
        fprintf(stderr, "pc98run: %s is not a font ROM image; using the repository's font\n", font_path);
    }
    pc98gfx_ank_from_unicode(ank_table, font8x16_glyph);
    pc98gfx_set_font(&m->gfx, ank_table, NULL, NULL);
}

static void write_ppm(Pc98 *m, const char *path) {
    static uint32_t px[640 * 400];
    pc98_render(m, px, 640);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "pc98run: cannot write %s\n", path); return; }
    fprintf(f, "P6\n640 400\n255\n");
    for (int i = 0; i < 640 * 400; i++) {
        uint8_t rgb[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// ---- sound: the FM board's output as a 16-bit stereo WAV, streamed (the header's sizes are patched at the end), and the
// board's register writes logged from the machine's port events.
static FILE *wav_file;
static uint32_t wav_frames;
static unsigned wav_rate_hz;

static void put16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)((v >> 8) & 255), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static void wav_header(FILE *f, uint32_t frames, unsigned rate) {
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);                // PCM
    put16(f, 2);                // stereo
    put32(f, rate);
    put32(f, rate * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
}

static void wav_write(const int16_t *stereo, size_t frames) {
    for (size_t i = 0; i < 2 * frames; i++) put16(wav_file, (uint16_t)stereo[i]);
    wav_frames += (uint32_t)frames;
}

// 16-bit mono PCM WAVs of the owner's pack (2608_BD.WAV ...: the YM2608's rhythm sounds live in its ROM, so the host supplies them).
static int16_t *read_wav_mono(const char *path, uint32_t *count, uint32_t *rate) {
    size_t len = 0;
    uint8_t *buf = slurp(path, &len);
    if (!buf) return NULL;
    int16_t *out = NULL;
    if (len >= 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
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
                out = malloc(n ? n : 2);
                for (uint32_t i = 0; i < *count; i++) out[i] = (int16_t)(d[2 * i] | d[2 * i + 1] << 8);
                break;
            }
            pos += 8 + n + (n & 1);
        }
    }
    free(buf);
    return out;
}

static void load_rhythm_dir(Opna *fm, const char *dir) {
    static const char *names[OPNA_RHYTHMS] = { "BD", "SD", "TOP", "HH", "TOM", "RIM" };
    for (int i = 0; i < OPNA_RHYTHMS; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/2608_%s.WAV", dir, names[i]);
        uint32_t n = 0, rate = 0;
        int16_t *s = read_wav_mono(path, &n, &rate);
        if (!s) { fprintf(stderr, "pc98run: cannot read %s\n", path); continue; }
        opna_set_rhythm_sample(fm, i, s, n, rate);      // used in place, kept for the whole run
    }
}

// The FM board's register writes, seen at the data ports: a log (cycle, bank, register, value) and counts by kind.
static FILE *opna_log;
static uint8_t opna_addr[2];
static uint64_t opna_writes[2][256];

static void opna_port_out(Run *r, uint32_t port, uint32_t v) {
    if (port == 0x188 || port == 0x18C) { opna_addr[port == 0x18C] = (uint8_t)v; return; }
    if (port != 0x18A && port != 0x18E) return;
    int bank = port == 0x18E;
    opna_writes[bank][opna_addr[bank]]++;
    if (opna_log) fprintf(opna_log, "%llu,%d,%02X,%02X\n", (unsigned long long)r->m->cpu.cycles, bank, opna_addr[bank], v);
}

static const char *opna_kind(int bank, unsigned reg) {
    if (!bank) {
        if (reg < 0x06) return "ssg-tone";
        if (reg == 0x06) return "ssg-noise";
        if (reg == 0x07) return "ssg-mixer";
        if (reg < 0x0B) return "ssg-level";
        if (reg < 0x0E) return "ssg-envelope";
        if (reg < 0x10) return "ssg-port";
        if (reg < 0x20) return "rhythm";
        if (reg == 0x28) return "fm-key";
        if (reg >= 0x24 && reg <= 0x27) return "timer";
        if (reg < 0x30) return "fm-control";
        if (reg >= 0xA0 && reg < 0xB0) return "fm-pitch";
        if (reg >= 0xB0) return "fm-channel";
        return "fm-operator";
    }
    if (reg <= 0x10) return "adpcm";
    if (reg >= 0xA0 && reg < 0xB0) return "fm-pitch";
    if (reg >= 0xB0) return "fm-channel";
    return reg >= 0x30 ? "fm-operator" : "other";
}

static double host_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// CPU seconds this process has used: unlike the wall clock it does not grow when a dozen other jobs share the PC.
static double cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// One STATS line (--stats-every): what a frame-pacing measurement needs. Differences between two lines are the rates.
static void print_stats(Run *r, double host_s, double cpu_s) {
    Pc98 *m = r->m;
    unsigned long long e = 0;
    for (unsigned p = 0x4A0; p <= 0x4AE; p++) e += r->out_count[p];
    printf("STATS cyc=%llu emu_s=%.3f host_s=%.3f cpu_s=%.3f frames=%llu instr=%llu", (unsigned long long)m->cpu.cycles,
           (double)m->cpu.cycles / m->cpu_hz, host_s, cpu_s, (unsigned long long)m->frames, (unsigned long long)m->cpu.instructions);
    for (unsigned v = 0; v < 256; v++) if (r->irq_count[v]) printf(" irq%02X=%llu", v, (unsigned long long)r->irq_count[v]);
    static const uint16_t outs[] = { 0x64, 0xA4, 0xA6, 0x71, 0x77, 0x7C, 0x7E, 0x188, 0x18A };
    for (unsigned i = 0; i < sizeof outs / sizeof *outs; i++)
        if (r->out_count[outs[i]]) printf(" out%X=%llu", outs[i], (unsigned long long)r->out_count[outs[i]]);
    static const uint16_t ins[] = { 0x41, 0x60, 0xA0, 0x71, 0x73, 0x75 };
    for (unsigned i = 0; i < sizeof ins / sizeof *ins; i++)
        if (r->in_count[ins[i]]) printf(" in%X=%llu", ins[i], (unsigned long long)r->in_count[ins[i]]);
    if (e) printf(" egc=%llu", e);
    printf(" bios=%u\n", m->bios.calls);
}

// --edit OLD=NEW: replaces every OLD in an image's bytes by NEW (the same length); returns the number of replacements.
static int apply_edit(uint8_t *img, size_t len, const char *spec) {
    const char *eq = strrchr(spec, '=');            // the last '=': OLD may contain one (DEVICE=HIMEM.SYS), NEW may not
    if (!eq) { fprintf(stderr, "pc98run: --edit OLD=NEW\n"); exit(2); }
    size_t n = (size_t)(eq - spec);
    if (!n || strlen(eq + 1) != n) { fprintf(stderr, "pc98run: --edit needs OLD and NEW of the same length\n"); exit(2); }
    int count = 0;
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(img + i, spec, n)) continue;
        memcpy(img + i, eq + 1, n);
        count++;
        i += n - 1;
    }
    return count;
}

static int parse_hex_list(const char *s, uint8_t *filter) {
    memset(filter, 0, 256);
    while (*s) {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) return -1;
        filter[v & 0xFF] = 1;
        s = *end == ',' ? end + 1 : end;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *disks[8], *dumps[16], *dumpbins[16];
    int ndisks = 0, ndumps = 0, ndumpbins = 0, boot = -1, dump_text = 0, until_key_wait = 0, readonly = 0, show_state = 0;
    const char *ppm = NULL, *font_path = NULL, *keys = NULL, *clock = "261102120000";
    const char *types[64], *edits[16], *plane_dumps[16];
    int nplane_dumps = 0;
    PpmAt ppms[64];
    int ntypes = 0, nppms = 0, nedits = 0, ppms_pos = 0, opna_board = -1, opna_irq = 12;
    const char *wav_path = NULL, *opna_log_path = NULL, *rhythm_dir = NULL;
    unsigned wav_rate = 48000;
    double stats_every = 0;
    uint32_t watch_lo = 0, watch_hi = 0, watch_plane_lo = 0, watch_plane_hi = 0;
    X86Model model = X86_MODEL_80286;
    unsigned mhz = 10;
    double mcycles = 400, keys_at = -1;
    memset(&run, 0, sizeof run);
    run.trace_unhandled = 1;
    run.watch_max = 300;
    memset(run.int_filter, 1, sizeof run.int_filter);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--type") && v) { if (ntypes < 64) types[ntypes++] = v; i++; }
        else if (!strcmp(a, "--ppm-at") && v) {
            char *end;
            double at = strtod(v, &end);
            if (*end != ':' || nppms == 64) { fprintf(stderr, "pc98run: --ppm-at N:FILE\n"); return 2; }
            ppms[nppms].at = (uint64_t)(at * 1e6);
            ppms[nppms++].path = end + 1;
            i++;
        }
        else if (!strcmp(a, "--stats-every") && v) { stats_every = strtod(v, NULL); i++; }
        else if (!strcmp(a, "--watch") && v) {
            unsigned addr, len;
            if (sscanf(v, "%x:%x", &addr, &len) != 2) { fprintf(stderr, "pc98run: --watch ADDR:LEN\n"); return 2; }
            watch_lo = addr;
            watch_hi = addr + len;
            i++;
        }
        else if (!strcmp(a, "--watch-plane") && v) {
            unsigned off, len;
            if (sscanf(v, "%x:%x", &off, &len) != 2) { fprintf(stderr, "pc98run: --watch-plane OFFSET:LEN\n"); return 2; }
            watch_plane_lo = off;
            watch_plane_hi = off + len;
            i++;
        }
        else if (!strcmp(a, "--watch-max") && v) { run.watch_max = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--profile") && v) {
            char *end;
            run.prof_from = strtoull(v, &end, 10);
            run.prof_to = *end == ':' ? strtoull(end + 1, NULL, 10) : run.prof_from + 1;
            run.prof_on = 1;
            i++;
        }
        else if (!strcmp(a, "--trace-calls") && v) {
            char *end;
            run.calls_from = strtoull(v, &end, 10);
            run.calls_max = *end == ':' ? strtoull(end + 1, NULL, 10) : 300;
            run.calls_on = 1;
            i++;
        }
        else if (!strcmp(a, "--edit") && v) { if (nedits < 16) edits[nedits++] = v; i++; }
        else if (!strcmp(a, "--virtual-config")) {      // TH04's CONFIG.SYS without its extended-memory lines (docs/PC98.md, M5.4)
            static const char *const vc[] = { "DEVICE=HIMEM.SYS=REM HIMEM.SYS   ", "DEVICE=EMM386.EXE /UMB=REM EMM386.EXE /UMB   ",
                                              "DOS=HIGH=REM HIGH", "DOS=UMB=REM UMB" };
            for (unsigned k = 0; k < sizeof vc / sizeof *vc && nedits < 16; k++) edits[nedits++] = vc[k];
        }
        else if (!strcmp(a, "--opna") && v) {
            if (!strcmp(v, "86")) opna_board = OPNA_BOARD_86;
            else if (!strcmp(v, "26k")) opna_board = OPNA_BOARD_26K;
            else { fprintf(stderr, "pc98run: --opna 86|26k\n"); return 2; }
            i++;
        }
        else if (!strcmp(a, "--opna-irq") && v) { opna_irq = atoi(v); i++; }
        else if (!strcmp(a, "--opna-log") && v) { opna_log_path = v; i++; }
        else if (!strcmp(a, "--opna-rhythm") && v) { rhythm_dir = v; i++; }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--wav-rate") && v) { wav_rate = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--disk") && v) { if (ndisks < 8) disks[ndisks++] = v; i++; }
        else if (!strcmp(a, "--boot") && v) { boot = (int)strtoul(v, NULL, 16); i++; }
        else if (!strcmp(a, "--cpu") && v) {
            model = !strcmp(v, "8086") ? X86_MODEL_8086 : !strcmp(v, "v30") ? X86_MODEL_V30
                  : !strcmp(v, "386") ? X86_MODEL_80386 : !strcmp(v, "486") ? X86_MODEL_80486 : X86_MODEL_80286;
            i++;
        }
        else if (!strcmp(a, "--mhz") && v) { mhz = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--cycles") && v) { mcycles = strtod(v, NULL); i++; }
        else if (!strcmp(a, "--until-key-wait")) until_key_wait = 1;
        else if (!strcmp(a, "--until-exec") && v) { run.until_exec = v; i++; }
        else if (!strcmp(a, "--break") && v) {
            unsigned seg, off;
            if (sscanf(v, "%x:%x", &seg, &off) != 2) { fprintf(stderr, "pc98run: --break SEG:OFF\n"); return 2; }
            run.break_set = 1;
            run.break_at = (seg & 0xFFFF) << 16 | (off & 0xFFFF);
            i++;
        }
        else if (!strcmp(a, "--readonly")) readonly = 1;
        else if (!strcmp(a, "--dump") && v) { if (ndumps < 16) dumps[ndumps++] = v; i++; }
        else if (!strcmp(a, "--plane-dump") && v) { if (nplane_dumps < 16) plane_dumps[nplane_dumps++] = v; i++; }
        else if (!strcmp(a, "--dumpbin") && v) { if (ndumpbins < 16) dumpbins[ndumpbins++] = v; i++; }
        else if (!strcmp(a, "--keys") && v) { keys = v; i++; }
        else if (!strcmp(a, "--keys-at") && v) { keys_at = strtod(v, NULL); i++; }
        else if (!strcmp(a, "--dump-text")) dump_text = 1;
        else if (!strcmp(a, "--state")) show_state = 1;
        else if (!strcmp(a, "--ppm") && v) { ppm = v; i++; }
        else if (!strcmp(a, "--font") && v) { font_path = v; i++; }
        else if (!strcmp(a, "--trace-int")) run.trace_int = 1;
        else if (!strncmp(a, "--trace-int=", 12)) {
            run.trace_int = 1;
            if (parse_hex_list(a + 12, run.int_filter)) { fprintf(stderr, "pc98run: bad vector list\n"); return 2; }
        }
        else if (!strcmp(a, "--trace-dos")) run.trace_dos = 1;
        else if (!strcmp(a, "--trace-irq")) run.trace_irq = 1;
        else if (!strcmp(a, "--trace-io")) run.trace_io = 1;
        else if (!strcmp(a, "--trace-unhandled")) run.trace_unhandled = 1;
        else if (!strcmp(a, "--trace-work")) run.trace_work = 1;
        else if (!strcmp(a, "--history") && v) { run.history_n = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--trace-faults")) run.trace_faults = 1;
        else if (!strcmp(a, "--clock") && v) { clock = v; i++; }
        else if (!strcmp(a, "--quiet")) run.quiet = 1;
        else { fprintf(stderr, "pc98run: unknown option %s\n", a); return 2; }
    }
    if (!ndisks) { fprintf(stderr, "usage: pc98run --disk IMAGE [options] (see the header of tests/host/pc98run.c)\n"); return 2; }

    Pc98 *m = calloc(1, sizeof *m);
    run.m = m;
    pc98_init(m, model, mhz * 1000000u);
    setup_fonts(m, font_path);
    pc98_bios_init(m);
    Opna *fm = NULL;                                    // the FM board (--opna): 280 KiB, so allocated
    OpnaResampler rs;
    int16_t *audio_in = NULL, *audio_out = NULL;
    if (opna_board >= 0) {
        fm = calloc(1, sizeof *fm);
        pc98_attach_opna(m, fm, opna_board, opna_irq);
        if (!wav_path) fm->synth = 0;                   // nobody listens: only the timers, flags and interrupts run
        if (rhythm_dir) load_rhythm_dir(fm, rhythm_dir);
        if (opna_log_path) {
            opna_log = fopen(opna_log_path, "w");
            if (opna_log) fprintf(opna_log, "cycle,bank,reg,val\n");
            else fprintf(stderr, "pc98run: cannot write %s\n", opna_log_path);
        }
    }
    if (wav_path) {
        if (!fm) { fprintf(stderr, "pc98run: --wav needs --opna\n"); return 2; }
        wav_file = fopen(wav_path, "wb");
        if (!wav_file) { fprintf(stderr, "pc98run: cannot write %s\n", wav_path); return 1; }
        wav_rate_hz = wav_rate ? wav_rate : opna_rate_num(fm) / opna_rate_den(fm);
        wav_header(wav_file, 0, wav_rate_hz);
        if (wav_rate) opna_resampler_init(&rs, opna_rate_num(fm), opna_rate_den(fm), wav_rate);
        audio_in = malloc(OPNA_FIFO_FRAMES * 2 * sizeof *audio_in);
        audio_out = malloc(OPNA_FIFO_FRAMES * 2 * 2 * sizeof *audio_out);       // up to twice the chip's rate
    }
    Pc98Disk disk[8];
    uint8_t *image[8];
    int first = -1, nhd = 0, nfd = 0;
    for (int i = 0; i < ndisks; i++) {
        size_t len = 0;
        image[i] = slurp(disks[i], &len);
        if (!image[i]) { fprintf(stderr, "pc98run: cannot read %s\n", disks[i]); return 1; }
        for (int e = 0; e < nedits; e++) {
            int n = apply_edit(image[i], len, edits[e]);
            printf("EDIT %s in %s: %d place%s (in memory only)\n", edits[e], disks[i], n, n == 1 ? "" : "s");
        }
        int rc = pc98_disk_open(&disk[i], image[i], len, PC98_DISK_NONE);
        if (rc) { fprintf(stderr, "pc98run: %s: not a disk image (%d)\n", disks[i], rc); return 1; }
        int hd = disk[i].format == PC98_DISK_HDI;
        uint8_t daua = hd ? (uint8_t)(0x80 + nhd++) : (uint8_t)(0x90 + nfd++);
        pc98_bios_attach(m, daua, &disk[i], readonly ? NULL : image[i]);
        printf("DISK %02X %s: %s, %u cylinders, %u heads, %u sectors of %u bytes\n", daua, disks[i],
               pc98_disk_format_name(disk[i].format), disk[i].cylinders, disk[i].heads, disk[i].sectors,
               disk[i].sector_size);
        if (first < 0) first = daua;
    }
    if (strlen(clock) == 12) {
        uint8_t bcd[6];
        for (int i = 0; i < 6; i++) bcd[i] = (uint8_t)((clock[2 * i] - '0') << 4 | (clock[2 * i + 1] - '0'));
        bcd[1] = (uint8_t)((bcd[1] >> 4) * 10 + (bcd[1] & 15)) << 4;   // month as a number in the high nibble
        pc98_bios_set_clock(m, bcd);
    }
    if (keys) keys_parse(keys);
    for (int t = 0; t < ntypes; t++) type_parse(types[t], mhz * 30000u, mhz * 1000u);
    qsort(tkeys, tkeys_len, sizeof *tkeys, tkey_cmp);
    qsort(ppms, (size_t)nppms, sizeof *ppms, ppm_cmp);
    m->host = &run;
    m->event = on_event;
    m->watch_lo = watch_lo;
    m->watch_hi = watch_hi;
    m->watch_plane_lo = watch_plane_lo;
    m->watch_plane_hi = watch_plane_hi;
    m->watch_work_area = (uint8_t)run.trace_work;
    if (run.history_n) run.history = calloc(run.history_n, sizeof *run.history);
    if (run.calls_on) run.call_seen = calloc(1 << 16, sizeof *run.call_seen);
    if (run.prof_on) run.prof = calloc(1 << 16, sizeof *run.prof);
    if (run.history_n || run.break_set || run.trace_faults || run.calls_on || run.prof_on) m->cpu.bus.trace = on_trace;
    if (boot < 0) boot = first;
    if (pc98_bios_boot(m, (uint8_t)boot)) { printf("END reason=no-boot-sector device=%02X\n", boot); return 1; }

    uint64_t budget = (uint64_t)(mcycles * 1e6), used = 0, slice = mhz * 10000u;   // 1 ms of machine time
    uint64_t key_gap = mhz * 30000u, next_key = 0;                                 // 30 ms between key events
    int keys_started = 0;
    const char *reason = "budget";
    double host0 = host_seconds(), cpu0 = cpu_seconds();
    uint64_t next_stats = stats_every > 0 ? (uint64_t)(stats_every * 1e6) : 0;
    while (used < budget) {
        if (pc98_dead(m)) { reason = m->cpu.stop ? "stop" : "dead"; break; }
        uint64_t s = budget - used < slice ? budget - used : slice;
        used += pc98_run(m, s);
        while (tkeys_pos < tkeys_len && tkeys[tkeys_pos].at <= m->cpu.cycles) {     // --type: keys due now
            uint8_t k = tkeys[tkeys_pos++].code;
            pc98_key(m, k & 0x7F, !(k & 0x80));
        }
        while (ppms_pos < nppms && ppms[ppms_pos].at <= m->cpu.cycles) write_ppm(m, ppms[ppms_pos++].path);
        if (next_stats && m->cpu.cycles >= next_stats) {
            print_stats(&run, host_seconds() - host0, cpu_seconds() - cpu0);
            next_stats += (uint64_t)(stats_every * 1e6);
        }
        if (fm && wav_file) {                                   // the chip's FIFO holds 74 ms: drain it every millisecond
            size_t n = opna_take(fm, audio_in, OPNA_FIFO_FRAMES);
            if (!wav_rate) wav_write(audio_in, n);
            for (const int16_t *p = audio_in; wav_rate && n;) {
                size_t used_in = n, made = opna_resample(&rs, p, &used_in, audio_out, OPNA_FIFO_FRAMES * 2);
                wav_write(audio_out, made);
                p += 2 * used_in;
                n -= used_in;
                if (!used_in && !made) break;
            }
        }
        if (run.stop_reason) { reason = run.stop_reason; break; }
        if (keys_at >= 0 && m->cpu.cycles >= (uint64_t)(keys_at * 1e6)) keys_started = 1;
        if (run.key_wait) {
            if (keyq_pos < keyq_len) { if (keys_at < 0) keys_started = 1; }
            else if (until_key_wait) { reason = "key-wait"; break; }
        }
        // DOS's console driver reads the BIOS buffer itself: many reads of the key count with nothing coming = idle
        if (m->bios.kb_polls >= 20000) {
            if (keyq_pos < keyq_len) { if (keys_at < 0) keys_started = 1; }
            else if (until_key_wait) { reason = "key-poll"; break; }
        }
        if (keys_started && keyq_pos < keyq_len && m->cpu.cycles >= next_key) {
            uint8_t k = keyq[keyq_pos++];
            pc98_key(m, k & 0x7F, !(k & 0x80));
            next_key = m->cpu.cycles + key_gap;
            run.key_wait = 0;
        }
    }
    if (pc98_dead(m) && strcmp(reason, "budget") == 0) reason = m->cpu.stop ? "stop" : "dead";

    if (run.fault_repeats)
        printf("FAULT (the one before repeated %llu more times)\n", (unsigned long long)run.fault_repeats);
    if (run.history_n) {
        uint64_t n = run.history_count < run.history_n ? run.history_count : run.history_n;
        printf("HISTORY (last %llu instructions, oldest first)\n", (unsigned long long)n);
        for (uint64_t i = 0; i < n; i++) {
            uint32_t h = run.history[(run.history_pos + run.history_n - n + i) % run.history_n];
            printf("  %04X:%04X\n", h >> 16, h & 0xFFFF);
        }
    }
    for (int d = 0; d < ndumps; d++) {
        unsigned addr, len;
        if (sscanf(dumps[d], "%x:%x", &addr, &len) != 2) continue;
        for (unsigned o = 0; o < len; o += 16) {
            printf("MEM %05X:", addr + o);
            for (unsigned k = 0; k < 16 && o + k < len; k++) printf(" %02X", pc98_read8(m, (addr + o + k) & 0xFFFFFF));
            printf("\n");
        }
    }
    for (int d = 0; d < nplane_dumps; d++) {
        unsigned pg, pl, off, len;
        if (sscanf(plane_dumps[d], "%x:%x:%x:%x", &pg, &pl, &off, &len) != 4 || pg > 1 || pl > 3) continue;
        const uint8_t *pp = pc98gfx_plane(&m->gfx, (int)pg, (int)pl);
        for (unsigned o = 0; o < len; o += 16) {
            printf("PLANE %u:%u:%04X:", pg, pl, off + o);
            for (unsigned k = 0; k < 16 && o + k < len; k++) printf(" %02X", pp[(off + o + k) % PC98_PLANE_SIZE]);
            printf("\n");
        }
    }
    for (int d = 0; d < ndumpbins; d++) {
        unsigned addr, len;
        char file[1024];
        if (sscanf(dumpbins[d], "%x:%x:%1023s", &addr, &len, file) != 3) continue;
        FILE *f = fopen(file, "wb");
        if (!f) { fprintf(stderr, "pc98run: cannot write %s\n", file); continue; }
        for (unsigned o = 0; o < len; o++) fputc(pc98_read8(m, (addr + o) & 0xFFFFFF), f);
        fclose(f);
    }
    double host_s = host_seconds() - host0;
    printf("CPUTIME cpu_s=%.3f (this process, all of it: the machine, the synthesis and the files) x_real_cpu=%.1f\n", cpu_seconds() - cpu0,
           cpu_seconds() - cpu0 > 0 ? (double)m->cpu.cycles / m->cpu_hz / (cpu_seconds() - cpu0) : 0.0);
    printf("SPEED host_s=%.3f emu_s=%.3f x_real=%.1f mcycles_per_host_s=%.1f frames=%llu frames_per_host_s=%.0f\n", host_s,
           (double)m->cpu.cycles / m->cpu_hz, host_s > 0 ? (double)m->cpu.cycles / m->cpu_hz / host_s : 0.0,
           host_s > 0 ? (double)m->cpu.cycles / 1e6 / host_s : 0.0, (unsigned long long)m->frames,
           host_s > 0 ? (double)m->frames / host_s : 0.0);
    if (stats_every > 0) print_stats(&run, host_s, cpu_seconds() - cpu0);
    if (watch_hi || watch_plane_hi) printf("WATCH %05X-%05X plane %04X-%04X: %llu byte writes (%llu printed)\n", watch_lo, watch_hi - 1,
                         watch_plane_lo, watch_plane_hi - 1,
                         (unsigned long long)run.watch_n, (unsigned long long)(run.watch_n < run.watch_max ? run.watch_n : run.watch_max));
    if (run.prof_on && run.prof_total) {                    // the busiest 16-byte pieces, as segment:offset of a typical form
        for (int top = 0; top < 14; top++) {
            uint64_t best = 0;
            uint32_t bk = 0;
            for (int k = 0; k < (1 << 16); k++)
                if (run.prof[k].key && run.prof[k].count > best) { best = run.prof[k].count; bk = run.prof[k].key; }
            if (!best) break;
            uint64_t clk = 0;
            for (int k = 0; k < (1 << 16); k++) if (run.prof[k].key == bk) clk = run.prof[k].clocks;
            printf("PROFILE linear %05X: %llu instructions (%.1f%%), %llu clocks (%.1f%%)\n", (bk - 1) << 4,
                   (unsigned long long)best, 100.0 * (double)best / (double)run.prof_total, (unsigned long long)clk,
                   run.prof_clocks_total ? 100.0 * (double)clk / (double)run.prof_clocks_total : 0.0);
            for (int k = 0; k < (1 << 16); k++) if (run.prof[k].key == bk) run.prof[k].count = 0;
        }
        printf("PROFILE total %llu instructions between %llu and %llu million cycles\n", (unsigned long long)run.prof_total,
               (unsigned long long)run.prof_from, (unsigned long long)run.prof_to);
    }
    for (int k = 0; k < run.writers_n; k++)
        printf("WRITER %04X:%04X %llu bytes\n", run.writers[k].at >> 16, run.writers[k].at & 0xFFFF,
               (unsigned long long)run.writers[k].count);
    if (fm) {
        uint64_t total = 0;
        for (int b = 0; b < 2; b++) for (int r = 0; r < 256; r++) total += opna_writes[b][r];
        printf("OPNA board=%s irq=%d register_writes=%llu samples=%llu irq_edges=%u flags=%02X timer_ctrl=%02X"
               " ssg_mixer=%02X fifo_dropped=%u\n", opna_board == OPNA_BOARD_86 ? "86" : "26K", opna_irq,
               (unsigned long long)total, (unsigned long long)fm->samples, fm->irq_count, fm->flags, fm->timer_ctrl,
               fm->ssg.reg[7], fm->fifo_dropped);
        static const char *kinds[] = { "ssg-tone", "ssg-noise", "ssg-mixer", "ssg-level", "ssg-envelope", "ssg-port", "rhythm",
                                       "timer", "fm-key", "fm-control", "fm-pitch", "fm-channel", "fm-operator", "adpcm", "other" };
        for (unsigned k = 0; k < sizeof kinds / sizeof *kinds; k++) {
            uint64_t n = 0;
            for (int b = 0; b < 2; b++) for (unsigned r = 0; r < 256; r++)
                if (!strcmp(opna_kind(b, r), kinds[k])) n += opna_writes[b][r];
            if (n) printf("OPNAWRITES %s=%llu\n", kinds[k], (unsigned long long)n);
        }
    }
    if (wav_file) {
        fflush(wav_file);
        fseek(wav_file, 0, SEEK_SET);
        wav_header(wav_file, wav_frames, wav_rate_hz);
        fclose(wav_file);
        printf("WAV %s %u frames at %u Hz (%.2f s)\n", wav_path, wav_frames, wav_rate_hz, (double)wav_frames / wav_rate_hz);
    }
    if (opna_log) fclose(opna_log);
    if (dump_text) {
        char buf[PC98_TEXT_ROWS * (PC98_TEXT_COLS + 1) + 1];
        pc98_text_dump(m, buf);
        for (char *p = buf; *p;) {
            char *e = strchr(p, '\n');
            printf("|%.*s\n", (int)(e - p), p);
            p = e + 1;
        }
    }
    if (ppm) write_ppm(m, ppm);
    X86Cpu *c = &m->cpu;
    if (show_state)
        printf("STATE text_on=%d gfx_on=%d ead0=%X cursor_shown=%d cursor_blink=%d cursor_addr=%X crt_mode=%X gfx_mode=%X "
               "beep_on=%d beep_hz=%u imr0=%02X imr1=%02X isr0=%02X frames=%llu kb_count=%u kb_retry=%u\n",
               m->gfx.gdc[0].display_on, m->gfx.gdc[1].display_on, m->gfx.gdc[0].ead, m->bios.cursor_shown, m->bios.cursor_blink,
               m->bios.cursor_addr, m->bios.crt_mode, m->bios.gfx_mode, m->beep_on, pc98_beep_hz(m), m->pic[0].imr,
               m->pic[1].imr, m->pic[0].isr, (unsigned long long)m->frames, m->ram[PC98_WA_KB_COUNT],
               m->ram[PC98_WA_KB_RETRY]);
    if (show_state) {
        printf("PAL analog=%d egc_mode=%d grcg=%02X page_shown=%d page_drawn=%d colours(RRGGBB):", m->gfx.analog, m->gfx.egc_mode,
               m->gfx.grcg.mode, m->gfx.disp_page, m->gfx.draw_page);
        for (unsigned k = 0; k < 16; k++) printf(" %06X", pc98gfx_colour(&m->gfx, k) & 0xFFFFFF);
        printf("\n");
        for (int g = 0; g < 2; g++) {
            const Pc98GfxGdc *d = &m->gfx.gdc[g];
            printf("GDC%d on=%d pitch=%u zoom=%02X csrform=%02X %02X %02X pram=", g, d->display_on, d->pitch, d->zoom,
                   d->csrform[0], d->csrform[1], d->csrform[2]);
            for (int k = 0; k < 16; k++) printf("%02X ", d->pram[k]);
            printf(" ead=%X\n", d->ead);
        }
    }
    printf("BIOSSTAT calls=%u unknown=%u key_waits=%u disk_reads=%u disk_writes=%u unhandled_ports=%u last_unhandled=%04X\n",
           m->bios.calls, m->bios.unknown, m->bios.key_waits, m->bios.disk_reads, m->bios.disk_writes, m->unhandled,
           m->last_unhandled_port);
    printf("END reason=%s stop=%d (%s) halted=%d if=%d cycles=%llu instructions=%llu cs:ip=%04X:%04X ss:sp=%04X:%04X "
           "opcode=%02X\n", reason, c->stop, pc98_stop_name(c->stop), c->halted, !!(c->flags & X86_IF),
           (unsigned long long)c->cycles, (unsigned long long)c->instructions, c->s[X86_CS], c->ip, c->s[X86_SS],
           c->r[X86_SP], c->bad_opcode);
    for (int i = 0; i < ndisks; i++) free(image[i]);
    free(run.history);
    free(m);
    return 0;
}
