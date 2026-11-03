// Host test driver for the PC-98 display module (user/lib/pc98/src/pc98gfx.c). Reads commands on stdin, numbers in
// hex unless noted, and prints one answer line per query (unknown commands print "E ..."):
//
//   init / reset                  pc98gfx_init (the tool's fonts are set again) / pc98gfx_reset
//   font none|synth|repo|rom FILE ANK glyphs: none; synthetic (row y of code c is the byte c + y); the repository's
//                                 shared/font8x16.c through pc98gfx_ank_from_unicode; a FONT.ROM image (also kanji)
//   kanji none|synth              kanji source: none; synthetic (byte i of JIS rrcc is rr ^ (cc << 1) ^ 11h * i,
//                                 no glyph for row 7Eh)
//   w8 A V / w16 A V              memory writes                            r8 A / r16 A  -> "M v"
//   wb A B0 B1 ... / ww A W0 ...  consecutive byte / word writes           rb A N / rw A N -> "B v.." / "W v.."
//   out P V [WIDTH] / outc ...    port write (outc prints "O claimed")     in P [WIDTH]  -> "I v claimed"
//   plane PG PL OFF N             raw plane bytes -> "P b.."               setplane PG PL OFF B0 ..  raw writes
//   fillplane PG PL V             raw fill of a whole plane
//   text                          25 lines "|<80 chars>"
//   render FILE / renderp FILE BLINK CURSOR   640x400 0x00RRGGBB little-endian (render_phase with the phases given)
//   ppm FILE [BLINK CURSOR]       pc98gfx_ppm of the picture
//   hash [BLINK CURSOR]           "H crc32" of the picture's bytes
//   vsync N(decimal) / retrace V H   frames / beam
//   state                         "Z key=value ..."
//   colours                       "C c0 .. c15" (pc98gfx_colour)
//   cg CODE                       "G rc hex64" (pc98gfx_cg_glyph)
//   memsw N / setmemsw N V        "S v" / set
//   ank C                         "U codepoint"
//   fontrom FILE                  "V valid"
//   size                          "Y sizeof"                               claims A      -> "K 0|1"
//   bench N(decimal)              "T render_us grcg_us egc_us" per frame / per 32000-byte fill / per 16000-word copy
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pc98gfx.h>
#include "../../shared/font8x16.h"

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

static Pc98Gfx G;
static uint8_t ank_synth[4096], ank_repo[4096];
static uint8_t *rom;
static const uint8_t *cur_ank;
static Pc98GfxKanjiFn cur_kanji;
static void *cur_user;
static uint32_t pixels[640 * 400];

static int kanji_synth(void *user, uint16_t jis, uint8_t glyph[32]) {
    unsigned row = jis >> 8, cell = jis & 0xFF;
    if (row == 0x7E) return 0;
    for (unsigned i = 0; i < 32; i++) glyph[i] = (uint8_t)(row ^ (cell << 1) ^ (0x11 * i));
    return 1;
}

static void apply_fonts(void) { pc98gfx_set_font(&G, cur_ank, cur_kanji, cur_user); }

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len > 0 ? (size_t)len : 1);
    *n = fread(b, 1, (size_t)(len > 0 ? len : 0), f);
    fclose(f);
    return b;
}

static void do_render(int phase, int blink, int cursor) {
    if (phase) pc98gfx_render_phase(&G, pixels, 640, blink, cursor);
    else pc98gfx_render(&G, pixels, 640);
}

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

#define MAXTOK 4200
static char *tok[MAXTOK];
static int ntok;
static unsigned long hx(int i) { return i < ntok ? strtoul(tok[i], NULL, 16) : 0; }
static long dec(int i) { return i < ntok ? strtol(tok[i], NULL, 10) : 0; }

int main(void) {
    crc_init();
    for (int c = 0; c < 256; c++) for (int y = 0; y < 16; y++) ank_synth[c * 16 + y] = (uint8_t)(c + y);
    pc98gfx_ank_from_unicode(ank_repo, font8x16_glyph);
    pc98gfx_init(&G);
    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        ntok = 0;
        for (char *t = strtok(line, " \t\r\n"); t && ntok < MAXTOK; t = strtok(NULL, " \t\r\n")) tok[ntok++] = t;
        if (!ntok) continue;
        const char *cmd = tok[0];
        if (!strcmp(cmd, "init")) {
            pc98gfx_init(&G);
            apply_fonts();
        } else if (!strcmp(cmd, "reset")) {
            pc98gfx_reset(&G);
        } else if (!strcmp(cmd, "font")) {
            const char *k = ntok > 1 ? tok[1] : "none";
            if (!strcmp(k, "synth")) cur_ank = ank_synth;
            else if (!strcmp(k, "repo")) cur_ank = ank_repo;
            else if (!strcmp(k, "rom")) {
                size_t n = 0;
                free(rom);
                rom = ntok > 2 ? slurp(tok[2], &n) : NULL;
                if (!rom || !pc98gfx_fontrom_valid(rom, n)) { printf("E bad font rom\n"); fflush(stdout); continue; }
                cur_ank = pc98gfx_fontrom_ank16(rom);
                cur_kanji = pc98gfx_fontrom_kanji;
                cur_user = rom;
            } else cur_ank = NULL;
            apply_fonts();
        } else if (!strcmp(cmd, "kanji")) {
            if (ntok > 1 && !strcmp(tok[1], "synth")) { cur_kanji = kanji_synth; cur_user = NULL; }
            else { cur_kanji = NULL; cur_user = NULL; }
            apply_fonts();
        } else if (!strcmp(cmd, "w8")) {
            pc98gfx_mem_write8(&G, (uint32_t)hx(1), (uint8_t)hx(2));
        } else if (!strcmp(cmd, "w16")) {
            pc98gfx_mem_write16(&G, (uint32_t)hx(1), (uint16_t)hx(2));
        } else if (!strcmp(cmd, "r8")) {
            printf("M %x\n", pc98gfx_mem_read8(&G, (uint32_t)hx(1)));
        } else if (!strcmp(cmd, "r16")) {
            printf("M %x\n", pc98gfx_mem_read16(&G, (uint32_t)hx(1)));
        } else if (!strcmp(cmd, "wb")) {
            uint32_t a = (uint32_t)hx(1);
            for (int i = 2; i < ntok; i++) pc98gfx_mem_write8(&G, a++, (uint8_t)hx(i));
        } else if (!strcmp(cmd, "ww")) {
            uint32_t a = (uint32_t)hx(1);
            for (int i = 2; i < ntok; i++, a += 2) pc98gfx_mem_write16(&G, a, (uint16_t)hx(i));
        } else if (!strcmp(cmd, "rb") || !strcmp(cmd, "rw")) {
            int word = cmd[1] == 'w';
            uint32_t a = (uint32_t)hx(1);
            long n = (long)hx(2);
            printf(word ? "W" : "B");
            for (long i = 0; i < n; i++, a += word ? 2 : 1)
                printf(" %x", word ? pc98gfx_mem_read16(&G, a) : pc98gfx_mem_read8(&G, a));
            printf("\n");
        } else if (!strcmp(cmd, "out") || !strcmp(cmd, "outc")) {
            int w = ntok > 3 ? (int)hx(3) : 1;
            int mine = pc98gfx_port_write(&G, (uint16_t)hx(1), (uint32_t)hx(2), w);
            if (cmd[3] == 'c') printf("O %d\n", mine);
        } else if (!strcmp(cmd, "in")) {
            uint32_t v = 0xFFFFFFFFu;
            int w = ntok > 2 ? (int)hx(2) : 1;
            int mine = pc98gfx_port_read(&G, (uint16_t)hx(1), &v, w);
            printf("I %x %d\n", mine ? v : 0, mine);
        } else if (!strcmp(cmd, "plane")) {
            uint8_t *p = pc98gfx_plane(&G, (int)hx(1), (int)hx(2));
            unsigned off = (unsigned)hx(3), n = (unsigned)hx(4);
            printf("P");
            for (unsigned i = 0; i < n; i++) printf(" %x", p[(off + i) & 0x7FFF]);
            printf("\n");
        } else if (!strcmp(cmd, "setplane")) {
            uint8_t *p = pc98gfx_plane(&G, (int)hx(1), (int)hx(2));
            unsigned off = (unsigned)hx(3);
            for (int i = 4; i < ntok; i++) p[(off++) & 0x7FFF] = (uint8_t)hx(i);
        } else if (!strcmp(cmd, "fillplane")) {
            memset(pc98gfx_plane(&G, (int)hx(1), (int)hx(2)), (int)hx(3), PC98GFX_PLANE_SIZE);
        } else if (!strcmp(cmd, "text")) {
            char buf[PC98GFX_TEXT_DUMP_SIZE];
            pc98gfx_text_dump(&G, buf);
            for (char *p = buf; *p;) {
                char *e = strchr(p, '\n');
                printf("|%.*s\n", (int)(e - p), p);
                p = e + 1;
            }
        } else if (!strcmp(cmd, "render") || !strcmp(cmd, "renderp")) {
            do_render(cmd[6] == 'p', (int)hx(2), (int)hx(3));
            write_file(tok[1], pixels, sizeof pixels);
        } else if (!strcmp(cmd, "ppm")) {
            do_render(ntok > 3, (int)hx(2), (int)hx(3));
            size_t need = pc98gfx_ppm(pixels, 640, 400, 640, NULL, 0);
            uint8_t *buf = malloc(need);
            size_t got = pc98gfx_ppm(pixels, 640, 400, 640, buf, need);
            write_file(tok[1], buf, got);
            free(buf);
        } else if (!strcmp(cmd, "hash")) {
            do_render(ntok > 2, (int)hx(1), (int)hx(2));
            printf("H %08x\n", crc32((const uint8_t *)pixels, sizeof pixels));
        } else if (!strcmp(cmd, "vsync")) {
            for (long i = 0; i < dec(1); i++) pc98gfx_vsync(&G);
        } else if (!strcmp(cmd, "retrace")) {
            pc98gfx_set_retrace(&G, (int)hx(1), (int)hx(2));
        } else if (!strcmp(cmd, "colours")) {
            printf("C");
            for (unsigned i = 0; i < 16; i++) printf(" %06x", pc98gfx_colour(&G, i));
            printf("\n");
        } else if (!strcmp(cmd, "cg")) {
            uint8_t gl[32];
            int rc = pc98gfx_cg_glyph(&G, (uint16_t)hx(1), gl);
            printf("G %d ", rc);
            for (int i = 0; i < 32; i++) printf("%02x", gl[i]);
            printf("\n");
        } else if (!strcmp(cmd, "memsw")) {
            printf("S %x\n", pc98gfx_memsw(&G, (int)hx(1)));
        } else if (!strcmp(cmd, "setmemsw")) {
            pc98gfx_set_memsw(&G, (int)hx(1), (uint8_t)hx(2));
        } else if (!strcmp(cmd, "ank")) {
            printf("U %x\n", (unsigned)pc98gfx_ank_unicode((uint8_t)hx(1)));
        } else if (!strcmp(cmd, "fontrom")) {
            size_t n = 0;
            uint8_t *b = ntok > 1 ? slurp(tok[1], &n) : NULL;
            printf("V %d\n", b ? pc98gfx_fontrom_valid(b, n) : -1);
            free(b);
        } else if (!strcmp(cmd, "size")) {
            printf("Y %zu\n", sizeof(Pc98Gfx));
        } else if (!strcmp(cmd, "claims")) {
            printf("K %d\n", pc98gfx_mem_claims((uint32_t)hx(1)));
        } else if (!strcmp(cmd, "state")) {
            const Pc98GfxGdc *t = &G.gdc[0], *g = &G.gdc[1];
            const Pc98GfxEgc *e = &G.egc;
            printf("Z gdc0_on=%d gdc1_on=%d ead0=%x ead1=%x dad1=%x mask0=%x mask1=%x pitch0=%x pitch1=%x "
                   "cmds0=%u cmds1=%u unk0=%u unk1=%u dma0=%u dma1=%u dots1=%u words0=%u words1=%u "
                   "dir1=%x flags1=%x dc1=%x d1=%x d2_1=%x d1_1=%x dm1=%x zoom1=%x mod1=%x "
                   "csr0=%02x%02x%02x pram0=%02x%02x%02x%02x pram1=%02x%02x%02x%02x "
                   "mode1=%x analog=%d egc_mode=%d egc_permit=%d disp=%d draw=%d border=%x "
                   "grcg=%x tiles=%02x%02x%02x%02x tile_next=%d "
                   "egc=%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x pat=%04x,%04x,%04x,%04x egc_qn=%d egc_done=%d "
                   "egc_under=%u frame=%u ff2_unknown=%u crtc=%02x%02x%02x%02x%02x%02x cg=%02x%02x%02x\n",
                   t->display_on, g->display_on, t->ead, g->ead, g->dad, t->mask, g->mask, t->pitch, g->pitch,
                   t->commands, g->commands, t->unknown, g->unknown, t->dma, g->dma, g->dots, t->words_written,
                   g->words_written, g->dir, g->figs_flags, g->dc, g->d, g->d2, g->d1, g->dm, g->zoom, g->mod,
                   t->csrform[0], t->csrform[1], t->csrform[2], t->pram[0], t->pram[1], t->pram[2], t->pram[3],
                   g->pram[0], g->pram[1], g->pram[2], g->pram[3],
                   G.mode1, G.analog, G.egc_mode, G.egc_permit, G.disp_page, G.draw_page, G.border,
                   G.grcg.mode, G.grcg.tile[0], G.grcg.tile[1], G.grcg.tile[2], G.grcg.tile[3], G.grcg.tile_next,
                   e->reg[0], e->reg[1], e->reg[2], e->reg[3], e->reg[4], e->reg[5], e->reg[6], e->reg[7],
                   e->pat[0], e->pat[1], e->pat[2], e->pat[3], e->qn, e->out_done, e->underruns,
                   G.frame, G.ff2_unknown, G.crtc[0], G.crtc[1], G.crtc[2], G.crtc[3], G.crtc[4], G.crtc[5],
                   G.cg_code_lo, G.cg_code_hi, G.cg_line);
        } else if (!strcmp(cmd, "bench")) {
            long n = dec(1) > 0 ? dec(1) : 10;
            double t0 = now_us();
            for (long i = 0; i < n; i++) pc98gfx_render(&G, pixels, 640);
            double t1 = now_us();
            pc98gfx_port_write(&G, 0x7C, 0x80, 1);                  /* GRCG TDW over all planes */
            for (long i = 0; i < n; i++)
                for (uint32_t a = 0; a < 32000; a++) pc98gfx_mem_write8(&G, 0xA8000 + a, 0xFF);
            double t2 = now_us();
            pc98gfx_port_write(&G, 0x6A, 0x07, 1);                  /* EGC: copy page 0 onto itself, word by word */
            pc98gfx_port_write(&G, 0x6A, 0x05, 1);
            pc98gfx_port_write(&G, 0x4A4, 0x29F0, 2);
            for (long i = 0; i < n; i++)
                for (uint32_t a = 0; a < 32000; a += 2) {
                    uint16_t v = pc98gfx_mem_read16(&G, 0xA8000 + a);
                    pc98gfx_mem_write16(&G, 0xA8000 + a, v);
                }
            double t3 = now_us();
            pc98gfx_port_write(&G, 0x7C, 0x00, 1);
            printf("T %.1f %.1f %.1f\n", (t1 - t0) / n, (t2 - t1) / n, (t3 - t2) / n);
        } else {
            printf("E unknown command %s\n", cmd);
        }
        fflush(stdout);
    }
    free(rom);
    return 0;
}
