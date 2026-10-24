/*
 * pc98disk: readers for the disk image formats PC-98 software is usually kept in. The image is a byte array the caller
 * owns (read from a file, or mapped); nothing is copied and nothing is allocated. Every offset and size in a header is
 * checked against the image size, so a damaged or hostile file gives an error, never a read outside the array.
 *
 *   D88  floppy images (the de facto standard of Japanese emulators): a 2B0h-byte header with a name, a write-protect
 *        flag, the media type (2D, 2DD, 2HD) and a table of up to 164 track offsets; each track is a list of sectors,
 *        each with its own 16-byte header (C, H, R, N, sectors in the track, density, deleted mark, FDC status, size).
 *        Sectors are looked up by their ID (C, H, R), as an FDC would, so odd layouts (copy protection, mixed sizes)
 *        read the way they were recorded.
 *   FDI  Anex86 floppy images: a header of eight little-endian 32-bit words (0, type, header size, data size, sector
 *        size, sectors per track, heads, cylinders), then the sectors in C/H/R order.
 *   HDI  Anex86 hard disk images: the same header layout for a hard disk.
 *   RAW  plain sector dumps (.hdm, .xdf, .img) of the usual floppy sizes: 1232 KiB 2HD (77x2x8x1024), 1440 KiB,
 *        720 KiB and 640 KiB.
 */
#ifndef GAP_PC98DISK_H
#define GAP_PC98DISK_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PC98_DISK_NONE = 0,     /* as a hint: detect */
    PC98_DISK_RAW,
    PC98_DISK_D88,
    PC98_DISK_FDI,
    PC98_DISK_HDI,
} Pc98DiskFormat;

enum {
    PC98_DISK_OK = 0,
    PC98_DISK_EFORMAT = -1,     /* not an image of the expected (or any known) format */
    PC98_DISK_ECORRUPT = -2,    /* a header field points outside the image or makes no sense */
    PC98_DISK_ENOSECTOR = -3,   /* no such sector */
};

#define PC98_D88_HEADER     0x2B0u
#define PC98_D88_MAXTRACKS  164
#define PC98_D88_WP         0x10    /* write-protect byte value */
#define PC98_D88_2D         0x00
#define PC98_D88_2DD        0x10
#define PC98_D88_2HD        0x20

typedef struct {
    Pc98DiskFormat format;
    const uint8_t *data;
    size_t size;
    /* Geometry: for FDI, HDI and RAW straight from the header (or the size); for D88 taken from the image: heads from
     * the track table, cylinders from the last present track, sectors and sector size from the first track. */
    uint32_t cylinders, heads, sectors, sector_size;
    uint32_t data_offset;           /* FDI, HDI, RAW: offset of the first sector */
    uint32_t media;                 /* D88 media byte, FDI/HDI type word */
    uint8_t write_protect;
    char name[17];                  /* D88 disk name (NUL-terminated, may be empty) */
    uint32_t ntracks;               /* D88: entries in the track table */
    uint32_t track[PC98_D88_MAXTRACKS];   /* D88: offset of each track, 0 = not recorded */
} Pc98Disk;

/* A sector as stored in the image. */
typedef struct {
    uint8_t c, h, r, n;             /* ID field: cylinder, head, record (1-based), size code (128 << n) */
    uint8_t deleted;                /* D88: deleted data mark */
    uint8_t status;                 /* D88: FDC status recorded with it (0 = fine) */
    uint8_t density;                /* D88: 00h MFM, 40h FM */
    const uint8_t *data;
    uint32_t size;
} Pc98Sector;

/* Opens an image of `size` bytes. `hint` is the format the file name suggests (PC98_DISK_NONE: detect). */
int pc98_disk_open(Pc98Disk *d, const uint8_t *data, size_t size, Pc98DiskFormat hint);
/* Finds the sector with ID (c, h, r) on physical track (cyl, head). For FDI/HDI/RAW the ID is the position. */
int pc98_disk_sector(const Pc98Disk *d, uint32_t cyl, uint32_t head, uint32_t r, Pc98Sector *out);
/* The idx-th sector of a D88 track in recorded order (0-based); other formats: the sector with R = idx + 1. */
int pc98_disk_sector_at(const Pc98Disk *d, uint32_t cyl, uint32_t head, uint32_t idx, Pc98Sector *out);
/* Number of sectors recorded on a track (0 for an absent D88 track). */
uint32_t pc98_disk_track_sectors(const Pc98Disk *d, uint32_t cyl, uint32_t head);
/* Logical block `lba` with the image's geometry (C/H/S order, sectors numbered from 1). */
int pc98_disk_read_lba(const Pc98Disk *d, uint32_t lba, Pc98Sector *out);
const char *pc98_disk_format_name(Pc98DiskFormat f);

#endif
