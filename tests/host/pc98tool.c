// Host test driver for the PC-98 machine skeleton and the disk image readers (user/lib/pc98). Reads commands on stdin,
// numbers in hex unless noted, and prints one answer line per query:
//
//   init MODEL MHZ          new machine: MODEL 8086|v30|286, MHZ in decimal          -> nothing
//   disk FILE HINT          open an image, HINT none|d88|fdi|hdi|raw                  -> "F rc format cyl heads spt ssz media wp name"
//   sectors                 every sector of every track, in recorded order            -> "S cyl head idx c h r n size deleted status crc32"
//   find CYL HEAD R         look a sector up by its ID                                -> "S ..." or "N rc"
//   lba N                   logical block N                                           -> "L n c h r size crc32" or "L n rc"
//   boot DAUA               hand the first sector over to the CPU                     -> "B rc"
//   run CYCLES              run until the machine is dead or the cycles are used up   -> "R used dead halted stop cs ip"
//   out PORT V / in PORT    port access                                               -> in: "I value"
//   adv CYCLES              let the devices see time pass, CPU stopped
//   key CODE DOWN           a key event
//   irq N LEVEL / ack       interrupt lines / an INTA cycle                           -> ack: "A vector" (-1: none)
//   rd ADDR / wr ADDR V     physical memory                                           -> rd: "M value"
//   regs                    CPU registers                                             -> "C ax cx dx bx sp bp si di es cs ss ds ip flags"
//   state                   device state                                              -> "Z key=value ..."
//   text                    the text screen                                           -> 25 lines "|<80 chars>"
//   render FILE             the picture, 640x400 32-bit little-endian 0x00RRGGBB, with a synthetic font (glyph row
//                           y of code c is the byte c + y)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pc98.h>

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    uint8_t *b = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; b = realloc(b, cap); }
        size_t r = fread(b + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    fclose(f);
    *n = len;
    return b;
}

static void print_sector(uint32_t cyl, uint32_t head, uint32_t idx, const Pc98Sector *s) {
    printf("S %x %x %x %x %x %x %x %x %x %x %08x\n", cyl, head, idx, s->c, s->h, s->r, s->n, s->size, s->deleted,
           s->status, crc32(s->data, s->size));
}

int main(void) {
    crc_init();
    Pc98 *m = calloc(1, sizeof *m);
    pc98_init(m, X86_MODEL_V30, 10000000);
    Pc98Disk disk;
    memset(&disk, 0, sizeof disk);
    uint8_t *image = NULL;
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        char cmd[32], a1[4096] = "", a2[64] = "", a3[64] = "";
        int n = sscanf(line, "%31s %4095s %63s %63s", cmd, a1, a2, a3);
        if (n < 1) continue;
        unsigned long x1 = strtoul(a1, NULL, 16), x2 = strtoul(a2, NULL, 16), x3 = strtoul(a3, NULL, 16);
        if (!strcmp(cmd, "init")) {
            X86Model md = !strcmp(a1, "8086") ? X86_MODEL_8086 : !strcmp(a1, "286") ? X86_MODEL_80286 : X86_MODEL_V30;
            pc98_init(m, md, (uint32_t)strtoul(a2, NULL, 10) * 1000000u);
        } else if (!strcmp(cmd, "disk")) {
            size_t len = 0;
            free(image);
            image = slurp(a1, &len);
            if (!image) { printf("F -9\n"); continue; }
            Pc98DiskFormat h = !strcmp(a2, "d88") ? PC98_DISK_D88 : !strcmp(a2, "fdi") ? PC98_DISK_FDI
                             : !strcmp(a2, "hdi") ? PC98_DISK_HDI : !strcmp(a2, "raw") ? PC98_DISK_RAW : PC98_DISK_NONE;
            int rc = pc98_disk_open(&disk, image, len, h);
            printf("F %d %s %x %x %x %x %x %x %s\n", rc, pc98_disk_format_name(disk.format), disk.cylinders, disk.heads,
                   disk.sectors, disk.sector_size, disk.media, disk.write_protect, disk.name[0] ? disk.name : "-");
        } else if (!strcmp(cmd, "sectors")) {
            uint32_t cyls = disk.format == PC98_DISK_D88 ? PC98_D88_MAXTRACKS / 2 : disk.cylinders;
            uint32_t heads = disk.format == PC98_DISK_D88 ? 2 : disk.heads;
            for (uint32_t c = 0; c < cyls; c++)
                for (uint32_t h = 0; h < heads; h++) {
                    uint32_t ns = pc98_disk_track_sectors(&disk, c, h);
                    for (uint32_t i = 0; i < ns; i++) {
                        Pc98Sector s;
                        int rc = pc98_disk_sector_at(&disk, c, h, i, &s);
                        if (rc) { printf("E %x %x %x %d\n", c, h, i, rc); break; }
                        print_sector(c, h, i, &s);
                    }
                }
            printf("END\n");
        } else if (!strcmp(cmd, "find")) {
            Pc98Sector s;
            int rc = pc98_disk_sector(&disk, (uint32_t)x1, (uint32_t)x2, (uint32_t)x3, &s);
            if (rc) printf("N %d\n", rc); else print_sector((uint32_t)x1, (uint32_t)x2, 0, &s);
        } else if (!strcmp(cmd, "lba")) {
            Pc98Sector s;
            int rc = pc98_disk_read_lba(&disk, (uint32_t)x1, &s);
            if (rc) printf("L %lx %d\n", x1, rc);
            else printf("L %lx %x %x %x %x %08x\n", x1, s.c, s.h, s.r, s.size, crc32(s.data, s.size));
        } else if (!strcmp(cmd, "boot")) {
            printf("B %d\n", pc98_boot(m, &disk, (uint8_t)x1));
        } else if (!strcmp(cmd, "run")) {
            uint64_t used = pc98_run(m, strtoull(a1, NULL, 10));
            printf("R %llu %d %d %d %x %x\n", (unsigned long long)used, pc98_dead(m), m->cpu.halted, m->cpu.stop,
                   m->cpu.s[X86_CS], m->cpu.ip);
        } else if (!strcmp(cmd, "out")) {
            pc98_out8(m, (uint16_t)x1, (uint8_t)x2);
        } else if (!strcmp(cmd, "in")) {
            printf("I %x\n", pc98_in8(m, (uint16_t)x1));
        } else if (!strcmp(cmd, "adv")) {
            pc98_advance(m, strtoull(a1, NULL, 10));
        } else if (!strcmp(cmd, "key")) {
            pc98_key(m, (uint8_t)x1, (int)x2);
        } else if (!strcmp(cmd, "irq")) {
            pc98_irq(m, (int)x1, (int)x2);
        } else if (!strcmp(cmd, "ack")) {
            int v = pc98_intr_ack(m);
            if (v < 0) printf("A -1\n"); else printf("A %x\n", v);
        } else if (!strcmp(cmd, "rd")) {
            printf("M %x\n", pc98_read8(m, (uint32_t)x1));
        } else if (!strcmp(cmd, "wr")) {
            pc98_write8(m, (uint32_t)x1, (uint8_t)x2);
        } else if (!strcmp(cmd, "regs")) {
            X86Cpu *c = &m->cpu;
            printf("C %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5],
                   c->r[6], c->r[7], c->s[0], c->s[1], c->s[2], c->s[3], c->ip, c->flags);
        } else if (!strcmp(cmd, "state")) {
            printf("Z beep_on=%d beep_hz=%u beep_changes=%u frames=%llu vsync=%d gdc0_on=%d gdc1_on=%d ead0=%x ead1=%x "
                   "pitch1=%x gdc0_cmds=%u gdc1_ignored=%u unhandled=%u last_unhandled=%x to_kbd=%u a20=%d addr_mask=%x "
                   "cpu_resets=%u disp_page=%d draw_page=%d analog=%d mode_ff1=%x pit_hz=%u cycles=%llu "
                   "instructions=%llu imr0=%x imr1=%x isr0=%x isr1=%x irr0=%x irr1=%x\n",
                   m->beep_on, pc98_beep_hz(m), m->beep_changes, (unsigned long long)m->frames, m->vsync,
                   m->gdc[0].display_on, m->gdc[1].display_on, m->gdc[0].ead, m->gdc[1].ead, m->gdc[1].pitch,
                   m->gdc[0].commands, m->gdc[1].ignored, m->unhandled, m->last_unhandled_port, m->kbd.to_keyboard,
                   m->a20, m->cpu.addr_mask, m->cpu_resets, m->disp_page, m->draw_page, m->mode_ff2_analog,
                   m->mode_ff1, m->pit_hz, (unsigned long long)m->cpu.cycles,
                   (unsigned long long)m->cpu.instructions, m->pic[0].imr, m->pic[1].imr, m->pic[0].isr,
                   m->pic[1].isr, m->pic[0].irr, m->pic[1].irr);
        } else if (!strcmp(cmd, "text")) {
            char buf[PC98_TEXT_ROWS * (PC98_TEXT_COLS + 1) + 1];
            pc98_text_dump(m, buf);
            for (char *p = buf; *p;) {
                char *e = strchr(p, '\n');
                printf("|%.*s\n", (int)(e - p), p);
                p = e + 1;
            }
        } else if (!strcmp(cmd, "render")) {
            static uint8_t font[256 * 16];
            for (int c = 0; c < 256; c++) for (int y = 0; y < 16; y++) font[c * 16 + y] = (uint8_t)(c + y);
            uint32_t *px = calloc(640 * 400, 4);
            pc98_render(m, px, 640, font);
            FILE *f = fopen(a1, "wb");
            if (f) { fwrite(px, 4, 640 * 400, f); fclose(f); }
            free(px);
        }
        fflush(stdout);
    }
    free(image);
    free(m);
    return 0;
}
