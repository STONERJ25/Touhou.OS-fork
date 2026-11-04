/*
 * disks: the disk images of the PC-98 application. Every image is read into memory whole when it is inserted; that copy is the
 * disk the guest sees (pc98_bios_attach(), the HLE BIOS writes sectors straight into it) and the FILE IS NEVER OPENED FOR WRITING
 * again: a game that saves its score, a DOS that writes a directory entry, a program that formats the disk, all of it lands in
 * the copy and is gone when the program ends unless the owner asks for it with "Save changes as...". That is the overlay of
 * the brief: the original is read-only for good, and what the guest changed is kept beside it in memory.
 *
 *   - "What changed" is known to the block: a checksum of every 512 bytes of the image is taken when it is loaded (or saved),
 *     and disk_changed() compares them with the copy as it is now. A guest that writes the same bytes it read has changed nothing.
 *   - disk_save_as() writes the whole copy to a NEW file and refuses an existing one (open with O_EXCL): the original can never
 *     be overwritten, not even by a click on the wrong name. After a save the new file is the image's file and nothing is
 *     "changed" until the guest writes again.
 *   - Formats are whatever pc98disk.h reads (D88, FDI, HDI, and raw floppy dumps); the drive is chosen by the format: a hard
 *     disk image (HDI) is a hard disk (DA/UA 80h, 81h), everything else a 1 MB floppy (90h, 91h).
 * A hostile or broken file gives an error message, never a crash: pc98disk checks every header field against the file's size.
 *
 * Pure C over the library's pc98disk (the host tests compile it as it is).
 */
#ifndef PC98APP_DISKS_H
#define PC98APP_DISKS_H

#include <stddef.h>
#include <stdint.h>
#include <pc98disk.h>

#define DISK_SLOTS 4
enum { SLOT_FD1 = 0, SLOT_FD2, SLOT_HD1, SLOT_HD2 };
#define DISK_BLOCK 512
#define DISK_MAX_BYTES (256u * 1024u * 1024u)
#define DISK_PATH_MAX 512

typedef struct {
    int loaded;
    char path[DISK_PATH_MAX];       /* the file the image came from or was last saved to ("" for an image made in memory) */
    uint8_t *data;                  /* the copy the guest sees and writes */
    size_t size;
    Pc98Disk disk;                  /* points into data */
    uint32_t *sums;                 /* one checksum per DISK_BLOCK bytes, of the image as loaded or last saved */
    uint32_t nblocks;
} DiskSlot;

uint8_t disk_slot_daua(int slot);                       /* 90h, 91h (floppies), 80h, 81h (hard disks) */
const char *disk_slot_name(int slot);                   /* "floppy 1", "floppy 2", "hard disk 1", "hard disk 2" */
int disk_slot_is_hd(int slot);
/* The slot an image of this format goes to: the first free one of its kind, else the first of its kind. */
int disk_slot_for(Pc98DiskFormat format, const DiskSlot slots[DISK_SLOTS]);

/* Reads the file whole, checks it and fills the slot (replacing what was in it). 0, or -1 with a sentence in err. */
int disk_load_file(DiskSlot *s, const char *path, char *err, size_t errcap);
/* The same for bytes the caller has (copied): tests, and images made by the application. `name` becomes the path. */
int disk_load_memory(DiskSlot *s, const uint8_t *bytes, size_t size, const char *name, char *err, size_t errcap);
void disk_unload(DiskSlot *s);

/* Blocks that differ from the image as loaded (or last saved); *first gets the index of the first one (when not NULL). */
uint32_t disk_changed(const DiskSlot *s, uint32_t *first);
/* Writes the copy to a new file. 0, or -1 with a sentence in err (the file exists, no room, the folder is read-only ...). */
int disk_save_as(DiskSlot *s, const char *path, char *err, size_t errcap);
/* Comments out the extended-memory lines of a DOS CONFIG.SYS in the copy (HIMEM.SYS, EMM386.EXE /UMB, DOS=HIGH, DOS=UMB: the emulator has
 * neither extended memory nor V86 mode and the games need neither): same length, and the image's starting point is taken after the edit.
 * Returns the number of lines changed. */
int disk_virtual_config(DiskSlot *s);
/* The last part of the path. */
const char *disk_basename(const char *path);

#endif
