/*
 * Writing the ESP the system started from, from user space: the ESP write grant (shared/blkabi.h, BLKESPGRANT) and the update sequence
 * that survives a power cut (docs/UPDATE.md, "Writing the ESP"). Used by the `espgrant` applet; meant for the `update` client too.
 *
 *   EspSession s;
 *   if (esp_open(&s, NULL, 0, err, sizeof err)) ...            grant + read-write mount on a private directory (root only)
 *   EspFile f[] = { { "kernel.elf", data, len }, { "initrd.img", data2, len2 } };
 *   if (esp_update(&s, f, 2, 0, say, err, sizeof err)) ...     stage *.new, sync, re-mount and compare, rename over, sync
 *   esp_close(&s);                                             sync, unmount, hand the grant back
 *
 * If the process ends early (a crash, kill -9), the kernel ends the grant when it closes the descriptor: the volume is written back and
 * stays mounted read-only on s.dir until somebody unmounts it.
 */
#ifndef KB_ESPGRANT_H
#define KB_ESPGRANT_H

#include <stddef.h>

typedef struct {
    char dev[32];               /* "nvme0n1p1" */
    char dir[128];              /* where it is mounted */
    int fd;                     /* the open partition node that holds the grant, -1 when none */
    int mounted, made_dir, force;
} EspSession;

typedef struct {
    const char *name;           /* relative to the ESP's root: "kernel.elf", "EFI/BOOT/BOOTX64.EFI" */
    const void *data;
    size_t len;
} EspFile;

/* The partition the grant may be given for, from /proc/bootdisk ("esp NAME ..."). 0, or -1 with the kernel's reason in why. */
int  esp_find(char *dev, size_t cap, char *why, size_t wn);
/* Grant and read-write mount on dir (NULL: /BambooForest/espgrant.PID, made with mode 0700 and removed again). force mounts a volume
 * that is flagged dirty (it then stays flagged). 0, or -1 with a message in err. */
int  esp_open(EspSession *s, const char *dir, int force, char *err, size_t en);
/* Unmount and mount again (with a fresh grant), so that what is read afterwards comes from the disk, not from the cache. */
int  esp_remount(EspSession *s, char *err, size_t en);
/* sync, unmount, remove the directory if esp_open made it, close the grant. 0, or -1 when the unmount failed. */
int  esp_close(EspSession *s);
/* The update sequence (docs/UPDATE.md): every file is staged as NAME.new and flushed; the volume is mounted again and every staged file
 * compared with what it should hold; with keep_old the current NAME is copied to its .old name (kernel.elf -> kernel.old) through
 * OLD.new and a rename; then each NAME.new is renamed over NAME (files below EFI/ last) with a sync after each. A power cut at any point
 * leaves every NAME either old or new in full. say(step, name) reports each step (may be NULL). 0, or -1 with a message in err; a
 * failure before the first rename removes the staged files again. */
int  esp_update(EspSession *s, const EspFile *files, int n, int keep_old, void (*say)(const char *step, const char *name),
                char *err, size_t en);
/* NAME -> its .old name: the extension replaced ("kernel.elf" -> "kernel.old"), so it stays an 8.3 name the firmware can open. */
void esp_old_name(const char *name, char *out, size_t cap);

#endif
