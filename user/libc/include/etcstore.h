#ifndef _ETCSTORE_H
#define _ETCSTORE_H
#include <stddef.h>

/*
 * Keine's records: the system files that survive a restart (docs/PERSISTENT_ETC.md).
 *
 * /etc (= /Hakurei/etc) lives in RAM and is rebuilt from the initrd at every boot. The files named below are also kept on the
 * HUMANVILLAGE volume, in /HumanVillage/.system/etc, and init copies them over the initrd defaults at boot (etc_restore).
 * A tool that changes one of them calls etc_commit(): the new text is checked, written to /etc atomically and, when a volume
 * is mounted, to the volume as well (temporary file, sync, rename, sync), so a power cut leaves either the old or the new file.
 */

#define ETC_KEEP_DIR   "/HumanVillage/.system/etc"
#define ETC_MAX_BYTES  65536

/* The kept files, NULL-terminated: passwd shadow group hostname timezone network.conf update.conf. */
extern const char *const etc_kept_names[];

int      etc_is_kept(const char *name);                       /* 1 if NAME is one of the kept files */
unsigned etc_file_mode(const char *name);                     /* 0600 for shadow, 0644 for the rest */

/* Checks the text of a kept file. 0 if it is well-formed; -1 and a reason ("line 3: the uid is not a number") otherwise. */
int etc_validate(const char *name, const char *data, size_t len, char *why, size_t whylen);

/* 1 when /HumanVillage is a mounted volume (and so can keep files), 0 when the homes are in RAM. *ro is set for a
 * read-only mount (a volume that was not cleanly unmounted). */
int etc_volume_present(int *ro);

/* Writes PATH atomically: PATH.new is written (mode MODE from the start), closed, then renamed over PATH. With `durable`
 * the file systems are synced before and after the rename. 0 or -1 with errno. */
int etc_write_atomic(const char *path, const void *data, size_t len, unsigned mode, int durable);

/* Replaces /etc/NAME with DATA (checked first) and keeps a copy on the volume.
 *   0  done (kept on the volume, or there is no volume: then /etc is all there is, as it always was)
 *   1  /etc changed, but the volume could not keep it: the change lasts until the next restart; etc_note() says why
 *  -1  nothing changed: errno, and etc_note() says why (EINVAL: the text is not well-formed) */
int etc_commit(const char *name, const void *data, size_t len);

/* Copies the current /etc/NAME to the volume (after a hand edit). Same results as etc_commit. */
int etc_save(const char *name);

/* Removes the volume's copy of NAME: the initrd default comes back at the next boot. 0, or -1 with errno. */
int etc_forget(const char *name);

/* The reason given by the last etc_commit/etc_save that did not return 0 ("the HUMANVILLAGE volume is read-only"). */
const char *etc_note(void);

/* Boot (init, as root): overlays /etc with the volume's copies. Every copy is checked; a damaged one is skipped (the initrd
 * default stays) with one line through `log`. Files whose /etc copy changed since `snap` was taken are left alone (a late
 * USB stick must not undo a change made in the meantime); pass NULL to restore everything. Returns how many files were
 * restored, -1 when there is no volume. */
typedef struct EtcSnapshot { long mtime[8]; long size[8]; } EtcSnapshot;
void etc_snapshot(EtcSnapshot *snap);
int  etc_restore(const EtcSnapshot *snap, void (*log)(const char *line));

/* After /etc/passwd changed: tells the volume on /HumanVillage which resident owns which home (FATIOC_USERS_RELOAD).
 * Root only; quietly does nothing without a volume. Returns the number of residents, or -1. */
int etc_reload_homes(void);

#endif
