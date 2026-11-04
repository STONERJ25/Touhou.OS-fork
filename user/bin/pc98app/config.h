/*
 * config: the PC-98 application's settings file (~/.config/pc98/config) and its list of recently used disk images
 * (~/.config/pc98/recent). Plain text, one `key=value` per line, `#` starts a comment:
 *
 *     font=/HumanVillage/pc98/FONT.ROM        the owner's own font ROM (288,768 bytes), read at run time, never shipped
 *     rhythm=/HumanVillage/pc98/rhythm        a folder with 2608_BD.WAV, 2608_SD.WAV, 2608_TOP.WAV, 2608_HH.WAV, 2608_TOM.WAV, 2608_RIM.WAV
 *     cpu=386            8086, v30, 286, 386 or 486        mhz=16      the CPU clock in MHz (1 to 200)
 *     fm=86              86 (PC-9801-86, YM2608), 26k (PC-9801-26K, YM2203) or none
 *     sound=on           on or off                         volume=80   0 to 100
 *     scale=1            1, 2, 3, 4 or fit (the largest whole number that fits the window)
 *     speed=100          percent of real time, or max      idle=on     skip the guest's busy waits (idle.h)
 *     virtual_config=on  comment out the extended-memory lines of CONFIG.SYS in the in-memory copy of a disk (TH04 needs this: disks.h)
 *
 * The parser is forgiving on purpose: an unknown key, a value that makes no sense or a line without `=` is skipped (and
 * reported in the `report` text, one line each) and everything else is applied: a file written by a newer version, or by hand,
 * never keeps the application from starting. Pure C with no dependencies (the host tests compile it as it is).
 */
#ifndef PC98APP_CONFIG_H
#define PC98APP_CONFIG_H

#include <stddef.h>

#define CONFIG_PATH_MAX 256
#define RECENT_MAX 8

enum { CFG_CPU_8086 = 0, CFG_CPU_V30 = 1, CFG_CPU_286 = 2, CFG_CPU_386 = 3, CFG_CPU_486 = 4 };      /* the values of X86Model */
enum { CFG_FM_NONE = 0, CFG_FM_86 = 1, CFG_FM_26K = 2 };

typedef struct {
    char font[CONFIG_PATH_MAX];     /* "" = none: the library's fallback font */
    char rhythm[CONFIG_PATH_MAX];   /* "" = no drum samples */
    int cpu;                        /* CFG_CPU_* */
    int mhz;                        /* 1..200 */
    int fm;                         /* CFG_FM_* */
    int sound;                      /* 0 / 1 */
    int volume;                     /* 0..100 */
    int scale;                      /* 1..4, or 0 = fit the window */
    int speed;                      /* percent, 0 = as fast as possible */
    int idle;                       /* 0 / 1: the idle skip */
    int vconfig;                    /* 0 / 1: comment out HIMEM.SYS, EMM386.EXE and DOS=HIGH/UMB in the in-memory copy of a disk (disks.h) */
} Pc98Config;

/* The defaults: a 386 at 16 MHz (what every PC-98 game of the pack was found to need, docs/PC98.md), the -86 FM board, sound
 * on at volume 80, 1x, real time, idle skip on. */
void config_defaults(Pc98Config *c);
/* Applies the lines of `text` (len bytes) to *c. Returns how many settings were applied. `report`, when not NULL, gets one
 * line "line N: ..." per line that was skipped. */
int config_parse(Pc98Config *c, const char *text, size_t len, char *report, size_t report_cap);
/* Writes the whole configuration in the same format. Returns the length, or the length it would need if cap was too small. */
size_t config_format(const Pc98Config *c, char *out, size_t cap);

const char *config_cpu_name(int cpu);
const char *config_fm_name(int fm);

/* The recently used images, newest first. */
typedef struct {
    char path[RECENT_MAX][CONFIG_PATH_MAX];
    int n;
} Pc98Recent;

void recent_parse(Pc98Recent *r, const char *text, size_t len);
/* Puts `path` first (moving it up when it is in the list already), drops the oldest beyond RECENT_MAX. */
void recent_add(Pc98Recent *r, const char *path);
size_t recent_format(const Pc98Recent *r, char *out, size_t cap);

#endif
