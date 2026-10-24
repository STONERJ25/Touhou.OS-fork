// Host test driver for the x86emu CPU core (user/lib/pc98). Reads a script on stdin, one command per line, numbers in hex:
//
//   model 8086|v30|286     pick the CPU model (resets everything)
//   a20 0|1                286: address mask 0xFFFFF or 0xFFFFFF
//   r AX CX DX BX SP BP SI DI ES CS SS DS IP FLAGS    set all registers
//   m ADDR B0 B1 ...       store bytes at a physical address
//   p PORT VALUE           what IN returns for PORT (default FF, or FFFF for a word)
//   irq VECTOR             assert INTR with this vector until it is acknowledged once
//   step N                 run N instructions (a REP instruction counts once, all its iterations included)
//   run MAX                run until HLT, a stop, or MAX instructions
//   load ADDR FILE         copy a binary file to a physical address
//   d ADDR LEN             print "D <hex bytes>"
//   clear                  zero every byte touched since the last clear
//
// After step/run it prints: "R ax cx dx bx sp bp si di es cs ss ds ip flags cycles halted stop instructions", then one
// "W addr value" line per byte written (final value, in address order) and one "O port value width" line per OUT.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86emu.h>

#define MEMSZ (16u << 20)
static uint8_t *mem;
static uint8_t *wbits;           /* written since the last report */
static uint8_t *tbits;           /* touched since the last clear */
static uint32_t *wlist, nw;
static uint32_t *tlist, nt;
static uint16_t portval[65536];
static int irq_vec = -1;

static void touch(uint32_t a) {
    if (!(tbits[a >> 3] & (1 << (a & 7)))) { tbits[a >> 3] |= (uint8_t)(1 << (a & 7)); tlist[nt++] = a; }
}
static uint8_t r8(void *ctx, uint32_t a) { (void)ctx; return mem[a]; }
static void w8(void *ctx, uint32_t a, uint8_t v) {
    (void)ctx;
    mem[a] = v;
    touch(a);
    if (!(wbits[a >> 3] & (1 << (a & 7)))) { wbits[a >> 3] |= (uint8_t)(1 << (a & 7)); wlist[nw++] = a; }
}
static uint8_t in8(void *ctx, uint16_t p) { (void)ctx; return (uint8_t)portval[p]; }
static uint16_t in16(void *ctx, uint16_t p) { (void)ctx; return portval[p] | (portval[p] == 0xFF ? 0xFF00 : 0); }
static void out8(void *ctx, uint16_t p, uint8_t v) { (void)ctx; printf("O %x %x 1\n", p, v); }
static void out16(void *ctx, uint16_t p, uint16_t v) { (void)ctx; printf("O %x %x 2\n", p, v); }
static int intr(void *ctx) { (void)ctx; int v = irq_vec; irq_vec = -1; return v; }

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void report(X86Cpu *c) {
    printf("R %x %x %x %x %x %x %x %x %x %x %x %x %x %x %llx %d %d %llx\n", c->r[0], c->r[1], c->r[2], c->r[3], c->r[4],
           c->r[5], c->r[6], c->r[7], c->s[0], c->s[1], c->s[2], c->s[3], c->ip, c->flags,
           (unsigned long long)c->cycles, c->halted, c->stop, (unsigned long long)c->instructions);
    qsort(wlist, nw, sizeof *wlist, cmp_u32);
    for (uint32_t i = 0; i < nw; i++) {
        printf("W %x %x\n", wlist[i], mem[wlist[i]]);
        wbits[wlist[i] >> 3] = 0;
    }
    nw = 0;
}

int main(void) {
    mem = calloc(MEMSZ, 1);
    wbits = calloc(MEMSZ / 8, 1);
    tbits = calloc(MEMSZ / 8, 1);
    wlist = malloc(sizeof(uint32_t) * (MEMSZ));
    tlist = malloc(sizeof(uint32_t) * (MEMSZ));
    for (int i = 0; i < 65536; i++) portval[i] = 0xFF;
    X86Bus bus = { .read8 = r8, .write8 = w8, .in8 = in8, .out8 = out8, .in16 = in16, .out16 = out16, .intr_ack = intr };
    X86Cpu cpu;
    x86_init(&cpu, X86_MODEL_8086, &bus);
    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        char *s = line, *e;
        char cmd[16];
        int n;
        if (sscanf(s, "%15s%n", cmd, &n) != 1) continue;
        s += n;
        if (!strcmp(cmd, "model")) {
            char m[16] = "";
            sscanf(s, "%15s", m);
            X86Model md = !strcmp(m, "v30") ? X86_MODEL_V30 : !strcmp(m, "286") ? X86_MODEL_80286 : X86_MODEL_8086;
            x86_init(&cpu, md, &bus);
        } else if (!strcmp(cmd, "a20")) {
            cpu.addr_mask = strtoul(s, NULL, 16) ? 0xFFFFFF : 0xFFFFF;
        } else if (!strcmp(cmd, "r")) {
            unsigned long v[14];
            for (int i = 0; i < 14; i++) { v[i] = strtoul(s, &e, 16); s = e; }
            for (int i = 0; i < 8; i++) cpu.r[i] = (uint16_t)v[i];
            for (int i = 0; i < 4; i++) cpu.s[i] = (uint16_t)v[8 + i];
            cpu.ip = (uint16_t)v[12];
            cpu.flags = (uint16_t)v[13];
            cpu.halted = cpu.stop = cpu.shadow = cpu.in_rep = 0;
            cpu.cycles = cpu.instructions = 0;
        } else if (!strcmp(cmd, "m")) {
            uint32_t a = (uint32_t)strtoul(s, &e, 16);
            for (s = e;; s = e) {
                unsigned long b = strtoul(s, &e, 16);
                if (e == s) break;
                mem[a & (MEMSZ - 1)] = (uint8_t)b;
                touch(a & (MEMSZ - 1));
                a++;
            }
        } else if (!strcmp(cmd, "p")) {
            unsigned long p = strtoul(s, &e, 16), v = strtoul(e, NULL, 16);
            portval[p & 0xFFFF] = (uint16_t)v;
        } else if (!strcmp(cmd, "irq")) {
            irq_vec = (int)strtol(s, NULL, 16);
        } else if (!strcmp(cmd, "step") || !strcmp(cmd, "run")) {
            unsigned long count = strtoul(s, NULL, 16);
            int until_hlt = cmd[0] == 'r';
            for (unsigned long i = 0; i < count && !cpu.stop; i++) {
                if (until_hlt && cpu.halted && irq_vec < 0) break;
                x86_step(&cpu);
                while (cpu.in_rep && !cpu.stop) x86_step(&cpu);
            }
            report(&cpu);
        } else if (!strcmp(cmd, "load")) {
            char path[4096];
            unsigned long a;
            if (sscanf(s, "%lx %4095s", &a, path) == 2) {
                FILE *f = fopen(path, "rb");
                if (!f) { printf("E cannot open %s\n", path); fflush(stdout); return 1; }
                int ch;
                while ((ch = fgetc(f)) != EOF) { mem[a & (MEMSZ - 1)] = (uint8_t)ch; touch(a & (MEMSZ - 1)); a++; }
                fclose(f);
            }
        } else if (!strcmp(cmd, "d")) {
            unsigned long a = strtoul(s, &e, 16), len = strtoul(e, NULL, 16);
            printf("D");
            for (unsigned long i = 0; i < len; i++) printf(" %x", mem[(a + i) & (MEMSZ - 1)]);
            printf("\n");
        } else if (!strcmp(cmd, "clear")) {
            for (uint32_t i = 0; i < nt; i++) { mem[tlist[i]] = 0; tbits[tlist[i] >> 3] = 0; }
            nt = 0;
            for (uint32_t i = 0; i < nw; i++) wbits[wlist[i] >> 3] = 0;
            nw = 0;
            for (int i = 0; i < 65536; i++) portval[i] = 0xFF;
            irq_vec = -1;
        }
    }
    return 0;
}
